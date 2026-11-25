// Rocketify — public API for mods (plain C).
// C++ mods: #include <rocketify> is the simpler way to use it.
//
// A mod is a 64-bit Windows DLL with the extension .rlmod, placed in
// Binaries/Win64/rocketify/mods/. The loader calls these exports if present:
//
//   const char* Rocketify_ModName(void);                    optional, display name
//   void        Rocketify_OnLoad(const RocketifyApi* api);  once, right after load.
//               Runs off the game thread, before the engine is ready: only log() is usable.
//   void        Rocketify_OnTick(const RocketifyApi* api);  about once per second, on the
//               game thread, while in the menus. The whole API is usable here.
//   void        Rocketify_OnEvent(const RocketifyApi* api, const char* place, const char* event);
//               since version 3, game thread: e.g. "MainMenu", "onSettings".
//   const char* Rocketify_Overrides(void);
//               since version 3, once after loading: the buttons whose normal action the mod
//               replaces, e.g. "MainMenu.onSettings;MainMenu.onQuit;".
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROCKETIFY_API_VERSION 4

typedef uintptr_t RlObject;  // a game object (UObject / UFunction), 0 = none

typedef struct RocketifyApi {
    int version;                       // ROCKETIFY_API_VERSION
    void (*log)(const char* message);  // writes to rocketify/rocketify.log

    // --- since version 2, game thread only ---
    RlObject (*find_instance)(const char* className);          // live object, slow: cache it
    RlObject (*find_function)(const char* fullName);           // "Package.Class.Function"
    int (*param_offset)(RlObject function, const char* name);  // -1 if there is no such parameter
    int (*is_instance)(RlObject object, const char* className);
    int (*object_name)(RlObject object, char* out, int capacity);
    void (*call)(RlObject object, RlObject function, void* params);

    // --- since version 3, game thread only ---
    // Shows the game's own dialog (UTF-8) with 1 to 4 buttons. type: "classic" or "warning".
    // Returns 0 if it cannot be shown right now (e.g. another dialog is open): try again later.
    int (*show_popup)(const char* title, const char* message, const char* const* buttons, int buttonCount, const char* type);

    // --- since version 4, game thread only ---
    RlObject (*find_object)(const char* fullName);  // e.g. "TAGame.Default__X", slow: cache it
    // Offset and element size of a property, e.g. "TAGame.Class.Property" or (what an array holds)
    // "TAGame.Class.Array.Array". Returns 0 if there is none. Slow: cache it.
    int (*property)(const char* fullName, int* offset, int* size);
    int32_t (*find_name)(const char* text);  // index of a game name, -1 = none. Slow: cache it.
    int (*name_text)(int32_t index, int32_t number, char* out, int capacity);  // text of a game name
} RocketifyApi;

typedef const char* (*Rocketify_ModNameFn)(void);
typedef void (*Rocketify_OnLoadFn)(const RocketifyApi* api);
typedef void (*Rocketify_OnTickFn)(const RocketifyApi* api);
typedef const char* (*Rocketify_OverridesFn)(void);
typedef void (*Rocketify_OnEventFn)(const RocketifyApi* api, const char* place, const char* event);

#ifdef __cplusplus
}
#endif
