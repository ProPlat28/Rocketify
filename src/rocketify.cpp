// rocketify.dll — entry point: logging, Easy Anti-Cheat guard, mod loading,
// and the engine hooks that add the native MODS entry to the main menu.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "rocketify.h"
#include "engine.h"
#include "internal.h"
#include "splash.h"

#define ROCKETIFY_STR2(x) #x
#define ROCKETIFY_STR(x) ROCKETIFY_STR2(x)

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
static RlObject ApiFindInstance(const char* cls) { return cls ? engine::FindInstance(cls) : 0; }
static RlObject ApiFindFunction(const char* name) { return name ? engine::FindFunction(name) : 0; }
static int ApiParamOffset(RlObject fn, const char* name) {
    return name && engine::IsObject(fn) ? engine::ParamOffset(fn, name) : -1;
}
static int ApiIsInstance(RlObject o, const char* cls) {
    return cls && engine::IsObject(o) && engine::ClassNameOf(o) == cls;
}
static int ApiObjectName(RlObject o, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    return snprintf(out, cap, "%s", engine::IsObject(o) ? engine::FullNameOf(o).c_str() : "");
}
static void ApiCall(RlObject o, RlObject fn, void* params) {
    if (!engine::IsDisabled() && engine::IsObject(o) && engine::IsObject(fn)) engine::CallFunction(o, fn, params);
}
static std::wstring Wide(const char* s) {
    const int n = s ? MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0) : 0;
    if (n <= 1) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    out.pop_back();
    return out;
}
static int ApiShowPopup(const char* title, const char* message, const char* const* buttons, int count, const char* type) {
    if (engine::IsDisabled() || !engine::CanShowNativeMessage()) return 0;
    std::vector<std::wstring> labels;
    for (int i = 0; buttons && i < count && i < 4; ++i) labels.push_back(Wide(buttons[i]));
    if (labels.empty()) labels.push_back(L"OK");
    const std::string t = type ? type : "";
    if (t != "classic" && t != "warning") LogF("[mod] unknown popup type '%s' - using classic", t.c_str());
    if (t == "warning" && !engine::HasWarningPopup()) LogF("[mod] the game has no warning popup - using classic");
    engine::ShowNativeMessage(Wide(title).c_str(), Wide(message).c_str(), labels, t == "warning");
    return 1;
}
static RlObject ApiFindObject(const char* name) { return name ? engine::FindObject(name, false) : 0; }
static int ApiProperty(const char* name, int* offset, int* size) {
    int o = 0, s = 0;
    if (!name || !engine::PropertyInfo(name, o, s)) return 0;
    if (offset) *offset = o;
    if (size) *size = s;
    return 1;
}
static int32_t ApiFindName(const char* text) { return text ? engine::FindName(text) : -1; }
static int ApiNameText(int32_t index, int32_t number, char* out, int cap) {
    return out && cap > 0 ? snprintf(out, cap, "%s", engine::NameText(index, number).c_str()) : 0;
}
static const RocketifyApi g_api = {ROCKETIFY_API_VERSION, ApiLog, ApiFindInstance, ApiFindFunction,
                                   ApiParamOffset, ApiIsInstance, ApiObjectName, ApiCall, ApiShowPopup,
                                   ApiFindObject, ApiProperty, ApiFindName, ApiNameText};

static std::vector<Rocketify_OnTickFn> g_ticks;  // filled before the engine thread starts
static std::vector<Rocketify_OnEventFn> g_events;
static volatile LONG g_tickPending = 0;

static void TickMods(void*) {
    if (!engine::IsDisabled())
        for (auto fn : g_ticks) fn(&g_api);
    InterlockedExchange(&g_tickPending, 0);
}

static void MenuEvent(const char* event) {
    for (auto fn : g_events) fn(&g_api, "MainMenu", event);
}

static int LoadModsMatching(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        const std::wstring path = dir + L"\\" + fd.cFileName;
        char nameA[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameA, MAX_PATH, nullptr, nullptr);
        SplashStage((L"Loading mod: " + std::wstring(fd.cFileName)).c_str());
        HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) {
            LogF("Failed to load %s (error %lu)", nameA, GetLastError());
            continue;
        }
        auto nameFn = reinterpret_cast<Rocketify_ModNameFn>(GetProcAddress(mod, "Rocketify_ModName"));
        auto loadFn = reinterpret_cast<Rocketify_OnLoadFn>(GetProcAddress(mod, "Rocketify_OnLoad"));
        LogF("Loaded mod %s (%s)", nameA, nameFn ? nameFn() : "unnamed");
        auto tickFn = reinterpret_cast<Rocketify_OnTickFn>(GetProcAddress(mod, "Rocketify_OnTick"));
        auto eventFn = reinterpret_cast<Rocketify_OnEventFn>(GetProcAddress(mod, "Rocketify_OnEvent"));
        if (eventFn) g_events.push_back(eventFn);
        if (loadFn) loadFn(&g_api);
        if (auto overridesFn = reinterpret_cast<Rocketify_OverridesFn>(GetProcAddress(mod, "Rocketify_Overrides"))) {
            const char* list = overridesFn();
            if (list && *list) {
                LogF("  replaces %s", list);
                engine::AddOverrides(list);
            }
        }
        if (tickFn) g_ticks.push_back(tickFn);
        ++count;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

static int LoadMods() {
    const std::wstring dir = g_root + L"\\mods";
    CreateDirectoryW(dir.c_str(), nullptr);
    return LoadModsMatching(dir, L"*.rlmod") + LoadModsMatching(dir, L"*.dll");
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
static void FindEngine();

static DWORD WINAPI EngineThread(LPVOID) {
    SplashWaitIdle();
    Sleep(2000);
    if (EacPresent()) {  // the DLL now loads early: check again before touching the engine
        LogF("Easy Anti-Cheat is loaded - Rocketify stays off.");
        engine::Disable();
    }
    if (engine::IsDisabled()) SplashDone();
    else FindEngine();
    return 0;
}

static void FindEngine() {
    struct Done { ~Done() { SplashDone(); } } done;  // closes the loading window on every path
    for (int i = 0; !engine::Init(); ++i) {
        if (i > 120) return;
        Sleep(1000);
    }
    if (!engine::FindReflectionLayout()) {
        LogF("[engine] reflection layout not found - MODS entry disabled.");
        return;
    }
    // The menu's player controller is spawned at the end of loading: that is when the
    // loading window ends (hooking the menu below takes a few seconds more).
    for (int i = 0; i < 600 && !engine::FindInstance("PlayerController_Menu_TA"); ++i) Sleep(200);
    SplashDone();
    bool pe = false;
    for (int i = 0; i < 60 && !(pe = engine::ProbeProcessEvent()); ++i) Sleep(2000);
    if (!pe || !engine::PrepareNativeMessage()) {
        LogF("[engine] engine hooks unavailable - MODS entry disabled.");
        return;
    }
    engine::StartMenuInjector(g_root + L"\\rocketify.ini");
    engine::SetMenuCallback(MenuEvent);
    engine::StartEventTap();
    StartInput();
    LogF("[engine] ready");
    while (!g_ticks.empty() && !engine::IsDisabled()) {
        if (InterlockedExchange(&g_tickPending, 1) == 0) engine::RunOnGameThread(TickMods, nullptr);
        Sleep(1000);
    }
    return;
}

static DWORD WINAPI Main(LPVOID self) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(static_cast<HMODULE>(self), p, MAX_PATH);
    g_root = p;
    g_root = g_root.substr(0, g_root.find_last_of(L"\\/"));
    g_log = _wfopen((g_root + L"\\rocketify.log").c_str(), L"w");
    LogF("Rocketify starting (API version " ROCKETIFY_STR(ROCKETIFY_API_VERSION) ")");  // also read by the CLI

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

    SplashOpen();
    SplashStage(L"Loading Rocketify");
    SplashStartFiles();
    LogF("%d mod(s) loaded", LoadMods());
    SplashStage(L"Loading Rocket League resources");
    SplashModsLoaded();
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
