/*
 * launcher.c — Roblox Launcher (Pure C / Win32 / WebView2)
 *
 * Compiled as C (/TC), linked with webview.cc (compiled as C++).
 * The webview API is extern "C" so it links cleanly.
 */

#include "launcher.h"

/* webview C API — WEBVIEW_STATIC makes WEBVIEW_API = extern */
#include <webview/api.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

/* ════════════════════════════════════════════════════════════════════════════
 * Global state
 * ════════════════════════════════════════════════════════════════════════════ */
static struct {
    webview_t        wv;
    AccountStore     accounts;
    VersionList      versions;
    char             accounts_path[PATH_LEN];
    char             versions_dir[PATH_LEN];
    CRITICAL_SECTION lock;
} g;

static HINTERNET g_session = NULL;

/* ════════════════════════════════════════════════════════════════════════════
 * Utility
 * ════════════════════════════════════════════════════════════════════════════ */
static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static void json_escape(const char *in, char *out, size_t len) {
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

static int json_str_arg(const char *arr, int idx, char *out, size_t len) {
    const char *p = arr;
    int i;
    while (*p && *p != '[') p++;
    if (!*p) return 0;
    p++;
    for (i = 0; ; i++) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ']' || !*p) return 0;
        if (i == idx) {
            if (*p == '"') {
                size_t n = 0;
                p++;
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
            } else {
                size_t n = 0;
                while (*p && *p != ',' && *p != ']' && n + 1 < len) out[n++] = *p++;
                out[n] = '\0';
                return (int)n;
            }
        }
        if (*p == '"') { p++; while (*p && !(*p == '"' && *(p-1) != '\\')) p++; if (*p) p++; }
        else { while (*p && *p != ',' && *p != ']') p++; }
        if (*p == ',') p++;
    }
}

static void strip_warning(char *cookie) {
    const char *pfx = "_|WARNING:-DO-NOT-SHARE-THIS.--Sharing-this-will-allow-someone-to-log-in-to-your-account--|_";
    size_t plen = strlen(pfx);
    if (!strncmp(cookie, pfx, plen))
        memmove(cookie, cookie + plen, strlen(cookie + plen) + 1);
}

static void url_encode(const char *in, char *out, size_t len) {
    size_t j = 0;
    size_t i;
    for (i = 0; in[i] && j + 3 < len; i++) {
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

/* ════════════════════════════════════════════════════════════════════════════
 * HTTP (WinHTTP)
 * ════════════════════════════════════════════════════════════════════════════ */
static void http_result_free(HttpResult *r) {
    if (r->data) { free(r->data); r->data = NULL; }
}

static int do_http(const wchar_t *verb, const wchar_t *host, const wchar_t *path,
                   const char *cookie, const char *csrf, HttpResult *out) {
    HINTERNET conn, req;
    DWORD flags, status, sz, chunk, total;
    BYTE *buf;
    BYTE tmp[8192];
    wchar_t hdrs[COOKIE_LEN + 512];
    wchar_t hdr_buf[TICKET_LEN];
    DWORD hdr_sz;
    int pos;

    memset(out, 0, sizeof(*out));
    if (!g_session) {
        g_session = WinHttpOpen(L"RobloxLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_session) return -1;
    }
    conn = WinHttpConnect(g_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) return -1;

    req = WinHttpOpenRequest(conn, verb, path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); return -1; }

    flags = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof(flags));

    pos = 0;
    pos += swprintf(hdrs + pos, (sizeof(hdrs)/sizeof(wchar_t)) - pos,
                    L"Content-Type: application/json\r\nAccept: application/json\r\n"
                    L"Referer: https://www.roblox.com/\r\nOrigin: https://www.roblox.com\r\n");
    if (cookie && cookie[0]) {
        wchar_t *wc = to_wide(cookie);
        pos += swprintf(hdrs + pos, (sizeof(hdrs)/sizeof(wchar_t)) - pos,
                        L"Cookie: .ROBLOSECURITY=%ls\r\n", wc);
        free(wc);
    }
    if (csrf && csrf[0]) {
        wchar_t *wt = to_wide(csrf);
        pos += swprintf(hdrs + pos, (sizeof(hdrs)/sizeof(wchar_t)) - pos,
                        L"X-CSRF-TOKEN: %ls\r\n", wt);
        free(wt);
    }

    if (!WinHttpSendRequest(req, hdrs, (DWORD)wcslen(hdrs), NULL, 0, 0, 0)) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); return -1;
    }
    WinHttpReceiveResponse(req, NULL);

    status = 0; sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out->status = (int)status;

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

    total = 0; buf = NULL;
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
    return do_http(L"GET", host, path, cookie, NULL, out);
}

static int http_post(const wchar_t *host, const wchar_t *path, const char *cookie,
                     const char *csrf, HttpResult *out) {
    return do_http(L"POST", host, path, cookie, csrf, out);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Account persistence
 * ════════════════════════════════════════════════════════════════════════════ */
static void accounts_add(AccountStore *s, const char *name, const char *cookie, const char *home) {
    int i;
    for (i = 0; i < s->count; i++) {
        if (!strcmp(s->items[i].name, name)) {
            strncpy(s->items[i].cookie, cookie, COOKIE_LEN-1);
            if (home && home[0]) strncpy(s->items[i].home_place, home, 63);
            return;
        }
    }
    if (s->count >= MAX_ACCOUNTS) return;
    memset(&s->items[s->count], 0, sizeof(Account));
    strncpy(s->items[s->count].name, name, NAME_LEN-1);
    strncpy(s->items[s->count].cookie, cookie, COOKIE_LEN-1);
    if (home && home[0]) strncpy(s->items[s->count].home_place, home, 63);
    s->count++;
}

static void accounts_remove(AccountStore *s, const char *name) {
    int i;
    for (i = 0; i < s->count; i++) {
        if (!strcmp(s->items[i].name, name)) {
            memmove(&s->items[i], &s->items[i+1], (s->count - i - 1) * sizeof(Account));
            s->count--;
            return;
        }
    }
}

static const char *accounts_get_cookie(const AccountStore *s, const char *name) {
    int i;
    for (i = 0; i < s->count; i++)
        if (!strcmp(s->items[i].name, name)) return s->items[i].cookie;
    return NULL;
}

static void accounts_save(const AccountStore *s, const char *path) {
    int i;
    char en[NAME_LEN*2], ec[COOKIE_LEN*2], eh[128];
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"accounts\":[");
    for (i = 0; i < s->count; i++) {
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
    long sz;
    char *buf;
    const char *p;
    FILE *f;

    s->count = 0;
    f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = 0;
    fclose(f);

    p = buf;
    while ((p = strstr(p, "{\"name\"")) != NULL) {
        const char *nk, *ck, *hk;
        int n;
        Account *a;
        if (s->count >= MAX_ACCOUNTS) break;
        a = &s->items[s->count];
        memset(a, 0, sizeof(*a));

        nk = strstr(p, "\"name\"");
        if (nk) { nk += 6; while (*nk && *nk != '"') nk++; if (*nk) nk++;
            n = 0; while (*nk && *nk != '"' && n < NAME_LEN-1) {
                if (*nk == '\\' && *(nk+1)) { nk++; a->name[n++] = *nk++; } else a->name[n++] = *nk++;
            } a->name[n] = 0;
        }
        ck = strstr(p, "\"cookie\"");
        if (ck) { ck += 8; while (*ck && *ck != '"') ck++; if (*ck) ck++;
            n = 0; while (*ck && *ck != '"' && n < COOKIE_LEN-1) {
                if (*ck == '\\' && *(ck+1)) { ck++; a->cookie[n++] = *ck++; } else a->cookie[n++] = *ck++;
            } a->cookie[n] = 0;
        }
        hk = strstr(p, "\"home_place\"");
        if (hk) { hk += 12; while (*hk && *hk != '"') hk++; if (*hk) hk++;
            n = 0; while (*hk && *hk != '"' && n < 63) {
                if (*hk == '\\' && *(hk+1)) { hk++; a->home_place[n++] = *hk++; } else a->home_place[n++] = *hk++;
            } a->home_place[n] = 0;
        }
        s->count++;
        p++;
    }
    free(buf);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Roblox API
 * ════════════════════════════════════════════════════════════════════════════ */
static void fetch_place_name(long long place_id, char *out, size_t len) {
    char pid[32];
    wchar_t *wpid;
    wchar_t wpath[256];
    HttpResult r = {0};

    snprintf(pid, sizeof(pid), "%lld", place_id);
    wpid = to_wide(pid);
    swprintf(wpath, 256, L"/v1/games/multiget-place-details?placeIds=%ls", wpid);
    free(wpid);

    http_get(L"games.roblox.com", wpath, NULL, &r);
    if (r.status == 200 && r.data) {
        const char *np = strstr((char*)r.data, "\"name\"");
        if (np) {
            size_t n = 0;
            np += 6; while (*np && *np != '"') np++;
            if (*np == '"') np++;
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

static int fetch_auth_ticket(const char *cookie, char *out, size_t len) {
    HttpResult r1 = {0}, r2 = {0};
    char csrf[128] = {0};

    http_post(L"auth.roblox.com", L"/v1/authentication-ticket", cookie, NULL, &r1);
    strncpy(csrf, r1.csrf, sizeof(csrf)-1);
    http_result_free(&r1);
    if (!csrf[0]) return -1;

    http_post(L"auth.roblox.com", L"/v1/authentication-ticket", cookie, csrf, &r2);
    if (r2.ticket[0]) {
        strncpy(out, r2.ticket, len-1); out[len-1] = '\0';
        http_result_free(&r2);
        return 0;
    }
    http_result_free(&r2);
    return -1;
}

static int fetch_username(const char *cookie, char *out, size_t len) {
    HttpResult r = {0};
    const char *p;
    size_t n;

    if (http_get(L"users.roblox.com", L"/v1/users/authenticated", cookie, &r) != 0 || r.status != 200) {
        http_result_free(&r); return -1;
    }
    p = strstr((char *)r.data, "\"name\"");
    if (!p) { http_result_free(&r); return -1; }
    p += 6;
    while (*p && *p != '"') p++;
    if (!*p) { http_result_free(&r); return -1; }
    p++;
    n = 0;
    while (*p && *p != '"' && n + 1 < len) out[n++] = *p++;
    out[n] = '\0';
    http_result_free(&r);
    return n > 0 ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Version scanning
 * ════════════════════════════════════════════════════════════════════════════ */
static void versions_scan(VersionList *vl, const char *base_dir) {
    char pattern[PATH_LEN], exe[PATH_LEN];
    wchar_t *wp;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char *name;

    vl->count = 0;
    snprintf(pattern, sizeof(pattern), "%s\\*", base_dir);
    wp = to_wide(pattern);
    h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        name = to_utf8(fd.cFileName);
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

/* ════════════════════════════════════════════════════════════════════════════
 * Launch
 * ════════════════════════════════════════════════════════════════════════════ */
static int roblox_launch(const char *exe, const char *ticket, long long place_id) {
    long long ts = (long long)time(NULL);
    long long tracker = ts * 1000 + (rand() % 999 + 1);
    char cmd[8192], work_dir[PATH_LEN], dbg[PATH_LEN];
    char *last_sep;
    wchar_t *wcmd, *wworkdir;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL ok;
    DWORD launch_err;
    FILE *df;

    if (place_id > 0) {
        char raw_url[1024], encoded_url[2048];
        snprintf(raw_url, sizeof(raw_url),
            "https://assetgame.roblox.com/game/PlaceLauncher.ashx"
            "?request=RequestGame"
            "&browserTrackerId=%lld"
            "&placeId=%lld"
            "&isPlayTogetherGame=false",
            tracker, place_id);
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

    strncpy(work_dir, exe, sizeof(work_dir)-1); work_dir[sizeof(work_dir)-1] = '\0';
    last_sep = strrchr(work_dir, '\\');
    if (last_sep) *last_sep = '\0';

    GetModuleFileNameA(NULL, dbg, sizeof(dbg));
    last_sep = strrchr(dbg, '\\');
    if (last_sep) *(last_sep+1) = '\0';
    strncat(dbg, "launch_debug.txt", sizeof(dbg)-strlen(dbg)-1);
    df = fopen(dbg, "w");
    if (df) {
        fprintf(df, "PLACE_ID:  %lld\n", place_id);
        fprintf(df, "MODE:      %s\n", place_id > 0 ? "GAME JOIN (protocol)" : "HOME (--app)");
        fprintf(df, "EXE:       %s\n", exe);
        fprintf(df, "WORK:      %s\n", work_dir);
        fprintf(df, "CMD:       %s\n", cmd);
        fclose(df);
    }

    wcmd = to_wide(cmd);
    wworkdir = to_wide(work_dir);
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
                        CREATE_NEW_PROCESS_GROUP, NULL, wworkdir, &si, &pi);
    launch_err = ok ? 0 : GetLastError();
    free(wcmd); free(wworkdir);

    df = fopen(dbg, "a");
    if (df) {
        fprintf(df, "LAUNCHED:  %s (err=%lu)\n", ok ? "YES" : "NO", launch_err);
        if (ok) {
            DWORD exit_code = 0;
            Sleep(2000);
            GetExitCodeProcess(pi.hProcess, &exit_code);
            fprintf(df, "EXIT_CODE: %s (%lu)\n",
                exit_code == STILL_ACTIVE ? "still running" : "exited", exit_code);
        }
        fclose(df);
    }

    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return 0; }
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * WebView helpers
 * ════════════════════════════════════════════════════════════════════════════ */
static void wv_eval(const char *js) { webview_eval(g.wv, js); }

/* ── get_state ── */
static void cb_get_state(const char *seq, const char *req, void *arg) {
    char buf[JSON_BUF];
    int pos = 0, i;
    (void)req; (void)arg;

    EnterCriticalSection(&g.lock);
    pos += snprintf(buf+pos, sizeof(buf)-pos, "{\"accounts\":[");
    for (i = 0; i < g.accounts.count; i++) {
        char en[NAME_LEN*2], eh[128];
        json_escape(g.accounts.items[i].name, en, sizeof(en));
        json_escape(g.accounts.items[i].home_place, eh, sizeof(eh));
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"name\":\"%s\",\"home_place\":\"%s\"}",
                        i ? "," : "", en, eh);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "],\"versions\":[");
    for (i = 0; i < g.versions.count; i++) {
        char en[NAME_LEN*2], ep[PATH_LEN*2];
        json_escape(g.versions.items[i].name, en, sizeof(en));
        json_escape(g.versions.items[i].path, ep, sizeof(ep));
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"name\":\"%s\",\"path\":\"%s\"}",
                        i ? "," : "", en, ep);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos, "]}");
    LeaveCriticalSection(&g.lock);
    webview_return(g.wv, seq, 0, buf);
}

/* ── add_account (threaded) ── */
typedef struct { char seq[64]; char cookie[COOKIE_LEN]; char home[64]; } AddAccCtx;

static DWORD WINAPI thread_add_account(LPVOID p) {
    AddAccCtx *ctx = (AddAccCtx *)p;
    char username[NAME_LEN] = {0};
    char resp[NAME_LEN+64], en[NAME_LEN*2];

    strip_warning(ctx->cookie);
    if (fetch_username(ctx->cookie, username, sizeof(username)) != 0) {
        webview_return(g.wv, ctx->seq, 0, "{\"ok\":false,\"error\":\"Invalid cookie\"}");
        free(ctx); return 0;
    }
    EnterCriticalSection(&g.lock);
    accounts_add(&g.accounts, username, ctx->cookie, ctx->home);
    accounts_save(&g.accounts, g.accounts_path);
    LeaveCriticalSection(&g.lock);
    json_escape(username, en, sizeof(en));
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\"}", en);
    webview_return(g.wv, ctx->seq, 0, resp);
    free(ctx); return 0;
}

static void cb_add_account(const char *seq, const char *req, void *arg) {
    HANDLE t;
    AddAccCtx *ctx = (AddAccCtx *)calloc(1, sizeof(AddAccCtx));
    (void)arg;
    strncpy(ctx->seq, seq, sizeof(ctx->seq)-1);
    json_str_arg(req, 0, ctx->cookie, COOKIE_LEN-1);
    json_str_arg(req, 1, ctx->home, sizeof(ctx->home)-1);
    t = CreateThread(NULL, 0, thread_add_account, ctx, 0, NULL);
    CloseHandle(t);
}

/* ── remove_account ── */
static void cb_remove_account(const char *seq, const char *req, void *arg) {
    char name[NAME_LEN] = {0};
    (void)arg;
    json_str_arg(req, 0, name, sizeof(name)-1);
    EnterCriticalSection(&g.lock);
    accounts_remove(&g.accounts, name);
    accounts_save(&g.accounts, g.accounts_path);
    LeaveCriticalSection(&g.lock);
    webview_return(g.wv, seq, 0, "{\"ok\":true}");
}

/* ── set_home_place ── */
static void cb_set_home_place(const char *seq, const char *req, void *arg) {
    char name[NAME_LEN]={0}, home[64]={0};
    int i;
    (void)arg;
    json_str_arg(req, 0, name, sizeof(name)-1);
    json_str_arg(req, 1, home, sizeof(home)-1);
    EnterCriticalSection(&g.lock);
    for (i = 0; i < g.accounts.count; i++) {
        if (!strcmp(g.accounts.items[i].name, name)) {
            strncpy(g.accounts.items[i].home_place, home, 63);
            accounts_save(&g.accounts, g.accounts_path);
            break;
        }
    }
    LeaveCriticalSection(&g.lock);
    webview_return(g.wv, seq, 0, "{\"ok\":true}");
}

/* ── versions_scan ── */
static void cb_versions_scan(const char *seq, const char *req, void *arg) {
    EnterCriticalSection(&g.lock);
    versions_scan(&g.versions, g.versions_dir);
    LeaveCriticalSection(&g.lock);
    cb_get_state(seq, req, arg);
}

/* ── launch (threaded) ── */
typedef struct { char account[NAME_LEN]; char place_id[64]; char version_path[PATH_LEN]; } LaunchCtx;

static DWORD WINAPI thread_launch(LPVOID p) {
    LaunchCtx *ctx = (LaunchCtx *)p;
    char cookie[COOKIE_LEN] = {0};
    char ticket[TICKET_LEN] = {0};
    char exe[PATH_LEN];
    long long place_id = 0;
    int rc, ci;
    const char *c;

    EnterCriticalSection(&g.lock);
    c = accounts_get_cookie(&g.accounts, ctx->account);
    if (c) strncpy(cookie, c, COOKIE_LEN-1);
    LeaveCriticalSection(&g.lock);

    if (!cookie[0]) {
        wv_eval("onLaunchResult({\"ok\":false,\"error\":\"No account selected\"})");
        free(ctx); return 0;
    }

    wv_eval("showStatus('launch-status','Getting auth ticket\xe2\x80\xa6','')");
    Sleep(300);
    if (fetch_auth_ticket(cookie, ticket, sizeof(ticket)) != 0) {
        wv_eval("onLaunchResult({\"ok\":false,\"error\":\"Auth ticket failed\"})");
        free(ctx); return 0;
    }
    wv_eval("showStatus('launch-status','Auth ticket acquired','ok')");
    Sleep(400);

    {
        char clean[64] = {0};
        ci = 0;
        for (int i = 0; ctx->place_id[i] && ci < 63; i++) {
            char ch = ctx->place_id[i];
            if (ch >= '0' && ch <= '9') clean[ci++] = ch;
        }
        clean[ci] = '\0';
        if (ci > 0) place_id = _atoi64(clean);
    }

    if (place_id > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "showStatus('launch-status','Joining game %lld\xe2\x80\xa6','')", place_id);
        wv_eval(msg);
    } else {
        wv_eval("showStatus('launch-status','No game ID \xe2\x80\x94 launching home\xe2\x80\xa6','')");
    }
    Sleep(400);

    wv_eval("showStatus('launch-status','Launching Roblox\xe2\x80\xa6','')");
    Sleep(200);

    snprintf(exe, sizeof(exe), "%s\\RobloxPlayerBeta.exe", ctx->version_path);
    rc = roblox_launch(exe, ticket, place_id);

    if (rc != 0) {
        wv_eval("onLaunchResult({\"ok\":false,\"error\":\"RobloxPlayerBeta.exe failed to start\"})");
        free(ctx); return 0;
    }

    if (place_id > 0) {
        char name[256] = {0}, ename[512], msg[768];
        fetch_place_name(place_id, name, sizeof(name));
        json_escape(name, ename, sizeof(ename));
        snprintf(msg, sizeof(msg),
            "onLaunchResult({\"ok\":true,\"mode\":\"game\",\"name\":\"%s\",\"placeId\":%lld})",
            ename, place_id);
        wv_eval(msg);
    } else {
        wv_eval("onLaunchResult({\"ok\":true,\"mode\":\"home\"})");
    }

    free(ctx); return 0;
}

static void cb_launch(const char *seq, const char *req, void *arg) {
    HANDLE t;
    LaunchCtx *ctx = (LaunchCtx *)calloc(1, sizeof(LaunchCtx));
    (void)arg;
    json_str_arg(req, 0, ctx->account,      NAME_LEN-1);
    json_str_arg(req, 1, ctx->place_id,     sizeof(ctx->place_id)-1);
    json_str_arg(req, 2, ctx->version_path, PATH_LEN-1);
    t = CreateThread(NULL, 0, thread_launch, ctx, 0, NULL);
    CloseHandle(t);
    webview_return(g.wv, seq, 0, "{\"ok\":true}");
}

/* ════════════════════════════════════════════════════════════════════════════
 * HTML UI
 * ════════════════════════════════════════════════════════════════════════════ */
static const char *HTML =
"<!DOCTYPE html>"
"<html><head><meta charset=\"UTF-8\"><title>Roblox Launcher</title>"
"<style>"
"*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
":root{--bg:#080808;--surface:#111;--surface2:#161616;--border:#222;--border2:#2a2a2a;"
"--text:#f0f0f0;--sub:#888;--accent:#fff;--danger:#e05555;--ok:#4caf50}"
"html{-webkit-user-select:none;user-select:none}"
"body{font-family:'Segoe UI',system-ui,sans-serif;font-size:13px;color:var(--text);"
"background:var(--bg);height:100vh;overflow:hidden;display:flex;flex-direction:column}"
".titlebar{height:32px;background:var(--surface);display:flex;align-items:center;"
"padding:0 12px;font-size:11px;color:var(--sub);border-bottom:1px solid var(--border);"
"-webkit-app-region:drag}.titlebar span{font-weight:600;color:var(--text);margin-right:6px}"
".wrap{display:flex;flex:1;overflow:hidden}"
".side{width:180px;min-width:180px;background:var(--surface);border-right:1px solid var(--border);"
"display:flex;flex-direction:column;padding:8px}"
".side h3{font-size:11px;color:var(--sub);text-transform:uppercase;letter-spacing:.8px;"
"margin-bottom:6px;padding:0 4px}"
".pill{padding:7px 10px;border-radius:6px;cursor:pointer;font-size:12px;margin-bottom:3px;"
"color:var(--sub);transition:all .15s;border:1px solid transparent}"
".pill:hover{background:var(--surface2);color:var(--text)}"
".pill.active{background:var(--surface2);color:var(--accent);font-weight:600;"
"border:1px solid var(--border2)}"
".pill .x{float:right;opacity:0;font-size:10px;color:var(--danger);cursor:pointer;"
"padding:0 4px;transition:opacity .15s}.pill:hover .x{opacity:1}"
"#add-area{margin-top:auto;padding-top:8px;border-top:1px solid var(--border)}"
"#add-btn{width:100%;padding:7px;border:1px dashed var(--border2);border-radius:6px;"
"background:none;color:var(--sub);cursor:pointer;font-size:12px;transition:all .15s}"
"#add-btn:hover{border-color:var(--accent);color:var(--text)}"
"#cookie-input{width:100%;padding:7px;border:1px solid var(--border2);border-radius:6px;"
"background:var(--bg);color:var(--text);font-size:12px;margin-top:6px;display:none;outline:none}"
"#cookie-input:focus{border-color:var(--accent)}"
".main{flex:1;padding:20px 24px;overflow-y:auto;display:flex;flex-direction:column;gap:16px}"
".row{display:flex;gap:10px;align-items:center}"
"label{font-size:11px;color:var(--sub);text-transform:uppercase;letter-spacing:.6px;min-width:70px}"
"select,input[type=text]{flex:1;padding:8px 10px;border:1px solid var(--border);"
"border-radius:6px;background:var(--surface);color:var(--text);font-size:13px;outline:none}"
"select:focus,input[type=text]:focus{border-color:var(--accent)}"
"#launch-btn{padding:10px 28px;border:none;border-radius:8px;background:var(--accent);"
"color:var(--bg);font-weight:700;font-size:14px;cursor:pointer;transition:all .15s;"
"align-self:flex-start}"
"#launch-btn:hover{opacity:.85}#launch-btn:active{transform:scale(.97)}"
".status{font-size:12px;min-height:18px;padding:4px 0}"
".status.ok{color:var(--ok)}.status.err{color:var(--danger)}"
"</style></head><body>"
"<div class=\"titlebar\"><span>Roblox Launcher</span> pure c edition</div>"
"<div class=\"wrap\">"
"<div class=\"side\"><h3>Accounts</h3><div id=\"acc-list\"></div>"
"<div id=\"add-area\"><button id=\"add-btn\" onclick=\"toggleAdd()\">+ Add Account</button>"
"<input id=\"cookie-input\" type=\"text\" placeholder=\"Paste .ROBLOSECURITY cookie\""
" onkeydown=\"if(event.key==='Enter')submitCookie()\"></div></div>"
"<div class=\"main\">"
"<div class=\"row\"><label>Version</label>"
"<select id=\"ver-select\"><option value=\"\">Loading...</option></select></div>"
"<div class=\"row\"><label>Place ID</label>"
"<input type=\"text\" id=\"place-input\" placeholder=\"e.g. 6654918151 (blank = home)\"></div>"
"<button id=\"launch-btn\" onclick=\"doLaunch()\">Launch</button>"
"<div id=\"launch-status\" class=\"status\"></div>"
"</div></div>"
"<script>"
"let selAcc=null,_vers=[];"
"function showStatus(id,msg,cls){const el=document.getElementById(id);if(!el)return;"
"el.textContent=msg;el.className='status'+(cls?' '+cls:'')}"
"window.onLaunchResult=function(r){"
"if(!r.ok){showStatus('launch-status','Error: '+(r.error||'unknown'),'err');return;}"
"if(r.mode==='game')showStatus('launch-status','Launched: '+(r.name||r.placeId),'ok');"
"else showStatus('launch-status','Launched to home','ok');};"
"function renderAccounts(accs){const el=document.getElementById('acc-list');el.innerHTML='';"
"accs.forEach(a=>{const d=document.createElement('div');"
"d.className='pill'+(selAcc===a.name?' active':'');"
"d.innerHTML=a.name+'<span class=\"x\" onclick=\"event.stopPropagation();removeAcc(\\''+"
"a.name.replace(/'/g,\"\\\\'\")+'\\')\">\\u00d7</span>';"
"d.onclick=()=>pickAcc(a.name);el.appendChild(d)})}"
"function pickAcc(n){selAcc=n;document.querySelectorAll('.pill').forEach(p=>{"
"p.classList.toggle('active',p.textContent.replace('\\u00d7','').trim()===n)})}"
"async function removeAcc(n){await remove_account(n);loadState()}"
"function toggleAdd(){const i=document.getElementById('cookie-input');"
"i.style.display=i.style.display==='none'?'block':'none';if(i.style.display==='block')i.focus()}"
"async function submitCookie(){const i=document.getElementById('cookie-input');"
"const c=i.value.trim();if(!c)return;showStatus('launch-status','Adding account\\u2026','');"
"const r=await add_account(c,'');if(!r.ok){showStatus('launch-status','Error: '+(r.error||''),'err');return;}"
"i.value='';i.style.display='none';showStatus('launch-status','Added: '+r.name,'ok');"
"selAcc=r.name;loadState()}"
"function renderVersions(vers){_vers=vers;const s=document.getElementById('ver-select');s.innerHTML='';"
"if(!vers.length){s.innerHTML='<option value=\"\">No versions found</option>';return;}"
"vers.forEach((v,i)=>{const o=document.createElement('option');o.value=i;o.textContent=v.name;s.appendChild(o)})}"
"async function loadState(){const st=await get_state();renderAccounts(st.accounts);"
"renderVersions(st.versions);if(st.accounts.length&&!selAcc)pickAcc(st.accounts[0].name)}"
"async function doLaunch(){if(!selAcc){showStatus('launch-status','Select an account first','err');return;}"
"const idx=document.getElementById('ver-select').value;const ver=_vers[parseInt(idx)];"
"if(!ver){showStatus('launch-status','Select a version first','err');return;}"
"const raw=document.getElementById('place-input').value.trim();"
"const place=(raw&&/^\\d+$/.test(raw))?raw:'home';"
"showStatus('launch-status','Starting\\u2026','');await launch(selAcc,place,ver.path)}"
"loadState();"
"</script></body></html>";

/* ════════════════════════════════════════════════════════════════════════════
 * Main
 * ════════════════════════════════════════════════════════════════════════════ */
static void app_run(void) {
    char exe_dir[PATH_LEN], local[PATH_LEN];
    char *sl;

    srand((unsigned)time(NULL));
    InitializeCriticalSection(&g.lock);

    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    sl = strrchr(exe_dir, '\\'); if (sl) *(sl+1) = '\0';
    snprintf(g.accounts_path, sizeof(g.accounts_path), "%saccounts.json", exe_dir);

    SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);
    snprintf(g.versions_dir, sizeof(g.versions_dir), "%s\\Roblox\\Versions", local);

    accounts_load(&g.accounts, g.accounts_path);
    versions_scan(&g.versions, g.versions_dir);

    g.wv = webview_create(0, NULL);
    webview_set_title(g.wv, "Roblox Launcher");
    webview_set_size(g.wv, 900, 600, WEBVIEW_HINT_NONE);
    webview_set_size(g.wv, 680, 450, WEBVIEW_HINT_MIN);

    webview_bind(g.wv, "get_state",       cb_get_state,       NULL);
    webview_bind(g.wv, "add_account",      cb_add_account,     NULL);
    webview_bind(g.wv, "remove_account",   cb_remove_account,  NULL);
    webview_bind(g.wv, "set_home_place",   cb_set_home_place,  NULL);
    webview_bind(g.wv, "versions_scan",    cb_versions_scan,   NULL);
    webview_bind(g.wv, "launch",           cb_launch,          NULL);

    webview_set_html(g.wv, HTML);
    webview_run(g.wv);
    webview_destroy(g.wv);

    if (g_session) WinHttpCloseHandle(g_session);
    DeleteCriticalSection(&g.lock);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    app_run();
    return 0;
}
