// Minimal example mod. Build output goes to rocketify/mods/hello_mod.dll.
#include "rocketify.h"

extern "C" __declspec(dllexport) const char* Rocketify_ModName(void) { return "Hello Mod"; }

extern "C" __declspec(dllexport) void Rocketify_OnLoad(const RocketifyApi* api) {
    api->log("Hello from hello_mod!");
}
