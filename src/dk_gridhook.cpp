// dk_gridhook.cpp - "every fragment touches every fragment" on the Ancestral Grid (2026-11-09).
//
// The game builds the grid's connections per fragment NODE in a native function (found with a hardware breakpoint
// on the graph's OnUpdated while a fragment moved; this build exe+0x582E990, called from the graph update
// exe+0x58559F5):
//   builder(graph, node): sums the node's own modifier power, then for each of the fragment's cells looks at the 6
//   hex neighbours and adds any other fragment node found there to node+0x168 (a TSet keyed by node+0x54) through
//   the game's set-add (exe+0x5698720: Emplace(set, &outId, &key, bool* = null)).
// Power, chains, the tooltip, the skill rank and the "enhanced" state are all computed from those sets afterwards.
// The hook lets the game build the node normally, then adds EVERY other node to its set and it to theirs, so to
// the game everything touches everything.
//
// Graph layout (UHnSSkillGridGraph, native part): +0x28 TSparseArray of nodes {data, num, max, allocation bits
// inline at +0x38 (128 bits) / heap pointer at +0x48}, element 0x278 bytes, node = element + 8.
//
// Hot reload: retired trainer copies stay loaded, so the hook is installed ONCE per game process; its on/off switch
// lives in a small block of process memory whose address is kept in the environment (DK_GRIDHOOK).
#include "common.h"
#include "ue.h"
#include "MinHook.h"
#include <cstdarg>

namespace dk {

static std::string sfmt(const char* f, ...) { char b[256]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }

struct GridHookShared {
    volatile LONG enabled;        // 1 = link everything
    uintptr_t set_add;            // game's TSet add
    uintptr_t original;           // trampoline to the game's builder
    volatile LONG links_added;    // statistics
};
static GridHookShared* g_gh = nullptr;   // this copy's state

static constexpr size_t kNodeStride = 0x278, kNodeInElem = 8, kSetOff = 0x168, kKeyOff = 0x54, kIdOff = 0x58;

using BuildFn = uintptr_t(__fastcall*)(uintptr_t graph, uintptr_t node, uintptr_t a3, uintptr_t a4);
using SetAddFn = void(__fastcall*)(uintptr_t set, void* out_id, const void* key, bool* already);

static bool node_alive(uintptr_t graph, int idx) {
    uintptr_t heap = 0; ue::rd_as(graph + 0x48, heap);
    const uintptr_t bits = heap ? heap : graph + 0x38;
    uint32_t w = 0; if (!ue::rd_as(bits + (idx / 32) * 4, w)) return false;
    return (w >> (idx % 32)) & 1;
}
static void link_all_seh(uintptr_t graph, uintptr_t node, uintptr_t set_add) {
    __try {
        uintptr_t data = *(uintptr_t*)(graph + 0x28); const int32_t num = *(int32_t*)(graph + 0x30);
        if (!data || num <= 0 || num > 4096) return;
        const int32_t my_id = *(int32_t*)(node + kIdOff);
        uint64_t out = 0; LONG n = 0;
        for (int32_t i = 0; i < num; ++i) {
            if (!node_alive(graph, i)) continue;
            const uintptr_t other = data + (uintptr_t)i * kNodeStride + kNodeInElem;
            if (other == node || *(int32_t*)(other + kIdOff) == my_id) continue;
            // count only real additions: TSet size = Elements.Data.Num (+8) - NumFreeIndices (+0x34)
            auto size = [](uintptr_t set) { return *(int32_t*)(set + 8) - *(int32_t*)(set + 0x34); };
            const int32_t a0 = size(node + kSetOff), b0 = size(other + kSetOff);
            ((SetAddFn)set_add)(node + kSetOff, &out, (const void*)(other + kKeyOff), nullptr);
            ((SetAddFn)set_add)(other + kSetOff, &out, (const void*)(node + kKeyOff), nullptr);
            n += (size(node + kSetOff) - a0) + (size(other + kSetOff) - b0);
        }
        InterlockedAdd(&g_gh->links_added, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Each trainer copy installs its OWN hook: on a trainer reload the retiring copy puts the game's code back (the
// framework restores every hook it made), so the new copy must hook again. ensure_hook() is called every second
// while the switch is on and re-installs when the builder's first byte is no longer our jump.
static uintptr_t g_build_addr = 0;
static uintptr_t __fastcall hk_build(uintptr_t graph, uintptr_t node, uintptr_t a3, uintptr_t a4) {
    GridHookShared* s = g_gh;
    const uintptr_t r = ((BuildFn)s->original)(graph, node, a3, a4);
    if (s->enabled && graph && node) link_all_seh(graph, node, s->set_add);
    return r;
}

static uintptr_t rel_target(uintptr_t call_at) { int32_t d = 0; return ue::rd_as(call_at + 1, d) ? call_at + 5 + d : 0; }
static bool hooked_now() { uint8_t b = 0; return g_build_addr && ue::rd_as(g_build_addr, b) && b == 0xE9; }
static bool g_installed = false;

// "" = ok; *fresh = true when the hook was (re)installed by this call
static std::string ensure_hook(bool* fresh = nullptr) {
    static GridHookShared shared{};
    if (!g_gh) g_gh = &shared;
    if (g_installed && hooked_now()) return "";
    if (!g_build_addr) {
        auto hits = ue::find_code("48 89 4C 24 08 55 56 57 41 55 41 57 48 83 EC 70 48 8B FA 48 8B F1 48 8B CF 49 8B D0 E8 ?? ?? ?? ?? 48 8D AF 68 01 00 00", 4);
        if (hits.size() != 1) return sfmt("grid link builder not found (%zu matches)", hits.size());
        g_build_addr = hits[0];
        auto adds = ue::find_code("4C 8D 40 54 45 33 C9 48 8D 94 24 A8 00 00 00 48 8B CD E8", 8);
        for (uintptr_t a : adds) if (a > g_build_addr && a < g_build_addr + 0x400) g_gh->set_add = rel_target(a + 18);
        if (!g_gh->set_add) return "grid set-add not found";
    }
    if (g_installed) { MH_DisableHook((void*)g_build_addr); MH_RemoveHook((void*)g_build_addr); g_installed = false; }
    void* orig = nullptr;
    MH_STATUS st = MH_CreateHook((void*)g_build_addr, (void*)&hk_build, &orig);
    if (st != MH_OK) return sfmt("hook failed (%d)", (int)st);
    g_gh->original = (uintptr_t)orig;
    if (MH_EnableHook((void*)g_build_addr) != MH_OK) return "hook enable failed";
    g_installed = true; if (fresh) *fresh = true;
    ::wb::log("[dk] grid link hook on the game's builder at exe+0x%llX (set add exe+0x%llX)",
        (unsigned long long)(g_build_addr - (uintptr_t)GetModuleHandleW(nullptr)), (unsigned long long)(g_gh->set_add - (uintptr_t)GetModuleHandleW(nullptr)));
    return "";
}

std::string gridlink_set(bool on, bool* fresh) {
    if (!on && !g_gh) return "";
    std::string e = ensure_hook(fresh); if (!e.empty()) return e;
    InterlockedExchange(&g_gh->enabled, on ? 1 : 0);
    return "";
}
long gridlink_links() { return g_gh ? g_gh->links_added : 0; }
// link every pair of fragments already in this graph right now (switching on mid-game: the hook only sees nodes the
// game rebuilds later). The game recomputes power on its next grid update (any fragment move / zone change).
int gridlink_link_graph(uintptr_t graph) {
    if (!g_gh || !g_gh->enabled || !graph) return 0;
    uintptr_t data = 0; int32_t num = 0;
    if (!ue::rd_as(graph + 0x28, data) || !ue::rd_as(graph + 0x30, num) || !data || num <= 0 || num > 4096) return 0;
    const LONG before = g_gh->links_added;
    for (int32_t i = 0; i < num; ++i) if (node_alive(graph, i)) link_all_seh(graph, data + (uintptr_t)i * kNodeStride + kNodeInElem, g_gh->set_add);
    return (int)(g_gh->links_added - before);
}

} // namespace dk
