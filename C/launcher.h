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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>

#define MAX_ACCOUNTS    64
#define MAX_VERSIONS    32
#define COOKIE_LEN      8192
#define NAME_LEN        256
#define PATH_LEN        1024
#define TICKET_LEN      512
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

#endif
