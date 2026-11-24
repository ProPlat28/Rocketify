// Rocketify — public API for mods.
//
// A mod is a Windows DLL placed in  Binaries/Win64/rocketify/mods/.
// The loader calls these exports if present:
//
//   const char* Rocketify_ModName(void);              optional, display name
//   void        Rocketify_OnLoad(const RocketifyApi* api);  called once after load
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define ROCKETIFY_API_VERSION 1

typedef struct RocketifyApi {
    int version;                       // ROCKETIFY_API_VERSION
    void (*log)(const char* message);  // writes to rocketify/rocketify.log
} RocketifyApi;

typedef const char* (*Rocketify_ModNameFn)(void);
typedef void (*Rocketify_OnLoadFn)(const RocketifyApi* api);

#ifdef __cplusplus
}
#endif
