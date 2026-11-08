#pragma once
#ifndef LAUNCHER_H
#define LAUNCHER_H

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_ACCOUNTS    64
#define MAX_VERSIONS    32
#define COOKIE_LEN      8192
#define NAME_LEN        256
#define PATH_LEN        1024
#define TICKET_LEN      512
#define URL_LEN         2048
#define JSON_BUF        131072

typedef struct {
    char name[NAME_LEN];
    char cookie[COOKIE_LEN];
    char home_place[64];
} Account;

typedef struct {
    Account items[MAX_ACCOUNTS];
    int     count;
} AccountStore;

typedef struct {
    char name[NAME_LEN];
    char path[PATH_LEN];
} VersionEntry;

typedef struct {
    VersionEntry items[MAX_VERSIONS];
    int          count;
} VersionList;

typedef struct {
    int     status;
    BYTE   *data;
    DWORD   data_len;
    char    csrf[128];
    char    ticket[TICKET_LEN];
} HttpResult;

static inline void accounts_add(AccountStore *s, const char *name, const char *cookie, const char *home) {
    for (int i = 0; i < s->count; i++) {
        if (!strcmp(s->items[i].name, name)) {
            strncpy(s->items[i].cookie, cookie, COOKIE_LEN-1);
            if (home && home[0]) strncpy(s->items[i].home_place, home, sizeof(s->items[i].home_place)-1);
            return;
        }
    }
    if (s->count >= MAX_ACCOUNTS) return;
    Account *a = &s->items[s->count++];
    memset(a, 0, sizeof(*a));
    strncpy(a->name, name, NAME_LEN-1);
    strncpy(a->cookie, cookie, COOKIE_LEN-1);
    if (home && home[0]) strncpy(a->home_place, home, sizeof(a->home_place)-1);
}

static inline void accounts_remove(AccountStore *s, const char *name) {
    for (int i = 0; i < s->count; i++) {
        if (!strcmp(s->items[i].name, name)) {
            memmove(&s->items[i], &s->items[i+1], (s->count - i - 1) * sizeof(Account));
            s->count--;
            return;
        }
    }
}

static inline const char *accounts_get(const AccountStore *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->items[i].name, name)) return s->items[i].cookie;
    return nullptr;
}

static inline const char *accounts_get_home(const AccountStore *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->items[i].name, name)) return s->items[i].home_place;
    return nullptr;
}

#ifdef __cplusplus
}
#endif
#endif
