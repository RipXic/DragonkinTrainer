// dk.cpp - Dragonkin trainer engine in C++ (see dk.h). Port of the Lua mod onto ue.h (native reflection).
#include "dk.h"
#include "ue.h"
#include <fstream>
#include <map>
#include <set>
#include <deque>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <climits>

namespace dk {
using namespace ::wb;
using ue::Prop; using ue::Params; using ue::FName; using ue::rd_as; using ue::mread; using ue::mwrite;

// ------------------------------------------------------------------ small helpers
static std::string g_data_dir, g_spec_path;
static json g_test = json::object();   // data/test.json - bisecting switches (readouts / keeper / skip)
static std::recursive_mutex g_mtx;                  // engine state shared with the panel thread
static std::atomic<uint64_t> g_version{1};
static void bump() { ++g_version; }

static std::string fmt(const char* f, ...) { char b[512]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }

// cached field lookup per class
static std::map<std::pair<uintptr_t, std::string>, Prop> g_fields;
static Prop F(uintptr_t obj_or_cls, const std::string& name, bool is_class = false) {
    const uintptr_t c = is_class ? obj_or_cls : ue::class_of(obj_or_cls);
    if (!c) return Prop();
    auto key = std::make_pair(c, name);
    auto it = g_fields.find(key); if (it != g_fields.end()) return it->second;
    Prop p = ue::find_prop(c, name); g_fields[key] = p; return p;
}
static Prop SF(uintptr_t ustruct, const std::string& name) { return F(ustruct, name, true); }
template <class T> static T rd(uintptr_t base, const Prop& p) { return ue::get<T>(base, p); }
static uintptr_t obj_field(uintptr_t obj, const char* name) { Prop p = F(obj, name); return p ? rd<uintptr_t>(obj, p) : 0; }

// ------------------------------------------------------------------ player roots (pc / pawn / player state)
struct Roots { uintptr_t pc = 0, pawn = 0, ps = 0; };
static uintptr_t g_pc = 0;
static Roots roots() {
    Roots r;
    static ULONGLONG last_search = 0;
    if ((!g_pc || !ue::alive(g_pc) || !obj_field(g_pc, "Player")) && GetTickCount64() - last_search >= 1000) {
        g_pc = 0; last_search = GetTickCount64();
        for (uintptr_t pc : ue::find_all(ue::find_class("PlayerController"))) if (obj_field(pc, "Player")) { g_pc = pc; break; }
    }
    r.pc = g_pc;
    if (r.pc && !(ue::alive(r.pc) && obj_field(r.pc, "Player"))) { g_pc = 0; r.pc = 0; }
    if (r.pc) { r.pawn = obj_field(r.pc, "Pawn"); r.ps = obj_field(r.pc, "PlayerState"); }
    if (r.pawn && !ue::alive(r.pawn)) r.pawn = 0;
    return r;
}
static std::string world_name() {
    static uintptr_t cls = 0; if (!cls) cls = ue::find_class("World");
    for (uintptr_t w : ue::find_all(cls, false, 4)) { std::string n = ue::name_of(w); if (!n.empty() && n.find("Untitled") == std::string::npos) return n; }
    return "";
}

// first component of class cls on the player (pawn, controller, player state)
static std::map<std::string, std::pair<uintptr_t, uintptr_t>> g_comp_cache;   // class -> (owner, component)
static uintptr_t player_component(const std::string& cls_name) {
    Roots r = roots();
    auto it = g_comp_cache.find(cls_name);
    if (it != g_comp_cache.end() && ue::alive(it->second.second) && (it->second.first == r.pawn || it->second.first == r.pc || it->second.first == r.ps)) return it->second.second;
    static std::map<std::string, ULONGLONG> miss;                 // a full object scan per miss: retry every 2 s at most
    auto m = miss.find(cls_name); if (m != miss.end() && GetTickCount64() - m->second < 2000) return 0;
    uintptr_t cls = ue::find_class(cls_name); if (!cls) return 0;
    for (uintptr_t c : ue::find_all(cls)) {
        const uintptr_t o = ue::outer_of(c);
        if (o && (o == r.pawn || o == r.pc || o == r.ps)) { g_comp_cache[cls_name] = { o, c }; miss.erase(cls_name); return c; }
    }
    miss[cls_name] = GetTickCount64();
    return 0;
}
static uintptr_t inv() { return player_component("HnSInventoryComponent"); }
static uintptr_t cdo(const char* path) { return ue::find_object(path); }

// ------------------------------------------------------------------ calls
static bool callf(uintptr_t obj, Params& p, std::string* err = nullptr) { return ue::call(obj, p, err); }
static uintptr_t lib(const char* cls_short) { uintptr_t c = ue::find_class(cls_short); return c ? ue::cdo_of(c) : 0; }

// solo / multiplayer: Standalone = offline solo; a listen server with one player counts as solo too
struct Session { bool standalone = false, server = false, solo = false; int players = 0; };
static Session session() {
    static Session cached; static ULONGLONG at = 0; static uintptr_t gs_cached = 0;
    if (GetTickCount64() - at < 1000) return cached;
    Session s; Roots r = roots(); const uintptr_t ctx = r.pawn ? r.pawn : r.pc;
    uintptr_t k = lib("KismetSystemLibrary");
    if (k && ctx) {
        Params a = ue::params_for(k, "IsStandalone"); if (a) { a.put("WorldContextObject", ctx); if (callf(k, a)) s.standalone = a.ret<bool>(); }
        Params b = ue::params_for(k, "IsServer"); if (b) { b.put("WorldContextObject", ctx); if (callf(k, b)) s.server = b.ret<bool>(); }
    }
    if (!gs_cached || !ue::alive(gs_cached)) gs_cached = ue::find_first(ue::find_class("GameStateBase"));
    if (gs_cached) { Prop pa = F(gs_cached, "PlayerArray"); if (pa) s.players = ue::array_of(gs_cached, pa).num; }
    s.solo = s.standalone || (s.players == 1 && s.server);
    cached = s; at = GetTickCount64();
    return s;
}
static bool multiplayer() { Session s = session(); return !s.solo && (s.server || s.players > 1); }

// ------------------------------------------------------------------ ability system + attributes (by NAME)
static uintptr_t asc_of(uintptr_t actor) {
    if (!actor) return 0;
    static uintptr_t c_actor = 0, c_asc = 0;
    if (actor == c_actor && c_asc && ue::alive(c_asc)) return c_asc;
    uintptr_t a = obj_field(actor, "AbilitySystemComponent");
    if (!(a && ue::alive(a))) {
        a = 0; static uintptr_t cls = 0; if (!cls) cls = ue::find_class("HnSAbilitySystemComponentBase");
        for (uintptr_t c : ue::find_all(cls)) if (ue::outer_of(c) == actor) { a = c; break; }
    }
    c_actor = actor; c_asc = a; return a;
}
static uintptr_t asc() { return asc_of(roots().pawn); }
static uintptr_t attrset(uintptr_t a) {
    if (!a) return 0;
    uintptr_t s = obj_field(a, "m_pCachedUberAttributeSet"); if (s && ue::alive(s)) return s;
    Prop sp = F(a, "SpawnedAttributes"); if (!sp) return 0;
    auto arr = ue::array_of(a, sp);
    for (int i = 0; i < arr.num && i < 32; ++i) { uintptr_t o = 0; rd_as((uintptr_t)arr.data + i * 8, o); if (o && ue::name_of(ue::class_of(o)).find("Uber") != std::string::npos) return o; }
    return 0;
}
struct Attr { bool ok = false; float base = 0, cur = 0; };
static Attr attr(const std::string& name, uintptr_t actor = 0) {
    Attr r; uintptr_t a = actor ? asc_of(actor) : asc(); uintptr_t s = attrset(a); if (!s) return r;
    Prop p = F(s, name); if (!p || p.type != "StructProperty") return r;
    const int o = p.size >= 16 ? 8 : 0;                   // FGameplayAttributeData: this game {Base, Current} (8 bytes); stock UE adds a vtable
    r.ok = rd_as(s + p.offset + o, r.base) && rd_as(s + p.offset + o + 4, r.cur);
    return r;
}
// game-made FGameplayAttribute structs (0x38) from the ASC, cached per ASC
static uintptr_t g_handles_asc = 0;
static std::map<std::string, std::vector<uint8_t>> g_handles;
static const std::vector<uint8_t>* attr_handle(const std::string& name, uintptr_t a) {
    if (a != g_handles_asc || g_handles.empty()) {
        g_handles.clear(); g_handles_asc = a;
        Params p = ue::params_for(a, "GetAllAttributes"); if (!p || !callf(a, p)) return nullptr;
        auto arr = p.out<ue::TArray<uint8_t>>("OutAttributes");
        for (int i = 0; i < arr.num && i < 2000; ++i) {
            const uintptr_t e = (uintptr_t)arr.data + (uintptr_t)i * 0x38;
            std::string n = ue::fstring(e);                 // FGameplayAttribute.AttributeName (FString at +0)
            std::vector<uint8_t> raw(0x38); if (n.empty() || !mread(e, raw.data(), 0x38)) continue;
            g_handles[n] = std::move(raw);
        }
    }
    auto it = g_handles.find(name); return it == g_handles.end() ? nullptr : &it->second;
}
// set through the game's own setter (the base value; the game adds gear/passives on top). op: 0 add 1 mult 2 div 3 override
static std::string set_attr(const std::string& name, double value, int op = 3) {
    if (multiplayer()) return "refused: not a solo session";
    uintptr_t a = asc(); if (!a) return "no character";
    const auto* h = attr_handle(name, a); if (!h) return "no attribute " + name;
    Params p = ue::params_for(a, "ServerModifyAttributeValue"); if (!p) return "no setter";
    p.put_raw("Attribute", h->data(), h->size()); p.put("Value", (float)value); p.put("ModifierOp", (uint8_t)op);
    std::string err; if (!callf(a, p, &err)) return err;
    return "";
}
static void refill(const char* cur, const char* max) {
    Attr a = attr(cur), m = attr(max);
    if (a.ok && m.ok && a.cur < m.cur - 0.5f) set_attr(cur, m.cur);
}

// ------------------------------------------------------------------ world helpers
static std::string set_speed(double v) {
    uintptr_t gs = lib("GameplayStatics"); Roots r = roots(); if (!gs || !r.pawn) return "no character";
    Params p = ue::params_for(gs, "SetGlobalTimeDilation"); if (!p) return "no function";
    p.put("WorldContextObject", r.pawn); p.put("TimeDilation", (float)v);
    std::string e; return callf(gs, p, &e) ? "" : e;
}
static bool tag_name(const std::string& tag, FName& out) { return ue::name_find(tag, out); }
static int64_t count(const std::string& tag) {
    uintptr_t i = inv(); FName n; if (!i || !tag_name(tag, n)) return -1;
    Params p = ue::params_for(i, "GetCountableCount"); if (!p) return -1;
    p.put("CountableID", n);                                   // FHnSCountableID { FGameplayTag m_Id { FName } }
    return callf(i, p) ? p.ret<int32_t>() : -1;
}
// every countable the inventory lists (tag + replicated count) - type 7 = Countable
static std::vector<std::string> countable_tags(const std::string& filter) {
    std::vector<std::string> out; uintptr_t i = inv(); if (!i) return out;
    Prop mc = F(i, "m_ItemContainers"); if (!mc) return out;
    Prop kp, vp; int32_t voff = 0, stride = 0; if (!ue::map_kv(mc, kp, vp, voff, stride)) return out;
    for (auto& kv : ue::map_entries(i, mc)) {
        uint8_t type = 0; rd_as(kv.first, type); if (type != 7) continue;
        Prop items = SF(vp.owner_struct, "Items"); if (!items) continue;
        auto arr = ue::array_of(kv.second, items); Prop ip = ue::array_inner(items);
        for (int k = 0; k < arr.num && k < 2000; ++k) {
            const uintptr_t e = (uintptr_t)arr.data + (uintptr_t)k * ip.size;
            uintptr_t base = 0; Prop tagp; if (!ue::resolve(e, ip.owner_struct, "m_Spec.m_ItemTag.TagName", base, tagp)) continue;
            std::string t = ue::name_str(rd<FName>(base, tagp));
            if (t != "None" && !t.empty() && (filter.empty() || t.find(filter) != std::string::npos)) out.push_back(t);
        }
    }
    return out;
}

// gold / currencies / materials: spawn the game's own loot pile and loot it (exactly like picking it up)
static uintptr_t load_class(const std::string& path) {
    if (uintptr_t c = ue::find_object(path)) return c;
    uintptr_t k = lib("KismetSystemLibrary"); if (!k) return 0;
    Params p = ue::params_for(k, "LoadClassAsset_Blocking"); if (!p) return 0;
    const size_t dot = path.rfind('.');
    FName pkg, asset; if (!ue::name_find(path.substr(0, dot), pkg) || !ue::name_find(path.substr(dot + 1), asset)) return 0;
    Prop sp = p.param("AssetClass");                           // FSoftObjectPtr { weak 8; FSoftObjectPath { PackageName, AssetName, SubPath FString } }
    memcpy(p.at(sp) + 0x08, &pkg, 8); memcpy(p.at(sp) + 0x10, &asset, 8);
    if (!callf(k, p)) return 0;
    return p.ret<uintptr_t>();
}
struct GiveResult { std::string err; int64_t before = -1, after = -1; };
static GiveResult give(const std::string& tag, int n) {
    GiveResult g;
    if (multiplayer()) { g.err = "refused: not a solo session"; return g; }
    if (n <= 0) { g.err = "amount must be > 0"; return g; }
    Roots r = roots(); if (!r.pawn || !r.pc) { g.err = "no character (not in gameplay?)"; return g; }
    FName tn; if (!tag_name(tag, tn)) { g.err = "unknown item " + tag; return g; }
    uintptr_t cls = load_class("/Game/Blueprints/Loot/LootActor/BP_CountableLootActorBase.BP_CountableLootActorBase_C");
    if (!cls) { g.err = "loot pile class not found"; return g; }
    uintptr_t gs = lib("GameplayStatics");
    Params loc = ue::params_for(r.pawn, "K2_GetActorLocation"); if (!loc || !callf(r.pawn, loc)) { g.err = "no position"; return g; }
    double xf[12] = { 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0 };   // FTransform (doubles): quat xyzw, translation xyz+pad, scale xyz+pad
    memcpy(&xf[4], loc.ret_ptr(), 24);
    g.before = count(tag);
    Params b = ue::params_for(gs, "BeginDeferredActorSpawnFromClass");
    b.put("WorldContextObject", r.pawn); b.put("ActorClass", cls); b.put_raw("SpawnTransform", xf, sizeof xf);
    b.put("CollisionHandlingOverride", (uint8_t)1); b.put("TransformScaleMethod", (uint8_t)0);
    std::string e; if (!callf(gs, b, &e)) { g.err = "spawn failed: " + e; return g; }
    const uintptr_t pile = b.ret<uintptr_t>(); if (!pile) { g.err = "spawn failed"; return g; }
    uintptr_t base = 0; Prop tp, cp;
    const bool okt = ue::resolve(pile, ue::class_of(pile), "m_Item.m_CountableID.m_Id.TagName", base, tp);
    if (okt) ue::set<FName>(base, tp, tn);
    if (ue::resolve(pile, ue::class_of(pile), "m_Item.m_nCounter", base, cp)) ue::set<int32_t>(base, cp, n);
    Params f = ue::params_for(gs, "FinishSpawningActor"); f.put("Actor", pile); f.put_raw("SpawnTransform", xf, sizeof xf); f.put("TransformScaleMethod", (uint8_t)0);
    callf(gs, f);
    Params l = ue::params_for(pile, "LootItem"); bool looted = false;
    if (l) { l.put("pController", r.pc); looted = callf(pile, l, &e); }
    if (ue::alive(pile)) { Params d = ue::params_for(pile, "K2_DestroyActor"); if (d) callf(pile, d); }
    g.after = count(tag);
    if (!okt) g.err = "pile has no item field"; else if (!looted) g.err = "loot failed: " + e;
    return g;
}

// ------------------------------------------------------------------ features (keeper) - port of features.lua
struct Def {
    std::string id, name, kind;              // toggle | value | tick | action
    double def = 1; std::string attr;
    std::function<std::string(double)> apply; std::function<bool(double)> check;
    std::function<void()> off; std::function<void(double)> tick;
};
static std::vector<Def> g_defs;
static std::map<std::string, Def*> g_by_id;
struct KState {
    std::map<std::string, double> active;     // feature -> value (toggles: 1)
    std::map<std::string, double> orig;       // per-pawn originals of attribute features
    std::map<std::string, std::string> errors;
    std::map<std::string, std::pair<int, ULONGLONG>> reapplies; std::set<std::string> stuck;
    uintptr_t pawn = 0; bool ready = false, had_pawn = false; ULONGLONG since = 0; int zone_changes = 0, reapplied = 0;
    std::string world; std::deque<std::string> log;
} K;
static void klog(const std::string& m) { SYSTEMTIME t; GetLocalTime(&t); K.log.push_back(fmt("%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond) + m); while (K.log.size() > 60) K.log.pop_front(); log("[dk] %s", m.c_str()); bump(); }

static std::string state_file() { return g_data_dir + "\\features.json"; }
static void kload() {
    std::ifstream f(state_file()); if (!f) return;
    json j = json::parse(f, nullptr, false); if (!j.is_object() || !j.contains("active") || !j["active"].is_object()) return;
    for (auto& kv : j["active"].items()) if (kv.value().is_number()) K.active[kv.key()] = kv.value().get<double>(); else if (kv.value().is_boolean() && kv.value().get<bool>()) K.active[kv.key()] = 1;
}
static void ksave() {
    json a = json::object(); for (auto& kv : K.active) a[kv.first] = kv.second;
    std::ofstream(state_file(), std::ios::trunc) << json{ {"active", a} }.dump(1);
}

// attribute value feature: base value through the game's setter
// ------------------------------------------------------------------ originals that survive a trainer reload
// Every "off" puts back the game's ORIGINAL value. Those originals used to live only in this DLL's memory: after a
// trainer reload (dev hot reload) the new copy read the already-cheated value as the original, so turning a feature
// off "restored" the cheat (bug found by the author 2026-11-09: fragment drop/divine stayed on after off). They are now
// kept in the game process environment: survives a trainer reload, dies with the game (when the game's own values
// reset anyway). Key -> text value.
static bool env_get(const std::string& key, std::string& out) {
    const DWORD need = GetEnvironmentVariableA(key.c_str(), nullptr, 0); if (need <= 1) return false;
    out.assign(need, ' '); GetEnvironmentVariableA(key.c_str(), out.data(), need); out.resize(need - 1); return true;
}
static void env_set(const std::string& key, const std::string& v) { SetEnvironmentVariableA(key.c_str(), v.c_str()); }
static void env_del(const std::string& key) { SetEnvironmentVariableA(key.c_str(), nullptr); }
static std::string attr_orig_key(const std::string& id, uintptr_t pawn) { return "DK_ORIG_" + id + "_" + hexstr(pawn); }
static Def attr_value(const char* id, const char* name, const char* at, double def) {
    Def d; d.id = id; d.name = name; d.kind = "value"; d.def = def; d.attr = at;
    std::string a = at;
    d.apply = [a](double v) { return set_attr(a, v); };
    d.check = [a](double v) { Attr x = attr(a); return x.ok && std::fabs(x.base - v) < 0.001; };
    return d;
}
// ------------------------------------------------------------------ attribute caps (the game's FHnSAttributeExtraData table)
// The game clamps each attribute to bounds from a DataTable copied into a native array (records of 0x1D8 bytes:
// m_Attribute FGameplayAttribute at +0x8 (AttributeName FString first), m_Max FHnSAttributeBound at +0xF8 = {u8 type,
// float constant}; type 1 = Constant). Found 2026-11-09: DodgeDistanceMultiplier max = Constant 2.0.
// The array is located by CONTENT (no fixed address): a TArray in the exe's writable sections whose records carry
// attribute names, validated by finding the attribute by name.
static constexpr size_t kExtraStride = 0x1D8, kExtraName = 0x8, kExtraMax = 0xF8;
static std::wstring wstr_at(uintptr_t fstring) {
    uintptr_t p = 0; int32_t n = 0; if (!rd_as(fstring, p) || !rd_as(fstring + 8, n) || !p || n <= 1 || n > 128) return L"";
    std::wstring w(n - 1, L'\0'); return mread(p, w.data(), (n - 1) * 2) ? w : L"";
}
static bool extra_table(uintptr_t& data, int32_t& num) {
    static uintptr_t c_data = 0; static int32_t c_num = 0; static bool searched = false;
    if (c_data) { data = c_data; num = c_num; return true; }
    if (searched) return false;
    searched = true;
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base; auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uintptr_t a0 = base + sec->VirtualAddress, a1 = a0 + sec->Misc.VirtualSize;
        std::vector<uint8_t> buf(sec->Misc.VirtualSize); if (!mread(a0, buf.data(), buf.size())) continue;
        for (size_t o = 0; o + 16 <= buf.size(); o += 8) {
            uintptr_t d; int32_t n, m; memcpy(&d, &buf[o], 8); memcpy(&n, &buf[o + 8], 4); memcpy(&m, &buf[o + 12], 4);
            if (!d || (d & 7) || n < 64 || n > 4096 || m < n || m > 8192 || (d >= a0 && d < a1)) continue;
            // records carry attribute names (some slots have no row = empty name): most of them must be named
            { uint8_t probe[16]; if (!mread(d, probe, sizeof probe) || !mread(d + (uintptr_t)(n - 1) * kExtraStride, probe, sizeof probe)) continue; }
            int named = 0, sampled = 0;
            for (int k = 0; k < n; k += 7, ++sampled) { const std::wstring w = wstr_at(d + k * kExtraStride + kExtraName); if (!w.empty() && iswupper(w[0])) ++named; }
            if (named * 2 < sampled) continue;
            c_data = d; c_num = n; log("[dk] attribute bounds table: %d records", n);
            data = d; num = n; return true;
        }
    }
    log("[dk] attribute bounds table not found");
    return false;
}
static uintptr_t extra_record(const std::string& attr_name) {
    uintptr_t d; int32_t n; if (!extra_table(d, n)) return 0;
    const std::wstring want(attr_name.begin(), attr_name.end());
    for (int32_t k = 0; k < n; ++k) if (wstr_at(d + k * kExtraStride + kExtraName) == want) return d + k * kExtraStride;
    return 0;
}
static std::map<std::string, std::pair<uint8_t, float>> g_cap_orig;      // attribute -> original max {type, constant}
// make sure the game lets the attribute reach v (only ever raises a Constant max; never touches other bound types)
static std::string raise_cap(const std::string& attr_name, double v) {
    const uintptr_t r = extra_record(attr_name); if (!r) return "bounds record not found for " + attr_name;
    uint8_t type = 0; float c = 0; rd_as(r + kExtraMax, type); rd_as(r + kExtraMax + 4, c);
    if (!g_cap_orig.count(attr_name)) {
        // the original is kept in the game PROCESS environment: it survives a trainer reload (a reloaded trainer would
        // otherwise take an already raised cap for the original) and dies with the game, like the patched table
        const std::string ev = "DK_CAP_" + attr_name; char buf[64] = {};
        if (GetEnvironmentVariableA(ev.c_str(), buf, sizeof buf)) { int t = 0; float f = 0; if (sscanf_s(buf, "%d %f", &t, &f) == 2) { type = (uint8_t)t; c = f; } }
        else { char v2[64]; sprintf_s(v2, "%d %.9g", (int)type, c); SetEnvironmentVariableA(ev.c_str(), v2); }
        g_cap_orig[attr_name] = { type, c };
    }
    const auto& o = g_cap_orig[attr_name];
    if (o.first != 1) return "";                                        // not a constant cap: nothing to raise
    const float want = (float)std::max<double>(o.second, v);
    if (type != 1 || c != want) { uint8_t t1 = 1; mwrite(r + kExtraMax, &t1, 1); mwrite(r + kExtraMax + 4, &want, 4); }
    return "";
}
static void restore_cap(const std::string& attr_name) {
    if (!g_cap_orig.count(attr_name)) {
        char buf[64] = {}; int t = 0; float f = 0;
        if (GetEnvironmentVariableA(("DK_CAP_" + attr_name).c_str(), buf, sizeof buf) && sscanf_s(buf, "%d %f", &t, &f) == 2) g_cap_orig[attr_name] = { (uint8_t)t, f };
    }
    auto it = g_cap_orig.find(attr_name); if (it == g_cap_orig.end()) return;
    if (const uintptr_t r = extra_record(attr_name)) { mwrite(r + kExtraMax, &it->second.first, 1); mwrite(r + kExtraMax + 4, &it->second.second, 4); }
    g_cap_orig.erase(it);
}
// attribute slider whose value can go past the game's own constant cap
static Def attr_value_uncapped(const char* id, const char* name, const char* at, double def) {
    Def d = attr_value(id, name, at, def); std::string a = at;
    d.apply = [a](double v) { std::string e = raise_cap(a, v); if (!e.empty()) log("[dk] %s", e.c_str()); return set_attr(a, v); };
    // off = the game's own default from the bounds table (m_fDefaultValue +0x78), not a remembered value: a value
    // remembered while the feature was already on (trainer reload) would be the cheated one
    const std::string fid = id;
    d.off = [a, fid]() {
        restore_cap(a);
        float dv = 0; const uintptr_t r = extra_record(a);
        if (r && rd_as(r + 0x78, dv)) { K.orig.erase(fid); env_del(attr_orig_key(fid, roots().pawn)); set_attr(a, dv); }
    };
    return d;
}
// ------------------------------------------------------------------ Ancestral Grid size (DT_GridModelData)
// Port of tools/grid.py (2026-11-09). The grid = a cell list per class in DT_GridModelData:
// UDataTable +0x30 RowMap {data, num, max}, element 24 bytes {FName, uint8* row, hash}; row +0x08 TArray<cell>
// {ptr, num, max}; cell 16 bytes {i32 col, i32 row, i32 m_nRequiredLevel, u8 m_bIsCitySlot}. Runtime only: the table
// resets every game start, so it is re-applied. Growing uses the GAME's allocator (ue::game_realloc).
// Limit (the author's test): size 2 = fill + 1 ring lines up; a 2nd ring no longer matches the grid's background art.
static const char* kGridDT = "/Game/Blueprints/UI/Menus/SkillGrid/DataTables/DT_GridModelData.DT_GridModelData";
#pragma pack(push, 1)
struct GCell { int32_t col, row, level; uint8_t city, pad[3]; };
#pragma pack(pop)
static_assert(sizeof(GCell) == 16, "cell size");
static std::map<std::string, std::vector<GCell>> g_grid_orig;
static uintptr_t g_grid_dt = 0;
static std::string g_grid_status = "-";
static bool g_grid_touched = false;
static bool grid_rows(std::vector<std::pair<std::string, uintptr_t>>& rows) {
    if (!g_grid_dt || !ue::alive(g_grid_dt)) g_grid_dt = ue::find_object(kGridDT, false);   // polled: never a full reindex
    if (!g_grid_dt) return false;
    uintptr_t data = 0; int32_t num = 0;
    if (!rd_as(g_grid_dt + 0x30, data) || !rd_as(g_grid_dt + 0x38, num) || !data || num <= 0 || num > 64) return false;
    for (int32_t i = 0; i < num; ++i) {
        FName n{}; uintptr_t row = 0;
        if (!mread(data + i * 24, &n, sizeof n) || !rd_as(data + i * 24 + 8, row) || !row) continue;
        rows.push_back({ ue::name_str(n), row });
    }
    return !rows.empty();
}
static bool grid_read(uintptr_t row, std::vector<GCell>& out, int32_t& mx) {
    uintptr_t arr = 0; int32_t n = 0; mx = 0;
    if (!rd_as(row + 8, arr) || !rd_as(row + 16, n) || !rd_as(row + 20, mx) || !arr || n < 0 || n > 4096 || mx < n) return false;
    out.resize(n); return n == 0 || mread(arr, out.data(), n * sizeof(GCell));
}
// the class's ORIGINAL cells: remembered in the game process environment (survives a trainer reload, dies with the game
// like the patched table) so a reloaded trainer never takes an already grown grid for the original
static const std::vector<GCell>* grid_orig(const std::string& name, uintptr_t row) {
    auto it = g_grid_orig.find(name); if (it != g_grid_orig.end()) return &it->second;
    const std::string ev = "DK_GRID_" + name; std::vector<GCell> cells;
    const DWORD need = GetEnvironmentVariableA(ev.c_str(), nullptr, 0);
    if (need > 1) {
        std::string hex(need, ' '); GetEnvironmentVariableA(ev.c_str(), hex.data(), need); hex.resize(need - 1);
        std::vector<uint8_t> b(hex.size() / 2); for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)std::stoi(hex.substr(i * 2, 2), nullptr, 16);
        cells.resize(b.size() / sizeof(GCell)); memcpy(cells.data(), b.data(), cells.size() * sizeof(GCell));
    } else {
        int32_t mx; if (!grid_read(row, cells, mx) || cells.empty()) return nullptr;
        std::string hex; hex.reserve(cells.size() * 32); const uint8_t* b = (const uint8_t*)cells.data();
        for (size_t i = 0; i < cells.size() * sizeof(GCell); ++i) hex += fmt("%02X", b[i]);
        SetEnvironmentVariableA(ev.c_str(), hex.c_str());
    }
    return &(g_grid_orig[name] = cells);
}
static std::vector<GCell> grid_want(const std::vector<GCell>& orig, int size, bool unlock) {
    std::vector<GCell> cells = orig;
    if (size >= 1 && !orig.empty()) {
        int c0 = INT_MAX, c1 = INT_MIN, r0 = INT_MAX, r1 = INT_MIN;
        for (auto& c : orig) { c0 = std::min(c0, c.col); c1 = std::max(c1, c.col); r0 = std::min(r0, c.row); r1 = std::max(r1, c.row); }
        const int ring = std::max(0, size - 1); c0 -= ring; c1 += ring; r0 -= ring; r1 += ring;
        std::set<std::pair<int, int>> have; for (auto& c : orig) have.insert({ c.col, c.row });
        for (int c = c0; c <= c1; ++c) for (int r = r0; r <= r1; ++r)
            if (!have.count({ c, r })) { GCell n{}; n.col = c; n.row = r; n.level = 1; cells.push_back(n); }
    }
    if (unlock) for (auto& c : cells) if (c.level > 1) c.level = 1;
    return cells;
}
// bring every class grid to the chosen state (size 0-2, unlock); "" = ok
static std::string grid_sync() {
    const bool mp = multiplayer();                                     // multiplayer: the game's own grid
    auto sz = K.active.find("grid_size"); const int size = mp || sz == K.active.end() ? 0 : (int)std::lround(std::clamp(sz->second, 0.0, 2.0));
    const bool unlock = !mp && K.active.count("grid_unlock") != 0;
    if (!size && !unlock && !g_grid_touched) return "";
    std::vector<std::pair<std::string, uintptr_t>> rows; if (!grid_rows(rows)) { g_grid_status = "grid table not loaded yet"; return ""; }
    std::string st, err;
    for (auto& rw : rows) {
        const std::string& name = rw.first; const uintptr_t row = rw.second;
        const std::vector<GCell>* orig = grid_orig(name, row); if (!orig) continue;
        const std::vector<GCell> want = grid_want(*orig, size, unlock);
        std::vector<GCell> cur; int32_t mx = 0; if (!grid_read(row, cur, mx)) continue;
        if (cur.size() != want.size() || memcmp(cur.data(), want.data(), want.size() * sizeof(GCell)) != 0) {
            uintptr_t arr = 0; rd_as(row + 8, arr);
            const int32_t n = (int32_t)want.size();
            if (n > mx) {
                void* np = ue::game_realloc((void*)arr, n * sizeof(GCell), 16);
                if (!np) { err = "could not grow the " + name + " grid (game allocator not found)"; continue; }
                arr = (uintptr_t)np; mwrite(row + 8, &arr, 8); mwrite(row + 20, &n, 4);
            }
            mwrite(arr, want.data(), n * sizeof(GCell)); mwrite(row + 16, &n, 4);
            g_grid_touched = true;
            klog(fmt("grid %s: %zu -> %d cells%s", name.c_str(), cur.size(), n, unlock ? " (unlocked)" : ""));
        }
        st += (st.empty() ? "" : ", ") + name + " " + std::to_string(want.size());
    }
    g_grid_status = st.empty() ? "-" : st;
    if (!size && !unlock) g_grid_touched = false;           // back to the original everywhere
    return err;
}
// ------------------------------------------------------------------ every grid modifier on every skill (2026-11-09)
// the author's rule: the power of EVERY modifier hex on the grid counts for EVERY skill, touching or not.
// - The grid's result lives on the pawn: AHnSPlayerCharacter.m_CurrentAbilityModifiers = TArray<{abilityCDO,
//   TArray<FHnSAbilityModifierSpec>}>, spec {modifierCDO, u8 rank, owner, abilityCDO, handle}.
// - Power per modifier hex = (rarity + 1) + (divine ? 2 : 0) (from the game's tooltips); summed over every fragment on
//   the grid; rank = the modifier's own FloorToLowestRank(power) (ladder 1/3/7/15/30).
// - Each player skill gets that rank: missing -> the game's TryBindAbilityModifier; present lower -> rank raised in place.
//   Never lowered (gear/codex power can make the game's own rank higher). Wyrmling abilities (GA_MOD_*) are left alone.
// - The game REBUILDS the lists on every fragment move / zone change, so this re-syncs every second. Off: there is no
//   unbind; the next rebuild (move a fragment / change zone) restores the game's own links.
json editor_call(const std::string& fn, const json& args);   // dk_editor.cpp
std::string gridlink_set(bool on, bool* fresh = nullptr);  // dk_gridhook.cpp
long gridlink_links();                                     // dk_gridhook.cpp
int gridlink_link_graph(uintptr_t graph);                  // dk_gridhook.cpp
std::map<std::string, int> editor_grid_power();            // dk_editor.cpp
static std::string g_mods_status = "-";
static std::map<std::string, uintptr_t> g_mod_by_tag;        // modifier tag -> CDO
static uintptr_t mod_cdo(const std::string& tag) {
    auto it = g_mod_by_tag.find(tag); if (it != g_mod_by_tag.end() && ue::alive(it->second)) return it->second;
    static ULONGLONG last_scan = 0; if (GetTickCount64() - last_scan < 30000 && last_scan) return 0;   // one full scan / 30 s max
    last_scan = GetTickCount64();
    static uintptr_t cls = 0; if (!cls) cls = ue::find_class("HnSAbilityModifierBase"); if (!cls) return 0;
    for (uintptr_t o : ue::find_all(cls, true)) {
        if (!ue::is_cdo(o)) continue;
        Prop tp = F(o, "m_IdTag"); if (!tp) continue;
        FName n{}; if (!mread(o + tp.offset, &n, sizeof n)) continue;
        const std::string t = ue::name_str(n); if (!t.empty() && t != "None") g_mod_by_tag[t] = o;
    }
    it = g_mod_by_tag.find(tag); return it == g_mod_by_tag.end() ? 0 : it->second;
}
static uint8_t mod_rank(uintptr_t cdo, int power) {
    Params p = ue::params_for(cdo, "FloorToLowestRank"); if (!p) return 0;
    const uint8_t pw = (uint8_t)std::clamp(power, 0, 255); memcpy(p.at(p.param("nCombinedPower")), &pw, 1);
    return callf(cdo, p) ? p.ret<uint8_t>() : 0;
}
static void mods_everywhere_sync() {
    Roots r = roots(); if (!r.pawn || !K.ready) return;
    Prop mp = F(r.pawn, "m_CurrentAbilityModifiers"); if (!mp) { g_mods_status = "no modifier list"; return; }
    Params bp0 = ue::params_for(r.pawn, "TryBindAbilityModifier"); if (!bp0) { g_mods_status = "no bind function"; return; }
    const Prop specp = bp0.param("AbilityModifierSpec");
    if (!specp || specp.size < 0x20 || specp.size > 0x80 || !bp0.param("pBoundAbility")) { g_mods_status = "unexpected bind layout"; return; }
    const size_t SS = specp.size;
    // 1) grid-wide power per modifier tag (cached 2 s: reading every fragment is the expensive part)
    static std::map<std::string, int> power; static ULONGLONG power_at = 0; static uintptr_t power_pawn = 0;
    if (GetTickCount64() - power_at > 2000 || power_pawn != r.pawn) {
        power = editor_grid_power(); power_at = GetTickCount64(); power_pawn = r.pawn;
    }
    int total = 0; for (auto& pw : power) total += pw.second;                 // whole-grid effective power -> skill rank
    std::map<uintptr_t, uint8_t> want;                                         // modifier CDO -> rank for every skill
    for (auto& pw : power) { const uintptr_t c = mod_cdo(pw.first); if (c) want[c] = mod_rank(c, pw.second); }
    // 2) every player skill gets every one of them at that rank
    uintptr_t items = 0; int32_t n = 0;
    if (!rd_as(r.pawn + mp.offset, items) || !rd_as(r.pawn + mp.offset + 8, n) || !items || n <= 0 || n > 64) { g_mods_status = "no skills yet"; return; }
    int added = 0, raised = 0, failed = 0, skills = 0;
    for (int32_t k = 0; k < n; ++k) {
        const uintptr_t it = items + (uintptr_t)k * 0x18; uintptr_t ab = 0, sp = 0; int32_t sn = 0;
        if (!rd_as(it, ab) || !rd_as(it + 8, sp) || !rd_as(it + 16, sn) || !ab || !sp || sn <= 0 || sn > 256) continue;
        if (ue::name_of(ab).find("GA_MOD_") != std::string::npos) continue;   // wyrmling ability
        ++skills;
        std::vector<uint8_t> first(SS); if (!mread(sp, first.data(), SS)) continue;
        std::map<uintptr_t, uintptr_t> has;                                    // modifier CDO -> its spec address
        for (int32_t j = 0; j < sn; ++j) { uintptr_t m = 0; if (rd_as(sp + j * SS, m) && m) has[m] = sp + j * SS; }
        // the skill's own RANK (I-IV: damage, cost, cooldown...) = its RANKSKILL_* entries; rank from the whole grid's power
        for (auto& h : has) {
            if (ue::name_of(h.first).find("RANKSKILL_") == std::string::npos) continue;
            const uint8_t tr = mod_rank(h.first, total); uint8_t cur = 0; rd_as(h.second + 8, cur);
            if (cur < tr) { mwrite(h.second + 8, &tr, 1); ++raised; }
        }
        for (auto& w : want) {
            auto h = has.find(w.first);
            if (h != has.end()) {                                              // present: raise, never lower
                uint8_t cur = 0; rd_as(h->second + 8, cur);
                if (cur < w.second) { mwrite(h->second + 8, &w.second, 1); ++raised; }
                continue;
            }
            Params c = ue::params_for(r.pawn, "TryBindAbilityModifier"); if (!c) break;
            std::vector<uint8_t> spec = first; memcpy(spec.data(), &w.first, 8); spec[8] = w.second;
            memcpy(c.at(c.param("AbilityModifierSpec")), spec.data(), SS); memcpy(c.at(c.param("pBoundAbility")), &ab, 8);
            std::string err; if (callf(r.pawn, c, &err) && c.ret<bool>()) ++added; else ++failed;
        }
    }
    if (added || raised || failed) klog(fmt("modifiers everywhere: +%d bound, %d raised%s", added, raised, failed ? fmt(", %d refused", failed).c_str() : ""));
    std::string top; int best = 0; for (auto& pw : power) if (pw.second > best) { best = pw.second; top = pw.first; }
    g_mods_status = fmt("%zu modifiers on %d skills", want.size(), skills) + (top.empty() ? "" : fmt(" (top: %s power %d, grid total %d)", top.substr(top.rfind('.') + 1).c_str(), best, total));
}

// ------------------------------------------------------------------ talents (2026-11-09)
// The Talents page shows UHnSTalentComponent.m_TalentPointsFromAllSources = the sum of 7 per-source lists
// (m_CurrentTalentPoints[EHnSTalentSource]: 0 Equipment ... 6 DEBUG). The trainer only ever writes the game's own
// DEBUG source through UHnSTalentBlueprintLibrary::AddTalentPoints, then ComputeTalentPoints + RefreshTalents, so
// gear / codex / dragon points stay exactly as the game made them. Ranks need 1 / 3 / 6 points (m_pTalentFloors).
// Settings: "talents_max" (every talent to 6) and "talent:<tag>" = extra points for one talent (saved like features).
// The debug source is runtime only (a new character / zone = a new component), so this re-syncs once a second.
static constexpr int kTalentDebug = 6, kTalentMax = 6;
struct TalentInfo { std::string tag, name, desc; std::vector<int> floors; int total = 0, debug = 0; };
static uintptr_t talent_comp() { return player_component("HnSTalentComponent"); }
static std::map<std::string, int> talent_map(uintptr_t base, const Prop& mp) {
    std::map<std::string, int> out; if (!mp) return out;
    for (auto& kv : ue::map_entries(base, mp)) { FName n{}; mread(kv.first, &n, sizeof n); int32_t v = 0; rd_as(kv.second, v); out[ue::name_str(n)] = v; }
    return out;
}
static std::map<std::string, int> talent_debug_points(uintptr_t comp) {
    Prop ap = F(comp, "m_CurrentTalentPoints"); if (!ap) return {};
    auto arr = ue::array_of(comp, ap); Prop ip = ue::array_inner(ap); if (arr.num <= kTalentDebug || !ip.owner_struct) return {};
    Prop mp = ue::find_prop(ip.owner_struct, "m_Magnitudes"); if (!mp) return {};
    return talent_map((uintptr_t)arr.data + (uintptr_t)kTalentDebug * ip.size, mp);
}
static std::vector<TalentInfo> talent_list(bool with_text) {
    std::vector<TalentInfo> out; const uintptr_t comp = talent_comp(); if (!comp) return out;
    const auto total = talent_map(comp, F(comp, "m_TalentPointsFromAllSources"));
    const auto debug = talent_debug_points(comp);
    static std::vector<uintptr_t> cdos; static ULONGLONG at = 0;
    if (cdos.empty() || GetTickCount64() - at > 60000) {               // the talent assets: one class scan a minute at most
        at = GetTickCount64(); cdos.clear(); static uintptr_t cls = 0; if (!cls) cls = ue::find_class("HnSTalentBase");
        for (uintptr_t o : ue::find_all(cls, true)) if (ue::is_cdo(o)) cdos.push_back(o);
    }
    for (uintptr_t o : cdos) {
        if (!ue::alive(o)) continue;
        Prop tp = F(o, "m_IdTag"); if (!tp) continue; FName n{}; mread(o + tp.offset, &n, sizeof n);
        TalentInfo t; t.tag = ue::name_str(n); if (t.tag.empty() || t.tag == "None") continue;
        {   // class talents only for the character's own class. Primary: the class NUMBER - the talent's eClass vs the
            // character's m_eCharacterType (Knight 0, Witch 1, Barbarian 2, Archer 3...; 8 = every class: common /
            // wyrmling). Works for classes added later (new class 2026-11-12). Fallback: tag segment vs pawn class name.
            const uintptr_t pw = roots().pawn;
            Prop tc = F(o, "eClass"); const int tcls = tc ? (int)ue::get_num(o, tc) : -1;
            Prop pc = pw ? F(pw, "m_eCharacterType") : Prop(); const int pcls = pc ? (int)ue::get_num(pw, pc) : -1;
            if (tcls >= 0 && pcls >= 0) { if (tcls != 8 && tcls != pcls) continue; }
            else {
                const size_t k = t.tag.find("Talent."); std::string seg = k == std::string::npos ? "" : t.tag.substr(k + 7);
                seg = seg.substr(0, seg.find('.'));
                if (!seg.empty() && seg != "Common" && seg != "Wyrmling") {
                    const std::string pn = pw ? ue::name_of(ue::class_of(pw)) : "";
                    if (pn.empty() || pn.find(seg) == std::string::npos) continue;
                }
            }
        }
        Prop fp = F(o, "m_pTalentFloors");
        if (fp) { auto arr = ue::array_of(o, fp); Prop ip = ue::array_inner(fp);
            for (int k = 0; k < arr.num && k < 16; ++k) { int32_t need = 0; rd_as((uintptr_t)arr.data + k * ip.size, need); t.floors.push_back(need); } }
        if (with_text) { Prop np = F(o, "m_NameText"), dp = F(o, "m_DescriptionText");
            if (np) t.name = ue::ftext(o + np.offset); if (dp) t.desc = ue::ftext(o + dp.offset); }
        auto a = total.find(t.tag); t.total = a == total.end() ? 0 : a->second;
        auto d = debug.find(t.tag); t.debug = d == debug.end() ? 0 : d->second;
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const TalentInfo& a, const TalentInfo& b) { return a.tag < b.tag; });
    return out;
}
static bool g_talent_touched = false;
static void talent_sync() {
    const bool max_all = K.active.count("talents_max") != 0;
    bool any_extra = false; for (auto& kv : K.active) if (kv.first.rfind("talent:", 0) == 0) { any_extra = true; break; }
    if (!max_all && !any_extra && !g_talent_touched) return;
    const uintptr_t comp = talent_comp(); if (!comp || !K.ready) return;
    uintptr_t L = lib("HnSTalentBlueprintLibrary"); if (!L) return;
    const bool online = multiplayer();                                 // multiplayer: take the trainer's points away
    int changed = 0, still = 0;
    for (auto& t : talent_list(false)) {
        const int gear = t.total - t.debug;                              // everything the game itself gave
        auto ex = K.active.find("talent:" + t.tag);
        int want = ex == K.active.end() ? 0 : (int)std::lround(ex->second);
        if (max_all) want = std::max(want, kTalentMax - gear);
        want = online ? 0 : std::max(0, want);
        if (want) ++still;
        const int delta = want - t.debug; if (!delta) continue;
        Params p = ue::params_for(L, "AddTalentPoints"); if (!p) return;
        FName tn{}; if (!ue::name_find(t.tag, tn)) continue;
        p.put("pComponent", comp); const uint8_t src = kTalentDebug; memcpy(p.at(p.param("eSource")), &src, 1);
        memcpy(p.at(p.param("TalentTag")), &tn, 8); const int32_t n = delta; memcpy(p.at(p.param("nPointsToAdd")), &n, 4);
        if (callf(L, p)) ++changed;
    }
    if (changed) {
        for (const char* fn : { "ComputeTalentPoints", "RefreshTalents" }) { Params c = ue::params_for(comp, fn); if (c) callf(comp, c); }
        klog(fmt("talents: %d updated", changed));
    }
    g_talent_touched = still > 0;
}
static json talent_json() {
    json out = json::array();
    for (auto& t : talent_list(true)) {
        auto ex = K.active.find("talent:" + t.tag);
        out.push_back({ {"tag", t.tag}, {"name", t.name}, {"desc", t.desc}, {"floors", t.floors}, {"total", t.total},
                        {"gear", t.total - t.debug}, {"extra", ex == K.active.end() ? 0 : (int)std::lround(ex->second)} });
    }
    return out;
}
static Def refill_tick(const char* id, const char* name, const char* cur, const char* max) {
    Def d; d.id = id; d.name = name; d.kind = "tick"; std::string c = cur, m = max;
    d.tick = [c, m](double) { refill(c.c_str(), m.c_str()); };
    return d;
}
// bool switch on the ability system component (the game's own developer switches)
static Def asc_bool(const char* id, const char* name, const char* field) {
    Def d; d.id = id; d.name = name; d.kind = "toggle"; std::string f = field;
    d.apply = [f](double) -> std::string { uintptr_t a = asc(); Prop p = F(a, f); if (!a || !p) return "no " + f; ue::set_bool(a, p, true); return ""; };
    d.check = [f](double) { uintptr_t a = asc(); Prop p = F(a, f); return a && p && ue::get_bool(a, p); };
    d.off = [f]() { uintptr_t a = asc(); Prop p = F(a, f); if (a && p) ue::set_bool(a, p, false); };
    return d;
}

// --- fragment loot tables (HnSLootListSet assets live for the whole session; originals captured once)
struct LootTier { std::vector<std::pair<float, int>> nb; std::vector<float> w; std::vector<int> q; std::vector<bool> can, force; };
static std::map<uintptr_t, std::vector<LootTier>> g_loot_orig;
static std::vector<uintptr_t> loot_lists() {
    static std::vector<uintptr_t> cache; static ULONGLONG at = 0; static uintptr_t for_pawn = 1;
    bool stale = at == 0 || (for_pawn != K.pawn && GetTickCount64() - at > 5000); for (uintptr_t o : cache) if (!ue::alive(o)) { stale = true; break; }
    for_pawn = K.pawn;
    if (!stale) return cache;
    std::vector<uintptr_t> out; static uintptr_t cls = 0; if (!cls) cls = ue::find_class("HnSLootListSet");
    for (uintptr_t o : ue::find_all(cls)) { std::string n = ue::name_of(o); if (n.find("Choice") == std::string::npos && n.find("Tuto") == std::string::npos) out.push_back(o); }
    cache = out; at = GetTickCount64();
    return out;
}
struct LootView { uintptr_t tiers = 0; int ntiers = 0; Prop tierp, nbp, nbinner, dlp, dlinner, chance, nnb, mult, qual, can, force; int tier_size = 0; };
static bool loot_view(uintptr_t o, LootView& v) {
    Prop L = F(o, "m_AncestralFragmentLootDropList"); if (!L) return false;
    auto arr = ue::array_of(o, L); v.tiers = (uintptr_t)arr.data; v.ntiers = arr.num;
    v.tierp = ue::array_inner(L); v.tier_size = v.tierp.size;
    v.nbp = SF(v.tierp.owner_struct, "m_DropNbChances"); v.dlp = SF(v.tierp.owner_struct, "m_DropList");
    if (!v.nbp || !v.dlp) return false;
    v.nbinner = ue::array_inner(v.nbp); v.dlinner = ue::array_inner(v.dlp);
    v.chance = SF(v.nbinner.owner_struct, "m_fDropChance"); v.nnb = SF(v.nbinner.owner_struct, "m_nNbDrop");
    v.mult = SF(v.dlinner.owner_struct, "m_fDropChanceMultiplier"); v.qual = SF(v.dlinner.owner_struct, "m_eLootQuality");
    v.can = SF(v.dlinner.owner_struct, "m_bCanBeDivine"); v.force = SF(v.dlinner.owner_struct, "m_bForceDivine");
    return v.chance && v.nnb && v.mult && v.qual && v.can && v.force;
}
static const std::vector<LootTier>* loot_orig(uintptr_t o) {
    auto it = g_loot_orig.find(o); if (it != g_loot_orig.end()) return &it->second;
    const std::string ek = "DK_LOOT_" + hexstr(o); std::string ev;
    if (env_get(ek, ev)) {                                    // captured by an earlier copy of the trainer in this game
        json j = json::parse(ev, nullptr, false);
        if (j.is_array()) {
            std::vector<LootTier> t;
            for (auto& tj : j) { LootTier lt;
                for (auto& x : tj[0]) lt.nb.push_back({ x[0].get<float>(), x[1].get<int>() });
                for (auto& x : tj[1]) lt.w.push_back(x.get<float>());
                for (auto& x : tj[2]) lt.q.push_back(x.get<int>());
                for (auto& x : tj[3]) lt.can.push_back(x.get<int>() != 0);
                for (auto& x : tj[4]) lt.force.push_back(x.get<int>() != 0);
                t.push_back(lt); }
            return &(g_loot_orig[o] = t);
        }
    }
    LootView v; if (!loot_view(o, v)) return nullptr;
    std::vector<LootTier> t(v.ntiers);
    for (int i = 0; i < v.ntiers; ++i) {
        const uintptr_t e = v.tiers + (uintptr_t)i * v.tier_size;
        auto nb = ue::array_of(e, v.nbp);
        for (int j = 0; j < nb.num; ++j) { uintptr_t c = (uintptr_t)nb.data + j * v.nbinner.size; t[i].nb.push_back({ rd<float>(c, v.chance), rd<int32_t>(c, v.nnb) }); }
        auto dl = ue::array_of(e, v.dlp);
        for (int j = 0; j < dl.num; ++j) { uintptr_t d = (uintptr_t)dl.data + j * v.dlinner.size;
            t[i].w.push_back(rd<float>(d, v.mult)); t[i].q.push_back((int)ue::get_num(d, v.qual)); t[i].can.push_back(ue::get_bool(d, v.can)); t[i].force.push_back(ue::get_bool(d, v.force)); }
    }
    {   json j = json::array();
        for (auto& lt : t) { json nb = json::array(), w = json::array(), q = json::array(), can = json::array(), force = json::array();
            for (auto& x : lt.nb) nb.push_back({ x.first, x.second });
            for (auto x : lt.w) w.push_back(x); for (auto x : lt.q) q.push_back(x);
            for (bool x : lt.can) can.push_back(x ? 1 : 0); for (bool x : lt.force) force.push_back(x ? 1 : 0);
            j.push_back({ nb, w, q, can, force }); }
        env_set(ek, j.dump());
    }
    return &(g_loot_orig[o] = t);
}
static void loot_settings(double& M, double& B, bool& D) {
    auto g = [](const char* k, double dv) { auto it = K.active.find(k); return it == K.active.end() ? dv : it->second; };
    M = g("frag_drop", 1); B = g("frag_rarity", 1); D = K.active.count("frag_divine") != 0;
}
static void loot_want(const LootTier& r, double M, double B, bool D, std::vector<float>& nb, std::vector<float>& w, std::vector<bool>& can, std::vector<bool>& force) {
    double pos = 0, total = 0; for (auto& c : r.nb) { total += c.first; if (c.second > 0) pos += c.first; }
    double scale = M; if (pos > 0 && pos * scale > total) scale = total / pos;     // cap: "always drops"
    nb.clear(); for (auto& c : r.nb) nb.push_back(pos <= 0 ? c.first : c.second > 0 ? (float)(c.first * scale) : (float)std::max(0.0, c.first - (scale - 1) * pos));
    w.clear(); can.clear(); force.clear();
    for (size_t j = 0; j < r.w.size(); ++j) { w.push_back((float)(r.w[j] * std::pow(B, r.q[j]))); can.push_back(D || r.can[j]); force.push_back(D || r.force[j]); }
}
static std::string loot_apply() {
    double M, B; bool D; loot_settings(M, B, D); int n = 0;
    for (uintptr_t o : loot_lists()) {
        const auto* t = loot_orig(o); LootView v; if (!t || !loot_view(o, v)) continue;
        for (int i = 0; i < v.ntiers && i < (int)t->size(); ++i) {
            std::vector<float> nb, w; std::vector<bool> can, force; loot_want((*t)[i], M, B, D, nb, w, can, force);
            const uintptr_t e = v.tiers + (uintptr_t)i * v.tier_size;
            auto nba = ue::array_of(e, v.nbp); auto dla = ue::array_of(e, v.dlp);
            for (int j = 0; j < nba.num && j < (int)nb.size(); ++j) ue::set<float>((uintptr_t)nba.data + j * v.nbinner.size, v.chance, nb[j]);
            for (int j = 0; j < dla.num && j < (int)w.size(); ++j) { uintptr_t d = (uintptr_t)dla.data + j * v.dlinner.size;
                ue::set<float>(d, v.mult, w[j]); ue::set_bool(d, v.can, can[j]); ue::set_bool(d, v.force, force[j]); }
            ++n;
        }
    }
    return n ? "" : "no loot tables found";
}
static bool loot_check() {
    double M, B; bool D; loot_settings(M, B, D);
    for (uintptr_t o : loot_lists()) {
        const auto* t = loot_orig(o); LootView v; if (!t || !loot_view(o, v)) continue;
        for (int i = 0; i < v.ntiers && i < (int)t->size(); ++i) {
            std::vector<float> nb, w; std::vector<bool> can, force; loot_want((*t)[i], M, B, D, nb, w, can, force);
            const uintptr_t e = v.tiers + (uintptr_t)i * v.tier_size;
            auto nba = ue::array_of(e, v.nbp); auto dla = ue::array_of(e, v.dlp);
            if (nba.num && !nb.empty() && std::fabs(rd<float>((uintptr_t)nba.data, v.chance) - nb[0]) > 1e-3) return false;
            if (dla.num && !w.empty() && (std::fabs(rd<float>((uintptr_t)dla.data, v.mult) - w[0]) > 1e-3 || ue::get_bool((uintptr_t)dla.data, v.force) != force[0])) return false;
        }
    }
    return true;
}
static Def loot_feature(const char* id, const char* name, const char* kind, double def) {
    Def d; d.id = id; d.name = name; d.kind = kind; d.def = def;
    d.apply = [](double) { return loot_apply(); }; d.check = [](double) { return loot_check(); }; d.off = []() { loot_apply(); };
    return d;
}

// --- global config CDO (Default__HnSGlobalConfig): storage capacities, floating cap, pity timer, divine chance
static uintptr_t gcfg() { return cdo("/Script/HnS.Default__HnSGlobalConfig"); }
static std::map<std::string, double> g_cfg_orig;
static bool cfg_slot(const std::string& field, uintptr_t& base, Prop& p, int map_key = -1) {
    uintptr_t g = gcfg(); if (!g) return false;
    if (map_key < 0) return ue::resolve(g, ue::class_of(g), field, base, p);
    uintptr_t mb = 0; Prop mp; if (!ue::resolve(g, ue::class_of(g), field, mb, mp)) return false;
    Prop kp, vp; int32_t voff, stride; if (!ue::map_kv(mp, kp, vp, voff, stride)) return false;
    for (auto& kv : ue::map_entries(mb, mp)) { if ((int)ue::get_num(kv.first - kp.offset, kp) == map_key) { base = kv.second - vp.offset; p = vp; return true; } }
    return false;
}
static Def cfg_feature(const char* id, const char* name, const char* field, double def, int map_key = -1) {
    Def d; d.id = id; d.name = name; d.kind = "value"; d.def = def; std::string f = field; std::string key = std::string(id);
    d.apply = [f, key, map_key](double v) -> std::string { uintptr_t b; Prop p; if (!cfg_slot(f, b, p, map_key)) return "global config field not found";
        if (!g_cfg_orig.count(key)) { std::string ev;
            if (env_get("DK_CFG_" + key, ev)) g_cfg_orig[key] = atof(ev.c_str());
            else { g_cfg_orig[key] = ue::get_num(b, p); env_set("DK_CFG_" + key, fmt("%.9g", g_cfg_orig[key])); } }
        ue::set_num(b, p, v); return ""; };
    d.check = [f, map_key](double v) { uintptr_t b; Prop p; return cfg_slot(f, b, p, map_key) && std::fabs(ue::get_num(b, p) - v) < 1e-4; };
    d.off = [f, key, map_key]() {
        uintptr_t b; Prop p; double ov = 0; bool have = false; std::string ev;
        auto it = g_cfg_orig.find(key); if (it != g_cfg_orig.end()) { ov = it->second; have = true; } else if (env_get("DK_CFG_" + key, ev)) { ov = atof(ev.c_str()); have = true; }
        if (have && cfg_slot(f, b, p, map_key)) ue::set_num(b, p, ov);
        g_cfg_orig.erase(key); env_del("DK_CFG_" + key);
    };
    return d;
}

static void build_defs() {
    g_defs.clear();
    g_defs.push_back(asc_bool("no_cooldowns", "No skill cooldowns", "m_bIgnoreCooldowns"));
    g_defs.push_back(asc_bool("no_costs", "No mana / resource cost", "m_bIgnoreCosts"));
    { Def d; d.id = "god"; d.name = "God mode (99% damage reduction + refill)"; d.kind = "tick"; d.attr = "DamageReductionPercent";
      d.apply = [](double) { return set_attr("DamageReductionPercent", 0.99); };
      d.check = [](double) { Attr a = attr("DamageReductionPercent"); return a.ok && a.base >= 0.98f; };
      d.tick = [](double) { refill("Health", "HealthMax"); refill("Shield", "ShieldMax"); };
      g_defs.push_back(d); }
    g_defs.push_back(refill_tick("inf_mana", "Infinite mana", "Mana", "ManaMax"));
    // barrier = the blue part of the health bar: Shield, capped by TotalMaxBarrier (bounds table "max = attribute");
    // ShieldMax (no cap) feeds it, gear/talents add on top of its base (2026-11-09)
    g_defs.push_back(refill_tick("inf_barrier", "Infinite barrier", "Shield", "TotalMaxBarrier"));
    g_defs.push_back(attr_value("barrier_max", "Barrier size", "ShieldMax", 500));
    { Def d; d.id = "invincible"; d.name = "Invincible (game's own switch)"; d.kind = "toggle";
      static uintptr_t inv_pawn = 0;
      d.apply = [](double) -> std::string { uintptr_t p = roots().pawn; Params a = ue::params_for(p, "SetInvincibility"); if (!p || !a) return "no character";
          a.put_bool("bEnable", true); std::string e; if (!callf(p, a, &e)) return e; inv_pawn = p; return ""; };
      d.check = [](double) { return inv_pawn && inv_pawn == roots().pawn; };
      d.off = []() { uintptr_t p = roots().pawn; Params a = ue::params_for(p, "SetInvincibility"); if (p && a) { a.put_bool("bEnable", false); callf(p, a); } inv_pawn = 0; };
      g_defs.push_back(d); }
    g_defs.push_back(refill_tick("inf_ultimate", "Ultimate always ready", "UltimateSkillJauge", "UltimateSkillJaugeMax"));
    g_defs.push_back(refill_tick("inf_potions", "Infinite potions", "PotionCharges", "PotionChargesMax"));
    g_defs.push_back(refill_tick("inf_dodge", "Infinite dodges", "DodgeCharges", "DodgeChargesMax"));
    g_defs.push_back(refill_tick("inf_rage", "Infinite rage", "CurrentRage", "RageMax"));
    { Def d; d.id = "grid_link"; d.name = "Every fragment touches every fragment"; d.kind = "toggle"; d.def = 1;
      d.apply = [](double) { std::string e = gridlink_set(true); if (!e.empty()) return e;
          uintptr_t i = inv(); Prop gp = i ? F(i, "m_pSkillGridGraph") : Prop(); uintptr_t g = gp ? rd<uintptr_t>(i, gp) : 0;
          if (g) { int n = gridlink_link_graph(g); if (n) klog(fmt("grid link: %d links added to the current grid", n)); }
          return std::string(); };
      d.check = [](double) {                       // 1/s: hook still in place (trainer reloads restore the game code)?
          bool fresh = false; const bool solo = !multiplayer(); gridlink_set(solo, &fresh);
          if (!solo) return true;                                      // multiplayer: hook switched off, nothing linked
          uintptr_t i = inv(); Prop gp = i ? F(i, "m_pSkillGridGraph") : Prop(); uintptr_t g = gp ? rd<uintptr_t>(i, gp) : 0;
          if (g) { int n = gridlink_link_graph(g); if (n) klog(fmt("grid link: %d missing links added%s", n, fresh ? " (hook re-installed)" : "")); }
          return true; };
      d.off = []() { gridlink_set(false); };
      g_defs.push_back(d); }
    // ("mods_everywhere" removed 2026-11-09: replaced by the grid link hook "grid_link"; the two fed each other a rebuild loop)
    { Def d; d.id = "grid_size"; d.name = "Ancestral Grid size"; d.kind = "value"; d.def = 1;
      d.apply = [](double) { return grid_sync(); }; d.off = []() { grid_sync(); }; g_defs.push_back(d); }
    { Def d; d.id = "grid_unlock"; d.name = "Unlock every grid cell"; d.kind = "toggle"; d.def = 1;
      d.apply = [](double) { return grid_sync(); }; d.off = []() { grid_sync(); }; g_defs.push_back(d); }
    g_defs.push_back(attr_value_uncapped("dodge_distance", "Dodge distance x", "DodgeDistanceMultiplier", 2));
    g_defs.push_back(attr_value("attack_speed", "Attack / animation speed x", "AnimSpeedMultiplier", 2));
    g_defs.push_back(attr_value("extra_projectiles", "Extra projectiles", "ProjectileCountBonus", 3));
    g_defs.push_back(attr_value("area_size", "Skill area size x", "AreaSizeMultiplier", 2));
    g_defs.push_back(attr_value("cdr", "Cooldown reduction (0-1)", "CooldownReductionPercent", 0.5));
    g_defs.push_back(attr_value("lifesteal", "Lifesteal (0-1)", "LifestealPercent", 0.2));
    g_defs.push_back(attr_value("health_regen", "Health regen", "HealthRegen", 50));
    g_defs.push_back(attr_value("gold_mult", "Gold multiplier", "ExtraGoldMultiplier", 5));
    g_defs.push_back(attr_value("loot_quality", "Loot quality multiplier", "LootQualityMultiplier", 3));
    g_defs.push_back(attr_value("loot_efficiency", "Loot efficiency multiplier", "LootEfficiencyMultiplier", 3));
    g_defs.push_back(attr_value("material_drop", "Material drop multiplier", "MaterialsDropMultiplier", 3));
    g_defs.push_back(attr_value("resource_gain", "Resource gain multiplier", "ResourceGainMultiplier", 3));
    g_defs.push_back(attr_value("egg_drop", "Egg drop multiplier", "EggDropMultiplier", 3));
    g_defs.push_back(attr_value("loot_range", "Loot pickup range", "LootRange", 1500));
    g_defs.push_back(attr_value("frag_drop_attr", "Skill fragment drop multiplier (stat)", "SkillFragmentDropMultiplier", 3));
    g_defs.push_back(attr_value("frag_quality_attr", "Fragment quality multiplier (stat)", "FragmentQualityMultiplier", 3));
    g_defs.push_back(attr_value("equip_drop", "Equipment drop multiplier", "EquipmentDropMultiplier", 3));
    g_defs.push_back(attr_value("equip_quality", "Equipment quality multiplier", "EquipmentQualityMultiplier", 3));
    g_defs.push_back(loot_feature("frag_drop", "Fragment drop chance x (loot tables)", "value", 100));
    g_defs.push_back(loot_feature("frag_rarity", "Fragment rarity boost", "value", 3));
    g_defs.push_back(loot_feature("frag_divine", "Fragments always Divine", "toggle", 1));
    g_defs.push_back(cfg_feature("floating_cap", "Floating fragment limit", "m_nMaxFloatingFragmentCount", 500));
    g_defs.push_back(cfg_feature("frag_cap", "Fragment inventory capacity", "m_PlayerInventoryCapacityProperties.m_InventorySizes", 3000, 4));
    g_defs.push_back(cfg_feature("frag_tabs", "Fragment stash tabs", "m_PlayerInventoryCapacityProperties.m_nSkillGridStorageTabCount", 8));
    g_defs.push_back(cfg_feature("equip_cap", "Equipment bag capacity", "m_PlayerInventoryCapacityProperties.m_InventorySizes", 72, 0));
    g_defs.push_back(cfg_feature("pity_time", "Pity loot timer (seconds)", "m_fPityLootTime", 60));
    g_defs.push_back(cfg_feature("divine_chance", "Divine loot base chance", "m_LootDivineRules.m_fChance", 0.25));
    g_defs.push_back(attr_value("xp_mult", "XP multiplier", "XPMultiplier", 3));
    g_defs.push_back(attr_value("wyrm_xp_mult", "Wyrmling XP multiplier", "WyrmlingXPMultiplier", 3));
    g_defs.push_back(attr_value("move_speed", "Movement speed", "MovementSpeed", 900));
    g_defs.push_back(attr_value("crit_chance", "Critical chance (0-1)", "CriticalChancePercent", 1));
    g_defs.push_back(attr_value("crit_damage", "Critical damage multiplier", "CriticalDamageMultiplier", 3));
    g_defs.push_back(attr_value("damage_mult", "Final damage multiplier", "FinalDamageMultiplier", 5));
    g_defs.push_back(attr_value("armor_pierce", "Armor pierce (0-1)", "ArmorPiercePercent", 1));
    g_defs.push_back(attr_value("bonus_points", "Bonus attribute points", "AttributePoints", 50));
    { Def d; d.id = "game_speed"; d.name = "Game speed"; d.kind = "action"; d.def = 1; d.apply = [](double v) { return set_speed(v); }; g_defs.push_back(d); }
    g_by_id.clear(); for (auto& d : g_defs) g_by_id[d.id] = &d;
}

static std::string apply_one(Def& d, double v) {
    if (!d.apply) return "";
    if (!d.attr.empty() && !K.orig.count(d.id)) {
        const std::string ek = attr_orig_key(d.id, roots().pawn); std::string ev;
        if (env_get(ek, ev)) K.orig[d.id] = atof(ev.c_str());
        else { Attr a = attr(d.attr); if (a.ok) { K.orig[d.id] = a.base; env_set(ek, fmt("%.9g", a.base)); } }
    }
    std::string e = d.apply(v);
    if (e.empty()) K.errors.erase(d.id); else K.errors[d.id] = e;
    return e;
}
// feature on/off/value. v = NAN -> off
static json fset(const std::string& id, double v, bool off) {
    auto it = g_by_id.find(id); if (it == g_by_id.end()) return json{ {"error", "unknown feature " + id} };
    Def& d = *it->second;
    if (multiplayer()) return json{ {"error", "refused: not a solo session"} };
    if (d.kind == "action") { std::string e = d.apply(off ? d.def : v); return e.empty() ? json{ {"id", id}, {"value", v} } : json{ {"error", e} }; }
    if (off) {
        K.active.erase(id); if (d.off) d.off();
        auto o = K.orig.find(id);
        if (!d.attr.empty()) {
            const std::string ek = attr_orig_key(id, roots().pawn); std::string ev;
            if (o != K.orig.end()) set_attr(d.attr, o->second); else if (env_get(ek, ev)) set_attr(d.attr, atof(ev.c_str()));
            env_del(ek);
        }
        K.orig.erase(id); K.errors.erase(id); K.stuck.erase(id); ksave(); klog("off " + id); return json{ {"id", id}, {"on", false} };
    }
    K.active[id] = v; ksave();
    std::string e = K.ready ? apply_one(d, v) : "";
    klog("on " + id + " = " + fmt("%g", v) + (K.ready ? "" : " (pending)"));
    return json{ {"id", id}, {"on", true}, {"value", v}, {"applied", K.ready && e.empty()}, {"error", e} };
}
static void talent_sync();
static void all_off() {
    std::vector<std::string> ids; for (auto& kv : K.active) ids.push_back(kv.first);
    for (auto& i : ids) { if (i.rfind("talent:", 0) == 0) K.active.erase(i); else fset(i, 0, true); }
    ksave();
}

static bool pawn_ready(uintptr_t pawn) {
    uintptr_t a = asc_of(pawn); if (!a || !attrset(a)) return false;
    Attr hm = attr("HealthMax", pawn); return hm.ok && hm.cur > 0;
}
static ULONGLONG g_last_check = 0;
static void keeper_tick() {
    Roots r = roots();
    if (r.pawn != K.pawn) {
        if (r.pawn && K.had_pawn) ++K.zone_changes;
        if (r.pawn) K.had_pawn = true;
        K.pawn = r.pawn; K.ready = false; K.since = GetTickCount64(); K.orig.clear(); g_comp_cache.clear(); g_handles.clear(); g_handles_asc = 0;
        K.world = world_name();
        klog("pawn changed -> " + hexstr(r.pawn) + " in " + K.world);
    }
    if (!r.pawn) return;
    if (!K.ready) {
        if (GetTickCount64() - K.since < 1500) return;
        if (!pawn_ready(r.pawn)) return;
        if (multiplayer()) return;
        K.ready = true;
        int n = 0; std::vector<std::string> reset;
        for (auto& kv : K.active) { auto it = g_by_id.find(kv.first); if (it == g_by_id.end()) continue;
            Def& d = *it->second; if (d.check && !d.check(kv.second)) reset.push_back(kv.first);
            if (apply_one(d, kv.second).empty()) ++n; }
        K.reapplied = n;
        std::string rs; for (auto& x : reset) rs += (rs.empty() ? "" : ",") + x;
        klog(fmt("ready: re-applied %d feature(s); game had reset: %s", n, rs.empty() ? "none" : rs.c_str()));
    }
    for (auto& kv : K.active) { auto it = g_by_id.find(kv.first); if (it != g_by_id.end() && it->second->kind == "tick" && it->second->tick) it->second->tick(kv.second); }
    if (GetTickCount64() - g_last_check > 1000) {
        g_last_check = GetTickCount64();
        for (auto& kv : K.active) {
            auto it = g_by_id.find(kv.first); if (it == g_by_id.end() || K.stuck.count(kv.first)) continue;
            Def& d = *it->second; if (!d.check || d.check(kv.second)) continue;
            auto& rr = K.reapplies[d.id]; if (GetTickCount64() - rr.second > 30000) rr = { 0, GetTickCount64() };
            if (++rr.first >= 8) { K.stuck.insert(d.id); K.errors[d.id] = "the game keeps undoing it - switched off"; klog("gave up on " + d.id); continue; }
            apply_one(d, kv.second); klog("re-applied " + d.id + " (game reset it)");
        }
    }
}

// ------------------------------------------------------------------ panel items (panel.json) - port of panel.lua apply()
struct PItem { std::string id, type, feature, label; double def = 0, off = 0, minv = 0, maxv = 1; bool has_off = false, danger = false; };
static std::map<std::string, PItem> g_items;
static std::map<std::string, double> g_values;
static void load_items() {
    g_items.clear();
    std::ifstream f(g_spec_path); if (!f) { log("[dk] panel spec not found: %s", g_spec_path.c_str()); return; }
    json j = json::parse(f, nullptr, false); if (!j.is_object()) return;
    for (auto& t : j.value("tabs", json::array())) for (auto& s : t.value("sections", json::array())) for (auto& it : s.value("items", json::array())) {
        if (!it.is_object() || !it.contains("id")) continue;
        PItem p; p.id = it["id"]; p.type = it.value("type", ""); p.feature = it.value("feature", ""); p.label = it.value("label", p.id);
        p.def = it.value("default", 0.0); p.minv = it.value("min", 0.0); p.maxv = it.value("max", 1.0); p.danger = it.value("danger", false);
        if (it.contains("off") && it["off"].is_number()) { p.has_off = true; p.off = it["off"].get<double>(); }
        g_items[p.id] = p;
    }
}
static void sync_values() {
    for (auto& kv : g_items) {
        const PItem& it = kv.second; if (it.feature.empty()) continue;
        auto a = K.active.find(it.feature);
        if (it.type == "toggle") g_values[it.id] = a != K.active.end() ? 1 : 0;
        else if (it.type == "slider") g_values[it.id] = a != K.active.end() ? a->second : (it.has_off ? it.off : it.def);
    }
}

static std::string wyrm_text();
static json city_info();
static json call_fn(const std::string& fn, const json& args);
static json item_action(const std::string& id, std::string& err);

static json item_apply(const std::string& id, const json& v, bool confirmed, std::string& err) {
    auto it = g_items.find(id); if (it == g_items.end()) { err = "no item " + id; return json(); }
    const PItem& p = it->second;
    if (!p.feature.empty() && (p.type == "toggle" || p.type == "slider")) {
        if (p.type == "toggle") {
            const bool on = v.is_boolean() ? v.get<bool>() : (v.is_number() && v.get<double>() != 0);
            json r = fset(p.feature, 1, !on); if (r.contains("error") && !r["error"].get<std::string>().empty()) err = r["error"];
            return K.errors.count(p.feature) ? json(K.errors[p.feature]) : json(on ? "on" : "off");
        }
        double x = v.is_number() ? v.get<double>() : p.def; x = std::clamp(x, p.minv, p.maxv);
        const bool off = p.has_off && std::fabs(x - p.off) < 1e-6;
        json r = fset(p.feature, x, off); if (r.contains("error") && !r["error"].get<std::string>().empty()) err = r["error"];
        return K.errors.count(p.feature) ? json(K.errors[p.feature]) : json(off ? "off" : "on");
    }
    if (p.type == "slider" && id == "game_speed") { double x = v.is_number() ? v.get<double>() : 1; json r = fset("game_speed", x, false); if (r.contains("error")) err = r["error"]; g_values[id] = x; return json("speed " + fmt("%g", x)); }
    if (p.type == "button") { if (p.danger && !confirmed) { err = "needs confirm"; return json(); } return item_action(id, err); }
    err = "item " + id + " is not interactive"; return json();
}

static std::string level_text() { Attr a = attr("Level"); return a.ok ? fmt("level %d", (int)std::lround(a.cur)) : "-"; }
static json item_action(const std::string& id, std::string& err) {
    Roots r = roots();
    auto gain_level = [&](int n) -> std::string {
        if (!r.pawn) return "no character";
        for (int i = 0; i < n; ++i) { Params p = ue::params_for(r.pawn, "GainALevel"); if (!p) return "no GainALevel"; for (auto& pp : ue::props(p.fn, false)) if (pp.type == "BoolProperty" && !(pp.flags & ue::CPF_ReturnParm)) p.put_bool(pp.name, i == n - 1);
            std::string e; if (!callf(r.pawn, p, &e)) return e; }
        return "";
    };
    auto add_xp = [&](double xp) -> std::string { if (!r.pawn) return "no character"; Params p = ue::params_for(r.pawn, "IncreaseXP"); if (!p) return "no IncreaseXP"; p.put("fXPToGain", (float)xp); std::string e; return callf(r.pawn, p, &e) ? "" : e; };
    auto give_txt = [&](const char* tag, int n) -> json { GiveResult g = give(tag, n); if (!g.err.empty()) { err = g.err; return json(); } std::string t = tag; return json(t.substr(t.rfind('.') + 1) + " " + std::to_string(g.after)); };
    if (multiplayer() && id != "all_off") { err = "refused: not a solo session"; return json(); }
    if (id == "lvl1") { std::string e = gain_level(1); if (!e.empty()) err = e; return json(level_text()); }
    if (id == "lvl5") { std::string e = gain_level(5); if (!e.empty()) err = e; return json(level_text()); }
    if (id == "xp10k") { std::string e = add_xp(10000); if (!e.empty()) err = e; return json(level_text()); }
    if (id == "xp1m") { std::string e = add_xp(1000000); if (!e.empty()) err = e; return json(level_text()); }
    if (id == "gold10k") return give_txt("Items.Currencies.Gold", 10000);
    if (id == "gold100k") return give_txt("Items.Currencies.Gold", 100000);
    if (id == "gold1m") return give_txt("Items.Currencies.Gold", 1000000);
    if (id == "dust100") return give_txt("Items.Currencies.PrimordialDust", 100);
    if (id == "dust1k") return give_txt("Items.Currencies.PrimordialDust", 1000);
    if (id == "echo100") return give_txt("Items.Currencies.EchoChains", 100);
    if (id == "mats50" || id == "res50") {
        int n = 0; for (auto& t : countable_tags(id == "mats50" ? "Items.Materials" : "Items.Resources")) if (give(t, 50).err.empty()) ++n;
        return json(std::to_string(n) + " kinds +50");
    }
    if (id == "all_off") { all_off(); return json("all off"); }
    if (id == "wyrm_l10" || id == "wyrm_max" || id == "heal_check" || id.rfind("city_", 0) == 0) {
        static const std::map<std::string, std::pair<std::string, json>> m = {
            {"wyrm_l10", {"DKE.wyrm_level_plus", json::array({10})}}, {"wyrm_max", {"DKE.wyrm_level", json::array({50})}},
            {"city_lvl_up", {"DKC.level_up", json::array()}}, {"city_lvl_10", {"DKC.set_level", json::array({10})}},
            {"city_lvl_25", {"DKC.set_level", json::array({25})}}, {"city_lvl_50", {"DKC.set_level", json::array({50})}},
            {"city_pts5", {"DKC.add_points", json::array({5})}}, {"city_unlock_all", {"DKC.unlock_all", json::array()}},
            {"heal_check", {"DKH.check", json::array()}} };
        auto f = m.find(id); if (f == m.end()) { err = "no action " + id; return json(); }
        json r = call_fn(f->second.first, f->second.second);
        if (r.is_object() && r.contains("error")) { err = r["error"]; return json(); }
        return r.is_object() && r.contains("text") ? r["text"] : r;
    }
    err = "no native action for " + id; return json();
}

static std::string readout(const std::string& id) {
    // readouts that CALL game functions only while a character is ready (never in menus / loading / shutdown)
    // (and readouts that search for the character's objects: in menus they find nothing = a full object scan each time)
    if (!K.ready && (id == "r_gold" || id == "r_solo" || id == "r_wyrm" || id.rfind("r_city_", 0) == 0)) return "-";
    auto pair = [](const char* a, const char* b) { Attr x = attr(a), y = attr(b); return x.ok && y.ok ? fmt("%.0f / %.0f", x.cur, y.cur) : std::string("-"); };
    if (id == "r_level") { Attr a = attr("Level"); return a.ok ? std::to_string((int)std::lround(a.cur)) : "-"; }
    if (id == "r_hp") return pair("Health", "HealthMax");
    if (id == "r_res") return pair("Mana", "ManaMax");
    if (id == "r_points") { Attr a = attr("AttributePoints"); return a.ok ? std::to_string((int)std::lround(a.cur)) : "-"; }
    if (id == "r_gold") { int64_t c = count("Items.Currencies.Gold"); return c >= 0 ? std::to_string(c) : "-"; }
    if (id == "r_ready") return K.ready ? "yes" : "waiting";
    if (id == "r_zone") return std::to_string(K.zone_changes);
    if (id == "r_world") return K.world.empty() ? "-" : K.world;
    if (id == "r_solo") { Session s = session(); return s.solo ? "yes" : "NO - cheats paused"; }
    if (id == "r_wyrm") return wyrm_text();
    if (id.rfind("r_city_", 0) == 0) {
        static json c; static ULONGLONG c_at = 0;            // one city lookup per readout pass (was one per city readout)
        if (GetTickCount64() - c_at > 250) { c = city_info(); c_at = GetTickCount64(); }
        if (!c.is_object() || c.contains("error")) return id == "r_city_level" ? "not in the city" : "-";
        if (id == "r_city_level") return std::to_string(c.value("level", 0));
        if (id == "r_city_xp") return fmt("%.0f / %.0f", c.value("xp", 0.0), c.value("next_xp", 0.0));
        if (id == "r_city_points") return std::to_string(c.value("points", 0));
        if (id == "r_city_upgrades") return std::to_string(c.value("upgrades", 0));
    }
    if (id == "r_grid") return g_grid_status;
    if (id == "r_mods") return g_mods_status;
    if (id == "r_gridlink") return K.active.count("grid_link") ? fmt("on - %ld links added", gridlink_links()) : "off";
    if (id == "r_heal") return "native engine";
    if (id == "r_heal_build") { static std::string v; if (v.empty()) { char x[MAX_PATH] = {}; GetModuleFileNameA(nullptr, x, MAX_PATH); WIN32_FILE_ATTRIBUTE_DATA d; v = GetFileAttributesExA(x, GetFileExInfoStandard, &d) ? std::to_string(d.nFileSizeLow) : "?"; } return v; }
    return "-";
}

// ------------------------------------------------------------------ editor / city / wyrmling (ported in dk_editor.cpp)
json editor_call(const std::string& fn, const json& args);   // dk_editor.cpp
std::string editor_wyrm_text();
json editor_city_info();
static std::string wyrm_text() { return editor_wyrm_text(); }
static json city_info() { return editor_city_info(); }
static json call_fn(const std::string& fn, const json& args) {
    if (fn == "DKF.all_off") { all_off(); return json("all off"); }
    if (fn == "DKT.list") return talent_json();
    if (fn == "DKT.set") {                                           // [tag, extra points 0..6]
        const std::string tag = args.is_array() && args.size() > 0 && args[0].is_string() ? args[0].get<std::string>() : "";
        const int v = args.is_array() && args.size() > 1 && args[1].is_number() ? std::clamp(args[1].get<int>(), 0, kTalentMax) : 0;
        if (tag.empty()) return json{ {"error", "no talent"} };
        if (v) K.active["talent:" + tag] = v; else K.active.erase("talent:" + tag);
        ksave(); g_talent_touched = true; talent_sync(); bump();
        return talent_json();
    }
    if (fn == "DKF.status") return json{ {"ready", K.ready}, {"zone_changes", K.zone_changes}, {"world", K.world} };
    return editor_call(fn, args);
}

// ------------------------------------------------------------------ request queue + state document
static std::mutex g_q_mtx;
static std::vector<json> g_q;
static std::deque<std::pair<int64_t, json>> g_results;       // last 20: seq -> {ok, res}
static int64_t g_seq_done = 0;
static json g_readouts = json::object(), g_player = json::object();
static ULONGLONG g_last_readouts = 0;

void post(const json& reqs) {
    std::lock_guard<std::mutex> lk(g_q_mtx);
    if (reqs.is_array()) for (auto& r : reqs) g_q.push_back(r); else if (reqs.is_object()) g_q.push_back(reqs);
}
static void result(int64_t seq, bool okv, const json& res) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_results.push_back({ seq, json{ {"ok", okv}, {"res", res} } });
    while (g_results.size() > 20) g_results.pop_front();
    if (seq > g_seq_done) g_seq_done = seq;
    bump();
}
static void process_requests() {
    std::vector<json> batch; { std::lock_guard<std::mutex> lk(g_q_mtx); batch.swap(g_q); }
    if (batch.empty()) return;
    ue::journal_scope(true);
    struct Off { ~Off() { ue::journal_scope(false); } } off_;
    for (auto& r : batch) {
        const int64_t seq = r.value("seq", (int64_t)0); const std::string op = r.value("op", "");
        std::string err; json res;
        try {
            if (op == "set") res = item_apply(r.value("id", ""), r.contains("v") ? r["v"] : json(), false, err);
            else if (op == "click") res = item_apply(r.value("id", ""), json(), r.value("confirmed", false), err);
            else if (op == "all_off") { all_off(); res = "all off"; }
            else if (op == "call") { res = call_fn(r.value("fn", ""), r.value("args", json::array())); if (res.is_object() && res.contains("error")) { err = res["error"]; } }
            else err = "unknown op " + op;
        } catch (std::exception& e) { err = e.what(); }
        result(seq, err.empty(), err.empty() ? res : json(err));
    }
}

static void tick() {
    static ULONGLONG last_keeper = 0;
    if (g_game_exiting) return;                 // the game is shutting down: hands off
    if (!ue::init()) return;
    // frame-time monitor: everything below runs inside one game frame; a slow pass is logged with its causes
    const double t_start = ue::now_ms(); double t_keeper = 0, t_ro = 0; std::string slow_ro;
    ue::Perf p0 = ue::perf();
    process_requests();
    const ULONGLONG now = GetTickCount64();
    if (now - last_keeper >= 160 && g_test.value("keeper", true)) {
        last_keeper = now;
        const double k0 = ue::now_ms();
        try { keeper_tick(); } catch (std::exception& e) { log("[dk] keeper: %s", e.what()); }
        t_keeper = ue::now_ms() - k0;
    }
    struct Report { double t_start; double& tk; double& tr; std::string& sr; ue::Perf p0; ~Report() {
        const double total = ue::now_ms() - t_start; if (total < 4.0) return;
        static ULONGLONG last = 0; if (GetTickCount64() - last < 3000) return; last = GetTickCount64();
        const ue::Perf& p = ue::perf();
        log("[perf] slow trainer frame %.1f ms: keeper %.1f, readouts %.1f%s | object scans %u (%.1f ms), name map %u (%.1f ms), object index %u (%.1f ms)",
            total, tk, tr, sr.c_str(), p.scans - p0.scans, p.scan_ms - p0.scan_ms, p.name_builds - p0.name_builds, p.name_ms - p0.name_ms,
            p.index_builds - p0.index_builds, p.index_ms - p0.index_ms); } } report{ t_start, t_keeper, t_ro, slow_ro, p0 };
    { static ULONGLONG last_tal = 0;      // talents: debug source is per character, re-sync 1/s
      if (now - last_tal >= 1000) { last_tal = now; try { talent_sync(); } catch (...) {} } }
    { static ULONGLONG last_hook = 0;      // grid link hook in place BEFORE a character loads (its grid is built while
      // loading; a hook that only arrives at "keeper ready" leaves the ranks as the game computed them unlinked)
      if (now - last_hook >= 1000 && K.active.count("grid_link")) { last_hook = now; try { gridlink_set(!multiplayer()); } catch (...) {} } }
    { static ULONGLONG last_grid = 0;      // once a second, also at the main menu: lands before the grid is first built
      if (now - last_grid >= 1000 && (K.active.count("grid_size") || K.active.count("grid_unlock") || g_grid_touched)) {
          last_grid = now; try { grid_sync(); } catch (...) {} } }
    if (now - g_last_readouts >= 500) {
        g_last_readouts = now;
        json ro = json::object();
        const bool ro_on = g_test.value("readouts", true); const json skip = g_test.value("skip", json::array());
        for (auto& kv : g_items) if (kv.second.type == "readout") {
            bool sk = !ro_on; for (auto& x : skip) if (x == kv.first) sk = true;
            if (sk) { ro[kv.first] = "-"; continue; }
            const double r0 = ue::now_ms();
            try { ro[kv.first] = readout(kv.first); } catch (...) { ro[kv.first] = "-"; }
            const double dt = ue::now_ms() - r0; t_ro += dt; if (dt >= 1.0) slow_ro += fmt(" %s=%.1f", kv.first.c_str(), dt); }
        Roots r = roots(); json pl = json::object();
        if (r.pawn) { pl["pawn"] = ue::name_of(ue::class_of(r.pawn)); pl["world"] = K.world; }
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        sync_values(); g_readouts = ro; g_player = pl; bump();
    }
}

json state() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    json res = json::object(); for (auto& r : g_results) res[std::to_string(r.first)] = r.second;
    json vals = json::object(); for (auto& kv : g_values) { auto it = g_items.find(kv.first); vals[kv.first] = (it != g_items.end() && it->second.type == "toggle") ? json(kv.second != 0) : json(kv.second); }
    json act = json::object(); for (auto& kv : K.active) act[kv.first] = kv.second;
    json errs = json::object(); for (auto& kv : K.errors) errs[kv.first] = kv.second;
    json lg = json::array(); for (size_t i = K.log.size() > 16 ? K.log.size() - 16 : 0; i < K.log.size(); ++i) lg.push_back(K.log[i]);
    return { {"version", "native"}, {"seq_done", g_seq_done}, {"results", res}, {"values", vals}, {"readouts", g_readouts},
             {"keeper", { {"ready", K.ready}, {"zone_changes", K.zone_changes}, {"world", K.world}, {"reapplied", K.reapplied}, {"active", act}, {"errors", errs}, {"log", lg} }},
             {"player", g_player} };
}
uint64_t state_version() { return g_version.load(); }

static bool g_native = false;
bool native_mode() { return g_native; }

void init(const std::string& data_dir, const std::string& panel_spec) {
    static bool done = false; if (done) return; done = true;
    g_native = true;
    g_data_dir = data_dir; g_spec_path = panel_spec;
    CreateDirectoryA(g_data_dir.c_str(), nullptr);
    ue::journal_init(g_data_dir);
    build_defs(); kload(); load_items(); sync_values();
    { std::ifstream tf(g_data_dir + "\\test.json"); if (tf) { json t = json::parse(tf, nullptr, false); if (t.is_object()) { g_test = t; log("[dk] test switches: %s", t.dump().c_str()); } } }
    add_main_tick([]() { try { tick(); } catch (...) {} });
    log("[dk] native engine on: %zu features, %zu panel items, %zu active from %s", g_defs.size(), g_items.size(), K.active.size(), state_file().c_str());
}

json cmd(const std::string& name, const json& a) {
    if (!g_native) return fail("OFF", "native engine is off (config.json \"engine\":\"native\")");
    if (name == "DK.STATUS") return ok(state());
    return run_on_main([name, a]() -> json {
        if (name == "DK.SET") { json r = fset(a.value("id", ""), a.value("v", 1.0), a.value("off", false)); return ok(r); }
        if (name == "DK.ATTR") { Attr x = attr(a.value("name", "Health")); return ok({ {"ok", x.ok}, {"base", x.base}, {"cur", x.cur} }); }
        if (name == "DK.CALL") return ok(call_fn(a.value("fn", ""), a.value("args", json::array())));
        if (name == "DK.ITEM") { std::string e; json r = item_apply(a.value("id", ""), a.contains("v") ? a["v"] : json(), a.value("confirmed", false), e); return e.empty() ? ok(r) : fail("ITEM", e); }
        if (name == "DK.LOOTSIG") {          // dev: fingerprint of every fragment loot table's live values (on/off tests)
            uint64_t h = 1469598103934665603ULL; int tables = 0; json sample = json::array(), per = json::object();
            auto mix = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; } };
            for (uintptr_t o : loot_lists()) { LootView v; if (!loot_view(o, v)) continue; ++tables; h = 1469598103934665603ULL;
                for (int i = 0; i < v.ntiers; ++i) { const uintptr_t e = v.tiers + (uintptr_t)i * v.tier_size;
                    auto nba = ue::array_of(e, v.nbp); auto dla = ue::array_of(e, v.dlp);
                    for (int j = 0; j < nba.num; ++j) { float c = rd<float>((uintptr_t)nba.data + j * v.nbinner.size, v.chance); mix(&c, 4); }
                    for (int j = 0; j < dla.num; ++j) { uintptr_t d = (uintptr_t)dla.data + j * v.dlinner.size;
                        float w = rd<float>(d, v.mult); bool cb = ue::get_bool(d, v.can), fb = ue::get_bool(d, v.force); mix(&w, 4); mix(&cb, 1); mix(&fb, 1);
                        if (sample.size() < 4 && i == 0) sample.push_back({ w, cb, fb }); } }
                per[ue::name_of(o)] = hexstr((uintptr_t)h); }
            return ok({ {"tables", tables}, {"per_table", per}, {"sample", sample} });
        }
        if (name == "DK.TALENTS") {          // dev: every talent {tag, class, points needed per rank, current points}
            uintptr_t comp = player_component("HnSTalentComponent");
            std::map<std::string, int> cur;
            if (comp) { Prop mp = F(comp, "m_TalentPointsFromAllSources"); if (mp) for (auto& kv : ue::map_entries(comp, mp)) {
                FName n{}; mread(kv.first, &n, sizeof n); int32_t v = 0; rd_as(kv.second, v); cur[ue::name_str(n)] = v; } }
            json out = json::array(); static uintptr_t cls = 0; if (!cls) cls = ue::find_class("HnSTalentBase");
            for (uintptr_t o : ue::find_all(cls, true)) {
                if (!ue::is_cdo(o)) continue;
                Prop tp = F(o, "m_IdTag"); if (!tp) continue; FName n{}; mread(o + tp.offset, &n, sizeof n);
                const std::string tag = ue::name_str(n); if (tag.empty() || tag == "None") continue;
                json floors = json::array(); Prop fp = F(o, "m_pTalentFloors");
                if (fp) { auto arr = ue::array_of(o, fp); Prop ip = ue::array_inner(fp);
                    for (int k = 0; k < arr.num && k < 16; ++k) { int32_t need = 0; rd_as((uintptr_t)arr.data + k * ip.size, need); floors.push_back(need); } }
                Prop cp = F(o, "eClass"); int cl = cp ? (int)ue::get_num(o, cp) : -1;
                out.push_back({ {"tag", tag}, {"class", cl}, {"floors", floors}, {"points", cur.count(tag) ? cur[tag] : 0}, {"obj", ue::name_of(o)} });
            }
            return ok({ {"component", hexstr(comp)}, {"talents", out} });
        }
        if (name == "DK.TALENT_ADD") {       // dev: the game's AddTalentPoints(component, source, tag, points); source 6 = Debug
            uintptr_t comp = player_component("HnSTalentComponent"); if (!comp) return fail("NO_COMP", "no talent component");
            uintptr_t L = lib("HnSTalentBlueprintLibrary"); Params p = ue::params_for(L, "AddTalentPoints"); if (!p) return fail("NO_FUNC", "no AddTalentPoints");
            FName t{}; if (!ue::name_find(a.value("tag", ""), t)) return fail("NO_TAG", "unknown tag");
            p.put("pComponent", comp); const uint8_t src = (uint8_t)a.value("source", 6); memcpy(p.at(p.param("eSource")), &src, 1);
            memcpy(p.at(p.param("TalentTag")), &t, 8); const int32_t n = a.value("points", 1); memcpy(p.at(p.param("nPointsToAdd")), &n, 4);
            std::string e; if (!callf(L, p, &e)) return fail("CALL", e);
            return ok(json("added"));
        }
        if (name == "DK.TAGS") { json o = json::array(); for (auto& t : countable_tags(a.value("filter", ""))) o.push_back(t); return ok(o); }
        if (name == "DK.SESSION") { Session s = session(); return ok({ {"standalone", s.standalone}, {"server", s.server}, {"players", s.players}, {"solo", s.solo} }); }
        return fail("UNKNOWN", name);
    }, 15000);
}

} // namespace dk
