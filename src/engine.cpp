// engine.cpp — finds Unreal Engine 3's GNames / GObjects in RocketLeague.exe
// and works out the UObject field layout, all by heuristics so it keeps
// working across game updates. Every read of game memory goes through
// ReadProcessMemory, so a bad guess or a freed object can never crash the game.
#include <windows.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "internal.h"

namespace engine {

static Layout L;
static volatile LONG g_off = 0;  // set by Disable() (EAC guard): every hook becomes pass-through

void Disable() { InterlockedExchange(&g_off, 1); }
bool IsDisabled() { return g_off != 0; }

struct TArr {
    uintptr_t data;
    int32_t num;
    int32_t max;
};

static bool Read(uintptr_t a, void* out, size_t n) {
    if (a < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
}
template <class T> static bool R(uintptr_t a, T& v) { return Read(a, &v, sizeof(v)); }

const Layout& GetLayout() { return L; }

// ---------- names ----------
static bool ReadStr(uintptr_t entry, int off, bool wide, char* buf, int cap) {
    if (!entry) return false;
    int n = 0;
    uintptr_t p = entry + off;
    while (n < cap - 1) {
        // read in small chunks so we never straddle into an unmapped page by much
        if (wide) {
            wchar_t w[16];
            if (!Read(p, w, sizeof(w))) {
                // fall back to one char at a time near a page edge
                if (!R(p, w[0])) return false;
                if (!w[0]) break;
                if (w[0] < 0x20 || w[0] > 0x7e) return false;
                buf[n++] = static_cast<char>(w[0]);
                p += 2;
                continue;
            }
            int i = 0;
            for (; i < 16 && n < cap - 1; ++i) {
                if (!w[i]) { buf[n] = 0; return n > 0; }
                if (w[i] < 0x20 || w[i] > 0x7e) return false;
                buf[n++] = static_cast<char>(w[i]);
            }
            p += i * 2;
        } else {
            char c;
            if (!R(p, c)) return false;
            if (!c) break;
            if (c < 0x20 || c > 0x7e) return false;
            buf[n++] = c;
            ++p;
        }
    }
    buf[n] = 0;
    return n > 0;
}

static bool StrAtEq(uintptr_t entry, int off, bool wide, const char* s) {
    char buf[64];
    return ReadStr(entry, off, wide, buf, sizeof(buf)) && strcmp(buf, s) == 0;
}

static int NameCount() {
    TArr a{};
    return R(L.gnames, a) ? a.num : 0;
}

static std::string NameByIndex(int idx) {
    TArr a{};
    if (!R(L.gnames, a) || idx < 0 || idx >= a.num) return "";
    uintptr_t e = 0;
    if (!R(a.data + static_cast<uintptr_t>(idx) * 8, e) || !e) return "";
    char buf[512];
    return ReadStr(e, L.nameStrOff, L.nameWide, buf, sizeof(buf)) ? buf : "";
}

// ---------- module sections ----------
struct Section {
    uintptr_t base;
    size_t size;
    std::vector<uint8_t> copy;  // snapshot of the section (unreadable pages left zero)
    bool At(uintptr_t p, TArr& a) const {
        const size_t off = p - base;
        if (off + sizeof(TArr) > copy.size()) return false;
        memcpy(&a, copy.data() + off, sizeof(TArr));
        return true;
    }
};

static std::vector<Section> WritableSections() {
    std::vector<Section> out;
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (s->Characteristics & IMAGE_SCN_MEM_WRITE) {
            Section sec{base + s->VirtualAddress, s->Misc.VirtualSize, {}};
            sec.copy.assign(sec.size, 0);
            for (size_t o = 0; o < sec.size; o += 0x1000)
                Read(sec.base + o, sec.copy.data() + o, (sec.size - o < 0x1000) ? sec.size - o : 0x1000);
            out.push_back(std::move(sec));
        }
    return out;
}

static bool FindGNames(const std::vector<Section>& secs) {
    for (const Section& s : secs) {
        for (uintptr_t p = s.base; p + sizeof(TArr) <= s.base + s.size; p += 8) {
            TArr a;
            if (!s.At(p, a)) continue;
            if (a.num < 20000 || a.num > a.max || a.max > (1 << 24) || (a.data & 7) || a.data < 0x10000) continue;
            uintptr_t e0 = 0, e1 = 0;
            if (!R(a.data, e0) || !R(a.data + 8, e1) || !e0 || !e1) continue;
            for (int wide = 1; wide >= 0; --wide)
                for (int off = 0; off <= 0x40; off += 2)
                    if (StrAtEq(e0, off, wide, "None") && StrAtEq(e1, off, wide, "ByteProperty")) {
                        L.gnames = p;
                        L.nameStrOff = off;
                        L.nameWide = wide != 0;
                        return true;
                    }
        }
    }
    return false;
}

// ---------- objects ----------
int ObjectCount() {
    TArr a{};
    return R(L.gobjects, a) ? a.num : 0;
}

uintptr_t ObjectAt(int i) {
    TArr a{};
    uintptr_t o = 0;
    if (!R(L.gobjects, a) || i < 0 || i >= a.num) return 0;
    R(a.data + static_cast<uintptr_t>(i) * 8, o);
    return o;
}

bool IsObject(uintptr_t p) {
    if (!p) return false;
    int32_t idx = -1;
    return R(p + L.objIndexOff, idx) && ObjectAt(idx) == p;
}

static uintptr_t Ptr(uintptr_t obj, int off) {
    uintptr_t v = 0;
    R(obj + off, v);
    return v;
}

std::string NameOf(uintptr_t obj) {
    int32_t idx = -1, num = 0;
    if (!R(obj + L.objNameOff, idx) || !R(obj + L.objNameOff + 4, num)) return "";
    std::string n = NameByIndex(idx);
    if (num > 0) n += "_" + std::to_string(num - 1);
    return n;
}

std::string ClassNameOf(uintptr_t obj) { return NameOf(Ptr(obj, L.objClassOff)); }

std::string FullNameOf(uintptr_t obj) {
    std::string out = NameOf(obj);
    uintptr_t o = Ptr(obj, L.objOuterOff);
    for (int depth = 0; o && depth < 32; ++depth) {
        out = NameOf(o) + "." + out;
        o = Ptr(o, L.objOuterOff);
    }
    return out;
}

static bool FindGObjects(const std::vector<Section>& secs) {
    const int nNames = NameCount();
    for (const Section& s : secs) {
        for (uintptr_t p = s.base; p + sizeof(TArr) <= s.base + s.size; p += 8) {
            if (p == L.gnames) continue;
            TArr a;
            if (!s.At(p, a)) continue;
            if (a.num < 50000 || a.num > a.max || a.max > (1 << 24) || (a.data & 7) || a.data < 0x10000) continue;

            // sample the first objects
            std::vector<std::pair<int, uintptr_t>> sample;
            for (int i = 0; i < 300 && i < a.num; ++i) {
                uintptr_t o = 0;
                if (R(a.data + static_cast<uintptr_t>(i) * 8, o) && o) sample.push_back({i, o});
            }
            if (sample.size() < 150) continue;

            // 1) internal index field: obj->Index == i
            int idxOff = -1;
            for (int k = 0x08; k <= 0x80 && idxOff < 0; k += 4) {
                bool ok = true;
                for (auto& [i, o] : sample) {
                    int32_t v;
                    if (!R(o + k, v) || v != i) { ok = false; break; }
                }
                if (ok) idxOff = k;
            }
            if (idxOff < 0) continue;
            L.gobjects = p;
            L.objIndexOff = idxOff;

            // 2) Class: pointer to an object whose class's class is named "Class"
            //    (needs the name field, so try name offsets together with class offsets)
            for (int n = 0x08; n <= 0x90; n += 4) {
                if (n == idxOff) continue;
                bool namesOk = true;
                for (auto& [i, o] : sample) {
                    int32_t ni, nn;
                    if (!R(o + n, ni) || !R(o + n + 4, nn) || ni < 0 || ni >= nNames || nn < 0 || nn > 100000) {
                        namesOk = false;
                        break;
                    }
                }
                if (!namesOk) continue;
                L.objNameOff = n;
                for (int c = 0x08; c <= 0x98; c += 8) {
                    if (c == n || c == idxOff) continue;
                    L.objClassOff = c;
                    bool ok = true;
                    for (auto& [i, o] : sample) {
                        uintptr_t cls = Ptr(o, c);
                        if (!IsObject(cls) || NameOf(Ptr(cls, c)) != "Class") { ok = false; break; }
                    }
                    if (!ok) continue;

                    // 3) Outer: null or an object; every chain ends at a Package
                    for (int ou = 0x08; ou <= 0x98; ou += 8) {
                        if (ou == c || ou == n || ou == idxOff) continue;
                        bool okO = true;
                        int nonNull = 0;
                        for (auto& [i, o] : sample) {
                            uintptr_t cur = Ptr(o, ou);
                            if (!cur) continue;
                            ++nonNull;
                            uintptr_t top = o;
                            for (int d = 0; cur && d < 32; ++d) {
                                if (!IsObject(cur)) { okO = false; break; }
                                top = cur;
                                cur = Ptr(cur, ou);
                            }
                            if (!okO) break;
                            if (NameOf(Ptr(top, c)) != "Package") { okO = false; break; }
                        }
                        if (okO && nonNull > static_cast<int>(sample.size()) / 2) {
                            L.objOuterOff = ou;
                            return true;
                        }
                    }
                }
            }
            L.gobjects = 0;
        }
    }
    return false;
}

bool Init() {
    const auto secs = WritableSections();
    if (!FindGNames(secs)) {
        LogF("[engine] GNames not found");
        return false;
    }
    LogF("[engine] GNames at exe+0x%llx, %d names, string at +0x%x (%s)",
         static_cast<unsigned long long>(L.gnames - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
         NameCount(), L.nameStrOff, L.nameWide ? "wide" : "ansi");
    if (!FindGObjects(secs)) {
        LogF("[engine] GObjects not found");
        return false;
    }
    LogF("[engine] GObjects at exe+0x%llx, %d objects; UObject: Index +0x%x, Outer +0x%x, Name +0x%x, Class +0x%x",
         static_cast<unsigned long long>(L.gobjects - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
         ObjectCount(), L.objIndexOff, L.objOuterOff, L.objNameOff, L.objClassOff);
    return true;
}

// ======================= reflection layout =======================
static bool EndsWith(const std::string& s, const char* suf) {
    const size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

static bool IsProperty(uintptr_t p) { return IsObject(p) && EndsWith(ClassNameOf(p), "Property"); }

uintptr_t FindInstance(const std::string& className) {
    const int n = ObjectCount();
    uintptr_t best = 0;
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o || ClassNameOf(o) != className) continue;
        const std::string full = FullNameOf(o);
        if (full.find("Default__") != std::string::npos || full.rfind("Archetypes.", 0) == 0) continue;
        best = o;  // prefer the newest one
    }
    return best;
}

static std::vector<uintptr_t> Chain(uintptr_t st, int max = 64) {
    std::vector<uintptr_t> out;
    uintptr_t c = Ptr(st, L.structChildrenOff);
    while (c && static_cast<int>(out.size()) < max) {
        out.push_back(c);
        c = Ptr(c, L.fieldNextOff);
    }
    return out;
}

bool FindReflectionLayout() {
    std::vector<uintptr_t> funcs;
    const int n = ObjectCount();
    for (int i = 0; i < n && funcs.size() < 3000; ++i) {
        uintptr_t o = ObjectAt(i);
        if (o && ClassNameOf(o) == "Function") funcs.push_back(o);
    }
    if (funcs.size() < 500) return false;

    // A property's Outer is the function that owns it; that rules out archetype
    // pointers, which also lead to properties.
    auto ownedBy = [](uintptr_t prop, uintptr_t owner) { return IsProperty(prop) && Ptr(prop, L.objOuterOff) == owner; };

    // UStruct::Children: null or a property owned by the function
    for (int c = 0x58; c <= 0x160 && L.structChildrenOff < 0; c += 8) {
        size_t nonNull = 0;
        bool ok = true;
        for (uintptr_t f : funcs) {
            uintptr_t ch = Ptr(f, c);
            if (!ch) continue;
            if (!ownedBy(ch, f)) { ok = false; break; }
            ++nonNull;
        }
        if (ok && nonNull > funcs.size() / 5) L.structChildrenOff = c;
    }
    if (L.structChildrenOff < 0) { LogF("[engine] UStruct::Children not found"); return false; }

    // UField::Next: null or another property of the same function
    for (int k = 0x58; k < L.structChildrenOff && L.fieldNextOff < 0; k += 8) {
        int nonNull = 0;
        bool ok = true;
        for (uintptr_t f : funcs) {
            uintptr_t ch = Ptr(f, L.structChildrenOff);
            if (!ch) continue;
            uintptr_t nx = Ptr(ch, k);
            if (!nx) continue;
            if (!ownedBy(nx, f)) { ok = false; break; }
            ++nonNull;
        }
        if (ok && nonNull > 50) L.fieldNextOff = k;
    }
    if (L.fieldNextOff < 0) { LogF("[engine] UField::Next not found"); return false; }

    std::vector<std::vector<uintptr_t>> chains;
    std::vector<uintptr_t> props;
    for (uintptr_t f : funcs) {
        auto ch = Chain(f);
        for (uintptr_t p : ch) props.push_back(p);
        if (ch.size() >= 2) chains.push_back(std::move(ch));
    }

    // UProperty::Offset: strictly monotonic along the chain (either order), smallest is 0
    for (int k = 0x58; k <= 0x160 && L.propOffsetOff < 0; k += 4) {
        size_t good = 0;
        for (auto& ch : chains) {
            std::vector<int32_t> v(ch.size());
            bool ok = true;
            for (size_t i = 0; i < ch.size() && ok; ++i) ok = R(ch[i] + k, v[i]) && v[i] >= 0 && v[i] < 0x10000;
            if (!ok) continue;
            bool inc = true, dec = true;
            for (size_t i = 1; i < v.size(); ++i) {
                inc = inc && v[i] > v[i - 1];
                dec = dec && v[i] < v[i - 1];
            }
            if ((inc && v.front() == 0) || (dec && v.back() == 0)) ++good;
        }
        if (chains.size() > 50 && good * 10 >= chains.size() * 8) L.propOffsetOff = k;
    }

    // UProperty::ElementSize: Int=4, Float=4, Str=16, Object=8
    for (int k = 0x58; k <= 0x160 && L.propSizeOff < 0; k += 4) {
        if (k == L.propOffsetOff) continue;
        int checked = 0, match = 0;
        for (uintptr_t p : props) {
            const std::string c = ClassNameOf(p);
            int want = c == "IntProperty" || c == "FloatProperty" ? 4 : c == "StrProperty" ? 16 : c == "ObjectProperty" ? 8 : 0;
            if (!want) continue;
            int32_t v;
            ++checked;
            if (R(p + k, v) && v == want) ++match;
        }
        if (checked > 50 && match * 100 >= checked * 95) L.propSizeOff = k;
    }

    // UProperty::PropertyFlags: ReturnValue has CPF_Parm|CPF_ReturnParm (0x80|0x400)
    for (int k = 0x58; k <= 0x160 && L.propFlagsOff < 0; k += 4) {
        if (k == L.propOffsetOff || k == L.propSizeOff) continue;
        int checked = 0, parmsWithout = 0;
        bool ok = true;
        for (uintptr_t p : props) {
            uint64_t v;
            if (!R(p + k, v)) { ok = false; break; }
            if (NameOf(p) == "ReturnValue") {
                if ((v & 0x480) != 0x480) { ok = false; break; }
                ++checked;
            } else if (!(v & 0x80)) {
                ++parmsWithout;  // locals
            }
        }
        if (ok && checked > 30 && parmsWithout > 0) L.propFlagsOff = k;
    }

    LogF("[engine] reflection: Next +0x%x, Children +0x%x, Prop.Offset +0x%x, Prop.Size +0x%x, Prop.Flags +0x%x",
         L.fieldNextOff, L.structChildrenOff, L.propOffsetOff, L.propSizeOff, L.propFlagsOff);
    return L.propOffsetOff >= 0 && L.propSizeOff >= 0;
}

static std::string ReadFString(uintptr_t at) {
    struct { uintptr_t data; int32_t num; int32_t max; } fs{};
    if (!R(at, fs) || !fs.data || fs.num <= 0 || fs.num > 4096) return "";
    std::wstring w(fs.num, L'\0');
    if (!Read(fs.data, w.data(), fs.num * 2)) return "";
    std::string out;
    for (wchar_t c : w) {
        if (!c) break;
        out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    return out;
}

// ======================= ProcessEvent =======================
struct SlotRec {
    volatile LONG64 hits;
    volatile uintptr_t lastArg;
    volatile LONG64 r9NonNull;  // ProcessEvent's 4th arg (Result) is almost always null
};

// Pass-through thunk: counts the call, records RDX (2nd arg), jumps to the original.
static void* MakeProbeStub(SlotRec* rec, void* orig) {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) return nullptr;
    int i = 0;
    p[i++] = 0x48; p[i++] = 0xB8; memcpy(p + i, &rec, 8); i += 8;           // mov rax, rec
    p[i++] = 0xF0; p[i++] = 0x48; p[i++] = 0xFF; p[i++] = 0x00;             // lock inc qword [rax]
    p[i++] = 0x48; p[i++] = 0x89; p[i++] = 0x50; p[i++] = 0x08;             // mov [rax+8], rdx
    p[i++] = 0x4D; p[i++] = 0x85; p[i++] = 0xC9;                             // test r9, r9
    p[i++] = 0x74; p[i++] = 0x05;                                            // jz +5
    p[i++] = 0xF0; p[i++] = 0x48; p[i++] = 0xFF; p[i++] = 0x40; p[i++] = 0x10; // lock inc qword [rax+16]
    p[i++] = 0x48; p[i++] = 0xB8; memcpy(p + i, &orig, 8); i += 8;          // mov rax, orig
    p[i++] = 0xFF; p[i++] = 0xE0;                                            // jmp rax
    FlushInstructionCache(GetCurrentProcess(), p, 64);
    return p;  // never freed: a thread may still be inside it
}

static bool WriteSlot(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

static bool InText(uintptr_t p) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) && p >= base + s->VirtualAddress &&
            p < base + s->VirtualAddress + s->Misc.VirtualSize)
            return true;
    return false;
}

using ProcessEventFn = void(__fastcall*)(uintptr_t obj, uintptr_t func, void* parms, void* result);
static ProcessEventFn g_pcProcessEvent = nullptr;  // original slot of the hooked controller class

static SRWLOCK g_taskLock = SRWLOCK_INIT;
static std::vector<std::pair<void (*)(void*), void*>> g_tasks;
static thread_local bool t_inTasks = false;
static volatile LONG g_hasTasks = 0;

void RunOnGameThread(void (*fn)(void*), void* ctx) {
    AcquireSRWLockExclusive(&g_taskLock);
    g_tasks.push_back({fn, ctx});
    InterlockedExchange(&g_hasTasks, 1);
    ReleaseSRWLockExclusive(&g_taskLock);
}

static void __fastcall HkControllerProcessEvent(uintptr_t obj, uintptr_t func, void* parms, void* result) {
    if (g_off) return g_pcProcessEvent(obj, func, parms, result);
    if (!t_inTasks && g_hasTasks) {
        AcquireSRWLockExclusive(&g_taskLock);
        auto tasks = std::move(g_tasks);
        g_tasks.clear();
        InterlockedExchange(&g_hasTasks, 0);
        ReleaseSRWLockExclusive(&g_taskLock);
        t_inTasks = true;
        for (auto& [fn, ctx] : tasks) fn(ctx);
        t_inTasks = false;
    }
    g_pcProcessEvent(obj, func, parms, result);
}

void CallFunction(uintptr_t obj, uintptr_t func, void* parms) {
    // Map changes destroy UI objects; never call into one that is gone.
    if (!IsObject(obj) || !IsObject(func)) return;
    // Use the target's own vtable: subclasses (e.g. actors) may override ProcessEvent.
    void** vt = *reinterpret_cast<void***>(obj);
    reinterpret_cast<ProcessEventFn>(vt[L.peIndex])(obj, func, parms, nullptr);
}

// ======================= native message dialog =======================
struct FString {
    const wchar_t* data;
    int32_t num;  // includes the terminator
    int32_t max;
};

static std::unordered_map<std::string, uintptr_t> g_functions;  // full name -> UFunction
static uintptr_t Fn(const char* full);
static uintptr_t g_shell = 0, g_modalClass = 0, g_warningClass = 0;
static uintptr_t g_fnCreateModal = 0, g_fnSetTitle = 0, g_fnSetBody = 0, g_fnAddButton = 0;

bool NativeMessageReady() {
    return g_shell && g_modalClass && g_fnCreateModal && g_fnSetTitle && g_fnSetBody && g_fnAddButton &&
           L.peIndex >= 0 && L.propOffsetOff >= 0;
}

// Offset of a named parameter inside a function's parameter block (-1 if missing).
int ParamOffset(uintptr_t fn, const char* name) {
    for (uintptr_t p : Chain(fn, 64))
        if (NameOf(p) == name) {
            int32_t off = -1;
            R(p + L.propOffsetOff, off);
            return off;
        }
    return -1;
}

uintptr_t FindFunction(const char* fullName) { return Fn(fullName); }

uintptr_t FindObject(const std::string& fullName, bool property) {
    const std::string name = fullName.substr(fullName.rfind('.') + 1);
    const int n = ObjectCount();
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o || NameOf(o) != name) continue;
        if (property) {
            const std::string cls = ClassNameOf(o);
            if (cls.size() < 8 || cls.compare(cls.size() - 8, 8, "Property") != 0) continue;
        }
        if (FullNameOf(o) == fullName) return o;
    }
    return 0;
}

bool PropertyInfo(const std::string& fullName, int& offset, int& size) {
    if (L.propOffsetOff < 0 || L.propSizeOff < 0) return false;
    uintptr_t p = FindObject(fullName, true);
    int32_t off = -1, sz = 0;
    if (!p || !R(p + L.propOffsetOff, off) || !R(p + L.propSizeOff, sz) || off < 0 || sz <= 0) return false;
    offset = off;
    size = sz;
    return true;
}

int FindName(const std::string& text) {
    for (int i = 0, n = NameCount(); i < n; ++i)
        if (NameByIndex(i) == text) return i;
    return -1;
}

std::string NameText(int index, int number) {
    std::string n = NameByIndex(index);
    if (number > 0) n += "_" + std::to_string(number - 1);
    return n;
}

bool PrepareNativeMessage() {
    // one pass over all objects
    const int n = ObjectCount();
    g_shell = g_modalClass = g_warningClass = g_fnCreateModal = g_fnSetTitle = g_fnSetBody = g_fnAddButton = 0;
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o) continue;
        const std::string cls = ClassNameOf(o);
        if (cls == "GFxShell_TA" && !g_shell && FullNameOf(o).rfind("Transient.", 0) == 0) g_shell = o;
        else if (cls == "Class" && NameOf(o) == "GFxModal_X") g_modalClass = o;
        else if (cls == "Class" && NameOf(o) == "GFxModal_Warning_TA") g_warningClass = o;
        else if (cls == "Function") {
            const std::string full = FullNameOf(o);
            g_functions[full] = o;
            if (full == "TAGame.GFxShell_TA.CreateModal") g_fnCreateModal = o;
            else if (full == "ProjectX.GFxModal_X.SetTitle") g_fnSetTitle = o;
            else if (full == "ProjectX.GFxModal_X.SetBody") g_fnSetBody = o;
            else if (full == "ProjectX.GFxModal_X.AddButton") g_fnAddButton = o;
        }
    }
    LogF("[engine] native dialog: shell %d, GFxModal_X %d, CreateModal %d, SetTitle %d, SetBody %d, AddButton %d",
         !!g_shell, !!g_modalClass, !!g_fnCreateModal, !!g_fnSetTitle, !!g_fnSetBody, !!g_fnAddButton);
    return NativeMessageReady();
}

static bool RefreshShell();

struct MsgTask {
    std::wstring title, body;
    std::vector<std::wstring> buttons;
    bool warning;
};

static void PutStr(uint8_t* buf, int off, const std::wstring& s) {
    FString fs{s.c_str(), static_cast<int32_t>(s.size() + 1), static_cast<int32_t>(s.size() + 1)};
    memcpy(buf + off, &fs, sizeof(fs));
}

// Calls modal.<fn>(string) for SetTitle / SetBody style functions.
static void CallWithString(uintptr_t obj, uintptr_t fn, const char* param, const std::wstring& value) {
    alignas(16) uint8_t buf[0x100] = {};
    const int off = ParamOffset(fn, param);
    if (off < 0 || off + 16 > static_cast<int>(sizeof(buf))) return;
    PutStr(buf, off, value);
    CallFunction(obj, fn, buf);
}

static volatile LONG g_messagePending = 0;
static uintptr_t g_ourModal = 0;  // the last popup we opened
static ULONGLONG g_ourModalAt = 0;
static bool g_ghost = false;      // it is gone from the screen, but the shell still lists it

static void ShowMessageTask(void* ctx) {
    InterlockedExchange(&g_messagePending, 0);
    auto* t = static_cast<MsgTask*>(ctx);
    if (!RefreshShell()) {
        LogF("[engine] GFxShell_TA instance is gone");
        delete t;
        return;
    }
    // modal = Shell.CreateModal(class'GFxModal_X')
    alignas(16) uint8_t buf[0x100] = {};
    const int clsOff = ParamOffset(g_fnCreateModal, "ModalClass");
    const int retOff = ParamOffset(g_fnCreateModal, "ReturnValue");
    if (clsOff < 0 || retOff < 0) {
        LogF("[engine] CreateModal parameters not found");
        delete t;
        return;
    }
    const uintptr_t modalClass = t->warning && g_warningClass ? g_warningClass : g_modalClass;
    memcpy(buf + clsOff, &modalClass, 8);
    CallFunction(g_shell, g_fnCreateModal, buf);
    uintptr_t modal = 0;
    memcpy(&modal, buf + retOff, 8);
    if (!IsObject(modal)) {
        LogF("[engine] CreateModal returned no modal");
        delete t;
        return;
    }
    CallWithString(modal, g_fnSetTitle, "LocalizedText", t->title);
    CallWithString(modal, g_fnSetBody, "LocalizedText", t->body);
    // AddButton(Label, OnClick = none): the modal closes itself when a button is clicked
    for (const std::wstring& b : t->buttons) CallWithString(modal, g_fnAddButton, "Label", b);
    g_ourModal = modal;
    g_ourModalAt = GetTickCount64();
    g_ghost = false;
    LogF("[engine] opened native dialog %s", FullNameOf(modal).c_str());
    delete t;
}

void ShowNativeMessage(const wchar_t* title, const wchar_t* body, const std::vector<std::wstring>& buttons, bool warning) {
    InterlockedExchange(&g_messagePending, 1);
    RunOnGameThread(ShowMessageTask, new MsgTask{title, body, buttons, warning});
}

bool HasWarningPopup() { return g_warningClass != 0; }

bool ProbeProcessEvent() {
    uintptr_t pc = FindInstance("PlayerController_Menu_TA");
    if (!pc) return false;
    void** vt = *reinterpret_cast<void***>(pc);
    int slots = 0;
    while (slots < 200 && InText(reinterpret_cast<uintptr_t>(vt[slots]))) ++slots;

    static SlotRec recs[200];
    std::vector<void*> orig(vt, vt + slots);
    for (int i = 0; i < slots; ++i) {
        recs[i] = {0, 0, 0};
        if (void* stub = MakeProbeStub(&recs[i], orig[i])) WriteSlot(&vt[i], stub);
    }
    Sleep(1500);
    for (int i = 0; i < slots; ++i) WriteSlot(&vt[i], orig[i]);

    int best = -1;
    LONG64 bestHits = 0;
    for (int i = 0; i < slots; ++i) {
        uintptr_t a = recs[i].lastArg;
        if (recs[i].hits > 0 && IsObject(a) && ClassNameOf(a) == "Function") {
            const bool resultMostlyNull = recs[i].r9NonNull * 10 < recs[i].hits;
            LogF("[engine] slot %d: %lld calls (%lld with 4th arg), last function %s", i,
                 static_cast<long long>(recs[i].hits), static_cast<long long>(recs[i].r9NonNull),
                 FullNameOf(a).c_str());
            if (resultMostlyNull && recs[i].hits > bestHits) { best = i; bestHits = recs[i].hits; }
        }
    }
    if (best < 0) { LogF("[engine] ProcessEvent slot not found (%d slots probed)", slots); return false; }
    L.peIndex = best;
    g_pcProcessEvent = reinterpret_cast<ProcessEventFn>(orig[best]);
    WriteSlot(&vt[best], reinterpret_cast<void*>(&HkControllerProcessEvent));
    LogF("[engine] ProcessEvent = vtable slot %d (exe+0x%llx); game-thread hook installed", best,
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(orig[best]) -
                                         reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    return true;
}

// ======================= Scaleform (GFx) access =======================
struct ASValue {  // GFxUI.GFxMoviePlayer.ASValue (0x28 bytes)
    uint8_t Type;
    uint8_t pad[3];
    uint32_t B;
    float N;
    int32_t I;
    FString S;
    uintptr_t T;  // GFxObject
};
static_assert(sizeof(ASValue) == 0x28, "ASValue layout");

static uintptr_t Fn(const char* full) {
    auto it = g_functions.find(full);
    return it == g_functions.end() ? 0 : it->second;
}

static std::string Narrow(const FString& fs) {
    std::string out;
    if (!fs.data || fs.num <= 0 || fs.num > 100000) return out;
    for (int i = 0; i < fs.num && fs.data[i]; ++i)
        out += (fs.data[i] >= 0x20 && fs.data[i] < 0x7f) ? static_cast<char>(fs.data[i]) : '?';
    return out;
}

static std::string GfxGetString(uintptr_t obj, const char* member) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.GetString");
    if (!fn) return "";
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    CallFunction(obj, fn, buf);
    FString r;
    memcpy(&r, buf + 0x10, sizeof(r));
    return Narrow(r);
}

static int GfxGetInt(uintptr_t obj, const char* member) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.GetInt");
    if (!fn) return 0;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    CallFunction(obj, fn, buf);
    int32_t r;
    memcpy(&r, buf + 0x10, 4);
    return r;
}

static ASValue GfxInvoke(uintptr_t obj, const char* member, std::vector<ASValue> args) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.Invoke");
    ASValue ret{};
    if (!fn) return ret;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x80] = {};
    PutStr(buf, 0x00, m);
    struct { ASValue* d; int32_t n, max; } arr{args.data(), static_cast<int32_t>(args.size()), static_cast<int32_t>(args.size())};
    memcpy(buf + 0x10, &arr, sizeof(arr));
    CallFunction(obj, fn, buf);
    memcpy(&ret, buf + 0x20, sizeof(ret));
    return ret;
}

static uintptr_t GfxGetObject(uintptr_t obj, const std::string& member) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.GetObject");
    if (!fn) return 0;
    std::wstring m(member.begin(), member.end());
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    CallFunction(obj, fn, buf);
    uintptr_t r = 0;
    memcpy(&r, buf + 0x18, 8);
    return r;
}

static uintptr_t GfxVar(uintptr_t movie, const std::string& path) {
    static uintptr_t fn = Fn("GFxUI.GFxMoviePlayer.GetVariableObject");
    if (!fn) return 0;
    std::wstring p(path.begin(), path.end());
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, p);
    CallFunction(movie, fn, buf);
    uintptr_t r = 0;
    memcpy(&r, buf + 0x18, 8);
    return IsObject(r) ? r : 0;
}

// Newest live object of a class. After a map change the previous one can linger until
// garbage collection; the engine numbers instances upwards (Name_4 is newer than Name_0).
static uintptr_t NewestTransient(const char* className) {
    const int n = ObjectCount();
    uintptr_t best = 0;
    int32_t bestNum = -1;
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o || ClassNameOf(o) != className || FullNameOf(o).rfind("Transient.", 0) != 0) continue;
        int32_t num = 0;
        R(o + L.objNameOff + 4, num);
        if (num > bestNum) { bestNum = num; best = o; }
    }
    return best;
}

static uintptr_t FindShellMovie() { return NewestTransient("GFxMoviePlayer_X"); }

static float GfxGetFloat(uintptr_t obj, const char* member) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.GetFloat");
    if (!fn) return 0;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    CallFunction(obj, fn, buf);
    float r;
    memcpy(&r, buf + 0x10, 4);
    return r;
}

// ======================= native MODS menu entry =======================
static void GfxSetString(uintptr_t obj, const char* member, const std::wstring& value) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.SetString");
    if (!fn || !obj) return;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    PutStr(buf, 0x10, value);
    CallFunction(obj, fn, buf);
}

static void GfxSetFloat(uintptr_t obj, const char* member, float v) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.SetFloat");
    if (!fn || !obj) return;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    memcpy(buf + 0x10, &v, 4);
    CallFunction(obj, fn, buf);
}

static uintptr_t GfxAttach(uintptr_t parent, const std::string& symbol, const std::string& inst, int depth) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.AttachMovie");
    if (!fn || !parent) return 0;
    std::wstring s(symbol.begin(), symbol.end()), i(inst.begin(), inst.end());
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, s);
    PutStr(buf, 0x10, i);
    memcpy(buf + 0x20, &depth, 4);
    CallFunction(parent, fn, buf);
    uintptr_t r = 0;
    memcpy(&r, buf + 0x30, 8);
    return IsObject(r) ? r : 0;
}

// The shell is recreated on map changes.
static bool RefreshShell() {
    if (IsObject(g_shell) && ClassNameOf(g_shell) == "GFxShell_TA") return true;
    g_shell = NewestTransient("GFxShell_TA");
    return g_shell != 0;
}

static bool ShellListsModal() {
    static uintptr_t fn = Fn("TAGame.GFxShell_TA.IsModalActive");
    if (!fn || !IsObject(g_shell)) return false;
    alignas(16) uint8_t buf[0x10] = {};
    CallFunction(g_shell, fn, buf);
    uint32_t r;
    memcpy(&r, buf, 4);
    return r != 0;
}

static uintptr_t TopModal() {
    static uintptr_t fn = Fn("TAGame.GFxShell_TA.GetTopModal");
    static const int off = fn ? ParamOffset(fn, "ReturnValue") : -1;
    if (off < 0 || off > 0x80 || !IsObject(g_shell)) return 0;
    alignas(16) uint8_t buf[0x100] = {};
    CallFunction(g_shell, fn, buf);
    uintptr_t m = 0;
    memcpy(&m, buf + off, 8);
    return IsObject(m) ? m : 0;
}

static bool ShellModalActive() { return ShellListsModal() && !(g_ghost && g_ourModal && TopModal() == g_ourModal); }

// Closing a popup with Back (or losing it on a screen change) can leave it in the shell's list,
// which would block every later popup. Called when the menu itself has the focus again.
static void DropGhostModal() {
    if (!g_ourModal || !IsObject(g_ourModal) || !ShellModalActive()) return;
    if (ClassNameOf(g_ourModal).rfind("GFxModal", 0) != 0) {
        g_ourModal = 0;
        return;
    }
    const uintptr_t top = TopModal();
    if (top && top != g_ourModal) return;  // a popup of the game
    static uintptr_t fn = Fn("ProjectX.GFxModal_X.Close");
    if (fn) {
        alignas(16) uint8_t buf[0x100] = {};
        CallFunction(g_ourModal, fn, buf);
    }
    const bool still = ShellListsModal() && TopModal() == g_ourModal;
    g_ghost = still;
    LogF("[menu] popup %s was left open: closed%s", FullNameOf(g_ourModal).c_str(), still ? " (still listed, ignored)" : "");
}

bool CanShowNativeMessage() { return !g_messagePending && NativeMessageReady() && RefreshShell() && !ShellModalActive(); }

static std::string TypeOf(uintptr_t o) { return o ? Narrow(GfxInvoke(o, "toString", {}).S) : ""; }

static const char* const kModsName = "rocketify_mods";

static void PollTopMenu(ULONGLONG now);
static bool OnRootMenu();
static void WatchPanels(ULONGLONG now);
static void CheckChallengesKey();
static void CheckFriendsClick();
static void ReleaseGates(ULONGLONG now);
static volatile LONG64 g_yDownAt = 0;  // last time Y was seen down
static bool g_panelsDirty = true;  // a panel may have opened or closed
static volatile LONG g_rbPressed = 0;
static volatile LONG g_mousePressed = 0, g_mouseReleased = 0, g_rtReleased = 0;
static int32_t g_selectName = 0;  // name index of the "Select" sound
static ULONGLONG g_menuShownAt = 0;
static bool g_findStack = false;  // the menu was recreated: look for the newest stack

struct Injector {
    bool active = false;
    uintptr_t movie = 0;
    ULONGLONG last = 0;
    ULONGLONG notBefore = 0;  // pause until this tick (loading / retry back-off)
    ULONGLONG lastFocus = 0;  // last focus sample
    ULONGLONG nextFind = 0;   // earliest tick of the next (slow) movie search
    std::wstring ini;
    // discovered instance names (screen / root menu / list / quit)
    std::string names[4];
    int level = 0;     // which name is being searched
    int scanN = 0;     // probes done for this level (centre-out around scanStart)
    int scanStart = 0; // instance number the search is centred on
    bool reported = false;
    ULONGLONG urgentUntil = 0;  // main menu just appeared: find + insert immediately
};
static Injector g_inj;

static std::string g_badList;  // looked like the menu's list, but has no entries
static std::string g_lastGood[4];  // names of the last complete find in this movie

static std::string LevelPath(int level) {
    std::string p = "root.screenContainer";
    for (int i = 0; i < level; ++i) p += "." + g_inj.names[i];
    return p;
}

static bool LevelMatches(int level, const std::string& path) {
    uintptr_t o = GfxVar(g_inj.movie, path);
    if (!o) return false;
    switch (level) {
        case 0: return TypeOf(o).rfind("[MainMenuScreen", 0) == 0;
        case 1: return TypeOf(o).rfind("[RootMenuMovie", 0) == 0;
        case 2:
            if (GfxVar(g_inj.movie, path + ".RootMenuMovie_playButton")) return true;
            return path.substr(path.rfind('.') + 1) != g_badList && GfxGetInt(o, "numChildren") >= 6;
        default: return TypeOf(o).rfind("[CLIK Button", 0) == 0 && path.find(kModsName) == std::string::npos;
    }
}

static void SaveInjectorNames() {
    static const wchar_t* const keys[] = {L"Screen", L"RootMenu", L"List", L"Quit"};
    for (int i = 0; i < 4; ++i) {
        std::wstring v(g_inj.names[i].begin(), g_inj.names[i].end());
        WritePrivateProfileStringW(L"NativeMenu", keys[i], v.c_str(), g_inj.ini.c_str());
    }
}

static void RememberNames() {
    for (int i = 0; i < 4; ++i) g_lastGood[i] = g_inj.names[i];
    g_badList.clear();
    SaveInjectorNames();
}

static void LoadInjectorNames() {
    static const wchar_t* const keys[] = {L"Screen", L"RootMenu", L"List", L"Quit"};
    for (int i = 0; i < 4; ++i) {
        wchar_t buf[128] = {};
        GetPrivateProfileStringW(L"NativeMenu", keys[i], L"", buf, 128, g_inj.ini.c_str());
        g_inj.names[i].clear();
        for (wchar_t* c = buf; *c; ++c) g_inj.names[i] += static_cast<char>(*c);
    }
}

// Verifies cached names level by level; returns the first level that is wrong (4 = all good).
static int VerifyLevels() {
    for (int l = 0; l < 4; ++l) {
        if (g_inj.names[l].empty() || !LevelMatches(l, LevelPath(l) + "." + g_inj.names[l])) return l;
    }
    return 4;
}

static std::string TypeOf(uintptr_t o);
static int InstanceNum(const std::string& name);
static std::string LowestButtonNear(const std::string& list, int centre);

static void BeginLevel(int level);

// The menu's list is known, its entries may have new numbers: look for them from `from` on.
static void SearchEntries(const std::string& screen, const std::string& root, const std::string& list, int from) {
    if (g_inj.level == 3 && g_inj.names[0] == screen && g_inj.names[1] == root && g_inj.names[2] == list) return;
    g_inj.names[0] = screen;
    g_inj.names[1] = root;
    g_inj.names[2] = list;
    g_inj.names[3].clear();
    BeginLevel(3);
    if (from >= 0) g_inj.scanStart = from;
}

// Whatever main menu entry has the focus leads straight to the list.
static bool LearnFromFocus() {
    uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
    uintptr_t btn = stage ? GfxGetObject(stage, "focus") : 0;
    if (!btn || !IsObject(btn)) return false;
    const std::string name = GfxGetString(btn, "name");
    const bool named = name.rfind("RootMenuMovie_", 0) == 0;
    if (!named && name.rfind("instance", 0) != 0 && name.rfind("rocketify_", 0) != 0) return false;
    uintptr_t list = GfxGetObject(btn, "parent");
    uintptr_t root = list && IsObject(list) ? GfxGetObject(list, "parent") : 0;
    uintptr_t screen = root && IsObject(root) ? GfxGetObject(root, "parent") : 0;
    if (!screen || !IsObject(screen) || (!named && TypeOf(root).rfind("[RootMenuMovie", 0) != 0)) return false;
    const std::string nScreen = GfxGetString(screen, "name"), nRoot = GfxGetString(root, "name"),
                      nList = GfxGetString(list, "name");
    if (nScreen.empty() || nRoot.empty() || nList.empty()) return false;
    const std::string path = "root.screenContainer." + nScreen + "." + nRoot + "." + nList;
    const int own = InstanceNum(name), last = InstanceNum(g_lastGood[3]), base = InstanceNum(nList);
    std::string quit = LowestButtonNear(path, own >= 0 ? own : base);
    if (quit.empty() && last >= 0) quit = LowestButtonNear(path, last);
    if (quit.empty()) {
        SearchEntries(nScreen, nRoot, nList, last >= 0 ? last + 1 : base + 1);
        return false;
    }
    g_inj.names[0] = nScreen;
    g_inj.names[1] = nRoot;
    g_inj.names[2] = nList;
    g_inj.names[3] = quit;
    return true;
}

static std::string g_want;  // entry the last up/down press must end on
static ULONGLONG g_wantUntil = 0;
// A step the game makes itself: checked a moment later, corrected only if it went wrong.
static std::string g_checkFrom, g_checkWant;
static ULONGLONG g_checkAt = 0;

static std::string g_modsName = kModsName;  // instance name of the current MODS entry
static std::string g_modsQuit;              // QUIT's path when that entry was created
static int g_modsRetired = 0, g_modsSerial = 0;

// Main menu entries whose action a mod replaces. The original is hidden and a look-alike,
// which has no click handler of the game, takes its place.
struct Override {
    std::string event, orig, clone, quit;
    bool failed = false;
    int waited = 0;  // checks spent waiting for the label
};
static std::vector<Override> g_overrides;
static int g_clones = 0, g_cloneSerial = 0;  // look-alikes in the current list (also retired ones)

static void ResetOverride(Override& o) {
    if (o.event == "onChallenges") return;  // not part of the list; checked on its own
    const std::string event = o.event;
    o = Override();
    o.event = event;
}

static Override* FindOverride(const std::string& event) {
    for (Override& o : g_overrides)
        if (o.event == event) return &o;
    return nullptr;
}

void AddOverrides(const char* list) {
    const std::string s = list, place = "MainMenu.";
    for (size_t i = 0; i < s.size();) {
        size_t end = s.find(';', i);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(i, end - i);
        if (item.rfind(place, 0) == 0 && !FindOverride(item.substr(place.size()))) {
            Override o;
            o.event = item.substr(place.size());
            g_overrides.push_back(o);
        }
        i = end + 1;
    }
}

static void GfxSetObject(uintptr_t obj, const char* member, uintptr_t val);

static void GfxSetBool(uintptr_t obj, const char* member, bool v) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.SetBool");
    if (!fn || !obj) return;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    const uint32_t b = v ? 1 : 0;
    memcpy(buf + 0x10, &b, 4);
    CallFunction(obj, fn, buf);
}

static void GfxSetFalse(uintptr_t obj, const char* member) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.SetBool");
    if (!fn || !obj) return;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);  // the value after it is left at false
    CallFunction(obj, fn, buf);
}

static std::string FindEntry(const std::string& event);

static void EnsureButton() {
    const std::string list = LevelPath(3);
    const std::string quitPath = list + "." + g_inj.names[3];
    uintptr_t listObj = GfxVar(g_inj.movie, list);
    uintptr_t quit = GfxVar(g_inj.movie, quitPath);
    if (!listObj || !quit) return;
    bool refocus = false;
    if (uintptr_t old = GfxVar(g_inj.movie, list + "." + g_modsName)) {
        if (g_modsQuit == quitPath) return;  // up to date
        // The game rebuilt its entries (e.g. after the garage). An entry kept from before
        // loses its controller icon, so retire it and build a fresh one.
        uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
        uintptr_t f = stage ? GfxGetObject(stage, "focus") : 0;
        refocus = f && IsObject(f) && GfxGetString(f, "name") == g_modsName;
        GfxSetFalse(old, "visible");
        GfxSetFalse(old, "enabled");
        GfxSetFalse(old, "focusable");
        GfxSetFloat(old, "y", 0.0f);
        ++g_modsRetired;
        g_modsName = std::string(kModsName) + std::to_string(++g_modsSerial);
    } else {
        g_modsRetired = 0;  // new list: earlier entries went away with the old one
        g_clones = 0;
        for (Override& o : g_overrides) ResetOverride(o);
        g_modsName = kModsName;
    }

    // Same Flash class as QUIT: "[class Foo]" -> "Foo"
    std::string cls = TypeOf(GfxGetObject(quit, "constructor"));
    if (cls.rfind("[class ", 0) == 0 && cls.back() == ']') cls = cls.substr(7, cls.size() - 8);
    const int depth = GfxGetInt(listObj, "numChildren");
    const int kids = depth - g_modsRetired - g_clones;
    uintptr_t btn = 0;
    std::string used;
    for (const std::string& sym : {cls, std::string("NewBasicMenuButton"), std::string("BasicMenuButton")}) {
        if (sym.empty() || sym[0] == '[') continue;
        if ((btn = GfxAttach(listObj, sym, g_modsName, depth))) { used = sym; break; }
    }
    if (!btn) {
        LogF("[menu] could not create a menu button (QUIT class '%s')", cls.c_str());
        return;
    }
    const float qx = GfxGetFloat(quit, "x"), qy = GfxGetFloat(quit, "y");
    float step = kids > 1 ? qy / static_cast<float>(kids - 1) : 35.0f;
    const std::string above = FindEntry("onSettings");
    if (uintptr_t o = above.empty() ? 0 : GfxVar(g_inj.movie, list + "." + above)) {
        const float gap = qy - GfxGetFloat(o, "y");
        if (gap > 20.0f && gap < 100.0f) step = gap;
    }
    GfxSetFloat(btn, "x", qx);
    GfxSetFloat(btn, "y", qy + (step > 1.0f ? step : 35.0f));
    GfxSetString(btn, "label", L"MODS");
    if (uintptr_t tl = GfxVar(g_inj.movie, list + "." + g_modsName + ".textLabel")) GfxSetString(tl, "text", L"MODS");
    // Subtitle shown when the entry is focused, like QUIT's "EXIT ROCKET LEAGUE®".
    GfxSetString(btn, "descLabel", L"MANAGE YOUR MODS");
    if (uintptr_t dl = GfxVar(g_inj.movie, list + "." + g_modsName + ".descTextLabel")) GfxSetString(dl, "text", L"MANAGE YOUR MODS");
    LogF("[menu] MODS entry added to %s (class %s, y=%.1f)", list.c_str(), used.c_str(), qy + step);
    g_modsQuit = quitPath;
    if (refocus)
        if (uintptr_t stage = GfxVar(g_inj.movie, "root.stage")) GfxSetObject(stage, "focus", btn);
}



static const struct { const char *entry, *event; } kEntries[] = {
    {"RootMenuMovie_playButton", "onPlay"},             {"RootMenuMovie_itemShopButton", "onItemShop"},
    {"RootMenuMovie_rocketPassButton", "onRocketPass"}, {"RootMenuMovie_garageButton", "onGarage"},
    {"RootMenuMovie_clubButton", "onClub"},             {"RootMenuMovie_profileButton", "onProfile"},
};

static std::wstring GfxGetWide(uintptr_t obj, const char* member) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.GetString");
    if (!fn || !obj) return L"";
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    CallFunction(obj, fn, buf);
    FString r;
    memcpy(&r, buf + 0x10, sizeof(r));
    if (!r.data || r.num <= 1 || r.num > 1000) return L"";
    return std::wstring(r.data, static_cast<size_t>(r.num) - 1);
}

// Instance name of the game's own entry for an event, "" if there is none.
static std::string FindEntry(const std::string& event) {
    const std::string list = LevelPath(3) + ".";
    for (auto& k : kEntries)
        if (event == k.event) return GfxVar(g_inj.movie, list + k.entry) ? k.entry : "";
    if (event == "onQuit") return g_inj.names[3];
    if (event != "onSettings") return "";
    // SETTINGS: the lowest unnamed entry above QUIT
    uintptr_t quit = GfxVar(g_inj.movie, list + g_inj.names[3]);
    if (!quit) return "";
    const float quitY = GfxGetFloat(quit, "y");
    std::string best;
    float bestY = -1e9f;
    const int quitNum = InstanceNum(g_inj.names[3]);  // SETTINGS is numbered right next to it
    for (int n = quitNum - 12; n <= quitNum + 6; ++n) {
        if (n < 0) continue;
        const std::string name = "instance" + std::to_string(n);
        uintptr_t o = GfxVar(g_inj.movie, list + name);
        if (!o || TypeOf(o).rfind("[CLIK Button", 0) != 0) continue;
        const float y = GfxGetFloat(o, "y");
        if (y < quitY && y > bestY) { bestY = y; best = name; }
    }
    return best;
}

static bool GfxGetBool(uintptr_t obj, const char* member) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.GetBool");
    static const int off = fn ? ParamOffset(fn, "ReturnValue") : -1;
    if (off < 0 || off > 0x80 || !obj) return false;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x100] = {};
    PutStr(buf, 0x00, m);
    CallFunction(obj, fn, buf);
    uint32_t r = 0;
    memcpy(&r, buf + off, 4);
    return r != 0;
}

// Calls obj.member("text").
static void GfxInvokeString(uintptr_t obj, const char* member, const std::wstring& text) {
    static const uint8_t stringType = GfxInvoke(obj, "toString", {}).Type;  // the engine's own tag for strings
    ASValue v{};
    v.Type = stringType;
    v.S = FString{text.c_str(), static_cast<int32_t>(text.size() + 1), static_cast<int32_t>(text.size() + 1)};
    GfxInvoke(obj, member, {v});
}

// VIEW CHALLENGES: the whole widget opens the screen when clicked, and the game's own button
// does for R1 while it is visible. Both are switched off and a look-alike lies over the button.
static void EnsureChallenges(Override& o) {
    const std::string sum = LevelPath(1) + ".challengesSummaryView", view = sum + ".activeChallengesView";
    uintptr_t sumObj = GfxVar(g_inj.movie, sum), viewObj = sumObj ? GfxVar(g_inj.movie, view) : 0,
              btn = viewObj ? GfxVar(g_inj.movie, view + ".viewChallengesButton") : 0;
    if (!btn) return;
    uintptr_t clone = o.clone.empty() ? 0 : GfxVar(g_inj.movie, sum + "." + o.clone);
    const bool created = !clone;
    if (!clone) {
        if (o.failed) return;
        // Wait until the game has built the widget: only then is its button in place.
        std::wstring label = GfxGetWide(btn, "label");
        // With a controller in use, the button's shortcut ("GP_RShoulder ") is part of the text.
        if (label.rfind(L"GP_", 0) == 0) {
            const size_t space = label.find(L' ');
            label = space == std::wstring::npos ? L"" : label.substr(space + 1);
        }
        if (label.empty() || !GfxGetBool(btn, "visible")) return;
        std::string cls = TypeOf(GfxGetObject(btn, "constructor"));
        if (cls.rfind("[class ", 0) == 0 && cls.back() == ']') cls = cls.substr(7, cls.size() - 8);
        const std::string name = "rocketify_onChallenges" + std::to_string(++g_cloneSerial);
        if (!cls.empty() && cls[0] != '[') clone = GfxAttach(sumObj, cls, name, GfxGetInt(sumObj, "numChildren"));
        if (!clone) {
            o.failed = true;
            LogF("[menu] could not replace onChallenges (class '%s')", cls.c_str());
            return;
        }
        o.clone = name;
        GfxInvokeString(clone, "setButtonMarkup", L"GP_RShoulder");
        GfxSetString(clone, "label", label);
        GfxSetFalse(clone, "focusable");
        GfxSetFalse(viewObj, "mouseEnabled");
        GfxSetFalse(viewObj, "mouseChildren");
        LogF("[menu] onChallenges replaced (class %s)", cls.c_str());
    }
    if (GfxGetBool(btn, "visible")) GfxSetFalse(btn, "visible");  // the game shows it on every update
    const float vx = GfxGetFloat(viewObj, "scaleX"), vy = GfxGetFloat(viewObj, "scaleY");
    if (created) {
        GfxSetFloat(clone, "scaleX", vx * GfxGetFloat(btn, "scaleX"));
        GfxSetFloat(clone, "scaleY", vy * GfxGetFloat(btn, "scaleY"));
        LogF("[menu] onChallenges size: game %.0fx%.0f (scale %.2f,%.2f), ours %.0fx%.0f", GfxGetFloat(btn, "width"),
             GfxGetFloat(btn, "height"), GfxGetFloat(btn, "scaleX"), GfxGetFloat(btn, "scaleY"),
             GfxGetFloat(clone, "width"), GfxGetFloat(clone, "height"));
    }
    GfxSetFloat(clone, "x", GfxGetFloat(viewObj, "x") + vx * GfxGetFloat(btn, "x"));
    GfxSetFloat(clone, "y", GfxGetFloat(viewObj, "y") + vy * GfxGetFloat(btn, "y"));
}

// Friends button: without its click handler it still looks and sounds the same.
static bool g_friendsOff = false;
static void EnsureFriends() {
    uintptr_t bar = GfxVar(g_inj.movie, "root.taskBar");
    uintptr_t btn = bar ? GfxVar(g_inj.movie, "root.taskBar.friendsButton") : 0;
    if (!btn) return;
    if (!g_friendsOff) {
        if (!GfxGetBool(btn, "visible")) return;  // not available (yet)
        GfxInvoke(bar, "disableFriendsButton", {});
        g_friendsOff = true;
        LogF("[menu] onFriendsLists replaced");
    }
    if (!GfxGetBool(btn, "visible")) GfxSetBool(btn, "visible", true);
}

// Outside the main menu the friends button works normally.
static void RestoreFriends() {
    if (!g_friendsOff) return;
    g_friendsOff = false;
    if (uintptr_t bar = GfxVar(g_inj.movie, "root.taskBar")) GfxInvoke(bar, "enableFriendsButton", {});
}

static Override* FindOverride(const std::string& event);

static void EnsureWidgets(ULONGLONG now) {
    static ULONGLONG last = 0;
    if (now - last < (now < g_inj.urgentUntil + 6000 ? 100u : 250u)) return;
    last = now;
    if (Override* o = FindOverride("onChallenges")) EnsureChallenges(*o);
    if (FindOverride("onFriendsLists")) EnsureFriends();
}

static void EnsureOverrides() {
    const std::string list = LevelPath(3);
    uintptr_t listObj = GfxVar(g_inj.movie, list);
    const std::string quitPath = list + "." + g_inj.names[3];
    if (!listObj) return;
    for (Override& o : g_overrides) {
        if (o.event == "onMods" || o.event == "onNews" || o.event == "onChallenges" || o.event == "onFriendsLists")
            continue;  // not entries of the menu list
        uintptr_t clone = o.clone.empty() ? 0 : GfxVar(g_inj.movie, list + "." + o.clone);
        if (clone && o.quit != quitPath) {  // the game rebuilt its entries: start over
            GfxSetFalse(clone, "visible");
            GfxSetFalse(clone, "enabled");
            GfxSetFalse(clone, "focusable");
            GfxSetFloat(clone, "y", 0.0f);
            clone = 0;
        }
        if (o.quit != quitPath) {
            o.orig.clear();
            o.clone.clear();
            o.failed = false;
            o.waited = 0;
        }
        if (o.failed) continue;
        if (!clone) {
            o.clone.clear();
            o.quit = quitPath;
            o.orig = FindEntry(o.event);
        }
        const bool fresh = !clone;
        uintptr_t orig = o.orig.empty() ? 0 : GfxVar(g_inj.movie, list + "." + o.orig);
        if (!orig) { if (!clone) o.quit.clear(); continue; }  // not there (yet)
        if (!clone) {
            std::string cls = TypeOf(GfxGetObject(orig, "constructor"));
            if (cls.rfind("[class ", 0) == 0 && cls.back() == ']') cls = cls.substr(7, cls.size() - 8);
            const std::string name = "rocketify_" + o.event + std::to_string(++g_cloneSerial);
            const int depth = GfxGetInt(listObj, "numChildren");
            const std::string from = list + "." + o.orig, to = list + "." + name;
            std::wstring label = GfxGetWide(orig, "label"), desc = GfxGetWide(orig, "descLabel");
            uintptr_t t = GfxVar(g_inj.movie, from + ".textLabel");
            if (label.empty() && t) label = GfxGetWide(t, "text");
            t = GfxVar(g_inj.movie, from + ".descTextLabel");
            if (desc.empty() && t) desc = GfxGetWide(t, "text");
            if (label.empty() && ++o.waited < 20) {  // the game has not filled it in yet
                o.quit.clear();
                continue;
            }
            // Same look as the original; entries with a special class may only work as QUIT's.
            uintptr_t quitObj = GfxVar(g_inj.movie, quitPath);
            std::string quitCls = quitObj ? TypeOf(GfxGetObject(quitObj, "constructor")) : "";
            if (quitCls.rfind("[class ", 0) == 0 && quitCls.back() == ']') quitCls = quitCls.substr(7, quitCls.size() - 8);
            for (const std::string& c : {cls, quitCls}) {
                if (c.empty() || c[0] == '[') continue;
                if ((clone = GfxAttach(listObj, c, name, depth))) { cls = c; break; }
            }
            if (!clone) {
                LogF("[menu] could not replace %s (class '%s')", o.event.c_str(), cls.c_str());
                o.failed = true;
                continue;
            }
            ++g_clones;
            o.clone = name;
            GfxSetString(clone, "label", label);
            if ((t = GfxVar(g_inj.movie, to + ".textLabel"))) GfxSetString(t, "text", label);
            GfxSetString(clone, "descLabel", desc);
            if ((t = GfxVar(g_inj.movie, to + ".descTextLabel"))) GfxSetString(t, "text", desc);
            LogF("[menu] %s replaced: %s -> %s (class %s, label %s)", o.event.c_str(), o.orig.c_str(), name.c_str(),
                 cls.c_str(), label.empty() ? "EMPTY" : "ok");
        }
        if (fresh) {
            GfxSetFloat(clone, "x", GfxGetFloat(orig, "x"));
            GfxSetFloat(clone, "y", GfxGetFloat(orig, "y"));
        }
        if (fresh || o.event == "onClub") GfxSetFalse(orig, "visible");  // the game shows CLUB again at times
    }
}

static void EnsureEntries() {
    EnsureButton();
    EnsureOverrides();
}

// ---- focus tracking (for controller / keyboard navigation) ----
constexpr int kFocusSamples = 256;
struct FocusSample {
    ULONGLONG t;
    std::string name;
};
static FocusSample g_focus[kFocusSamples];
static int g_focusHead = 0;

static void GfxSetObject(uintptr_t obj, const char* member, uintptr_t val) {
    static uintptr_t fn = Fn("GFxUI.GFxObject.SetObject");
    if (!fn || !obj) return;
    std::wstring m(member, member + strlen(member));
    alignas(16) uint8_t buf[0x40] = {};
    PutStr(buf, 0x00, m);
    memcpy(buf + 0x10, &val, 8);
    CallFunction(obj, fn, buf);
}

static void PushFocus(ULONGLONG t, const std::string& name) {
    const FocusSample& last = g_focus[(g_focusHead + kFocusSamples - 1) % kFocusSamples];
    static int logged = 0;
    if (name != last.name && logged < 400) {
        ++logged;
        LogF("[menu] focus -> '%s'", name.c_str());
    }
    g_focus[g_focusHead] = {t, name};
    g_focusHead = (g_focusHead + 1) % kFocusSamples;
}

// Which main menu entry is focused, as an event name; nullptr if the focus is elsewhere.
static const char* g_focusEvent = nullptr;

static const char* EntryEvent(uintptr_t f, const std::string& name) {
    for (const Override& o : g_overrides)
        if (!o.clone.empty() && name == o.clone) return o.event.c_str();
    for (auto& k : kEntries)
        if (name == k.entry) return k.event;
    if (name.rfind("instance", 0) != 0) return nullptr;
    // SETTINGS and QUIT have no names of their own. Other screens use instance names too,
    // so make sure this one really is in the main menu's list.
    uintptr_t list = GfxGetObject(f, "parent");
    uintptr_t root = list && IsObject(list) ? GfxGetObject(list, "parent") : 0;
    if (!root || !IsObject(root) || GfxGetString(list, "name") != g_inj.names[2] ||
        GfxGetString(root, "name") != g_inj.names[1])
        return nullptr;
    if (name == g_inj.names[3]) return "onQuit";
    uintptr_t quit = GfxVar(g_inj.movie, LevelPath(3) + "." + g_inj.names[3]);
    return quit && GfxGetFloat(f, "y") < GfxGetFloat(quit, "y") ? "onSettings" : nullptr;
}

static void SampleFocus(ULONGLONG now) {
    uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
    uintptr_t f = stage ? GfxGetObject(stage, "focus") : 0;
    const bool live = f && IsObject(f);
    static uintptr_t lastObj = 0;
    static std::string lastName, lastQuit;
    std::string focusName = live ? GfxGetString(f, "name") : "";
    if (live)
        for (const Override& o : g_overrides) {  // keyboard / controller reached a hidden original
            if (o.clone.empty() || focusName != o.orig) continue;
            uintptr_t clone = GfxVar(g_inj.movie, LevelPath(3) + "." + o.clone);
            uintptr_t parent = GfxGetObject(f, "parent");
            if (!clone || !parent || !IsObject(parent) || GfxGetString(parent, "name") != g_inj.names[2]) continue;
            GfxSetObject(stage, "focus", clone);
            f = clone;
            focusName = o.clone;
            break;
        }
    if (live && now < g_wantUntil && focusName != g_want &&
        (focusName == g_modsName || EntryEvent(f, focusName)) && !ShellModalActive()) {
        if (uintptr_t target = GfxVar(g_inj.movie, LevelPath(3) + "." + g_want)) {
            GfxSetObject(stage, "focus", target);
            f = target;
            focusName = g_want;
        }
    }
    if (g_checkAt && now >= g_checkAt) {
        g_checkAt = 0;
        if (live && focusName != g_checkWant && (focusName == g_modsName || EntryEvent(f, focusName)) &&
            !ShellModalActive()) {
            LogF("[menu] step from '%s': game went to '%s', corrected to '%s'", g_checkFrom.c_str(), focusName.c_str(),
                 g_checkWant.c_str());
            if (uintptr_t target = GfxVar(g_inj.movie, LevelPath(3) + "." + g_checkWant)) {
                GfxSetObject(stage, "focus", target);
                f = target;
                focusName = g_checkWant;
            }
        }
    }
    static ULONGLONG menuSince = 0;
    if (g_focusEvent || focusName == g_modsName) {
        if (!menuSince) menuSince = now;
        if (g_ourModal && now - menuSince > 300 && now - g_ourModalAt > 800) {
            if (!ShellListsModal()) g_ourModal = 0;  // closed normally
            else DropGhostModal();
        }
    } else {
        menuSince = 0;
    }
    if (f != lastObj || focusName != lastName || lastQuit != g_inj.names[3]) {
        lastQuit = g_inj.names[3];
        lastObj = f;
        lastName = focusName;
        g_focusEvent = live ? EntryEvent(f, focusName) : nullptr;
    }
    PushFocus(now, focusName);
}

// Focus as it was just before an input edge at time t.
static std::string FocusBefore(ULONGLONG t) {
    std::string best;
    ULONGLONG bestT = 0;
    for (const FocusSample& f : g_focus)
        if (f.t && f.t <= t && f.t >= bestT) { bestT = f.t; best = f.name; }
    if (bestT == 0) best = g_focus[(g_focusHead + kFocusSamples - 1) % kFocusSamples].name;
    return best;
}

struct NavTask {
    int kind;  // 1 up, 2 down, 3 accept
    ULONGLONG t;
};

// Up/down between QUIT and MODS (the game's own focus list does not include MODS).
static void NavTaskFn(void* ctx) {
    auto* n = static_cast<NavTask*>(ctx);
    const int kind = n->kind;
    const ULONGLONG t = n->t;
    delete n;
    if (g_off || !g_inj.active || g_inj.level < 4 || !OnRootMenu() || ShellModalActive()) return;
    const std::string list = LevelPath(3);
    // The game may react to the same press before our input thread sees it (a stick even more
    // so): judge by the focus a little earlier, or the press would move two entries.
    const std::string before = FocusBefore(t - 60);
    g_wantUntil = 0;
    g_checkAt = 0;
    static int logged = 0;
    if (++logged <= 300) LogF("[menu] %s pressed on '%s'", kind == 1 ? "up" : "down", before.c_str());
    uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
    if (!stage) return;
    // The entries from top to bottom: the game's instance name, and the one to focus.
    std::vector<std::pair<std::string, std::string>> order;
    if (!g_overrides.empty()) {
        for (const char* e : {"onPlay", "onItemShop", "onRocketPass", "onGarage", "onClub", "onProfile", "onSettings"}) {
            const Override* o = FindOverride(e);
            const bool replaced = o && !o->clone.empty();
            const std::string orig = replaced ? o->orig : FindEntry(e);
            if (!orig.empty()) order.push_back({orig, replaced ? o->clone : orig});
        }
    }
    const Override* q = FindOverride("onQuit");
    order.push_back({g_inj.names[3], q && !q->clone.empty() ? q->clone : g_inj.names[3]});
    order.push_back({"", g_modsName});
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        if (before.empty() || (before != order[i].first && before != order[i].second)) continue;
        int to = kind == 1 ? i - 1 : i + 1;
        if (to >= static_cast<int>(order.size())) to = i;  // MODS is the last one
        if (to < 0) return;
        // Only step in where one of our own entries is involved; the game does the rest.
        if (order[i].first == order[i].second && order[to].first == order[to].second) return;
        // The game finds its way between its entries and up from MODS; only MODS itself it may
        // not reach. Moving first would make it move once more from there.
        if (order[to].second != g_modsName) {
            g_checkFrom = before;
            g_checkWant = order[to].second;
            g_checkAt = GetTickCount64() + 60;
            return;
        }
        if (uintptr_t target = GfxVar(g_inj.movie, list + "." + order[to].second)) {
            GfxSetObject(stage, "focus", target);
            PushFocus(GetTickCount64(), order[to].second);
            g_want = order[to].second;  // and keep it there if the game reacts later than we did
            g_wantUntil = GetTickCount64() + 150;
        }
        return;
    }
}


void NativeMenuNav(int kind, unsigned long long tick) {
    if (kind == 8) {
        InterlockedExchange(&g_rtReleased, 1);
        return;
    }
    if (kind == 6 || kind == 7) {
        InterlockedExchange(kind == 6 ? &g_mousePressed : &g_mouseReleased, 1);
        return;
    }
    if (kind == 5) {
        InterlockedExchange64(&g_yDownAt, static_cast<LONG64>(tick));
        return;
    }
    if (kind == 4) {
        InterlockedExchange(&g_rbPressed, 1);
        return;
    }
    RunOnGameThread(NavTaskFn, new NavTask{kind, tick});
}

// The last entry (QUIT) is the button with the largest y among the list's buttons.
static std::string LowestButtonNear(const std::string& list, int centre) {
    std::string best;
    float bestY = -1e9f;
    for (int n = centre - 12; n <= centre + 40; ++n) {
        if (n < 0) continue;
        const std::string name = "instance" + std::to_string(n);
        uintptr_t o = GfxVar(g_inj.movie, list + "." + name);
        if (!o || TypeOf(o).rfind("[CLIK Button", 0) != 0) continue;
        const float y = GfxGetFloat(o, "y");
        if (y > bestY) { bestY = y; best = name; }
    }
    return best;
}

static int InstanceNum(const std::string& name) {
    return name.rfind("instance", 0) == 0 ? atoi(name.c_str() + 8) : -1;
}

// Children are numbered right after their parent, so search outward from the
// parent's number (start, start+1, start-1, start+2, ...).
static void BeginLevel(int level) {
    g_inj.level = level;
    g_inj.scanN = 0;
    const int parentNum = level > 0 ? InstanceNum(g_inj.names[level - 1]) : -1;
    g_inj.scanStart = parentNum >= 0 ? parentNum + 1 : 0;
}

static int ProbeNumber(int k) {
    const int start = g_inj.scanStart;
    if (start == 0) return k;
    const int off = (k + 1) / 2;
    return (k % 2) ? start + off : start - off;  // k=0 -> start
}

static void TapObject(uintptr_t obj);

// Switches to a (new) UI movie: everything learned from the old one is void.
static void UseMovie(uintptr_t movie) {
    g_inj.movie = movie;
    for (auto& n : g_inj.names) n.clear();
    for (auto& f : g_focus) f = FocusSample{};
    g_focusEvent = nullptr;
    g_inj.reported = false;
    g_modsName = kModsName;
    g_modsQuit.clear();
    g_badList.clear();
    for (auto& n : g_lastGood) n.clear();
    g_want.clear();
    g_wantUntil = g_checkAt = 0;
    g_modsRetired = 0;
    g_clones = 0;
    for (Override& o : g_overrides) ResetOverride(o);
    BeginLevel(0);
    g_findStack = true;
    g_friendsOff = false;
    for (Override& o : g_overrides)
        if (o.event == "onChallenges") { o.clone.clear(); o.failed = false; }
    if (!movie) return;
    LogF("[menu] using UI movie %s", FullNameOf(movie).c_str());
    TapObject(movie);
    const uintptr_t before = g_shell;
    g_shell = 0;  // the old shell may still exist: always look for the newest
    if (!RefreshShell()) LogF("[menu] no GFxShell_TA yet");
    else LogF("[menu] shell %s (%s)", FullNameOf(g_shell).c_str(), g_shell == before ? "same" : "new");
}

// Leaving the menu for a match destroys its UI movie; coming back creates a new one.
static bool MovieReady(ULONGLONG now) {
    if (IsObject(g_inj.movie) && ClassNameOf(g_inj.movie) == "GFxMoviePlayer_X") return true;
    if (g_inj.movie) {
        LogF("[menu] UI movie is gone (map change)");
        UseMovie(0);
    }
    if (now < g_inj.nextFind) return false;
    g_inj.nextFind = now + 1000;
    if (uintptr_t m = FindShellMovie()) UseMovie(m);
    return g_inj.movie != 0;
}

// Called from the UI event hook when the main menu screen becomes active.
static void OnMainMenuShown(uintptr_t movie) {
    if (g_inj.active && movie != g_inj.movie && ClassNameOf(movie) == "GFxMoviePlayer_X") UseMovie(movie);
    if (GetTickCount64() >= g_inj.urgentUntil) LogF("[menu] main menu shown");
    g_inj.urgentUntil = GetTickCount64() + 4000;
    g_inj.notBefore = 0;
    g_inj.last = 0;
}

// An entry numbered just after `name` that lies below it, "" if there is none.
static std::string LowestEntryAfter(const std::string& name) {
    const std::string list = LevelPath(3) + ".";
    uintptr_t cur = GfxVar(g_inj.movie, list + name);
    const int num = InstanceNum(name);
    if (!cur || num < 0) return "";
    float bestY = GfxGetFloat(cur, "y");
    std::string best;
    for (int n = num + 1; n <= num + 4; ++n) {
        const std::string cand = "instance" + std::to_string(n);
        uintptr_t o = GfxVar(g_inj.movie, list + cand);
        if (!o || TypeOf(o).rfind("[CLIK Button", 0) != 0) continue;
        const float y = GfxGetFloat(o, "y");
        if (y > bestY + 1.0f) { bestY = y; best = cand; }
    }
    return best;
}

static void InjectorSlice(void*) {
    Injector& j = g_inj;
    if (!j.active) return;
    const ULONGLONG now = GetTickCount64();
    PollTopMenu(now);
    if (!MovieReady(now)) {
        RunOnGameThread(InjectorSlice, nullptr);
        return;
    }
    if (!OnRootMenu()) {  // another screen or a match: what we know stays valid until we are back
        if (g_focusEvent || !g_focus[(g_focusHead + kFocusSamples - 1) % kFocusSamples].name.empty()) {
            g_focusEvent = nullptr;
            g_wantUntil = g_checkAt = 0;
            PushFocus(now, "");
        }
        InterlockedExchange(&g_rbPressed, 0);
        InterlockedExchange(&g_rtReleased, 0);
        RestoreFriends();
        ReleaseGates(now + 1000);
        RunOnGameThread(InjectorSlice, nullptr);
        return;
    }
    if (g_panelsDirty) {
        g_panelsDirty = false;
        WatchPanels(now);
    }
    CheckChallengesKey();
    CheckFriendsClick();
    ReleaseGates(now);
    if (j.level == 4) EnsureWidgets(now);
    const bool urgent = now < j.urgentUntil;
    const bool scanning = j.level < 4;
    const ULONGLONG sampleEvery = now < g_wantUntil || g_checkAt ? 12u : 30u;
    if (!scanning && now - j.lastFocus >= sampleEvery) {
        j.lastFocus = now;
        SampleFocus(now);
    }
    const ULONGLONG cadence = scanning ? (urgent ? 10u : 16u) : urgent ? 100u : 500u;
    if ((!urgent && now < j.notBefore) || now - j.last < cadence) {
        RunOnGameThread(InjectorSlice, nullptr);
        return;
    }
    j.last = now;

    if (!scanning) {
        // Steady state: verify the chain is still valid and the entry exists.
        const int bad = VerifyLevels();
        if (bad < 4) {
            for (int l = bad; l < 4; ++l) j.names[l].clear();
            BeginLevel(bad);
            if (bad == 3 && InstanceNum(g_lastGood[3]) >= 0) j.scanStart = InstanceNum(g_lastGood[3]);
        } else {
            // Found while the menu was still being built? Then QUIT is further down.
            const std::string lowest = LowestEntryAfter(j.names[3]);
            if (!lowest.empty()) {
                j.names[3] = lowest;
                RememberNames();
                LogF("[menu] QUIT is %s", lowest.c_str());
            }
            EnsureEntries();
        }
        RunOnGameThread(InjectorSlice, nullptr);
        return;
    }

    // Fast path: the focused PLAY entry leads straight to the list.
    if (LearnFromFocus() && VerifyLevels() == 4) {
        j.level = 4;
        RememberNames();
        LogF("[menu] menu list found from focus: %s (QUIT = %s)", LevelPath(3).c_str(), j.names[3].c_str());
        EnsureEntries();  // same frame
        RunOnGameThread(InjectorSlice, nullptr);
        return;
    }
    // The list itself usually survives; only its entries get new numbers.
    if (j.level < 3 && !g_lastGood[2].empty()) {
        const std::string p0 = "root.screenContainer." + g_lastGood[0], p1 = p0 + "." + g_lastGood[1];
        if (LevelMatches(0, p0) && LevelMatches(1, p1) && LevelMatches(2, p1 + "." + g_lastGood[2]))
            SearchEntries(g_lastGood[0], g_lastGood[1], g_lastGood[2], InstanceNum(g_lastGood[3]));
        else
            g_lastGood[2].clear();
    }
    const std::string parent = LevelPath(j.level);
    if (!GfxVar(j.movie, parent)) {  // not on the main menu right now
        BeginLevel(0);
        if (!urgent) j.notBefore = now + 1000;
        RunOnGameThread(InjectorSlice, nullptr);
        return;
    }
    // Time-boxed scan: 8 ms per frame right after the menu appears, 2 ms otherwise.
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const LONGLONG budget = freq.QuadPart / (urgent ? 125 : 500);
    const int kMaxProbes = j.level == 3 ? 2000 : 6000;
    for (; j.scanN < kMaxProbes; ++j.scanN) {
        const int num = ProbeNumber(j.scanN);
        if (num < 0) continue;
        const std::string name = "instance" + std::to_string(num);
        if (LevelMatches(j.level, parent + "." + name)) {
            j.names[j.level] = name;
            if (j.level == 3) {
                const std::string last = LowestButtonNear(parent, num);
                if (!last.empty()) j.names[3] = last;
            }
            BeginLevel(j.level + 1);
            if (j.level == 4) {
                RememberNames();
                LogF("[menu] menu list found: %s (QUIT = %s)", LevelPath(3).c_str(), j.names[3].c_str());
                EnsureEntries();  // insert in the same frame
            }
            break;
        }
        QueryPerformanceCounter(&t1);
        if (t1.QuadPart - t0.QuadPart > budget) { ++j.scanN; break; }
    }
    if (j.level < 4 && j.scanN >= kMaxProbes) {
        // Menu not built yet (still loading) or names differ: wait, then rescan from the top.
        if (!j.reported) LogF("[menu] main menu list not found yet; retrying");
        j.reported = true;
        if (j.level == 3) g_badList = j.names[2];
        g_lastGood[2].clear();
        for (auto& n : j.names) n.clear();
        BeginLevel(0);
        if (!urgent) j.notBefore = now + 2000;
    }
    RunOnGameThread(InjectorSlice, nullptr);
}

struct InjStart {
    std::wstring ini;
};

static void InjectorStart(void* ctx) {
    auto* t = static_cast<InjStart*>(ctx);
    g_inj = Injector{};
    g_inj.ini = t->ini;
    delete t;
    g_inj.movie = FindShellMovie();
    if (!g_inj.movie) LogF("[menu] no shell movie yet");  // MovieReady keeps looking
    LoadInjectorNames();
    const int valid = g_inj.movie ? VerifyLevels() : 0;
    for (int l = valid; l < 4; ++l) g_inj.names[l].clear();
    BeginLevel(valid);
    g_inj.active = true;
    LogF("[menu] injector started (%d cached levels valid)", g_inj.level);
    RunOnGameThread(InjectorSlice, nullptr);
}

void StartMenuInjector(const std::wstring& iniPath) {
    RunOnGameThread(InjectorStart, new InjStart{iniPath});
}

static bool MouseOverPath(const std::string& path) {
    uintptr_t btn = GfxVar(g_inj.movie, path);
    if (!btn) return false;
    const float mx = GfxGetFloat(btn, "mouseX"), my = GfxGetFloat(btn, "mouseY");
    float sx = GfxGetFloat(btn, "scaleX"), sy = GfxGetFloat(btn, "scaleY");
    if (sx <= 0.0f) sx = 1.0f;
    if (sy <= 0.0f) sy = 1.0f;
    const float w = GfxGetFloat(btn, "width") / sx, h = GfxGetFloat(btn, "height") / sy;
    return mx >= 0.0f && my >= 0.0f && mx <= w && my <= h;
}

// Near enough, for buttons that are rotated or have their origin elsewhere.
static bool MouseNearPath(const std::string& path) {
    uintptr_t btn = GfxVar(g_inj.movie, path);
    if (!btn) return false;
    float sx = GfxGetFloat(btn, "scaleX"), sy = GfxGetFloat(btn, "scaleY");
    if (sx < 0) sx = -sx;
    if (sy < 0) sy = -sy;
    if (sx < 0.01f) sx = 1.0f;
    if (sy < 0.01f) sy = 1.0f;
    const float w = GfxGetFloat(btn, "width") / sx, h = GfxGetFloat(btn, "height") / sy, r = (w > h ? w : h) * 1.1f;
    const float mx = GfxGetFloat(btn, "mouseX"), my = GfxGetFloat(btn, "mouseY");
    static int logged = 0;
    if (++logged <= 10) LogF("[menu] mouse at %.0f,%.0f of %s (%.0fx%.0f)", mx, my, path.c_str(), w, h);
    return mx >= -r && mx <= r && my >= -r && my <= r;
}

static bool MouseOver(const std::string& entry) {
    return g_inj.active && g_inj.level == 4 && MouseOverPath(LevelPath(3) + "." + entry);
}

static bool MouseOverMods() { return MouseOver(g_modsName); }

// The entry under the mouse, as an event name (used when the menu has no focus).
static const char* MouseEvent() {
    if (!g_inj.active || g_inj.level < 4) return nullptr;
    for (const Override& o : g_overrides)
        if (!o.clone.empty() && MouseOver(o.clone)) return o.event.c_str();
    for (auto& k : kEntries)
        if (MouseOver(k.entry)) return k.event;
    if (MouseOver(g_inj.names[3])) return "onQuit";
    const std::string settings = FindEntry("onSettings");
    if (!settings.empty() && MouseOver(settings)) return "onSettings";
    return nullptr;
}

// ======================= event tap (diagnostics) =======================
// Logs ProcessEvent calls received by the UI objects (movie player, shell,
// main-menu data, sound packs) so we can see what the Flash UI sends on a click.
struct TapSlot {
    void** vt;
    ProcessEventFn orig;
};
static TapSlot g_taps[16];
static int g_tapCount = 0;

static ProcessEventFn TapOrig(uintptr_t obj) {
    void** vt = *reinterpret_cast<void***>(obj);
    for (int i = 0; i < g_tapCount; ++i)
        if (g_taps[i].vt == vt) return g_taps[i].orig;
    return nullptr;
}

static void OpenModsFromGame(void*) {
    if (ShellModalActive()) return;
    LogF("[menu] MODS activated (game click)");
    ShowMessageTask(new MsgTask{L"MODS", L"Hello, World!", {L"OK"}, false});
}

static void (*g_menuCallback)(const char* event) = nullptr;
void SetMenuCallback(void (*fn)(const char* event)) { g_menuCallback = fn; }

static std::string g_lastEvent;
static ULONGLONG g_lastEventTick = 0;

static void MenuEventTask(void* event) {
    const char* e = static_cast<const char*>(event);
    g_lastEvent = e;
    g_lastEventTick = GetTickCount64();
    LogF("[menu] event %s", e);
    if (g_menuCallback && !g_off) g_menuCallback(e);
}

static std::string g_topMenu = "?";  // "?" = unknown

// Other screens reuse the same names: only the main menu itself counts.
static bool OnRootMenu() { return g_topMenu == "?" || g_topMenu == "RootMenuMovie"; }

// The NEWS tab and the friends list are panels of the shell, opened by mouse, key or controller
// button: watch them open. A replaced one is closed again right away.
static void WatchPanels(ULONGLONG now) {
    struct Panel {
        const char *path, *state, *close, *event;
        bool open, known;
    };
    static Panel panels[] = {{"root.newsView", "bIsExpanded", "collapseView", "onNews", false, false},
                             {"root.friendsView", "shown", "hide", "onFriendsLists", false, false}};
    for (Panel& p : panels) {
        uintptr_t o = GfxVar(g_inj.movie, p.path);
        if (!o) {
            p.known = false;
            continue;
        }
        const bool open = GfxGetBool(o, p.state);
        const bool opened = p.known && open && !p.open;
        p.open = open;
        p.known = true;
        // The game may show the news by itself when the menu appears.
        const bool replaced = FindOverride(p.event) != nullptr;
        if (!opened || now - g_menuShownAt < 1500 || (!replaced && ShellModalActive())) continue;
        if (replaced) {
            GfxInvoke(o, p.close, {});
            p.open = GfxGetBool(o, p.state);
        }
        if (g_lastEvent == p.event && now - g_lastEventTick < 1500) continue;  // reported by the click
        if (!ShellModalActive()) MenuEventTask(const_cast<char*>(p.event));
    }
}

// Watches the game's menu stack: opening the settings menu is the same for every input device.
static void PollTopMenu(ULONGLONG now) {
    static ULONGLONG last = 0, nextFind = 0;
    static uintptr_t stack = 0;
    if (g_findStack) { g_findStack = false; stack = 0; nextFind = 0; }
    std::string& top = g_topMenu;
    if (now - last < 50) return;
    last = now;
    static uintptr_t fn = Fn("TAGame.GFxData_MenuStack_TA.GetTopMenu");
    static const int off = fn ? ParamOffset(fn, "ReturnValue") : -1;
    if (off < 0 || off > 0x80) return;
    if (!stack || !IsObject(stack) || ClassNameOf(stack) != "GFxData_MenuStack_TA") {
        stack = 0;
        if (now < nextFind) return;
        nextFind = now + 3000;
        if (!(stack = NewestTransient("GFxData_MenuStack_TA"))) stack = FindInstance("GFxData_MenuStack_TA");
        LogF("[menu] menu stack: %s", stack ? FullNameOf(stack).c_str() : "not found yet");
        if (!stack) return;
    }
    alignas(16) uint8_t buf[0x100] = {};
    CallFunction(stack, fn, buf);
    int32_t ni = 0;
    memcpy(&ni, buf + off, 4);
    const std::string name = NameByIndex(ni);
    if (name == top) return;
    static int logged = 0;
    if (++logged <= 80) LogF("[menu] top menu '%s' -> '%s'", top.c_str(), name.c_str());
    const bool first = top == "?";
    top = name;
    if (!first && name.find("Challenge") != std::string::npos && !(g_lastEvent == "onChallenges" && now - g_lastEventTick < 2000))
        MenuEventTask(const_cast<char*>("onChallenges"));
    g_panelsDirty = true;
    if (name == "RootMenuMovie" && !first && g_ourModal) DropGhostModal();  // its screen went away
    if (name == "RootMenuMovie") g_menuShownAt = now;
    if (name == "RootMenuMovie") {  // back on the main menu: check our entries right away
        g_inj.urgentUntil = now + 4000;
        g_inj.notBefore = 0;
        g_inj.last = 0;
    }
    std::string low = name;
    for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (!first && !FindOverride("onSettings") && (low.find("option") != std::string::npos || low.find("settings") != std::string::npos) &&
        !(g_lastEvent == "onSettings" && now - g_lastEventTick < 2000))  // already reported by the click
        MenuEventTask(const_cast<char*>("onSettings"));
}

// The menu entry whose click this is: the game plays the sound while the button is in its
// "release" state. (The focus alone can be on another button than the one clicked.)
static const char* PressedEntry(std::string& name, std::string& state) {
    const auto stateOf = [&](uintptr_t o) { return state = GfxGetString(o, "state"); };
    const auto released = [&](uintptr_t o) { return stateOf(o).find("release") != std::string::npos || state.find("down") != std::string::npos; };
    uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
    uintptr_t f = stage ? GfxGetObject(stage, "focus") : 0;
    const bool live = f && IsObject(f);
    if (live) name = GfxGetString(f, "name");
    const char* entry = live ? (name == g_modsName ? "onMods" : EntryEvent(f, name)) : nullptr;
    // The Y button clicks the NEWS tab.
    if (GetTickCount64() - static_cast<ULONGLONG>(g_yDownAt) < 150) {
        name = "Y";
        return "onNews";
    }
    // A controller click is always on the focused entry.
    if (entry && stateOf(f).find("kb_") != std::string::npos && released(f)) return entry;
    // Buttons outside the list: pressed, and under the mouse.
    const Override* ch = FindOverride("onChallenges");
    const struct { std::string path; const char* event; } others[] = {
        {ch && !ch->clone.empty() ? LevelPath(1) + ".challengesSummaryView." + ch->clone : "", "onChallenges"},
        {"root.newsView.newsTabButton", "onNews"}};
    for (auto& b : others) {
        uintptr_t o = b.path.empty() ? 0 : GfxVar(g_inj.movie, b.path);
        const bool news = strcmp(b.event, "onNews") == 0;
        if (o && released(o) && (news ? MouseNearPath(b.path) : MouseOverPath(b.path))) {
            name = b.event;
            return b.event;
        }
    }
    if (entry && released(f)) return entry;
    if (live) return nullptr;
    const char* e = MouseOverMods() ? "onMods" : MouseEvent();
    if (e) state = "mouse";
    return e;
}

// NEWS does not open while the playlist panel counts as shown. The game plays the click sound
// before it acts on the click: set the flag then, and clear it right afterwards.
static const char* const kPlaylist = "root.musicPlayerView.playlistSelectionView";
static ULONGLONG g_newsGateAt = 0;

static bool BlockNews() {
    uintptr_t p = GfxVar(g_inj.movie, kPlaylist);
    if (!p || GfxGetBool(p, "bIsShown")) return false;
    GfxSetBool(p, "bIsShown", true);
    g_newsGateAt = GetTickCount64();
    return true;
}

static void ReleaseGates(ULONGLONG now) {
    if (!g_newsGateAt || now - g_newsGateAt < 20) return;
    g_newsGateAt = 0;
    if (uintptr_t p = GfxVar(g_inj.movie, kPlaylist)) GfxSetBool(p, "bIsShown", false);
}

// The sound of a panel opening or closing: true if it belongs to a replaced one (not heard).
static bool MutePanelSound() {
    if (!OnRootMenu() || !g_inj.movie) return false;
    static bool newsOpen = false;
    uintptr_t news = GfxVar(g_inj.movie, "root.newsView");
    const bool open = news && GfxGetBool(news, "bIsExpanded");
    const bool isNews = open != newsOpen;
    newsOpen = open;
    if (GetTickCount64() - g_menuShownAt < 1500) return false;
    return FindOverride(isNews ? "onNews" : "onFriendsLists") != nullptr;
}

// The game's click sound, for clicks the game itself no longer handles.
static bool g_ownSound = false;

static void PlayClick() {
    static uintptr_t fn = Fn("ProjectX.GFxMoviePlayer_X.PlaySound");
    static const int off = fn ? ParamOffset(fn, "EventName") : -1;
    static int32_t select = -1;
    if (off < 0 || off > 0x80 || !IsObject(g_inj.movie)) return;
    if (select < 0) {
        if (g_selectName > 0) select = g_selectName;  // heard before
        else
            for (int i = 0, n = NameCount(); i < n && select < 0; ++i)
                if (NameByIndex(i) == "Select") select = i;
        if (select < 0) return;
    }
    alignas(16) uint8_t buf[0x100] = {};
    memcpy(buf + off, &select, 4);
    g_ownSound = true;
    CallFunction(g_inj.movie, fn, buf);
    g_ownSound = false;
}

// The friends button: the game no longer reacts to it. Mouse click, or R2 on a controller.
static void FriendsClicked() {
    if (!g_friendsOff || g_inj.level < 4 || ShellModalActive()) return;
    uintptr_t news = GfxVar(g_inj.movie, "root.newsView");
    if (news && GfxGetBool(news, "bIsExpanded")) return;  // in front of it
    uintptr_t friends = GfxVar(g_inj.movie, "root.friendsView");
    if (friends && GfxGetBool(friends, "shown")) return;
    PlayClick();
    MenuEventTask(const_cast<char*>("onFriendsLists"));
}

// The friends button makes no click sound: watch the mouse button instead.


static void CheckFriendsClick() {
    if (InterlockedExchange(&g_rtReleased, 0)) {
        const std::string& focus = g_focus[(g_focusHead + kFocusSamples - 1) % kFocusSamples].name;
        if (focus.find("extField") == std::string::npos) FriendsClicked();  // not while typing
    }
    static bool pressedOnIt = false;
    const bool down = InterlockedExchange(&g_mousePressed, 0) != 0, up = InterlockedExchange(&g_mouseReleased, 0) != 0;
    if (!down && !up) return;
    const bool over = g_friendsOff && g_inj.level == 4 && MouseOverPath("root.taskBar.friendsButton");
    if (down) pressedOnIt = over;
    if (!up) return;
    const bool click = pressedOnIt && over;
    pressedOnIt = false;
    if (click) FriendsClicked();
}

// R1 is the game's shortcut for VIEW CHALLENGES; replaced, the game no longer reacts to it.
static void CheckChallengesKey() {
    if (!InterlockedExchange(&g_rbPressed, 0)) return;
    const Override* o = FindOverride("onChallenges");
    if (!o || o->clone.empty() || g_inj.level < 4 || ShellModalActive()) return;
    uintptr_t news = GfxVar(g_inj.movie, "root.newsView"), friends = GfxVar(g_inj.movie, "root.friendsView");
    if ((news && GfxGetBool(news, "bIsExpanded")) || (friends && GfxGetBool(friends, "shown"))) return;
    PlayClick();
    MenuEventTask(const_cast<char*>("onChallenges"));
}

static void __fastcall HkTap(uintptr_t obj, uintptr_t func, void* parms, void* result) {
    ProcessEventFn orig = TapOrig(obj);
    if (g_off) {
        if (orig) orig(obj, func, parms, result);
        return;
    }
    static uintptr_t playSoundFn = Fn("ProjectX.GFxMoviePlayer_X.PlaySound");
    static uintptr_t receiveInputFn = Fn("ProjectX.GFxMoviePlayer_X.SetReceiveInput");
    static uintptr_t cursorFn = Fn("ProjectX.GFxMoviePlayer_X.SetCursorVisible");
    if (func && parms && (func == receiveInputFn || func == cursorFn)) {
        // The main menu screen just became active: add MODS right away.
        static const int sceneOff = ParamOffset(receiveInputFn, "SceneName");
        if (sceneOff >= 0 && ReadFString(reinterpret_cast<uintptr_t>(parms) + sceneOff).rfind("[MainMenuScreen", 0) == 0)
            OnMainMenuShown(obj);
    }
    if (func && func == playSoundFn && parms && !g_ownSound) {
        // The game's own button click ('Select') on MODS opens the dialog, so the
        // click sound and the dialog always happen together.
        static const int eventOff = ParamOffset(func, "EventName");
        int32_t ni = 0;
        if (eventOff >= 0) R(reinterpret_cast<uintptr_t>(parms) + eventOff, ni);
        const std::string sound = NameByIndex(ni);
        if (sound == "Select") g_selectName = ni;
        if (sound == "switchTabs") {  // NEWS and the friends list play it
            g_panelsDirty = true;
            if (MutePanelSound()) return;
        }
        if (sound == "Select") {
            PollTopMenu(GetTickCount64());
            const bool menu = g_inj.active && g_inj.level == 4 && OnRootMenu();
            std::string name, state;
            const char* e = menu ? PressedEntry(name, state) : nullptr;
            if (e && strcmp(e, "onNews") == 0) {
                uintptr_t news = GfxVar(g_inj.movie, "root.newsView");
                // Not replaced (or being closed): the panel itself tells when it opens.
                if (!FindOverride("onNews") || (news && GfxGetBool(news, "bIsExpanded")) || !BlockNews()) e = nullptr;
            }
            bool modal = ShellModalActive();
            // The click was accepted by the menu: no popup can be in front of it.
            if (modal && e && GetTickCount64() - g_ourModalAt > 400) {
                DropGhostModal();
                modal = ShellModalActive();
            }
            static int logged = 0;
            if (++logged <= 200)
                LogF("[menu] click '%s' (%s)%s%s%s", name.c_str(), state.c_str(), OnRootMenu() ? "" : ", other screen",
                     g_inj.level == 4 ? "" : ", list unknown", modal ? ", popup open" : "");
            g_wantUntil = g_checkAt = 0;
            if (e && !modal) {
                if (strcmp(e, "onMods") == 0 && !FindOverride("onMods")) RunOnGameThread(OpenModsFromGame, nullptr);
                else RunOnGameThread(MenuEventTask, const_cast<char*>(e));
            }
        }
    }
    if (orig) orig(obj, func, parms, result);
}

static void TapObject(uintptr_t obj) {
    if (!obj || L.peIndex < 0 || g_tapCount >= 16) return;
    void** vt = *reinterpret_cast<void***>(obj);
    for (int i = 0; i < g_tapCount; ++i)
        if (g_taps[i].vt == vt) return;
    if (vt[L.peIndex] == reinterpret_cast<void*>(&HkTap)) return;
    g_taps[g_tapCount] = {vt, reinterpret_cast<ProcessEventFn>(vt[L.peIndex])};
    ++g_tapCount;
    WriteSlot(&vt[L.peIndex], reinterpret_cast<void*>(&HkTap));
    LogF("[tap] watching %s (%s)", FullNameOf(obj).c_str(), ClassNameOf(obj).c_str());
}

void StartEventTap() {
    const int n = ObjectCount();
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o) continue;
        const std::string cls = ClassNameOf(o);
        const bool live = FullNameOf(o).rfind("Transient.", 0) == 0;
        if (live && cls == "GFxMoviePlayer_X") TapObject(o);
    }
}

}  // namespace engine
