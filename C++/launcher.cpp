#include "webview/webview.h"
#include "miniz.h"
#include "launcher.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

struct AppState {
    webview_t        wv;
    AccountStore     accounts;
    VersionList      versions;
    char             accounts_path[PATH_LEN];
    char             versions_dir[PATH_LEN];
    CRITICAL_SECTION lock;
};
static AppState    g_app;
static HINTERNET   g_session = nullptr;

static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    wchar_t *w = (wchar_t *)malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    char *s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, nullptr, nullptr);
    return s;
}

void json_escape(const char *in, char *out, size_t len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < len; i++) {
        switch (in[i]) {
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:   out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

int json_str_arg(const char *arr, int idx, char *out, size_t len) {
    const char *p = arr;
    while (*p && *p != '[') p++;
    if (!*p) return 0;
    p++;
    for (int i = 0; ; i++) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ']' || !*p) return 0;
        if (i == idx) {
            if (*p == '"') {
                p++;
                size_t n = 0;
                while (*p && *p != '"' && n + 1 < len) {
                    if (*p == '\\' && *(p + 1)) {
                        p++;
                        switch (*p) {
                        case 'n': out[n++] = '\n'; break;
                        case 'r': out[n++] = '\r'; break;
                        case 't': out[n++] = '\t'; break;
                        default:  out[n++] = *p;
                        }
                    } else {
                        out[n++] = *p;
                    }
                    p++;
                }
                out[n] = '\0';
                return (int)n;
            }
            size_t n = 0;
            while (*p && *p != ',' && *p != ']' && n + 1 < len) out[n++] = *p++;
            out[n] = '\0';
            return (int)n;
        }
        if (*p == '"') { p++; while (*p && !(*p == '"' && *(p-1) != '\\')) p++; if (*p) p++; }
        else { while (*p && *p != ',' && *p != ']') p++; }
        if (*p == ',') p++;
    }
}

static void strip_warning(char *cookie) {
    const char *prefix = "_|WARNING:-DO-NOT-SHARE-THIS.--Sharing-this-will-allow-someone-to-log-in-to-your-account--|_";
    size_t plen = strlen(prefix);
    if (!strncmp(cookie, prefix, plen))
        memmove(cookie, cookie + plen, strlen(cookie + plen) + 1);
}

static void url_encode(const char *in, char *out, size_t len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 3 < len; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = c;
        } else {
            j += snprintf(out + j, len - j, "%%%02X", (unsigned char)c);
        }
    }
    out[j] = '\0';
}

static void http_result_free(HttpResult *r) {
    if (r->data) { free(r->data); r->data = nullptr; }
}

static int do_http(const wchar_t *verb, const wchar_t *host, const wchar_t *path,
                   const char *cookie, const char *csrf, const BYTE *body, DWORD body_len,
                   HttpResult *out) {
    memset(out, 0, sizeof(*out));
    if (!g_session) {
        g_session = WinHttpOpen(L"RobloxLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_session) return -1;
    }
    HINTERNET conn = WinHttpConnect(g_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) return -1;

    HINTERNET req = WinHttpOpenRequest(conn, verb, path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); return -1; }

    DWORD flags = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags));

    wchar_t hdrs[COOKIE_LEN + 512];
    int pos = 0;
    pos += swprintf(hdrs + pos, sizeof(hdrs)/sizeof(wchar_t) - pos,
                    L"Content-Type: application/json\r\nAccept: application/json\r\n"
                    L"Referer: https://www.roblox.com/\r\nOrigin: https://www.roblox.com\r\n");
    if (cookie && cookie[0]) {
        wchar_t *wc = to_wide(cookie);
        pos += swprintf(hdrs + pos, sizeof(hdrs)/sizeof(wchar_t) - pos,
                        L"Cookie: .ROBLOSECURITY=%ls\r\n", wc);
        free(wc);
    }
    if (csrf && csrf[0]) {
        wchar_t *wt = to_wide(csrf);
        pos += swprintf(hdrs + pos, sizeof(hdrs)/sizeof(wchar_t) - pos,
                        L"X-CSRF-TOKEN: %ls\r\n", wt);
        free(wt);
    }

    BOOL ok = WinHttpSendRequest(req, hdrs, (DWORD)wcslen(hdrs),
                                  (LPVOID)body, body_len, body_len, 0);
    if (!ok) { WinHttpCloseHandle(req); WinHttpCloseHandle(conn); return -1; }

    WinHttpReceiveResponse(req, nullptr);

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out->status = (int)status;

    wchar_t hdr_buf[TICKET_LEN];
    DWORD hdr_sz;

    hdr_sz = sizeof(hdr_buf);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"x-csrf-token",
                            hdr_buf, &hdr_sz, WINHTTP_NO_HEADER_INDEX)) {
        char *u = to_utf8(hdr_buf);
        strncpy(out->csrf, u, sizeof(out->csrf)-1);
        free(u);
    }
    hdr_sz = sizeof(hdr_buf);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"rbx-authentication-ticket",
                            hdr_buf, &hdr_sz, WINHTTP_NO_HEADER_INDEX)) {
        char *u = to_utf8(hdr_buf);
        strncpy(out->ticket, u, sizeof(out->ticket)-1);
        free(u);
    }

    DWORD total = 0, chunk;
    BYTE *buf = nullptr;
    BYTE tmp[8192];
    while (WinHttpReadData(req, tmp, sizeof(tmp), &chunk) && chunk > 0) {
        buf = (BYTE *)realloc(buf, total + chunk + 1);
        memcpy(buf + total, tmp, chunk);
        total += chunk;
    }
    if (buf) { buf[total] = 0; out->data = buf; out->data_len = total; }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return 0;
}

static int http_get(const wchar_t *host, const wchar_t *path, const char *cookie, HttpResult *out) {
    return do_http(L"GET", host, path, cookie, nullptr, nullptr, 0, out);
}

static int http_post(const wchar_t *host, const wchar_t *path, const char *cookie,
                     const char *csrf, HttpResult *out) {
    return do_http(L"POST", host, path, cookie, csrf, nullptr, 0, out);
}

static void accounts_save(const AccountStore *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"accounts\":[");
    for (int i = 0; i < s->count; i++) {
        char en[NAME_LEN*2], ec[COOKIE_LEN*2], eh[128];
        json_escape(s->items[i].name, en, sizeof(en));
        json_escape(s->items[i].cookie, ec, sizeof(ec));
        json_escape(s->items[i].home_place, eh, sizeof(eh));
        fprintf(f, "%s{\"name\":\"%s\",\"cookie\":\"%s\",\"home_place\":\"%s\"}",
                i ? "," : "", en, ec, eh);
    }
    fprintf(f, "]}");
    fclose(f);
}

static void accounts_load(AccountStore *s, const char *path) {
    s->count = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = 0;
    fclose(f);

    const char *p = buf;
    while ((p = strstr(p, "{\"name\"")) != nullptr) {
        if (s->count >= MAX_ACCOUNTS) break;
        Account *a = &s->items[s->count];
        memset(a, 0, sizeof(*a));
        const char *nk = strstr(p, "\"name\"");
        if (nk) { nk += 6; while (*nk && *nk != '"') nk++; if (*nk) nk++;
            int n = 0; while (*nk && *nk != '"' && n < NAME_LEN-1) {
                if (*nk == '\\' && *(nk+1)) { nk++; a->name[n++] = *nk++; } else a->name[n++] = *nk++;
            } a->name[n] = 0;
        }
        const char *ck = strstr(p, "\"cookie\"");
        if (ck) { ck += 8; while (*ck && *ck != '"') ck++; if (*ck) ck++;
            int n = 0; while (*ck && *ck != '"' && n < COOKIE_LEN-1) {
                if (*ck == '\\' && *(ck+1)) { ck++; a->cookie[n++] = *ck++; } else a->cookie[n++] = *ck++;
            } a->cookie[n] = 0;
        }
        const char *hk = strstr(p, "\"home_place\"");
        if (hk) { hk += 12; while (*hk && *hk != '"') hk++; if (*hk) hk++;
            int n = 0; while (*hk && *hk != '"' && n < 63) {
                if (*hk == '\\' && *(hk+1)) { hk++; a->home_place[n++] = *hk++; } else a->home_place[n++] = *hk++;
            } a->home_place[n] = 0;
        }
        s->count++;
        p++;
    }
    free(buf);
}

static void fetch_place_name(long long place_id, char *out, size_t len) {
    char pid[32]; snprintf(pid, sizeof(pid), "%lld", place_id);
    wchar_t *wpid = to_wide(pid);
    wchar_t wpath[256];
    swprintf(wpath, 256, L"/v1/games/multiget-place-details?placeIds=%ls", wpid);
    free(wpid);
    HttpResult r = {};
    http_get(L"games.roblox.com", wpath, nullptr, &r);
    if (r.status == 200 && r.data) {
        const char *np = strstr((char*)r.data, "\"name\"");
        if (np) {
            np += 6; while (*np && *np != '"') np++;
            if (*np == '"') np++;
            size_t n = 0;
            while (*np && *np != '"' && n + 1 < len) {
                if (*np == '\\' && *(np+1)) { np++; out[n++] = *np++; }
                else out[n++] = *np++;
            }
            out[n] = '\0';
        }
    }
    http_result_free(&r);
    if (!out[0]) snprintf(out, len, "Place %lld", place_id);
}

int fetch_auth_ticket(const char *cookie, char *out, size_t len) {
    HttpResult r1 = {};
    http_post(L"auth.roblox.com", L"/v1/authentication-ticket", cookie, nullptr, &r1);
    char csrf[128] = {};
    strncpy(csrf, r1.csrf, sizeof(csrf)-1);
    http_result_free(&r1);
    if (!csrf[0]) return -1;

    HttpResult r2 = {};
    http_post(L"auth.roblox.com", L"/v1/authentication-ticket", cookie, csrf, &r2);
    if (r2.ticket[0]) {
        strncpy(out, r2.ticket, len-1); out[len-1] = '\0';
        http_result_free(&r2);
        return 0;
    }
    http_result_free(&r2);
    return -1;
}

int fetch_username(const char *cookie, char *out, size_t len) {
    HttpResult r = {};
    if (http_get(L"users.roblox.com", L"/v1/users/authenticated", cookie, &r) != 0
        || r.status != 200) {
        http_result_free(&r); return -1;
    }
    const char *p = strstr((char *)r.data, "\"name\"");
    if (!p) { http_result_free(&r); return -1; }
    p += 6;
    while (*p && *p != '"') p++; if (!*p) { http_result_free(&r); return -1; } p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < len) out[n++] = *p++;
    out[n] = '\0';
    http_result_free(&r);
    return n > 0 ? 0 : -1;
}

static void versions_scan(VersionList *vl, const char *base_dir) {
    vl->count = 0;
    char pattern[PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", base_dir);
    wchar_t *wp = to_wide(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        char *name = to_utf8(fd.cFileName);
        char exe[PATH_LEN];
        snprintf(exe, sizeof(exe), "%s\\%s\\RobloxPlayerBeta.exe", base_dir, name);
        if (GetFileAttributesA(exe) != INVALID_FILE_ATTRIBUTES && vl->count < MAX_VERSIONS) {
            VersionEntry *v = &vl->items[vl->count++];
            strncpy(v->name, name, NAME_LEN-1);
            snprintf(v->path, PATH_LEN, "%s\\%s", base_dir, name);
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int roblox_launch(const char *exe, const char *ticket, long long place_id) {
    long long ts = (long long)time(nullptr);
    long long tracker = ts * 1000 + (rand() % 999 + 1);

    char cmd[8192];

    if (place_id > 0) {
        char raw_url[1024];
        snprintf(raw_url, sizeof(raw_url),
            "https://assetgame.roblox.com/game/PlaceLauncher.ashx"
            "?request=RequestGame"
            "&browserTrackerId=%lld"
            "&placeId=%lld"
            "&isPlayTogetherGame=false",
            tracker, place_id);

        char encoded_url[2048];
        url_encode(raw_url, encoded_url, sizeof(encoded_url));

        snprintf(cmd, sizeof(cmd),
            "\"%s\" "
            "roblox-player:1"
            "+launchmode:play"
            "+gameinfo:%s"
            "+launchtime:%lld"
            "+placelauncherurl:%s"
            "+browsertrackerid:%lld"
            "+robloxLocale:en_us"
            "+gameLocale:en_us"
            "+channel:",
            exe, ticket, ts, encoded_url, tracker);
    } else {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" --app "
            "-a https://auth.roblox.com/v1/authentication-ticket/redeem "
            "-t %s "
            "--launchtime=%lld",
            exe, ticket, ts);
    }

    char work_dir[PATH_LEN]; strncpy(work_dir, exe, sizeof(work_dir)-1);
    char *last_sep = strrchr(work_dir, '\\'); if (last_sep) *last_sep = '\0';

    {
        char dbg[PATH_LEN];
        GetModuleFileNameA(nullptr, dbg, sizeof(dbg));
        char *sl = strrchr(dbg, '\\'); if (sl) *(sl+1) = '\0';
        strncat(dbg, "launch_debug.txt", sizeof(dbg)-strlen(dbg)-1);
        FILE *df = fopen(dbg, "w");
        if (df) {
            fprintf(df, "PLACE_ID:  %lld\n", place_id);
            fprintf(df, "MODE:      %s\n", place_id > 0 ? "GAME JOIN (protocol)" : "HOME (--app)");
            fprintf(df, "EXE:       %s\n", exe);
            fprintf(df, "WORK:      %s\n", work_dir);
            fprintf(df, "CMD:       %s\n", cmd);
            fclose(df);
        }
    }

    wchar_t *wcmd     = to_wide(cmd);
    wchar_t *wworkdir = to_wide(work_dir);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, wcmd, nullptr, nullptr, FALSE,
                             CREATE_NEW_PROCESS_GROUP, nullptr, wworkdir, &si, &pi);
    DWORD launch_err = ok ? 0 : GetLastError();
    free(wcmd); free(wworkdir);

    {
        char dbg[PATH_LEN];
        GetModuleFileNameA(nullptr, dbg, sizeof(dbg));
        char *sl = strrchr(dbg, '\\'); if (sl) *(sl+1) = '\0';
        strncat(dbg, "launch_debug.txt", sizeof(dbg)-strlen(dbg)-1);
        FILE *df = fopen(dbg, "a");
        if (df) {
            fprintf(df, "LAUNCHED:  %s (err=%lu)\n", ok ? "YES" : "NO", launch_err);
            if (ok) {
                Sleep(2000);
                DWORD exit_code = 0;
                GetExitCodeProcess(pi.hProcess, &exit_code);
                fprintf(df, "EXIT_CODE: %s (%lu)\n",
                    exit_code == STILL_ACTIVE ? "still running" : "exited", exit_code);
            }
            fclose(df);
        }
    }

    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return 0; }
    return -1;
}

static void wv_eval(const char *js) {
    webview_eval(g_app.wv, js);
}

static void cb_get_state(const char *seq, const char *req, void *) {
    EnterCriticalSection(&g_app.lock);
    char buf[JSON_BUF]; int pos = 0;
    pos += snprintf(buf+pos, sizeof(buf)-pos, "{\"accounts\":[");
    for (int i = 0; i < g_app.accounts.count; i++) {
        char en[NAME_LEN*2], eh[128];
        json_escape(g_app.accounts.items[i].name, en, sizeof(en));
        json_escape(g_app.accounts.items[i].home_place, eh, sizeof(eh));
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"name\":\"%s\",\"home_place\":\"%s\"}",
                        i ? "," : "", en, eh);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "],\"versions\":[");
    for (int i = 0; i < g_app.versions.count; i++) {
        char en[NAME_LEN*2], ep[PATH_LEN*2];
        json_escape(g_app.versions.items[i].name, en, sizeof(en));
        json_escape(g_app.versions.items[i].path, ep, sizeof(ep));
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"name\":\"%s\",\"path\":\"%s\"}",
                        i ? "," : "", en, ep);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "]}");
    LeaveCriticalSection(&g_app.lock);
    webview_return(g_app.wv, seq, 0, buf);
}

struct AddAccCtx { char seq[64]; char cookie[COOKIE_LEN]; char home[64]; };
static DWORD WINAPI thread_add_account(LPVOID p) {
    auto *ctx = (AddAccCtx *)p;
    strip_warning(ctx->cookie);
    char username[NAME_LEN] = {};
    if (fetch_username(ctx->cookie, username, sizeof(username)) != 0) {
        webview_return(g_app.wv, ctx->seq, 0, "{\"ok\":false,\"error\":\"Invalid cookie\"}");
        delete ctx; return 0;
    }
    EnterCriticalSection(&g_app.lock);
    accounts_add(&g_app.accounts, username, ctx->cookie, ctx->home);
    accounts_save(&g_app.accounts, g_app.accounts_path);
    LeaveCriticalSection(&g_app.lock);
    char resp[NAME_LEN+64]; char en[NAME_LEN*2];
    json_escape(username, en, sizeof(en));
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\"}", en);
    webview_return(g_app.wv, ctx->seq, 0, resp);
    delete ctx; return 0;
}
static void cb_add_account(const char *seq, const char *req, void *) {
    auto *ctx = new AddAccCtx();
    strncpy(ctx->seq, seq, sizeof(ctx->seq)-1);
    json_str_arg(req, 0, ctx->cookie, COOKIE_LEN-1);
    json_str_arg(req, 1, ctx->home, sizeof(ctx->home)-1);
    HANDLE t = CreateThread(nullptr, 0, thread_add_account, ctx, 0, nullptr);
    CloseHandle(t);
}

static void cb_remove_account(const char *seq, const char *req, void *) {
    char name[NAME_LEN] = {};
    json_str_arg(req, 0, name, sizeof(name)-1);
    EnterCriticalSection(&g_app.lock);
    accounts_remove(&g_app.accounts, name);
    accounts_save(&g_app.accounts, g_app.accounts_path);
    LeaveCriticalSection(&g_app.lock);
    webview_return(g_app.wv, seq, 0, "{\"ok\":true}");
}

static void cb_set_home_place(const char *seq, const char *req, void *) {
    char name[NAME_LEN]={}, home[64]={};
    json_str_arg(req, 0, name, sizeof(name)-1);
    json_str_arg(req, 1, home, sizeof(home)-1);
    EnterCriticalSection(&g_app.lock);
    for (int i = 0; i < g_app.accounts.count; i++) {
        if (!strcmp(g_app.accounts.items[i].name, name)) {
            strncpy(g_app.accounts.items[i].home_place, home, sizeof(g_app.accounts.items[i].home_place)-1);
            accounts_save(&g_app.accounts, g_app.accounts_path);
            break;
        }
    }
    LeaveCriticalSection(&g_app.lock);
    webview_return(g_app.wv, seq, 0, "{\"ok\":true}");
}

static void cb_versions_scan(const char *seq, const char *req, void *) {
    EnterCriticalSection(&g_app.lock);
    versions_scan(&g_app.versions, g_app.versions_dir);
    LeaveCriticalSection(&g_app.lock);
    cb_get_state(seq, req, nullptr);
}

struct LaunchCtx { char account[NAME_LEN]; char place_id[64]; char version_path[PATH_LEN]; };
static DWORD WINAPI thread_launch(LPVOID p) {
    auto *ctx = (LaunchCtx *)p;

    EnterCriticalSection(&g_app.lock);
    char cookie[COOKIE_LEN]={};
    const char *c = accounts_get(&g_app.accounts, ctx->account);
    if (c) strncpy(cookie, c, COOKIE_LEN-1);
    LeaveCriticalSection(&g_app.lock);
    if (!cookie[0]) {
        wv_eval("onLaunchResult({\"ok\":false,\"error\":\"No account selected\"})");
        delete ctx; return 0;
    }

    wv_eval("showStatus('launch-status','Getting auth ticket\xe2\x80\xa6','')");
    Sleep(300);
    char ticket[TICKET_LEN]={};
    if (fetch_auth_ticket(cookie, ticket, sizeof(ticket)) != 0) {
        wv_eval("onLaunchResult({\"ok\":false,\"error\":\"Auth ticket failed \xe2\x80\x94 cookie expired?\"})");
        delete ctx; return 0;
    }
    wv_eval("showStatus('launch-status','Auth ticket acquired','ok')");
    Sleep(400);

    long long place_id = 0;
    {
        char clean[64]={}; int ci = 0;
        for (int i = 0; ctx->place_id[i] && ci < 63; i++) {
            char ch = ctx->place_id[i];
            if (ch >= '0' && ch <= '9') clean[ci++] = ch;
        }
        clean[ci] = '\0';
        if (ci > 0) place_id = atoll(clean);
    }

    if (place_id > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "showStatus('launch-status','Joining game %lld\xe2\x80\xa6','')", place_id);
        wv_eval(msg);
    } else {
        wv_eval("showStatus('launch-status','No game ID \xe2\x80\x94 launching home\xe2\x80\xa6','')");
    }
    Sleep(400);

    wv_eval("showStatus('launch-status','Launching Roblox\xe2\x80\xa6','')");
    Sleep(200);

    char exe[PATH_LEN];
    snprintf(exe, sizeof(exe), "%s\\RobloxPlayerBeta.exe", ctx->version_path);
    int rc = roblox_launch(exe, ticket, place_id);

    if (rc != 0) {
        wv_eval("onLaunchResult({\"ok\":false,\"error\":\"RobloxPlayerBeta.exe failed to start\"})");
        delete ctx; return 0;
    }

    if (place_id > 0) {
        char name[256] = {};
        fetch_place_name(place_id, name, sizeof(name));
        char ename[512]; json_escape(name, ename, sizeof(ename));
        char msg[768];
        snprintf(msg, sizeof(msg),
            "onLaunchResult({\"ok\":true,\"mode\":\"game\",\"name\":\"%s\",\"placeId\":%lld})",
            ename, place_id);
        wv_eval(msg);
    } else {
        wv_eval("onLaunchResult({\"ok\":true,\"mode\":\"home\"})");
    }

    delete ctx; return 0;
}

static void cb_launch(const char *seq, const char *req, void *) {
    auto *ctx = new LaunchCtx();
    json_str_arg(req, 0, ctx->account,      NAME_LEN-1);
    json_str_arg(req, 1, ctx->place_id,     sizeof(ctx->place_id)-1);
    json_str_arg(req, 2, ctx->version_path, PATH_LEN-1);
    HANDLE t = CreateThread(nullptr, 0, thread_launch, ctx, 0, nullptr);
    CloseHandle(t);
    webview_return(g_app.wv, seq, 0, "{\"ok\":true}");
}

static const char *HTML = R"HTML(
<!DOCTYPE html>
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
  color:var(--sub);transition:all .15s;border:1px solid transparent}
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
  background:var(--bg);color:var(--text);font-size:12px;margin-top:6px;display:none;outline:none}
#cookie-input:focus{border-color:var(--accent)}

.main{flex:1;padding:20px 24px;overflow-y:auto;display:flex;flex-direction:column;gap:16px}

.row{display:flex;gap:10px;align-items:center}
label{font-size:11px;color:var(--sub);text-transform:uppercase;letter-spacing:.6px;min-width:70px}
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

.hist-title{font-size:11px;color:var(--sub);text-transform:uppercase;letter-spacing:.8px;margin-top:8px}
.hist-grid{display:flex;flex-wrap:wrap;gap:10px;margin-top:8px}
.hist-card{width:120px;border-radius:8px;overflow:hidden;cursor:pointer;
  border:1px solid var(--border);background:var(--surface);transition:all .15s}
.hist-card:hover{border-color:var(--border2);transform:translateY(-2px)}
.hist-card img{width:100%;height:80px;object-fit:cover;display:block;background:var(--surface2)}
.hist-card .info{padding:6px 8px;font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.hist-card .pid{font-size:10px;color:var(--sub)}
</style>
</head>
<body>

<div class="titlebar"><span>Roblox Launcher</span> c++ edition</div>

<div class="wrap">
  <div class="side">
    <h3>Accounts</h3>
    <div id="acc-list"></div>
    <div id="add-area">
      <button id="add-btn" onclick="toggleAdd()">+ Add Account</button>
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
  if (!r.ok) { showStatus('launch-status', 'Error: ' + (r.error||'unknown'), 'err'); return; }
  if (r.mode === 'game') showStatus('launch-status', 'Launched: ' + (r.name||r.placeId), 'ok');
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
    p.classList.toggle('active', p.textContent.replace('×','').trim() === name);
  });
}

async function removeAcc(name) {
  await remove_account(name);
  loadState();
}

function toggleAdd() {
  const inp = document.getElementById('cookie-input');
  inp.style.display = inp.style.display === 'none' ? 'block' : 'none';
  if (inp.style.display === 'block') inp.focus();
}

async function submitCookie() {
  const inp = document.getElementById('cookie-input');
  const cookie = inp.value.trim();
  if (!cookie) return;
  showStatus('launch-status', 'Adding account…', '');
  const r = await add_account(cookie, '');
  if (!r.ok) { showStatus('launch-status', 'Error: ' + (r.error||'failed'), 'err'); return; }
  inp.value = '';
  inp.style.display = 'none';
  showStatus('launch-status', 'Added: ' + r.name, 'ok');
  selAcc = r.name;
  loadState();
}

function renderVersions(vers) {
  _vers = vers;
  const sel = document.getElementById('ver-select');
  sel.innerHTML = '';
  if (!vers.length) { sel.innerHTML = '<option value="">No versions found</option>'; return; }
  vers.forEach((v, i) => {
    const o = document.createElement('option');
    o.value = i; o.textContent = v.name;
    sel.appendChild(o);
  });
}

async function loadState() {
  const state = await get_state();
  renderAccounts(state.accounts);
  renderVersions(state.versions);
  if (state.accounts.length && !selAcc) pickAcc(state.accounts[0].name);
}

async function doLaunch() {
  if (!selAcc) { showStatus('launch-status','Select an account first','err'); return; }
  const idx = document.getElementById('ver-select').value;
  const ver = _vers[parseInt(idx)];
  if (!ver) { showStatus('launch-status','Select a version first','err'); return; }
  const raw = document.getElementById('place-input').value.trim();
  const place = (raw && /^\d+$/.test(raw)) ? raw : 'home';
  showStatus('launch-status','Starting…','');
  await launch(selAcc, place, ver.path);
}

loadState();
</script>
</body>
</html>
)HTML";

static void app_run() {
    srand((unsigned)time(nullptr));
    InitializeCriticalSection(&g_app.lock);

    char exe_dir[PATH_LEN];
    GetModuleFileNameA(nullptr, exe_dir, sizeof(exe_dir));
    char *sl = strrchr(exe_dir, '\\'); if (sl) *(sl+1) = '\0';
    snprintf(g_app.accounts_path, sizeof(g_app.accounts_path), "%saccounts.json", exe_dir);

    char local[PATH_LEN];
    SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    snprintf(g_app.versions_dir, sizeof(g_app.versions_dir), "%s\\Roblox\\Versions", local);

    accounts_load(&g_app.accounts, g_app.accounts_path);
    versions_scan(&g_app.versions, g_app.versions_dir);

    g_app.wv = webview_create(0, nullptr);
    webview_set_title(g_app.wv, "Roblox Launcher");
    webview_set_size(g_app.wv, 900, 600, WEBVIEW_HINT_NONE);
    webview_set_size(g_app.wv, 680, 450, WEBVIEW_HINT_MIN);

    webview_bind(g_app.wv, "get_state",       cb_get_state,       nullptr);
    webview_bind(g_app.wv, "add_account",      cb_add_account,     nullptr);
    webview_bind(g_app.wv, "remove_account",   cb_remove_account,  nullptr);
    webview_bind(g_app.wv, "set_home_place",   cb_set_home_place,  nullptr);
    webview_bind(g_app.wv, "versions_scan",    cb_versions_scan,   nullptr);
    webview_bind(g_app.wv, "launch",           cb_launch,          nullptr);

    webview_set_html(g_app.wv, HTML);
    webview_run(g_app.wv);
    webview_destroy(g_app.wv);

    if (g_session) WinHttpCloseHandle(g_session);
    DeleteCriticalSection(&g_app.lock);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    app_run();
    return 0;
}
