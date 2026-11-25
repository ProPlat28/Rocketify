// Rocketify.exe — starts the plain RocketLeague.exe (the same exe the
// EAC bootstrapper would start) and loads rocketify\rocketify.dll into it.
// Arguments passed to this launcher (Epic auth args from Heroic/legendary)
// are forwarded unchanged.
#include <windows.h>
#include <objidl.h>
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>
#include <cstdio>

#include "splash.h"

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

static int Fail(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"Rocketify", MB_ICONERROR | MB_OK);
    return 1;
}

static std::wstring ForwardedArgs() {
    const wchar_t* cl = GetCommandLineW();
    if (*cl == L'"') {
        ++cl;
        while (*cl && *cl != L'"') ++cl;
        if (*cl) ++cl;
    } else {
        while (*cl && *cl != L' ' && *cl != L'\t') ++cl;
    }
    while (*cl == L' ' || *cl == L'\t') ++cl;

    // Steam's "%command%" (Launch Options: "...\Rocketify.exe" %command%) passes the
    // original game exe as the first argument: skip it, keep the real game arguments.
    const wchar_t* p = cl;
    std::wstring first;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') first += *p++;
        if (*p) ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') first += *p++;
    }
    std::wstring lower = first;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0 &&
        GetFileAttributesW(first.c_str()) != INVALID_FILE_ATTRIBUTES) {
        while (*p == L' ' || *p == L'\t') ++p;
        return p;
    }
    return cl;
}

static bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---------------------------------------------------------------- loading window
static splash::Shared* g_sh = nullptr;
static volatile LONG g_closing = 0;
static LONG g_stage = 0;             // stage currently shown
static ULONGLONG g_stageSince = 0;
static ULONGLONG g_start = 0;

static const wchar_t* Kind(const wchar_t* f) {
    static const struct { const wchar_t* prefix; const wchar_t* kind; } kinds[] = {
        {L"body_", L"Car"}, {L"skin_", L"Decal"}, {L"wheel_", L"Wheels"}, {L"boost_", L"Boost"}, {L"hat_", L"Topper"},
        {L"antenna_", L"Antenna"}, {L"flag_", L"Flag"}, {L"ss_", L"Trail"}, {L"explosion_", L"Goal explosion"},
        {L"engineaudio_", L"Engine sound"}, {L"playerbanner_", L"Banner"}, {L"paintfinish_", L"Paint finish"},
        {L"gfx_", L"Interface"}, {L"gfxsounds_", L"Interface sound"}, {L"menu_", L"Menu"}, {L"gameinfo_", L"Game mode"}};
    for (auto& k : kinds)
        if (_wcsnicmp(f, k.prefix, wcslen(k.prefix)) == 0) return k.kind;
    const wchar_t* dot = wcsrchr(f, L'.');
    if (!dot) return L"File";
    if (_wcsicmp(dot, L".tfc") == 0) return L"Textures";
    if (_wcsicmp(dot, L".upk") == 0) return L"Package";
    if (_wcsicmp(dot, L".bin") == 0) return L"Data";
    return L"Sound";
}

static void PutText(HDC dc, HFONT font, COLORREF color, int x, int y, int w, const wchar_t* s, UINT align = DT_LEFT) {
    RECT r{x, y, x + w, y + 60};
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s, -1, &r, align | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void Fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

constexpr int kW = 800, kH = 450;

static Gdiplus::Image* g_art = nullptr;

// Every file shown was really opened by the game, and the bar only moves when the game
// reads asset data.
static std::unordered_set<std::wstring> g_seen;  // files shown this time
static LONG g_next = 0;                          // next published file to show
static std::wstring g_file;
static FILE* g_trace = nullptr;
static volatile LONG g_modsShown = 0;            // the mod steps have been displayed
static float g_top = 0.0f;
constexpr float kModsPart = 0.10f;

static std::wstring LowerName(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// How a whole start looks: how far through the loading time the game was when it had
// read a given amount. Recorded on every start and used for the next one. Percent of
// bytes alone would be wrong: nearly everything is read in the first half.
struct Sample { double ms, kb; };
static std::vector<Sample> g_curve, g_record;
static ULONGLONG g_resSince = 0;
static ULONGLONG g_lastRead = 0;
static LONG64 g_lastBytes = 0;
static std::wstring g_curvePath;

static void LoadCurve() {
    if (FILE* f = _wfopen(g_curvePath.c_str(), L"r")) {
        Sample p;
        while (fscanf(f, "%lf %lf", &p.ms, &p.kb) == 2)
            if (g_curve.empty() || (p.ms >= g_curve.back().ms && p.kb >= g_curve.back().kb)) g_curve.push_back(p);
        fclose(f);
    }
    if (g_curve.size() < 2 || g_curve.back().ms <= 0)  // first start: a measured typical one
        g_curve = {{0, 0}, {700, 1024}, {1600, 58368}, {11400, 411648}, {12000, 466944}, {14600, 468992},
                   {22400, 473088}, {22900, 484352}, {23300, 487424}};
}

static void SaveCurve() {
    if (g_record.size() < 2) return;
    if (FILE* f = _wfopen(g_curvePath.c_str(), L"w")) {
        for (auto& p : g_record) fprintf(f, "%.0f %.1f\n", p.ms, p.kb);
        fclose(f);
    }
}

static void Sampling(ULONGLONG now) {
    if (!g_resSince) { g_resSince = g_lastRead = now; g_record.push_back({0, 0}); }
    const LONG64 b = g_sh->bytes;
    if (b == g_lastBytes && !g_sh->done) return;
    if (b != g_lastBytes) g_lastRead = now;
    g_lastBytes = b;
    if (now - g_resSince >= g_record.back().ms + 50 || g_sh->done)
        g_record.push_back({static_cast<double>(now - g_resSince), b / 1024.0});
}

// Share of the loading time at which the recorded start had read this much.
static float CurveAt(double kb) {
    const double total = g_curve.back().ms;
    for (size_t i = 1; i < g_curve.size(); ++i) {
        const Sample &a = g_curve[i - 1], &c = g_curve[i];
        if (kb > c.kb) continue;
        // before a pause use its start: waiting does not move the bar
        const double f = c.kb > a.kb ? (kb - a.kb) / (c.kb - a.kb) : 0.0;
        return static_cast<float>((a.ms + (c.ms - a.ms) * max(0.0, f)) / total);
    }
    return 1.0f;
}

static bool ResourcesStage() {
    return g_sh->stageCount > 0 && wcsncmp(g_sh->stage[g_stage], L"Loading Rocket League resources", 31) == 0;
}

static bool AllShown() {
    return g_stage + 1 >= g_sh->stageCount && g_next >= min((LONG)g_sh->lineCount, splash::kLines);
}

// One step per frame: no file is skipped, however quickly the game opens them.
static void Advance() {
    const ULONGLONG now = GetTickCount64();
    if (!ResourcesStage()) {
        if (g_stage + 1 < g_sh->stageCount && now - g_stageSince >= 250) { ++g_stage; g_stageSince = now; }
        return;
    }
    InterlockedExchange(&g_modsShown, 1);
    if (!g_sh->done) Sampling(now);
    while (g_next < min((LONG)g_sh->lineCount, splash::kLines) && g_sh->lineReady[g_next]) {
        const LONG n = g_next++;
        const std::wstring key = LowerName(g_sh->line[n]);
        if (!g_seen.insert(key).second) continue;  // opened again: already shown and counted
        g_file = g_sh->line[n];
        if (g_trace)
            fwprintf(g_trace, L"opened %6llu ms  shown %6llu ms  %3d%%  %5lld MB  %ls\n", g_sh->lineTick[n] - g_start, now - g_start, static_cast<int>(g_top * 100), static_cast<long long>(g_sh->bytes >> 20), g_file.c_str());
        break;
    }
}

static float Progress() {
    float p;
    if (g_sh->done && AllShown()) p = 1.0f;
    else if (!ResourcesStage()) p = g_sh->stageCount > 1 ? kModsPart * g_stage / (g_sh->stageCount - 1) : 0.0f;
    else p = min(0.99f, kModsPart + (1.0f - kModsPart) * CurveAt(g_sh->bytes / 1024.0));
    return g_top = max(g_top, p);
}

// The one line of status.
static std::wstring StatusLine() {
    if (!g_sh->stageCount) return L"Starting Rocket League";
    const std::wstring st = g_sh->stage[g_stage];
    if (st.rfind(L"Loading mod: ", 0) == 0) {
        std::wstring mod = st.substr(13);
        mod = mod.substr(0, mod.rfind(L'.'));
        return L"Loading Rocket League mods: " + mod;
    }
    if (!ResourcesStage() || g_file.empty()) return st;
    // Nothing read for a while. Per the game's startup log, the early gap is script
    // loading and the long later one is the engine and online services starting.
    if (!g_sh->done && AllShown() && GetTickCount64() - g_lastRead > 1500)
        return CurveAt(g_sh->bytes / 1024.0) < 0.5f ? L"Loading Rocket League game scripts"
                                                    : L"Starting Rocket League and online services";
    return std::wstring(L"Loading Rocket League resources: ") + Kind(g_file.c_str()) + L" - " + g_file;
}

static void Paint(HDC out) {
    using namespace Gdiplus;
    static HFONT font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HDC dc = CreateCompatibleDC(out);
    HBITMAP bmp = CreateCompatibleBitmap(out, kW, kH);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, 0, 0, kW, kH, RGB(9, 18, 34));
    {
        Graphics g(dc);
        if (g_art) g.DrawImage(g_art, 0, 0, kW, kH);
        LinearGradientBrush shade(Point(0, kH - 121), Point(0, kH), Color(0, 0, 0, 0), Color(230, 0, 6, 18));
        g.FillRectangle(&shade, 0, kH - 120, kW, 120);
        SolidBrush track(Color(150, 10, 24, 48));
        g.FillRectangle(&track, 24, kH - 26, kW - 48, 8);
        const int w = static_cast<int>((kW - 48) * max(0.0f, Progress()));
        if (w > 0) {
            LinearGradientBrush bar(Point(24, 0), Point(kW - 24, 0), Color(255, 0, 120, 255), Color(255, 90, 215, 255));
            g.FillRectangle(&bar, 24, kH - 26, w, 8);
        }
    }
    if (!g_art) PutText(dc, font, RGB(255, 255, 255), 24, 24, 400, L"ROCKETIFY");
    PutText(dc, font, RGB(255, 255, 255), 24, kH - 54, kW - 170, StatusLine().c_str());
    wchar_t pct[48];
    wsprintfW(pct, L"%d%%", static_cast<int>(Progress() * 100));
    PutText(dc, font, RGB(120, 205, 255), kW - 134, kH - 54, 110, pct, DT_RIGHT);
    BitBlt(out, 0, 0, kW, kH, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK SplashProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_TIMER: {
            Advance();
            // Ends as soon as the game has loaded and its last file was shown.
            if (g_sh->done && AllShown()) {
                static int frames = 0;
                if (frames == 0) { Sampling(GetTickCount64()); SaveCurve(); }
                if (++frames > 8) { DestroyWindow(w); return 0; }  // let 100% be drawn
            }
            if (g_closing || GetTickCount64() - g_start > 180000) DestroyWindow(w);
            else InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            Paint(BeginPaint(w, &ps));
            EndPaint(w, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static DWORD WINAPI SplashThread(LPVOID) {
    // Rocket League's own splash art, read from the installed game.
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok) {
        g_art = Gdiplus::Image::FromFile((ExeDir() + L"\\EasyAntiCheat\\SplashScreen.png").c_str());
        if (g_art && g_art->GetLastStatus() != Gdiplus::Ok) { delete g_art; g_art = nullptr; }
    }
    g_curvePath = ExeDir() + L"\\rocketify\\splash_curve.txt";
    LoadCurve();
    g_trace = _wfopen((ExeDir() + L"\\rocketify\\splash_trace.log").c_str(), L"w, ccs=UTF-8");
    WNDCLASSW wc{};
    wc.lpfnWndProc = SplashProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"RocketifySplash";
    RegisterClassW(&wc);
    g_start = g_stageSince = GetTickCount64();
    // Never takes focus, so the game's window behaves as usual behind it.
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Rocketify", WS_POPUP,
                             (GetSystemMetrics(SM_CXSCREEN) - kW) / 2, (GetSystemMetrics(SM_CYSCREEN) - kH) / 2, kW, kH,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return 0;
    ShowWindow(w, SW_SHOWNOACTIVATE);
    SetTimer(w, 1, 16, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    if (g_trace) fclose(g_trace);
    return 0;
}

// Loads rocketify.dll into the game.
static bool Inject(HANDLE process, const std::wstring& dll) {
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote && WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr);
    bool finished = true;
    if (ok) {
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE t = CreateRemoteThread(process, nullptr, 0, loadLib, remote, 0, nullptr);
        ok = false;
        if (t) {
            finished = WaitForSingleObject(t, 20000) == WAIT_OBJECT_0;
            DWORD code = 0;
            if (finished && GetExitCodeThread(t, &code)) ok = code != 0;
            CloseHandle(t);
        }
    }
    if (remote && finished) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const std::wstring dir  = ExeDir();
    const std::wstring game = dir + L"\\RocketLeague.exe";
    const std::wstring dll  = dir + L"\\rocketify\\rocketify.dll";

    if (!Exists(game)) return Fail(L"RocketLeague.exe not found next to the launcher:\n" + game);
    if (!Exists(dll))  return Fail(L"Mod loader DLL not found:\n" + dll);

    std::wstring cmd = L"\"" + game + L"\" " + ForwardedArgs();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(game.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, dir.c_str(), &si, &pi))
        return Fail(L"Could not start RocketLeague.exe (error " +
                    std::to_wstring(GetLastError()) + L")");

    HANDLE splashThread = nullptr;
    wchar_t mapName[64];
    splash::MappingName(pi.dwProcessId, mapName);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(splash::Shared), mapName);
    if (map) g_sh = static_cast<splash::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(splash::Shared)));
    if (g_sh) splashThread = CreateThread(nullptr, 0, SplashThread, nullptr, 0, nullptr);

    // The game stays paused until Rocketify and every mod are loaded (and shown); only
    // then does it start loading its own resources.
    bool ok = Inject(pi.hProcess, dll);
    for (int i = 0; ok && g_sh && i < 1500 && !(g_sh->modsLoaded && (g_modsShown || !splashThread)); ++i) {
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) break;
    }
    ResumeThread(pi.hThread);
    WaitForInputIdle(pi.hProcess, 60000);
    if (g_sh) InterlockedExchange(&g_sh->idle, 1);
    if (!ok) InterlockedExchange(&g_closing, 1);
    if (!ok)
        MessageBoxW(nullptr, L"The game started, but the mod loader could not be loaded.",
                    L"Rocketify", MB_ICONWARNING | MB_OK);

    // Stay alive while the game runs so Heroic tracks it as running.
    WaitForSingleObject(pi.hProcess, INFINITE);
    InterlockedExchange(&g_closing, 1);
    if (splashThread) WaitForSingleObject(splashThread, 1000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
}
