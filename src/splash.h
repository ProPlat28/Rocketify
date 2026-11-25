// splash.h — shared memory between Rocketify.exe (loading window) and rocketify.dll.
#pragma once
#include <windows.h>

namespace splash {

constexpr LONG kLines = 2048;
constexpr LONG kStages = 64;

struct Shared {
    volatile LONG done;        // set by the DLL once the menu world exists
    volatile LONG idle;        // set by the launcher once the game window is up
    volatile LONG stageCount;  // stages published so far
    volatile LONG modsLoaded;  // set by the DLL once every mod is loaded
    volatile LONG64 bytes;     // asset bytes the game has read
    volatile LONG lineCount;   // files published so far
    wchar_t stage[kStages][128];
    volatile LONG lineReady[kLines];  // set once the entry below is complete
    ULONGLONG lineTick[kLines];       // when the game opened it
    wchar_t line[kLines][128];
};

// The name carries the game's process id, so two sessions never mix.
inline void MappingName(DWORD pid, wchar_t* out) { wsprintfW(out, L"Local\\RocketifySplash_%lu", pid); }

}  // namespace splash

void SplashOpen();                         // DLL side: attach to the launcher's window, if any
void SplashStage(const wchar_t* text);
void SplashModsLoaded();                   // the launcher now lets the game run
void SplashWaitIdle();                     // blocks until the game window is up
void SplashStartFiles();                   // report every asset file the game opens
void SplashDone();
