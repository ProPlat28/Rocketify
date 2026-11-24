#pragma once
#include <cstdint>
#include <string>

// Minimal Unreal Engine 3 access for RocketLeague.exe. Offsets are discovered
// at runtime by heuristics, not hardcoded, so game updates don't break them.
namespace engine {

struct Layout {
    uintptr_t gnames = 0;    // address of TArray<FNameEntry*>
    uintptr_t gobjects = 0;  // address of TArray<UObject*>
    int nameStrOff = -1;     // FNameEntry: offset of the string
    bool nameWide = true;
    int objIndexOff = -1;    // UObject: internal index
    int objNameOff = -1;     // UObject: FName
    int objClassOff = -1;    // UObject: Class
    int objOuterOff = -1;    // UObject: Outer
    int fieldNextOff = -1;       // UField::Next
    int structChildrenOff = -1;  // UStruct::Children
    int propOffsetOff = -1;      // UProperty::Offset
    int propSizeOff = -1;        // UProperty::ElementSize
    int propFlagsOff = -1;       // UProperty::PropertyFlags (uint64)
    int peIndex = -1;            // vtable slot of UObject::ProcessEvent
};

// Startup, in order (off the game thread).
bool Init();                  // find GNames / GObjects and the UObject layout
bool FindReflectionLayout();  // property / function parameter layout
bool ProbeProcessEvent();     // ProcessEvent's vtable slot + game-thread hook
bool PrepareNativeMessage();  // resolve the objects the MODS dialog needs
void StartMenuInjector(const std::wstring& iniPath);  // native MODS entry in the main menu
void StartEventTap();         // watches the UI for "menu shown" and the MODS click

void NativeMenuNav(int kind, unsigned long long tick);  // 1 = up, 2 = down (controller / keyboard)
void Disable();               // EAC guard: make every hook pass-through

// Object access used inside engine.cpp
int ObjectCount();
uintptr_t ObjectAt(int i);
bool IsObject(uintptr_t p);
std::string NameOf(uintptr_t obj);
std::string ClassNameOf(uintptr_t obj);
std::string FullNameOf(uintptr_t obj);
uintptr_t FindInstance(const std::string& className);
bool NativeMessageReady();
void ShowNativeMessage(const wchar_t* title, const wchar_t* body, const wchar_t* button);
void RunOnGameThread(void (*fn)(void*), void* ctx);
void CallFunction(uintptr_t obj, uintptr_t func, void* parms);

}  // namespace engine
