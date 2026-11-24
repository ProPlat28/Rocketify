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

void RunOnGameThread(void (*fn)(void*), void* ctx) {
    AcquireSRWLockExclusive(&g_taskLock);
    g_tasks.push_back({fn, ctx});
    ReleaseSRWLockExclusive(&g_taskLock);
}

static void __fastcall HkControllerProcessEvent(uintptr_t obj, uintptr_t func, void* parms, void* result) {
    if (g_off) return g_pcProcessEvent(obj, func, parms, result);
    if (!t_inTasks && !g_tasks.empty()) {
        AcquireSRWLockExclusive(&g_taskLock);
        auto tasks = std::move(g_tasks);
        g_tasks.clear();
        ReleaseSRWLockExclusive(&g_taskLock);
        t_inTasks = true;
        for (auto& [fn, ctx] : tasks) fn(ctx);
        t_inTasks = false;
    }
    g_pcProcessEvent(obj, func, parms, result);
}

void CallFunction(uintptr_t obj, uintptr_t func, void* parms) {
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
static uintptr_t g_shell = 0, g_modalClass = 0;
static uintptr_t g_fnCreateModal = 0, g_fnSetTitle = 0, g_fnSetBody = 0, g_fnAddButton = 0;

bool NativeMessageReady() {
    return g_shell && g_modalClass && g_fnCreateModal && g_fnSetTitle && g_fnSetBody && g_fnAddButton &&
           L.peIndex >= 0 && L.propOffsetOff >= 0;
}

// Offset of a named parameter inside a function's parameter block (-1 if missing).
static int ParamOffset(uintptr_t fn, const char* name) {
    for (uintptr_t p : Chain(fn, 64))
        if (NameOf(p) == name) {
            int32_t off = -1;
            R(p + L.propOffsetOff, off);
            return off;
        }
    return -1;
}

bool PrepareNativeMessage() {
    // one pass over all objects
    const int n = ObjectCount();
    g_shell = g_modalClass = g_fnCreateModal = g_fnSetTitle = g_fnSetBody = g_fnAddButton = 0;
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o) continue;
        const std::string cls = ClassNameOf(o);
        if (cls == "GFxShell_TA" && !g_shell && FullNameOf(o).rfind("Transient.", 0) == 0) g_shell = o;
        else if (cls == "Class" && NameOf(o) == "GFxModal_X") g_modalClass = o;
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

struct MsgTask {
    std::wstring title, body, button;
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

static void ShowMessageTask(void* ctx) {
    auto* t = static_cast<MsgTask*>(ctx);
    if (!(IsObject(g_shell) && ClassNameOf(g_shell) == "GFxShell_TA")) {
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
    memcpy(buf + clsOff, &g_modalClass, 8);
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
    CallWithString(modal, g_fnAddButton, "Label", t->button);
    LogF("[engine] opened native dialog %s", FullNameOf(modal).c_str());
    delete t;
}

void ShowNativeMessage(const wchar_t* title, const wchar_t* body, const wchar_t* button) {
    RunOnGameThread(ShowMessageTask, new MsgTask{title, body, button});
}

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

static uintptr_t FindShellMovie() {
    const int n = ObjectCount();
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (o && ClassNameOf(o) == "GFxMoviePlayer_X" && FullNameOf(o).rfind("Transient.", 0) == 0) return o;
    }
    return 0;
}

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

static bool ShellModalActive() {
    static uintptr_t fn = Fn("TAGame.GFxShell_TA.IsModalActive");
    if (!fn || !IsObject(g_shell)) return false;
    alignas(16) uint8_t buf[0x10] = {};
    CallFunction(g_shell, fn, buf);
    uint32_t r;
    memcpy(&r, buf, 4);
    return r != 0;
}

static std::string TypeOf(uintptr_t o) { return o ? Narrow(GfxInvoke(o, "toString", {}).S) : ""; }

static const char* const kModsName = "rocketify_mods";

struct Injector {
    bool active = false;
    uintptr_t movie = 0;
    ULONGLONG last = 0;
    ULONGLONG notBefore = 0;  // pause until this tick (loading / retry back-off)
    ULONGLONG lastFocus = 0;  // last focus sample
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
        case 2: return GfxGetInt(o, "numChildren") >= 6;
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

static int InstanceNum(const std::string& name);
static std::string LowestButtonNear(const std::string& list, int centre);

static bool LearnFromFocus() {
    uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
    uintptr_t btn = stage ? GfxGetObject(stage, "focus") : 0;
    if (!btn || !IsObject(btn)) return false;
    if (GfxGetString(btn, "name").rfind("RootMenuMovie_", 0) != 0) return false;  // a main-menu entry
    uintptr_t list = GfxGetObject(btn, "parent");
    uintptr_t root = list ? GfxGetObject(list, "parent") : 0;
    uintptr_t screen = root ? GfxGetObject(root, "parent") : 0;
    if (!screen) return false;
    const std::string nScreen = GfxGetString(screen, "name"), nRoot = GfxGetString(root, "name"),
                      nList = GfxGetString(list, "name");
    if (nScreen.empty() || nRoot.empty() || nList.empty()) return false;
    g_inj.names[0] = nScreen;
    g_inj.names[1] = nRoot;
    g_inj.names[2] = nList;
    g_inj.names[3] = LowestButtonNear(LevelPath(3), InstanceNum(nList));
    return !g_inj.names[3].empty();
}

static void EnsureButton() {
    const std::string list = LevelPath(3);
    const std::string quitPath = list + "." + g_inj.names[3];
    if (GfxVar(g_inj.movie, list + "." + kModsName)) return;  // already there

    uintptr_t listObj = GfxVar(g_inj.movie, list);
    uintptr_t quit = GfxVar(g_inj.movie, quitPath);
    if (!listObj || !quit) return;

    // Same Flash class as QUIT: "[class Foo]" -> "Foo"
    std::string cls = TypeOf(GfxGetObject(quit, "constructor"));
    if (cls.rfind("[class ", 0) == 0 && cls.back() == ']') cls = cls.substr(7, cls.size() - 8);
    const int kids = GfxGetInt(listObj, "numChildren");
    uintptr_t btn = 0;
    std::string used;
    for (const std::string& sym : {cls, std::string("NewBasicMenuButton"), std::string("BasicMenuButton")}) {
        if (sym.empty() || sym[0] == '[') continue;
        if ((btn = GfxAttach(listObj, sym, kModsName, kids))) { used = sym; break; }
    }
    if (!btn) {
        LogF("[menu] could not create a menu button (QUIT class '%s')", cls.c_str());
        return;
    }
    const float qx = GfxGetFloat(quit, "x"), qy = GfxGetFloat(quit, "y");
    const float step = kids > 1 ? qy / static_cast<float>(kids - 1) : 35.0f;
    GfxSetFloat(btn, "x", qx);
    GfxSetFloat(btn, "y", qy + (step > 1.0f ? step : 35.0f));
    GfxSetString(btn, "label", L"MODS");
    if (uintptr_t tl = GfxVar(g_inj.movie, list + "." + kModsName + ".textLabel")) GfxSetString(tl, "text", L"MODS");
    // Subtitle shown when the entry is focused, like QUIT's "EXIT ROCKET LEAGUE®".
    GfxSetString(btn, "descLabel", L"MANAGE YOUR MODS");
    if (uintptr_t dl = GfxVar(g_inj.movie, list + "." + kModsName + ".descTextLabel")) GfxSetString(dl, "text", L"MANAGE YOUR MODS");
    LogF("[menu] MODS entry added to %s (class %s, y=%.1f)", list.c_str(), used.c_str(), qy + step);
}

// ---- focus tracking (for controller / keyboard navigation) ----
struct FocusSample {
    ULONGLONG t;
    std::string name;
};
static FocusSample g_focus[32];
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
    const FocusSample& last = g_focus[(g_focusHead + 31) % 32];
    static int logged = 0;
    if (name != last.name && logged < 60) {
        ++logged;
        LogF("[menu] focus -> '%s'", name.c_str());
    }
    g_focus[g_focusHead] = {t, name};
    g_focusHead = (g_focusHead + 1) % 32;
}

static void SampleFocus(ULONGLONG now) {
    uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
    uintptr_t f = stage ? GfxGetObject(stage, "focus") : 0;
    PushFocus(now, f && IsObject(f) ? GfxGetString(f, "name") : "");
}

// Focus as it was just before an input edge at time t.
static std::string FocusBefore(ULONGLONG t) {
    std::string best;
    ULONGLONG bestT = 0;
    for (const FocusSample& f : g_focus)
        if (f.t && f.t <= t && f.t >= bestT) { bestT = f.t; best = f.name; }
    if (bestT == 0) best = g_focus[(g_focusHead + 31) % 32].name;
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
    if (g_off || !g_inj.active || g_inj.level < 4 || ShellModalActive()) return;
    const std::string list = LevelPath(3);
    const std::string before = FocusBefore(t);
    uintptr_t stage = GfxVar(g_inj.movie, "root.stage");
    if (!stage) return;
    if (kind == 2 && (before == g_inj.names[3] || before == kModsName)) {
        if (uintptr_t mods = GfxVar(g_inj.movie, list + "." + kModsName)) {
            GfxSetObject(stage, "focus", mods);
            PushFocus(GetTickCount64(), kModsName);
        }
    } else if (kind == 1 && before == kModsName) {
        if (uintptr_t quit = GfxVar(g_inj.movie, list + "." + g_inj.names[3])) {
            GfxSetObject(stage, "focus", quit);
            PushFocus(GetTickCount64(), g_inj.names[3]);
        }
    }
}


void NativeMenuNav(int kind, unsigned long long tick) { RunOnGameThread(NavTaskFn, new NavTask{kind, tick}); }

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

// Called from the UI event hook when the main menu screen becomes active.
static void OnMainMenuShown() {
    if (GetTickCount64() >= g_inj.urgentUntil) LogF("[menu] main menu shown");
    g_inj.urgentUntil = GetTickCount64() + 4000;
    g_inj.notBefore = 0;
    g_inj.last = 0;
}

static void InjectorSlice(void*) {
    Injector& j = g_inj;
    if (!j.active) return;
    const ULONGLONG now = GetTickCount64();
    const bool urgent = now < j.urgentUntil;
    const bool scanning = j.level < 4;
    if (!scanning && now - j.lastFocus >= 30) {
        j.lastFocus = now;
        SampleFocus(now);
    }
    const ULONGLONG cadence = urgent ? 0u : (scanning ? 16u : 500u);
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
        } else {
            EnsureButton();
        }
        RunOnGameThread(InjectorSlice, nullptr);
        return;
    }

    // Fast path: the focused PLAY entry leads straight to the list.
    if (LearnFromFocus() && VerifyLevels() == 4) {
        j.level = 4;
        SaveInjectorNames();
        LogF("[menu] menu list found from focus: %s (QUIT = %s)", LevelPath(3).c_str(), j.names[3].c_str());
        EnsureButton();  // same frame
        RunOnGameThread(InjectorSlice, nullptr);
        return;
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
    constexpr int kMaxProbes = 6000;
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
                SaveInjectorNames();
                LogF("[menu] menu list found: %s (QUIT = %s)", LevelPath(3).c_str(), j.names[3].c_str());
                EnsureButton();  // insert in the same frame
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
    if (!g_inj.movie) { LogF("[menu] no shell movie"); return; }
    LoadInjectorNames();
    const int valid = VerifyLevels();
    for (int l = valid; l < 4; ++l) g_inj.names[l].clear();
    BeginLevel(valid);
    g_inj.active = true;
    LogF("[menu] injector started (%d cached levels valid)", g_inj.level);
    RunOnGameThread(InjectorSlice, nullptr);
}

void StartMenuInjector(const std::wstring& iniPath) {
    RunOnGameThread(InjectorStart, new InjStart{iniPath});
}

// Mouse-up anywhere: if it landed on our entry, open the real dialog.
static bool MouseOverMods() {
    if (!g_inj.active || g_inj.level < 4) return false;
    uintptr_t btn = GfxVar(g_inj.movie, LevelPath(3) + "." + kModsName);
    if (!btn) return false;
    const float mx = GfxGetFloat(btn, "mouseX"), my = GfxGetFloat(btn, "mouseY");
    float sx = GfxGetFloat(btn, "scaleX"), sy = GfxGetFloat(btn, "scaleY");
    if (sx <= 0.0f) sx = 1.0f;
    if (sy <= 0.0f) sy = 1.0f;
    const float w = GfxGetFloat(btn, "width") / sx, h = GfxGetFloat(btn, "height") / sy;
    return mx >= 0.0f && my >= 0.0f && mx <= w && my <= h;
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
    ShowMessageTask(new MsgTask{L"MODS", L"Hello, World!", L"OK"});
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
            OnMainMenuShown();
    }
    if (func && func == playSoundFn && parms) {
        // The game's own button click ('Select') on MODS opens the dialog, so the
        // click sound and the dialog always happen together.
        int32_t ni = 0;
        R(reinterpret_cast<uintptr_t>(parms) + ParamOffset(func, "EventName"), ni);
        if (NameByIndex(ni) == "Select" && !ShellModalActive()) {
            const std::string focus = g_focus[(g_focusHead + 31) % 32].name;
            if (focus == kModsName || MouseOverMods()) RunOnGameThread(OpenModsFromGame, nullptr);
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
