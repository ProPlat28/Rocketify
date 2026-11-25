// splash.cpp — tells the launcher's loading window what is being loaded. Asset
// files are seen by redirecting the game's CreateFileW import; the redirect only
// looks at the name, and is removed once loading is finished.
#include "splash.h"

#include <cstring>
#include <unordered_set>

#include "engine.h"
#include "internal.h"

namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
splash::Shared* g_sh = nullptr;
CreateFileWFn g_orig = nullptr;
void** g_slot = nullptr;
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
ReadFileFn g_origRead = nullptr;
void** g_readSlot = nullptr;
SRWLOCK g_handleLock = SRWLOCK_INIT;
std::unordered_set<HANDLE> g_handles;  // open asset files

bool IsAsset(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    for (const wchar_t* e : {L".upk", L".tfc", L".bnk", L".ebnk", L".wem", L".ewem", L".bin"})
        if (lstrcmpiW(dot, e) == 0) return true;
    return false;
}

HANDLE WINAPI HkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                            DWORD flags, HANDLE tmpl) {
    HANDLE h = g_orig(name, access, share, sa, disp, flags, tmpl);
    const DWORD err = GetLastError();
    if (h != INVALID_HANDLE_VALUE && name && g_sh && !g_sh->done && !engine::IsDisabled()) {
        const wchar_t* base = name;
        for (const wchar_t* p = name; *p; ++p)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        const bool asset = IsAsset(base);
        AcquireSRWLockExclusive(&g_handleLock);
        if (asset) g_handles.insert(h);
        else g_handles.erase(h);  // handle value reused for something else
        ReleaseSRWLockExclusive(&g_handleLock);
        if (asset) {
            const LONG n = InterlockedIncrement(&g_sh->lineCount) - 1;
            if (n < splash::kLines) {
                g_sh->lineTick[n] = GetTickCount64();
                lstrcpynW(g_sh->line[n], base, 128);
                InterlockedExchange(&g_sh->lineReady[n], 1);
            }
        }
    }
    SetLastError(err);
    return h;
}

BOOL WINAPI HkReadFile(HANDLE h, LPVOID buf, DWORD size, LPDWORD got, LPOVERLAPPED ov) {
    const BOOL ok = g_origRead(h, buf, size, got, ov);
    const DWORD err = GetLastError();
    if (!g_sh->done && !engine::IsDisabled() && (ok || err == ERROR_IO_PENDING)) {
        AcquireSRWLockShared(&g_handleLock);
        const bool asset = g_handles.count(h) != 0;
        ReleaseSRWLockShared(&g_handleLock);
        // asynchronous reads report their size later: count what was asked for
        if (asset) InterlockedExchangeAdd64(&g_sh->bytes, ok && got && !ov ? *got : size);
    }
    SetLastError(err);
    return ok;
}

void** FindImport(const char* fn) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (!d->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto by = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by->Name), fn) == 0)
                return reinterpret_cast<void**>(&addrs->u1.Function);
        }
    }
    return nullptr;
}

bool WriteImport(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

void SplashOpen() {
    wchar_t name[64];
    splash::MappingName(GetCurrentProcessId(), name);
    if (HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name))
        g_sh = static_cast<splash::Shared*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(splash::Shared)));
}

void SplashStage(const wchar_t* text) {
    if (!g_sh) return;
    const LONG n = g_sh->stageCount;  // only called from one thread at a time
    // With very many mods the last place stays free for the final stage, which the launcher waits for.
    const bool mod = wcsncmp(text, L"Loading mod: ", 13) == 0;
    if (n >= splash::kStages - (mod ? 1 : 0)) return;
    lstrcpynW(g_sh->stage[n], text, 128);
    InterlockedExchange(&g_sh->stageCount, n + 1);
}

void SplashModsLoaded() {
    if (g_sh) InterlockedExchange(&g_sh->modsLoaded, 1);
}

void SplashWaitIdle() {
    for (int i = 0; g_sh && !g_sh->idle && i < 1200; ++i) Sleep(50);
}

void SplashStartFiles() {
    if (!g_sh) return;
    g_slot = FindImport("CreateFileW");
    if (!g_slot) { LogF("[splash] CreateFileW import not found"); return; }
    g_orig = reinterpret_cast<CreateFileWFn>(*g_slot);
    if (!WriteImport(g_slot, reinterpret_cast<void*>(&HkCreateFileW))) g_slot = nullptr;
    if ((g_readSlot = FindImport("ReadFile"))) {
        g_origRead = reinterpret_cast<ReadFileFn>(*g_readSlot);
        if (!WriteImport(g_readSlot, reinterpret_cast<void*>(&HkReadFile))) g_readSlot = nullptr;
    }
}

void SplashDone() {
    if (!g_sh || InterlockedExchange(&g_sh->done, 1)) return;
    if (g_slot && *g_slot == reinterpret_cast<void*>(&HkCreateFileW)) WriteImport(g_slot, reinterpret_cast<void*>(g_orig));
    if (g_readSlot && *g_readSlot == reinterpret_cast<void*>(&HkReadFile)) WriteImport(g_readSlot, reinterpret_cast<void*>(g_origRead));
    LogF("[splash] %ld file opens, %lld MB read while loading", g_sh->lineCount, static_cast<long long>(g_sh->bytes >> 20));
}
