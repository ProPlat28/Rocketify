// Rocketify.exe — starts the plain RocketLeague.exe (the same exe the
// EAC bootstrapper would start) and loads rocketify\rocketify.dll into it.
// Arguments passed to this launcher (Epic auth args from Heroic/legendary)
// are forwarded unchanged.
#include <windows.h>
#include <cwctype>
#include <string>

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
    if (!CreateProcessW(game.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, dir.c_str(), &si, &pi))
        return Fail(L"Could not start RocketLeague.exe (error " +
                    std::to_wstring(GetLastError()) + L")");

    // Let the process finish its own startup before we add our DLL.
    WaitForInputIdle(pi.hProcess, 30000);

    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote && WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr);
    if (ok) {
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE t = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLib, remote, 0, nullptr);
        if (t) {
            WaitForSingleObject(t, 20000);
            DWORD code = 0;
            GetExitCodeThread(t, &code);
            ok = code != 0;
            CloseHandle(t);
        } else {
            ok = false;
        }
    }
    if (remote) VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (!ok)
        MessageBoxW(nullptr, L"The game started, but the mod loader could not be loaded.",
                    L"Rocketify", MB_ICONWARNING | MB_OK);

    // Stay alive while the game runs so Heroic tracks it as running.
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
}
