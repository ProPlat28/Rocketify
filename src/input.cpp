// input.cpp — polls the controller (XInput) and the arrow keys so up/down can
// move between QUIT and MODS: the game's own focus list does not include MODS.
#include <windows.h>

#include "engine.h"
#include "internal.h"

namespace {

struct XState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);

volatile LONG g_stop = 0;

bool GameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI InputThread(LPVOID) {
    XInputGetStateFn getState = nullptr;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryW(dll))
            if ((getState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState")))) break;

    bool wasUp = false, wasDown = false;
    while (!g_stop) {
        bool up = false, down = false;
        if (GameFocused()) {
            for (DWORD i = 0; getState && i < 4; ++i) {
                XState st{};
                if (getState(i, &st) != 0) continue;
                up = up || (st.buttons & 0x0001) || st.ly > 16000;     // d-pad up / stick up
                down = down || (st.buttons & 0x0002) || st.ly < -16000;
            }
            up = up || (GetAsyncKeyState(VK_UP) & 0x8000);
            down = down || (GetAsyncKeyState(VK_DOWN) & 0x8000);
        }
        const unsigned long long now = GetTickCount64();
        if (up && !wasUp) engine::NativeMenuNav(1, now);
        if (down && !wasDown) engine::NativeMenuNav(2, now);
        wasUp = up;
        wasDown = down;
        Sleep(8);
    }
    return 0;
}

}  // namespace

void StartInput() {
    if (HANDLE t = CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr)) CloseHandle(t);
}

void StopInput() { InterlockedExchange(&g_stop, 1); }
