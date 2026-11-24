// rocketify.dll — entry point: logging, Easy Anti-Cheat guard, mod loading,
// and the engine hooks that add the native MODS entry to the main menu.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>

#include "rocketify.h"
#include "engine.h"
#include "internal.h"

static std::wstring g_root;
static FILE* g_log = nullptr;
static SRWLOCK g_logLock = SRWLOCK_INIT;

const std::wstring& RootDir() { return g_root; }

void LogF(const char* fmt, ...) {
    AcquireSRWLockExclusive(&g_logLock);
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// True if any Easy Anti-Cheat module is loaded in this process.
// Rocketify never runs alongside EAC.
static bool EacPresent() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (Lower(me.szModule).find(L"easyanticheat") != std::wstring::npos) {
            found = true;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

static void ApiLog(const char* msg) { LogF("[mod] %s", msg ? msg : "(null)"); }
static const RocketifyApi g_api = {ROCKETIFY_API_VERSION, ApiLog};

static int LoadMods() {
    const std::wstring dir = g_root + L"\\mods";
    CreateDirectoryW(dir.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        const std::wstring path = dir + L"\\" + fd.cFileName;
        char nameA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameA, MAX_PATH, nullptr, nullptr);
        HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) {
            LogF("Failed to load %s (error %lu)", nameA, GetLastError());
            continue;
        }
        auto nameFn = reinterpret_cast<Rocketify_ModNameFn>(GetProcAddress(mod, "Rocketify_ModName"));
        auto loadFn = reinterpret_cast<Rocketify_OnLoadFn>(GetProcAddress(mod, "Rocketify_OnLoad"));
        LogF("Loaded mod %s (%s)", nameA, nameFn ? nameFn() : "unnamed");
        if (loadFn) loadFn(&g_api);
        ++count;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

static DWORD WINAPI EacWatchdog(LPVOID) {
    for (;;) {
        Sleep(2000);
        if (EacPresent()) {
            LogF("Easy Anti-Cheat detected in process - Rocketify switches itself off.");
            engine::Disable();
            StopInput();
            return 0;
        }
    }
}

// Finds the engine's tables, hooks ProcessEvent once the main-menu controller
// exists, then adds the native MODS entry.
static DWORD WINAPI EngineThread(LPVOID) {
    Sleep(2000);
    for (int i = 0; !engine::Init(); ++i) {
        if (i > 120) return 0;
        Sleep(1000);
    }
    if (!engine::FindReflectionLayout()) {
        LogF("[engine] reflection layout not found - MODS entry disabled.");
        return 0;
    }
    bool pe = false;
    for (int i = 0; i < 60 && !(pe = engine::ProbeProcessEvent()); ++i) Sleep(2000);
    if (!pe || !engine::PrepareNativeMessage()) {
        LogF("[engine] engine hooks unavailable - MODS entry disabled.");
        return 0;
    }
    engine::StartMenuInjector(g_root + L"\\rocketify.ini");
    engine::StartEventTap();
    StartInput();
    LogF("[engine] ready");
    return 0;
}

static DWORD WINAPI Main(LPVOID self) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(static_cast<HMODULE>(self), p, MAX_PATH);
    g_root = p;
    g_root = g_root.substr(0, g_root.find_last_of(L"\\/"));
    g_log = _wfopen((g_root + L"\\rocketify.log").c_str(), L"w");
    LogF("Rocketify starting");

    GetModuleFileNameW(nullptr, p, MAX_PATH);
    const std::wstring exe = Lower(p);
    if (exe.size() < 16 || exe.compare(exe.size() - 16, 16, L"rocketleague.exe") != 0) {
        LogF("Not running inside RocketLeague.exe - doing nothing.");
        return 0;
    }
    if (EacPresent()) {
        LogF("Easy Anti-Cheat is loaded - Rocketify only runs in non-EAC sessions. Doing nothing.");
        return 0;
    }

    LogF("%d mod(s) loaded", LoadMods());
    if (HANDLE t = CreateThread(nullptr, 0, EacWatchdog, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, EngineThread, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, Main, inst, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
