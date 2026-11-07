# Dependencies — Roblox Launcher (Pure C)

## Build Requirements

| Dependency | Version | Purpose | Source |
|---|---|---|---|
| **MSVC (cl.exe)** | VS 2019+ | C17 compiler | Visual Studio / Build Tools |
| **Windows SDK** | 10.0+ | Win32 APIs, WinHTTP, COM | Visual Studio installer |

## Auto-Downloaded by build.bat

These are fetched on first build into `deps\` — no manual setup needed.

| Dependency | What It Is | License | URL |
|---|---|---|---|
| **webview** | C/C++ WebView2 wrapper (webview.cc compiled as C++, exposes extern "C" API) | MIT | https://github.com/webview/webview |
| **Microsoft.Web.WebView2** | WebView2 SDK + static loader lib | Microsoft | https://www.nuget.org/packages/Microsoft.Web.WebView2 |

## System Libraries (linked via pragma/linker)

| Library | Used For |
|---|---|
| `winhttp.lib` | HTTPS requests (auth tickets, user info, place details) |
| `ole32.lib` | COM initialization (WebView2) |
| `shlwapi.lib` | Path utilities |
| `shell32.lib` | SHGetFolderPath (LocalAppData) |
| `advapi32.lib` | Registry/security (WebView2 internals) |
| `user32.lib` | Window management (WebView2) |
| `version.lib` | Version info (WebView2 internals) |
| `WebView2LoaderStatic.lib` | WebView2 bootstrap (from NuGet package) |

## Runtime Requirements

| Requirement | Notes |
|---|---|
| **WebView2 Runtime** | Ships with Windows 11, available for Win10 via [Evergreen installer](https://developer.microsoft.com/en-us/microsoft-edge/webview2/) |
| **Windows 10+** | x64 only |

## File Structure

```
C/
├── launcher.c          # main source (compiled as C with /TC)
├── launcher.h          # structs and constants
├── build.bat           # one-click build script
├── DEPENDENCIES.md     # this file
└── deps/               # auto-created on first build
    ├── webview/         # webview library source
    └── webview2/        # WebView2 SDK (NuGet extract)
```
