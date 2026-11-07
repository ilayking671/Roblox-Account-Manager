import webview
import requests
import json
import os
import sys
import time
import subprocess
import threading
import random
import re
from pathlib import Path

APP_DIR = Path(__file__).parent.resolve()
ACCOUNTS_FILE = APP_DIR / "accounts.json"
HISTORY_FILE  = APP_DIR / "history.json"
DEBUG_FILE    = APP_DIR / "launch_debug.txt"
VERSIONS_DIR  = Path(os.environ.get("LOCALAPPDATA", "")) / "Roblox" / "Versions"

WARNING_PREFIX = "_|WARNING:-DO-NOT-SHARE-THIS.--Sharing-this-will-allow-someone-to-log-in-to-your-account--|_"

def strip_warning(cookie):
    c = cookie.strip()
    if c.startswith(WARNING_PREFIX):
        c = c[len(WARNING_PREFIX):]
    return c

def load_json(path, default):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return default

def save_json(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

def rblx_headers(cookie=None, csrf=None):
    h = {
        "User-Agent": "Roblox/WinInet",
        "Accept": "application/json",
        "Content-Type": "application/json",
        "Referer": "https://www.roblox.com/",
        "Origin": "https://www.roblox.com",
    }
    if cookie:
        h["Cookie"] = f".ROBLOSECURITY={cookie}"
    if csrf:
        h["X-CSRF-TOKEN"] = csrf
    return h

def fetch_auth_ticket(cookie):
    url = "https://auth.roblox.com/v1/authentication-ticket"
    try:
        r1 = requests.post(url, headers=rblx_headers(cookie), data=b"")
        csrf = r1.headers.get("x-csrf-token", "")
        if not csrf:
            return None, f"CSRF failed (HTTP {r1.status_code})"
        r2 = requests.post(url, headers=rblx_headers(cookie, csrf), data=b"")
        ticket = r2.headers.get("rbx-authentication-ticket", "")
        if ticket:
            return ticket, None
        return None, f"No ticket in response (HTTP {r2.status_code})"
    except Exception as e:
        return None, str(e)

def fetch_username(cookie):
    try:
        r = requests.get("https://users.roblox.com/v1/users/authenticated", headers=rblx_headers(cookie))
        if r.status_code == 200:
            return r.json().get("name")
    except Exception:
        pass
    return None

def fetch_place_name(place_id):
    try:
        r = requests.get(
            f"https://games.roblox.com/v1/games/multiget-place-details?placeIds={place_id}",
            headers={"User-Agent": "Roblox/WinInet"},
        )
        if r.status_code == 200:
            data = r.json()
            if data:
                return data[0].get("name", f"Place {place_id}")
    except Exception:
        pass
    return f"Place {place_id}"

def fetch_thumbnail(place_id):
    try:
        r = requests.get(
            f"https://games.roblox.com/v1/games/multiget-place-details?placeIds={place_id}",
            headers={"User-Agent": "Roblox/WinInet"},
        )
        if r.status_code == 200:
            uid = r.json()[0].get("universeId")
            if uid:
                r2 = requests.get(
                    f"https://thumbnails.roblox.com/v1/games/icons?universeIds={uid}"
                    "&returnPolicy=PlaceHolder&size=150x150&format=Png&isCircular=false",
                    headers={"User-Agent": "Roblox/WinInet"},
                )
                if r2.status_code == 200:
                    td = r2.json()
                    if td.get("data"):
                        return td["data"][0].get("imageUrl", "")
    except Exception:
        pass
    return ""

def scan_versions():
    versions = []
    if not VERSIONS_DIR.exists():
        return versions
    for d in sorted(VERSIONS_DIR.iterdir(), reverse=True):
        if d.is_dir() and (d / "RobloxPlayerBeta.exe").exists():
            versions.append({"name": d.name, "path": str(d)})
    return versions

def url_encode(s):
    out = []
    for c in s:
        if c.isalnum() or c in "-_.~":
            out.append(c)
        else:
            out.append(f"%{ord(c):02X}")
    return "".join(out)

def roblox_launch(exe, ticket, place_id):
    ts = int(time.time())
    tracker = ts * 1000 + random.randint(1, 999)

    if place_id > 0:
        launcher_url = (
            "https://assetgame.roblox.com/game/PlaceLauncher.ashx"
            f"?request=RequestGame"
            f"&browserTrackerId={tracker}"
            f"&placeId={place_id}"
            f"&isPlayTogetherGame=false"
        )
        encoded_url = url_encode(launcher_url)
        protocol_arg = (
            f"roblox-player:1"
            f"+launchmode:play"
            f"+gameinfo:{ticket}"
            f"+launchtime:{ts}"
            f"+placelauncherurl:{encoded_url}"
            f"+browsertrackerid:{tracker}"
            f"+robloxLocale:en_us"
            f"+gameLocale:en_us"
            f"+channel:"
        )
        cmd = f'"{exe}" {protocol_arg}'
    else:
        cmd = (
            f'"{exe}" --app'
            f' -a https://auth.roblox.com/v1/authentication-ticket/redeem'
            f' -t {ticket}'
            f' --launchtime={ts}'
        )

    work_dir = os.path.dirname(exe)

    with open(DEBUG_FILE, "w") as f:
        f.write(f"PLACE_ID:  {place_id}\n")
        f.write(f"MODE:      {'GAME JOIN (protocol)' if place_id > 0 else 'HOME (--app)'}\n")
        f.write(f"EXE:       {exe}\n")
        f.write(f"WORK:      {work_dir}\n")
        f.write(f"CMD:       {cmd}\n")

    try:
        proc = subprocess.Popen(cmd, cwd=work_dir, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        time.sleep(2)
        with open(DEBUG_FILE, "a") as f:
            f.write(f"LAUNCHED:  YES (pid={proc.pid})\n")
            rc = proc.poll()
            f.write(f"EXIT_CODE: {'still running' if rc is None else f'exited ({rc})'}\n")
        return True
    except Exception as e:
        with open(DEBUG_FILE, "a") as f:
            f.write(f"LAUNCHED:  NO ({e})\n")
        return False


class Api:
    def __init__(self):
        self._window = None
        self._accounts = load_json(ACCOUNTS_FILE, [])
        self._versions = scan_versions()

    def set_window(self, w):
        self._window = w

    def _js(self, code):
        if self._window:
            self._window.evaluate_js(code)

    def get_accounts(self):
        return [{"name": a["name"], "home_place": a.get("home_place", "")} for a in self._accounts]

    def add_account(self, cookie, home_place=""):
        cookie = strip_warning(cookie.strip())
        if not cookie:
            return {"ok": False, "error": "Empty cookie"}
        username = fetch_username(cookie)
        if not username:
            return {"ok": False, "error": "Invalid cookie"}
        for a in self._accounts:
            if a["name"] == username:
                a["cookie"] = cookie
                if home_place:
                    a["home_place"] = home_place
                save_json(ACCOUNTS_FILE, self._accounts)
                return {"ok": True, "name": username, "updated": True}
        self._accounts.append({"name": username, "cookie": cookie, "home_place": home_place})
        save_json(ACCOUNTS_FILE, self._accounts)
        return {"ok": True, "name": username}

    def remove_account(self, name):
        self._accounts = [a for a in self._accounts if a["name"] != name]
        save_json(ACCOUNTS_FILE, self._accounts)
        return {"ok": True}

    def set_home_place(self, name, place_id):
        for a in self._accounts:
            if a["name"] == name:
                a["home_place"] = place_id
                save_json(ACCOUNTS_FILE, self._accounts)
                return {"ok": True}
        return {"ok": False}

    def get_versions(self):
        self._versions = scan_versions()
        return self._versions

    def get_history(self):
        return load_json(HISTORY_FILE, [])

    def launch(self, account_name, place_id_str, version_path):
        cookie = None
        for a in self._accounts:
            if a["name"] == account_name:
                cookie = a["cookie"]
                break
        if not cookie:
            self._js("onLaunchResult({ok:false, error:'No account selected'})")
            return

        self._js("showStatus('launch-status','Getting auth ticket…','')")
        time.sleep(0.3)
        ticket, auth_err = fetch_auth_ticket(cookie)
        if not ticket:
            err_msg = json.dumps(f"Auth ticket failed: {auth_err}")[1:-1]
            self._js(f"onLaunchResult({{ok:false, error:'{err_msg}'}})")
            return
        self._js("showStatus('launch-status','Auth ticket acquired','ok')")
        time.sleep(0.4)

        place_id = 0
        clean = re.sub(r"[^0-9]", "", place_id_str or "")
        if clean:
            place_id = int(clean)

        if place_id > 0:
            self._js(f"showStatus('launch-status','Joining game {place_id}…','')")
        else:
            self._js("showStatus('launch-status','No game ID — launching home…','')")
        time.sleep(0.4)

        self._js("showStatus('launch-status','Launching Roblox…','')")
        time.sleep(0.2)

        exe = os.path.join(version_path, "RobloxPlayerBeta.exe")
        ok = roblox_launch(exe, ticket, place_id)

        if not ok:
            self._js("onLaunchResult({ok:false, error:'RobloxPlayerBeta.exe failed to start'})")
            return

        if place_id > 0:
            name = fetch_place_name(place_id)
            ename = json.dumps(name)[1:-1]
            self._js(f'onLaunchResult({{ok:true, mode:"game", name:"{ename}", placeId:{place_id}}})')
            self._record_history(place_id, name)
        else:
            self._js('onLaunchResult({ok:true, mode:"home"})')

    def _record_history(self, place_id, name):
        history = load_json(HISTORY_FILE, [])
        history = [h for h in history if str(h.get("place_id")) != str(place_id)]
        thumb = fetch_thumbnail(place_id)
        history.insert(0, {
            "place_id": str(place_id), "name": name,
            "thumbnail": thumb, "timestamp": int(time.time()),
        })
        save_json(HISTORY_FILE, history[:20])


HTML = r"""<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Roblox Launcher</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#080808;--surface:#111;--surface2:#161616;
  --border:#222;--border2:#2a2a2a;
  --text:#f0f0f0;--sub:#888;--accent:#fff;--danger:#e05555;
  --ok:#4caf50;
}
html{-webkit-user-select:none;user-select:none}
body{font-family:'Segoe UI',system-ui,sans-serif;font-size:13px;color:var(--text);
  background:var(--bg);height:100vh;overflow:hidden;display:flex;flex-direction:column}

.titlebar{height:32px;background:var(--surface);display:flex;align-items:center;
  padding:0 12px;font-size:11px;color:var(--sub);border-bottom:1px solid var(--border);
  -webkit-app-region:drag}
.titlebar span{font-weight:600;color:var(--text);margin-right:6px}

.wrap{display:flex;flex:1;overflow:hidden}

.side{width:180px;min-width:180px;background:var(--surface);border-right:1px solid var(--border);
  display:flex;flex-direction:column;padding:8px}
.side h3{font-size:11px;color:var(--sub);text-transform:uppercase;letter-spacing:.8px;
  margin-bottom:6px;padding:0 4px}
.pill{padding:7px 10px;border-radius:6px;cursor:pointer;font-size:12px;margin-bottom:3px;
  color:var(--sub);transition:all .15s}
.pill:hover{background:var(--surface2);color:var(--text)}
.pill.active{background:var(--surface2);color:var(--accent);font-weight:600;
  border:1px solid var(--border2)}
.pill .x{float:right;opacity:0;font-size:10px;color:var(--danger);cursor:pointer;
  padding:0 4px;transition:opacity .15s}
.pill:hover .x{opacity:1}

#add-area{margin-top:auto;padding-top:8px;border-top:1px solid var(--border)}
#add-btn{width:100%;padding:7px;border:1px dashed var(--border2);border-radius:6px;
  background:none;color:var(--sub);cursor:pointer;font-size:12px;transition:all .15s}
#add-btn:hover{border-color:var(--accent);color:var(--text)}
#cookie-input{width:100%;padding:7px;border:1px solid var(--border2);border-radius:6px;
  background:var(--bg);color:var(--text);font-size:12px;margin-top:6px;display:none;
  outline:none}
#cookie-input:focus{border-color:var(--accent)}

.main{flex:1;padding:20px 24px;overflow-y:auto;display:flex;flex-direction:column;gap:16px}

.row{display:flex;gap:10px;align-items:center}
label{font-size:11px;color:var(--sub);text-transform:uppercase;letter-spacing:.6px;
  min-width:70px}
select,input[type=text]{flex:1;padding:8px 10px;border:1px solid var(--border);
  border-radius:6px;background:var(--surface);color:var(--text);font-size:13px;outline:none}
select:focus,input[type=text]:focus{border-color:var(--accent)}

#launch-btn{padding:10px 28px;border:none;border-radius:8px;background:var(--accent);
  color:var(--bg);font-weight:700;font-size:14px;cursor:pointer;transition:all .15s;
  align-self:flex-start}
#launch-btn:hover{opacity:.85}
#launch-btn:active{transform:scale(.97)}

.status{font-size:12px;min-height:18px;padding:4px 0}
.status.ok{color:var(--ok)}
.status.err{color:var(--danger)}

.hist-title{font-size:11px;color:var(--sub);text-transform:uppercase;letter-spacing:.8px;
  margin-top:8px}
.hist-grid{display:flex;flex-wrap:wrap;gap:10px;margin-top:8px}
.hist-card{width:120px;border-radius:8px;overflow:hidden;cursor:pointer;
  border:1px solid var(--border);background:var(--surface);transition:all .15s}
.hist-card:hover{border-color:var(--border2);transform:translateY(-2px)}
.hist-card img{width:100%;height:80px;object-fit:cover;display:block;background:var(--surface2)}
.hist-card .info{padding:6px 8px;font-size:11px;white-space:nowrap;overflow:hidden;
  text-overflow:ellipsis}
.hist-card .pid{font-size:10px;color:var(--sub)}
</style>
</head>
<body>

<div class="titlebar"><span>Roblox Launcher</span> python edition</div>

<div class="wrap">
  <div class="side">
    <h3>Accounts</h3>
    <div id="acc-list"></div>
    <div id="add-area">
      <button id="add-btn" onclick="toggleAddAccount()">+ Add Account</button>
      <input id="cookie-input" type="text" placeholder="Paste .ROBLOSECURITY cookie"
             onkeydown="if(event.key==='Enter')submitCookie()">
    </div>
  </div>

  <div class="main">
    <div class="row">
      <label>Version</label>
      <select id="ver-select"><option value="">Loading...</option></select>
    </div>

    <div class="row">
      <label>Place ID</label>
      <input type="text" id="place-input" placeholder="e.g. 6654918151 (blank = home)">
    </div>

    <button id="launch-btn" onclick="doLaunch()">Launch</button>

    <div id="launch-status" class="status"></div>

    <div class="hist-title">Recent</div>
    <div id="hist-grid" class="hist-grid"></div>
  </div>
</div>

<script>
let selAcc = null;
let _vers = [];

function showStatus(id, msg, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = msg;
  el.className = 'status' + (cls ? ' ' + cls : '');
}

window.onLaunchResult = function(r) {
  if (!r.ok) { showStatus('launch-status', 'Error: ' + (r.error || 'unknown'), 'err'); return; }
  if (r.mode === 'game') showStatus('launch-status', 'Launched: ' + (r.name || r.placeId), 'ok');
  else showStatus('launch-status', 'Launched to home', 'ok');
};

function renderAccounts(accounts) {
  const el = document.getElementById('acc-list');
  el.innerHTML = '';
  accounts.forEach(a => {
    const d = document.createElement('div');
    d.className = 'pill' + (selAcc === a.name ? ' active' : '');
    d.innerHTML = a.name + '<span class="x" onclick="event.stopPropagation();removeAcc(\'' +
      a.name.replace(/'/g, "\\'") + '\')">×</span>';
    d.onclick = () => pickAcc(a.name);
    el.appendChild(d);
  });
}

function pickAcc(name) {
  selAcc = name;
  document.querySelectorAll('.pill').forEach(p => {
    p.classList.toggle('active', p.textContent.replace('×', '').trim() === name);
  });
}

async function removeAcc(name) {
  await pywebview.api.remove_account(name);
  if (selAcc === name) selAcc = null;
  const accs = await pywebview.api.get_accounts();
  renderAccounts(accs);
}

function toggleAddAccount() {
  const inp = document.getElementById('cookie-input');
  inp.style.display = inp.style.display === 'none' ? 'block' : 'none';
  if (inp.style.display === 'block') inp.focus();
}

async function submitCookie() {
  const inp = document.getElementById('cookie-input');
  const cookie = inp.value.trim();
  if (!cookie) return;
  showStatus('launch-status', 'Adding account…', '');
  const r = await pywebview.api.add_account(cookie);
  if (!r.ok) {
    showStatus('launch-status', 'Error: ' + (r.error || 'failed'), 'err');
    return;
  }
  inp.value = '';
  inp.style.display = 'none';
  showStatus('launch-status', 'Added: ' + r.name, 'ok');
  selAcc = r.name;
  const accs = await pywebview.api.get_accounts();
  renderAccounts(accs);
}

function renderVersions(vers) {
  _vers = vers;
  const sel = document.getElementById('ver-select');
  sel.innerHTML = '';
  if (!vers.length) {
    sel.innerHTML = '<option value="">No versions found</option>';
    return;
  }
  vers.forEach((v, i) => {
    const o = document.createElement('option');
    o.value = i;
    o.textContent = v.name;
    sel.appendChild(o);
  });
}

function renderHistory(entries) {
  const grid = document.getElementById('hist-grid');
  grid.innerHTML = '';
  entries.forEach(e => {
    const card = document.createElement('div');
    card.className = 'hist-card';
    card.onclick = () => {
      document.getElementById('place-input').value = e.place_id;
    };
    card.innerHTML =
      '<img src="' + (e.thumbnail || '') + '" onerror="this.style.display=\'none\'">' +
      '<div class="info">' + (e.name || 'Unknown') + '</div>' +
      '<div class="info pid">' + e.place_id + '</div>';
    grid.appendChild(card);
  });
}

async function doLaunch() {
  if (!selAcc) { showStatus('launch-status', 'Select an account first', 'err'); return; }
  const idx = document.getElementById('ver-select').value;
  const ver = _vers[parseInt(idx)];
  if (!ver) { showStatus('launch-status', 'Select a version first', 'err'); return; }
  const raw = document.getElementById('place-input').value.trim();
  const place = (raw && /^\d+$/.test(raw)) ? raw : 'home';
  showStatus('launch-status', 'Starting…', '');
  await pywebview.api.launch(selAcc, place, ver.path);
  if (place !== 'home') {
    const hist = await pywebview.api.get_history();
    renderHistory(hist);
  }
}

window.addEventListener('pywebviewready', async () => {
  const accs = await pywebview.api.get_accounts();
  renderAccounts(accs);
  if (accs.length) pickAcc(accs[0].name);

  const vers = await pywebview.api.get_versions();
  renderVersions(vers);

  const hist = await pywebview.api.get_history();
  renderHistory(hist);
});
</script>
</body>
</html>"""


if __name__ == "__main__":
    api = Api()
    window = webview.create_window(
        "Roblox Launcher",
        html=HTML,
        js_api=api,
        width=900,
        height=600,
        min_size=(680, 450),
        background_color="#080808",
    )
    api.set_window(window)
    webview.start(debug=False)
