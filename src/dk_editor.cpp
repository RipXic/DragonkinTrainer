// dk_editor.cpp - fragment + gear editor, wyrmling, city progression and names in C++ (port of editor.lua / city.lua).
// Everything goes through the game's own functions (UHnSItemRerollLibrary, UHnSInventoryComponent, UHnSCityHubDataComponent).
// Structs are passed as the game's own bytes (copied raw from the game's memory): no Lua-style rebuilding, so the
// FGameplayAttribute / FFieldPath crash of 2026-11-09 cannot happen here.
#include "dk.h"
#include "ue.h"
#include <map>
#include <set>
#include <algorithm>
#include <cmath>

namespace dk {
using namespace ::wb;
using ue::Prop; using ue::Params; using ue::FName; using ue::rd_as; using ue::mread; using ue::mwrite;

static std::string fmt2(const char* f, ...) { char b[512]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }
static json err(const std::string& e) { return json{ {"error", e} }; }

// ------------------------------------------------------------------ basics (local copies: small and keeps dk.cpp readable)
static uintptr_t first_of(const char* cls) { uintptr_t c = ue::find_class(cls); return c ? ue::find_first(c) : 0; }
static uintptr_t lib(const char* cls) { uintptr_t c = ue::find_class(cls); return c ? ue::cdo_of(c) : 0; }
static uintptr_t player_pc() {
    static uintptr_t pcc = 0, cached = 0; if (!pcc) pcc = ue::find_class("PlayerController");
    auto good = [](uintptr_t pc) { Prop pl = ue::find_prop(ue::class_of(pc), "Player"); return pl && ue::get<uintptr_t>(pc, pl); };
    if (cached && ue::alive(cached) && good(cached)) return cached;
    static ULONGLONG last = 0; if (GetTickCount64() - last < 1000) return 0;     // no player (menus): full scan at most 1/s
    last = GetTickCount64();
    cached = 0; for (uintptr_t pc : ue::find_all(pcc)) if (good(pc)) { cached = pc; last = 0; break; }
    return cached;
}
static uintptr_t player_pawn() { uintptr_t pc = player_pc(); Prop pw = pc ? ue::find_prop(ue::class_of(pc), "Pawn") : Prop(); return pw ? ue::get<uintptr_t>(pc, pw) : 0; }
static uintptr_t player_inv() {
    uintptr_t pc = player_pc(), pawn = player_pawn(), ps = 0;
    if (pc) { Prop p = ue::find_prop(ue::class_of(pc), "PlayerState"); if (p) ps = ue::get<uintptr_t>(pc, p); }
    static uintptr_t cached = 0;
    if (cached && ue::alive(cached)) { uintptr_t o = ue::outer_of(cached); if (o && (o == pc || o == pawn || o == ps)) return cached; }
    static ULONGLONG last = 0; if (GetTickCount64() - last < 1000) return 0;     // miss: full scan at most 1/s
    last = GetTickCount64();
    cached = 0; uintptr_t cls = ue::find_class("HnSInventoryComponent");
    for (uintptr_t c : ue::find_all(cls)) { uintptr_t o = ue::outer_of(c); if (o && (o == pc || o == pawn || o == ps)) { cached = c; last = 0; break; } }
    return cached;
}
// i-th non-return parameter
static Prop arg(const Params& p, int i) {
    int k = 0; for (auto& pp : ue::props(p.fn, false)) { if (pp.flags & ue::CPF_ReturnParm) continue; if (k++ == i) return pp; }
    return Prop();
}
static bool call_on(uintptr_t obj, Params& p, std::string* e = nullptr) { return ue::call(obj, p, e); }
static Prop sf(uintptr_t s, const char* n) { return ue::find_prop(s, n); }
static bool field(uintptr_t base, uintptr_t s, const std::string& path, uintptr_t& b, Prop& p) { return ue::resolve(base, s, path, b, p); }
static double num(uintptr_t base, uintptr_t s, const std::string& path, double dv = 0) { uintptr_t b; Prop p; return field(base, s, path, b, p) ? ue::get_num(b, p) : dv; }
static std::string tagname(uintptr_t base, uintptr_t s, const std::string& path) { uintptr_t b; Prop p; return field(base, s, path, b, p) ? ue::name_str(ue::get<FName>(b, p)) : ""; }

// ------------------------------------------------------------------ names (game's own localized text)
static std::string ascii(std::string s) {
    auto rep = [&](const char* from, const char* to) { size_t k; while ((k = s.find(from)) != std::string::npos) s.replace(k, strlen(from), to); };
    rep("\xE2\x80\x98", "'"); rep("\xE2\x80\x99", "'"); rep("\xE2\x80\x9C", "\""); rep("\xE2\x80\x9D", "\""); rep("\xE2\x80\x93", "-"); rep("\xE2\x80\x94", "-"); rep("\xE2\x80\xA6", "...");
    return s;
}
struct NameDesc { std::string name, desc; };
static std::map<std::string, NameDesc> g_tag_names;
static NameDesc tag_name(const std::string& tag) {
    auto it = g_tag_names.find(tag); if (it != g_tag_names.end()) return it->second;
    NameDesc nd; uintptr_t h = lib("HnSCharacterAbilitiesHelper"); FName fn;
    if (h && !tag.empty() && tag != "None" && ue::name_find(tag, fn)) {
        const bool skill = tag.find("PlayerSkill") != std::string::npos || tag.find("DragonSkill") != std::string::npos;
        Params p = ue::params_for(h, skill ? "GetDragonOrPlayerAbilityByTag" : "GetAbilityModifierByTag");
        if (p) {
            Prop a0 = arg(p, 0), a1 = arg(p, 1);
            memcpy(p.at(a0), &fn, 8); if (a1) *p.at(a1) = 0;   // EEkoAssertEnabled::NoAssert
            if (call_on(h, p)) {
                uintptr_t o = p.ret<uintptr_t>();
                if (o) {
                    Prop nt = ue::find_prop(ue::class_of(o), "m_NameText"), dt = ue::find_prop(ue::class_of(o), "m_DescriptionText");
                    if (nt) nd.name = ascii(ue::ftext(o + nt.offset));
                    if (dt) nd.desc = ascii(ue::ftext(o + dt.offset));
                }
            }
        }
    }
    if (h) g_tag_names[tag] = nd;
    return nd;
}
static std::map<std::string, std::string> g_item_names;
static std::string item_name(const std::string& tag) {
    // FindItemDescByTag returns FHnSItemDesc (FText members) BY VALUE into a zeroed buffer -> unsafe (see wyrm()).
    // The panel falls back to the readable tag; a safe path (reading the item data table directly) is a TODO.
    (void)tag; return "";
}

// ------------------------------------------------------------------ inventory items (m_ItemContainers: type 0 bag, 1 equipped, 4 fragments)
struct ItemRef { int type = 0; uintptr_t entry = 0; uintptr_t entry_struct = 0; };
static std::vector<ItemRef> items_of(std::initializer_list<int> types) {
    std::vector<ItemRef> out; uintptr_t inv = player_inv(); if (!inv) return out;
    Prop mc = ue::find_prop(ue::class_of(inv), "m_ItemContainers"); if (!mc) return out;
    Prop kp, vp; int32_t voff, stride; if (!ue::map_kv(mc, kp, vp, voff, stride)) return out;
    Prop items = sf(vp.owner_struct, "Items"); if (!items) return out;
    Prop ip = ue::array_inner(items);
    for (auto& kv : ue::map_entries(inv, mc)) {
        uint8_t t = 0; rd_as(kv.first, t);
        bool want = false; for (int x : types) if (x == t) want = true; if (!want) continue;
        auto arr = ue::array_of(kv.second, items);   // the value struct starts at kv.second
        for (int i = 0; i < arr.num && i < 5000; ++i) out.push_back({ t, (uintptr_t)arr.data + (uintptr_t)i * ip.size, ip.owner_struct });
    }
    return out;
}
static int64_t handle_of(const ItemRef& r) { return (int64_t)num(r.entry, r.entry_struct, "m_Spec.m_Handle.Handle", 0); }
static bool find_item(int64_t h, ItemRef& out) {
    for (auto& r : items_of({ 0, 1, 4 })) if (handle_of(r) == h) { out = r; return true; }
    return false;
}
static uintptr_t spec_of(const ItemRef& r, Prop& specp) { specp = sf(r.entry_struct, "m_Spec"); return specp ? r.entry + specp.offset : 0; }

// fragments -------------------------------------------------------------------------------------------------
static json slot_json(uintptr_t s, uintptr_t st) {
    const std::string tag = tagname(s, st, "m_Tag.TagName");
    NameDesc nd = tag_name(tag);
    json j = { {"tag", tag}, {"rarity", (int)num(s, st, "m_eRarity")}, {"type", (int)num(s, st, "m_eType")}, {"divine", num(s, st, "m_bDivine") != 0} };
    if (!nd.name.empty()) j["name"] = nd.name; if (!nd.desc.empty()) j["desc"] = nd.desc;
    return j;
}
static json frag_json(const ItemRef& r) {
    Prop specp; uintptr_t spec = spec_of(r, specp);
    uintptr_t fb; Prop fp; if (!field(spec, specp.owner_struct, "m_FragmentSubSpec", fb, fp)) return json();
    const uintptr_t f = fb + fp.offset, fst = fp.owner_struct;
    Prop slots = sf(fst, "m_StorageSlots"), preset = sf(fst, "m_PresetSlots");
    auto arr = ue::array_of(f, slots); Prop sp = ue::array_inner(slots);
    json js = json::array(); for (int i = 0; i < arr.num && i < 64; ++i) js.push_back(slot_json((uintptr_t)arr.data + i * sp.size, sp.owner_struct));
    return { {"handle", handle_of(r)}, {"rarity", (int)num(f, fst, "m_eRarity")}, {"onGrid", preset && ue::array_of(f, preset).num > 0},
             {"floating", num(f, fst, "m_bIsFloating") != 0}, {"size", arr.num}, {"slots", js} };
}
// Light read for "every grid modifier on every skill": modifier power per tag over the fragments ON THE GRID only.
// No names / descriptions (those call into the game) and the property lookups are done once - this runs every 2 s.
std::map<std::string, int> editor_grid_power() {
    std::map<std::string, int> out;
    static Prop specp, fragp, slotsp, presetp, innerp, tagp, rarp, typep, divp; static bool init = false;
    auto refs = items_of({ 4 }); if (refs.empty()) return out;
    if (!init) {
        specp = sf(refs[0].entry_struct, "m_Spec"); if (!specp) return out;
        fragp = sf(specp.owner_struct, "m_FragmentSubSpec"); if (!fragp) return out;
        slotsp = sf(fragp.owner_struct, "m_StorageSlots"); presetp = sf(fragp.owner_struct, "m_PresetSlots");
        if (!slotsp || !presetp) return out;
        innerp = ue::array_inner(slotsp); if (!innerp.owner_struct) return out;
        tagp = sf(innerp.owner_struct, "m_Tag"); rarp = sf(innerp.owner_struct, "m_eRarity");
        typep = sf(innerp.owner_struct, "m_eType"); divp = sf(innerp.owner_struct, "m_bDivine");
        if (!tagp || !rarp || !typep || !divp) return out;
        init = true;
    }
    for (auto& r : refs) {
        const uintptr_t f = r.entry + specp.offset + fragp.offset;
        if (ue::array_of(f, presetp).num <= 0) continue;                     // not on the grid
        auto arr = ue::array_of(f, slotsp);
        for (int i = 0; i < arr.num && i < 64; ++i) {
            const uintptr_t sl = (uintptr_t)arr.data + (uintptr_t)i * innerp.size;
            if ((int)ue::get_num(sl, typep) != 0) continue;                  // 0 = modifier
            FName n{}; if (!mread(sl + tagp.offset, &n, sizeof n)) continue;
            const std::string tag = ue::name_str(n); if (tag.empty() || tag == "None") continue;
            out[tag] += (int)ue::get_num(sl, rarp) + 1 + (ue::get_bool(sl, divp) ? 2 : 0);
        }
    }
    return out;
}
static json fragments(const std::string& filter) {
    json out = json::array();
    for (auto& r : items_of({ 4 })) { json f = frag_json(r); if (!f.is_object() || f["size"].get<int>() == 0) continue;
        if (!filter.empty() && (f["slots"].empty() || f["slots"][0]["tag"].get<std::string>().find(filter) == std::string::npos)) continue;
        out.push_back(f); }
    return out;
}
static bool frag_parts(int64_t h, int slot1, ItemRef& r, uintptr_t& spec, Prop& specp, uintptr_t& frag, uintptr_t& fst, uintptr_t& slot, Prop& slotp) {
    if (!find_item(h, r)) return false;
    spec = spec_of(r, specp); uintptr_t fb; Prop fp; if (!field(spec, specp.owner_struct, "m_FragmentSubSpec", fb, fp)) return false;
    frag = fb + fp.offset; fst = fp.owner_struct;
    Prop slots = sf(fst, "m_StorageSlots"); auto arr = ue::array_of(frag, slots); slotp = ue::array_inner(slots);
    if (slot1 < 1 || slot1 > arr.num) return false;
    slot = (uintptr_t)arr.data + (slot1 - 1) * slotp.size; return true;
}
static json frag_options(int64_t h, int slot1) {
    ItemRef r; uintptr_t spec, frag, fst, slot; Prop specp, slotp;
    if (!frag_parts(h, slot1, r, spec, specp, frag, fst, slot, slotp)) return err("no fragment/slot");
    uintptr_t L = lib("HnSItemRerollLibrary"); Params p = ue::params_for(L, "GetRerollFragmentSlotPossibilities"); if (!p) return err("no function");
    uintptr_t pawn = player_pawn(); memcpy(p.at(arg(p, 0)), &pawn, 8);
    Prop a1 = arg(p, 1), a2 = arg(p, 2); mread(frag, p.at(a1), a1.size); mread(slot, p.at(a2), a2.size);
    std::string e; if (!call_on(L, p, &e)) return err(e);
    auto arr = *(ue::TArray<FName>*)p.ret_ptr();
    json out = json::array();
    for (int i = 0; i < arr.num && i < 500; ++i) { FName n; rd_as((uintptr_t)arr.data + i * 8, n); std::string t = ue::name_str(n); NameDesc nd = tag_name(t);
        json o = { {"tag", t} }; if (!nd.name.empty()) o["name"] = nd.name; if (!nd.desc.empty()) o["desc"] = nd.desc; out.push_back(o); }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) { auto k = [](const json& x) { std::string s = x.value("name", x.value("tag", "")); std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }; return k(a) < k(b); });
    return out;
}
static json frag_set(int64_t h, int slot1, const std::string& tag_in, int rarity, bool divine) {
    ItemRef r; uintptr_t spec, frag, fst, slot; Prop specp, slotp;
    if (!frag_parts(h, slot1, r, spec, specp, frag, fst, slot, slotp)) return err("no fragment/slot");
    std::vector<uint8_t> ns(slotp.size); mread(slot, ns.data(), ns.size());       // start from the game's own slot bytes
    const uintptr_t nb = (uintptr_t)ns.data(), st = slotp.owner_struct;
    std::string tag = tag_in.empty() ? tagname(slot, st, "m_Tag.TagName") : tag_in;
    FName fn; if (!ue::name_find(tag, fn)) return err("unknown tag " + tag);
    uintptr_t b; Prop p;
    if (field(nb, st, "m_Tag.TagName", b, p)) ue::set<FName>(b, p, fn);
    if (field(nb, st, "m_eRarity", b, p)) ue::set_num(b, p, rarity);
    if (field(nb, st, "m_eType", b, p)) ue::set_num(b, p, tag.find("PlayerSkill") != std::string::npos ? 1 : (tag.find("Wyrm") != std::string::npos || tag.find("Dragon") != std::string::npos) ? 2 : 0);
    if (field(nb, st, "m_bRerolled", b, p)) ue::set_num(b, p, 0);
    if (field(nb, st, "m_bDivine", b, p)) ue::set_num(b, p, divine ? 1 : 0);
    uintptr_t L = lib("HnSItemRerollLibrary"); Params c = ue::params_for(L, "ApplyRerollChangeFragmentSlot"); if (!c) return err("no function");
    uintptr_t hb; Prop hp; if (!field(spec, specp.owner_struct, "m_Handle", hb, hp)) return err("no handle");
    mread(hb + hp.offset, c.at(arg(c, 0)), arg(c, 0).size);
    int32_t idx = slot1 - 1; memcpy(c.at(arg(c, 1)), &idx, 4);
    memcpy(c.at(arg(c, 2)), ns.data(), std::min<size_t>(ns.size(), arg(c, 2).size));
    uintptr_t inv = player_inv(); memcpy(c.at(arg(c, 3)), &inv, 8);
    c.put_bool(arg(c, 4).name, true); c.put_bool(arg(c, 5).name, true);
    std::string e; if (!call_on(L, c, &e)) return err(e);
    {   // ApplyRerollChangeFragmentSlot ignores the new slot's m_bDivine -> set it on the stored slot afterwards (2026-11-09)
        ItemRef r2; uintptr_t sp2, fr2, fs2, sl2; Prop spp2, slp2;
        if (frag_parts(h, slot1, r2, sp2, spp2, fr2, fs2, sl2, slp2) && field(sl2, slp2.owner_struct, "m_bDivine", b, p)) ue::set_num(b, p, divine ? 1 : 0);
    }
    ItemRef now; json fj = find_item(h, now) ? frag_json(now) : json();
    return { {"ok", c.ret<bool>()}, {"fragment", fj} };
}
static json frag_max(int64_t h) {
    ItemRef r; if (!find_item(h, r)) return err("no fragment");
    json f = frag_json(r); int n = f.value("size", 0);
    for (int s = 1; s <= n; ++s) frag_set(h, s, "", 4, true);
    return find_item(h, r) ? frag_json(r) : json();
}

// gear -------------------------------------------------------------------------------------------------------
static json affix_json(uintptr_t a, uintptr_t st) {
    std::string attr = "?"; uintptr_t b; Prop p;
    if (field(a, st, "m_AttributeToModify", b, p)) attr = ue::fstring(b + p.offset);   // FGameplayAttribute.AttributeName (FString at +0)
    return { {"attr", attr}, {"value", num(a, st, "m_fAffixValue")}, {"eff", num(a, st, "m_fAffixEffectiveness")}, {"divine", num(a, st, "m_bDivine") != 0},
             {"primary", num(a, st, "m_bIsPrimary") != 0}, {"corrupted", num(a, st, "m_bCorrupted") != 0} };
}
static bool gear_parts(int64_t h, ItemRef& r, uintptr_t& spec, Prop& specp, uintptr_t& affixes_base, Prop& affp) {
    if (!find_item(h, r)) return false;
    spec = spec_of(r, specp); uintptr_t eb; Prop ep; if (!field(spec, specp.owner_struct, "m_EquipmentItemSubSpec", eb, ep)) return false;
    affixes_base = eb + ep.offset; affp = sf(ep.owner_struct, "m_Affixes"); return (bool)affp;
}
static json gear_json(const ItemRef& r) {
    Prop specp; uintptr_t spec = spec_of(r, specp);
    uintptr_t eb; Prop ep; if (!field(spec, specp.owner_struct, "m_EquipmentItemSubSpec", eb, ep)) return json();
    const uintptr_t e = eb + ep.offset; Prop affp = sf(ep.owner_struct, "m_Affixes");
    auto arr = ue::array_of(e, affp); Prop ip = ue::array_inner(affp);
    json af = json::array(); for (int i = 0; i < arr.num && i < 32; ++i) af.push_back(affix_json((uintptr_t)arr.data + i * ip.size, ip.owner_struct));
    const std::string tag = tagname(spec, specp.owner_struct, "m_ItemTag.TagName");
    json j = { {"handle", handle_of(r)}, {"equipped", r.type == 1}, {"tag", tag}, {"level", (int)num(spec, specp.owner_struct, "m_nItemLevel")},
               {"rarity", (int)num(e, ep.owner_struct, "m_eEquipmentRarity")}, {"rerollsLeft", (int)num(e, ep.owner_struct, "m_nRerollLeft")}, {"affixes", af} };
    std::string nm = item_name(tag); if (!nm.empty()) j["name"] = nm;
    return j;
}
static json gear() {
    json out = json::array();
    for (auto& r : items_of({ 1, 0 })) { Prop sp; uintptr_t s = spec_of(r, sp); if (handle_of(r) == 0 || tagname(s, sp.owner_struct, "m_ItemTag.TagName") == "None") continue; json g = gear_json(r); if (g.is_object()) out.push_back(g); }
    return out;
}
static std::map<std::string, std::vector<std::vector<uint8_t>>> g_affix_cache;   // "h:ai" -> game-made affix structs
static Prop g_affix_prop;
static json gear_roll(int64_t h, int ai, double quality, const std::string& want_attr, int tries, int& rolls) {
    ItemRef r; uintptr_t spec, ab; Prop specp, affp; if (!gear_parts(h, r, spec, specp, ab, affp)) return err("no item");
    auto arr = ue::array_of(ab, affp); Prop ip = ue::array_inner(affp); g_affix_prop = ip;
    if (ai < 1 || ai > arr.num) return err("no affix");
    const uintptr_t cur = (uintptr_t)arr.data + (ai - 1) * ip.size;
    uintptr_t L = lib("HnSItemRerollLibrary"); uintptr_t pawn = player_pawn();
    std::vector<std::vector<uint8_t>> got; json list = json::array();
    for (rolls = 1; rolls <= std::max(1, tries); ++rolls) {
        Params p = ue::params_for(L, "GetRerollEquipmentAffix"); if (!p) return err("no function");
        memcpy(p.at(arg(p, 0)), &pawn, 8); Prop a1 = arg(p, 1), a2 = arg(p, 2), a3 = arg(p, 3);
        mread(spec, p.at(a1), a1.size); mread(cur, p.at(a2), a2.size); float q = (float)quality; memcpy(p.at(a3), &q, 4);
        std::string e; if (!call_on(L, p, &e)) return err(e);
        auto res = *(ue::TArray<uint8_t>*)p.ret_ptr();
        for (int i = 0; i < res.num && i < 16; ++i) {
            std::vector<uint8_t> raw(ip.size); mread((uintptr_t)res.data + i * ip.size, raw.data(), ip.size);
            json aj = affix_json((uintptr_t)raw.data(), ip.owner_struct);
            if (want_attr.empty()) { got.push_back(raw); list.push_back(aj); }
            else if (aj["attr"] == want_attr) { got = { raw }; aj["rolls"] = rolls; g_affix_cache[std::to_string(h) + ":" + std::to_string(ai)] = got; return aj; }
        }
        if (want_attr.empty()) break;
    }
    if (!want_attr.empty()) return err("the game did not offer " + want_attr + " in " + std::to_string(tries) + " rolls");
    g_affix_cache[std::to_string(h) + ":" + std::to_string(ai)] = got;
    return list;
}
static json gear_attrs(int64_t h, int ai) {
    ItemRef r; uintptr_t spec, ab; Prop specp, affp; if (!gear_parts(h, r, spec, specp, ab, affp)) return err("no item");
    auto arr = ue::array_of(ab, affp); Prop ip = ue::array_inner(affp); if (ai < 1 || ai > arr.num) return err("no affix");
    const uintptr_t cur = (uintptr_t)arr.data + (ai - 1) * ip.size;
    uintptr_t L = lib("HnSItemRerollLibrary"); uintptr_t pawn = player_pawn();
    Params p = ue::params_for(L, "GetRerollEquipmentAffixPossibilities"); if (!p) return err("no function");
    memcpy(p.at(arg(p, 0)), &pawn, 8); Prop a1 = arg(p, 1), a2 = arg(p, 2); mread(spec, p.at(a1), a1.size); mread(cur, p.at(a2), a2.size);
    std::string e; if (!call_on(L, p, &e)) return err(e);
    Prop rp; for (auto& pp : ue::props(p.fn, false)) if (pp.flags & ue::CPF_ReturnParm) rp = pp;
    Prop gp = ue::array_inner(rp); auto res = *(ue::TArray<uint8_t>*)p.ret_ptr();
    json out = json::array(); std::set<std::string> seen;
    for (int i = 0; i < res.num && i < 128; ++i) {
        const uintptr_t g = (uintptr_t)res.data + i * gp.size; uintptr_t b; Prop ap;
        if (!field(g, gp.owner_struct, "m_AttributeToModify", b, ap)) continue;
        std::string n = ue::fstring(b + ap.offset); if (n.empty() || seen.count(n)) continue; seen.insert(n);
        json o = { {"attr", n} };
        Params l = ue::params_for(L, "GetAttributesLimitsForGivenItem");
        if (l) { Prop l0 = arg(l, 0), l1 = arg(l, 1); mread(g, l.at(l0), l0.size); mread(spec, l.at(l1), l1.size);
            if (call_on(L, l)) { o["max"] = l.out<float>(arg(l, 2).name); o["min"] = l.out<float>(arg(l, 3).name); } }
        out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) { return a["attr"].get<std::string>() < b["attr"].get<std::string>(); });
    return out;
}
static json gear_set(int64_t h, int ai, int n1, const json& value, const json& divine) {
    auto it = g_affix_cache.find(std::to_string(h) + ":" + std::to_string(ai));
    if (it == g_affix_cache.end() || n1 < 1 || n1 > (int)it->second.size()) return err("call gear_options first");
    std::vector<uint8_t> a = it->second[n1 - 1]; const uintptr_t ab = (uintptr_t)a.data(); const uintptr_t st = g_affix_prop.owner_struct;
    uintptr_t b; Prop p;
    if (value.is_number() && field(ab, st, "m_fAffixValue", b, p)) ue::set_num(b, p, value.get<double>());
    if (divine.is_boolean() && field(ab, st, "m_bDivine", b, p)) ue::set_num(b, p, divine.get<bool>() ? 1 : 0);
    ItemRef r; uintptr_t spec, afb; Prop specp, affp; if (!gear_parts(h, r, spec, specp, afb, affp)) return err("no item");
    uintptr_t L = lib("HnSItemRerollLibrary"); Params c = ue::params_for(L, "ApplyRerollChangeEquipmentAffix"); if (!c) return err("no function");
    uintptr_t hb; Prop hp; if (!field(spec, specp.owner_struct, "m_Handle", hb, hp)) return err("no handle");
    mread(hb + hp.offset, c.at(arg(c, 0)), arg(c, 0).size);
    int32_t idx = ai - 1; memcpy(c.at(arg(c, 1)), &idx, 4);
    memcpy(c.at(arg(c, 2)), a.data(), std::min<size_t>(a.size(), arg(c, 2).size));
    uintptr_t inv = player_inv(); memcpy(c.at(arg(c, 3)), &inv, 8);
    c.put_bool(arg(c, 4).name, true); c.put_bool(arg(c, 5).name, true);
    // bDebugFree also gives a reroll back (+1 m_nRerollLeft per apply) -> keep the item's own count unchanged
    double rerolls = -1; { uintptr_t rb; Prop rp; if (field(spec, specp.owner_struct, "m_EquipmentItemSubSpec.m_nRerollLeft", rb, rp)) rerolls = ue::get_num(rb, rp); }
    std::string e; if (!call_on(L, c, &e)) return err(e);
    ItemRef now; const bool have = find_item(h, now);
    if (have && rerolls >= 0) { Prop sp2; uintptr_t s2 = spec_of(now, sp2); uintptr_t rb; Prop rp; if (field(s2, sp2.owner_struct, "m_EquipmentItemSubSpec.m_nRerollLeft", rb, rp)) ue::set_num(rb, rp, rerolls); }
    return { {"ok", c.ret<bool>()}, {"item", have ? gear_json(now) : json()} };
}

// wyrmling ---------------------------------------------------------------------------------------------------
// equipped wyrmling = the item in inventory container 2, read straight from memory.
// (NOT GetEquippedDragonCompanion: it returns a 360-byte FHnSItemSpec by value; the game assigns it into our zeroed
// parameter buffer, and zero bytes are not a valid FText / shared pointer -> corrupted refcounts -> crash at game exit,
// bisected 2026-11-09 06:1x. Rule: never call a function that returns or fills a struct with FText/TSharedPtr members.)
// Container 2 holds every wyrmling the character owns; the equipped one has m_bEquipped set (fixed 2026-11-09:
// the first entry was taken before, which showed an unequipped level-1 wyrm).
static json wyrm() {
    if (!player_inv()) return err("no character");
    for (int pass = 0; pass < 2; ++pass)
    for (auto& r : items_of({ 2 })) {
        const int64_t h = handle_of(r); if (!h) continue;
        Prop specp; uintptr_t s = spec_of(r, specp); const uintptr_t st = specp.owner_struct;
        uintptr_t db; Prop dp; if (!field(s, st, "m_DragonCompanionItemSubSpec", db, dp)) continue;
        const uintptr_t d = db + dp.offset;
        { Prop ep = sf(dp.owner_struct, "m_bEquipped"); if (pass == 0 && !(ep && ue::get_bool(d, ep))) continue; }
        json traits = json::object(); Prop tp = sf(dp.owner_struct, "m_TraitPoints");
        if (tp) { auto arr = ue::array_of(d, tp); const char* T[] = { "Power", "Malice", "Ferocity", "Loyalty" };
            for (int i = 0; i < arr.num && i < 4; ++i) { int32_t v = 0; rd_as((uintptr_t)arr.data + i * 4, v); traits[T[i]] = v; } }
        uintptr_t g = ue::find_object("/Script/HnS.Default__HnSGlobalConfig");
        return { {"handle", h}, {"equipped", pass == 0}, {"tag", tagname(s, st, "m_ItemTag.TagName")}, {"level", (int)num(d, dp.owner_struct, "m_nLevel")}, {"xp", num(d, dp.owner_struct, "m_fXp")}, {"traits", traits},
                 {"maxLevel", g ? (int)num(g, ue::class_of(g), "m_nMaxDragonCompanionLevel", 50) : 50} };
    }
    return err("no wyrmling equipped");
}
static json wyrm_level(int lvl) {
    json w = wyrm(); if (w.contains("error")) return w;
    uintptr_t inv = player_inv(); Params p = ue::params_for(inv, "SetLvlXpForDragonCompanion"); if (!p) return err("no function");
    // FHnSItemSpecHandle is {u8 m_eInvType; int32 Handle} - copy the item's own m_Handle bytes (writing the bare
    // number put it in the wrong fields: the game found no item and silently did nothing; fixed 2026-11-09).
    bool found = false;
    for (auto& r : items_of({ 2 })) {
        if (handle_of(r) != w["handle"].get<int64_t>()) continue;
        Prop specp; uintptr_t spec = spec_of(r, specp);
        uintptr_t hb; Prop hp; if (!field(spec, specp.owner_struct, "m_Handle", hb, hp)) break;
        mread(hb + hp.offset, p.at(arg(p, 0)), std::min<int>(hp.size, arg(p, 0).size)); found = true; break;
    }
    if (!found) return err("wyrmling handle not found");
    int32_t l = std::clamp(lvl, 1, w.value("maxLevel", 50)); memcpy(p.at(arg(p, 1)), &l, 4);
    float xp = 0; memcpy(p.at(arg(p, 2)), &xp, 4);
    std::string e; if (!call_on(inv, p, &e)) return err(e);
    json n = wyrm(); json t = n.value("traits", json::object());
    n["text"] = fmt2("level %d  P%d M%d F%d L%d", n.value("level", 0), t.value("Power", 0), t.value("Malice", 0), t.value("Ferocity", 0), t.value("Loyalty", 0));
    return n;
}
std::string editor_wyrm_text() { json w = wyrm(); return w.contains("error") ? "none equipped" : fmt2("level %d / %d", w.value("level", 0), w.value("maxLevel", 50)); }

// city -------------------------------------------------------------------------------------------------------
static bool city_parts(uintptr_t& w, uintptr_t& h, uintptr_t& prog, uintptr_t& prog_st, std::string& e) {
    // Found once per character load (two full object scans). Outside the city the miss is remembered: the old code
    // rescanned for every city readout = 8 full scans in one frame every 0.5 s = the "pause every half second" stutter.
    static uintptr_t c_pawn = 1, c_w = 0, c_h = 0; static ULONGLONG c_since = 0, c_try = 0;
    const uintptr_t pawn = player_pawn(); const ULONGLONG t = GetTickCount64();
    if (pawn != c_pawn) { c_pawn = pawn; c_w = c_h = 0; c_since = t; c_try = 0; }
    const bool have = c_w && c_h && ue::alive(c_w) && ue::alive(c_h);
    if (!have && pawn && (c_try == 0 || (t - c_since < 20000 && t - c_try >= 3000))) {   // retry while the level streams in
        c_try = t; c_w = first_of("HnSShared_CityState_ServerWrapper"); c_h = c_w ? first_of("HnSCityHubDataComponent") : 0;
    }
    w = have || (c_w && c_h) ? c_w : 0; h = w ? c_h : 0;
    if (!w || !h) { e = "not in the city hub (Montescail) - city data is only loaded there"; return false; }
    Params a = ue::params_for(w, "HasAuthority"); if (a && call_on(w, a) && !a.ret<bool>()) { e = "not the host - city edits are solo only"; return false; }
    uintptr_t b; Prop p; if (!field(w, ue::class_of(w), "m_CityState.m_Progress", b, p)) { e = "no city progress"; return false; }
    prog = b + p.offset; prog_st = p.owner_struct; return true;
}
static double hub_num(uintptr_t h, const char* fn, double in) {
    Params p = ue::params_for(h, fn); if (!p) return 0;
    Prop a0 = arg(p, 0); if (a0) ue::set_num((uintptr_t)p.buf.data(), a0, in);
    if (!call_on(h, p)) return 0;
    Prop rp; for (auto& pp : ue::props(p.fn, false)) if (pp.flags & ue::CPF_ReturnParm) rp = pp;
    return rp ? ue::get_num((uintptr_t)p.buf.data(), rp) : 0;
}
static void city_notify(uintptr_t w) {
    uintptr_t s = first_of("HnSGameEventSubsystem"); if (!s) return;
    Params p = ue::params_for(s, "TriggerCityStateUpdated"); if (!p) return;
    uintptr_t b; Prop cs; if (!field(w, ue::class_of(w), "m_CityState", b, cs)) return;
    Prop a0 = arg(p, 0), a1 = arg(p, 1);
    mread(b + cs.offset, p.at(a0), std::min(a0.size, cs.size)); mread(b + cs.offset, p.at(a1), std::min(a1.size, cs.size));
    call_on(s, p);
}
json editor_city_info() {
    uintptr_t w, h, pr, pst; std::string e; if (!city_parts(w, h, pr, pst, e)) return err(e);
    const int lvl = (int)num(pr, pst, "m_nCityLevel");
    Prop up = sf(pst, "m_Upgrades");
    return { {"level", lvl}, {"xp", num(pr, pst, "m_fCityXp")}, {"points", (int)num(pr, pst, "m_nUpgradePoints")},
             {"upgrades", up ? ue::array_of(pr, up).num : 0}, {"next_xp", hub_num(h, "GetXpForCityLevel", lvl + 1)} };
}
static int reward_points(uintptr_t h, int from, int to) {
    int n = 0;
    for (int l = from + 1; l <= to; ++l) {
        Params p = ue::params_for(h, "K2_GetCityLevelReward"); if (!p) break;
        Prop a0 = arg(p, 0), a1 = arg(p, 1); int32_t L = l; memcpy(p.at(a0), &L, 4);
        if (!call_on(h, p)) continue;
        Prop pts = sf(a1.owner_struct, "m_CityUpgradePoint"); if (pts) n += (int)ue::get_num((uintptr_t)p.at(a1), pts);
    }
    return n;
}
static json city_set_level(int L) {
    uintptr_t w, h, pr, pst; std::string e; if (!city_parts(w, h, pr, pst, e)) return err(e);
    L = std::clamp(L, 1, 50);
    uintptr_t b; Prop lp, xp, pp;
    field(pr, pst, "m_nCityLevel", b, lp); field(pr, pst, "m_fCityXp", b, xp); field(pr, pst, "m_nUpgradePoints", b, pp);
    const int old = (int)ue::get_num(pr, lp); const int delta = L > old ? reward_points(h, old, L) : 0;
    ue::set_num(pr, lp, L); ue::set_num(pr, xp, hub_num(h, "GetXpForCityLevel", L));
    const int pts = (int)ue::get_num(pr, pp) + delta; ue::set_num(pr, pp, pts);
    city_notify(w);
    return { {"from", old}, {"to", L}, {"points_added", delta}, {"points", pts}, {"text", fmt2("level %d (+%d pts)", L, delta)} };
}
static json city_add_points(int n) {
    uintptr_t w, h, pr, pst; std::string e; if (!city_parts(w, h, pr, pst, e)) return err(e);
    uintptr_t b; Prop pp; field(pr, pst, "m_nUpgradePoints", b, pp);
    const int pts = std::max(0, (int)ue::get_num(pr, pp) + n); ue::set_num(pr, pp, pts); city_notify(w);
    return { {"points", pts}, {"text", fmt2("%d points", pts)} };
}
static bool hub_tag_bool(uintptr_t h, const char* fn, FName tag) {
    Params p = ue::params_for(h, fn); if (!p) return false; memcpy(p.at(arg(p, 0)), &tag, 8);
    return call_on(h, p) && p.ret<bool>();
}
static json city_unlock_all() {
    uintptr_t w, h, pr, pst; std::string e; if (!city_parts(w, h, pr, pst, e)) return err(e);
    uintptr_t pc = player_pc(); int n = 0, tried = 0, left = 0;
    for (int round = 0; round < 12; ++round) {
        bool moved = false; left = 0;
        for (int d = 0; d <= 6; ++d) {
            Params p = ue::params_for(h, "K2_GetAllUpgradeForSpecificDistrict"); if (!p) return err("no function");
            Prop a0 = arg(p, 0), a1 = arg(p, 1); ue::set_num((uintptr_t)p.buf.data(), a0, d);
            if (!call_on(h, p)) continue;
            auto arr = *(ue::TArray<FName>*)p.at(a1);
            for (int i = 0; i < arr.num && i < 200; ++i) {
                FName t; rd_as((uintptr_t)arr.data + i * 8, t);
                if (hub_tag_bool(h, "IsUpgradeUnlocked", t)) continue;
                const bool avail = hub_tag_bool(h, "IsUpgradeNextToUnlocked", t) && !hub_tag_bool(h, "IsUpgradeLockedByRequirement", t) && hub_tag_bool(h, "IsUpgradeAvailableInThisVersion", t);
                if (!avail) { if (hub_tag_bool(h, "IsUpgradeAvailableInThisVersion", t)) ++left; continue; }
                Params u = ue::params_for(h, "TryUnlockUpgrade"); if (!u) continue;
                memcpy(u.at(arg(u, 0)), &pc, 8); memcpy(u.at(arg(u, 1)), &t, 8); ++tried;
                call_on(h, u);
                if (hub_tag_bool(h, "IsUpgradeUnlocked", t)) { ++n; moved = true; } else ++left;
            }
        }
        if (!moved) break;
    }
    uintptr_t b; Prop pp; field(pr, pst, "m_nUpgradePoints", b, pp); const int pts = (int)ue::get_num(pr, pp);
    return { {"n", n}, {"tried", tried}, {"still_locked", left}, {"points_left", pts}, {"text", fmt2("%d unlocked, %d still locked, %d pts left", n, left, pts)} };
}

// ------------------------------------------------------------------ dispatch ("DKE.x" / "DKC.x" names kept from the Lua API)
// ------------------------------------------------------------------ Ravenous weapons (2026-11-09, research + giver)
static bool find_any_item(int64_t h, ItemRef& out) {
    for (auto& r : items_of({ 0, 1, 2, 3, 4, 5, 6, 7 })) if (handle_of(r) == h) { out = r; return true; }
    return false;
}
static std::string hexdump(uintptr_t a, size_t n) {
    std::vector<uint8_t> b(n); if (!mread(a, b.data(), n)) return "";
    static const char* H = "0123456789ABCDEF"; std::string o; o.reserve(n * 2);
    for (uint8_t c : b) { o += H[c >> 4]; o += H[c & 15]; } return o;
}
static json item_spec(int64_t h) {
    ItemRef r; if (!find_any_item(h, r)) return err("no item");
    Prop specp; uintptr_t spec = spec_of(r, specp);
    json j = { {"container", r.type}, {"entry", fmt2("0x%llX", (unsigned long long)r.entry)}, {"spec", fmt2("0x%llX", (unsigned long long)spec)},
               {"spec_size", specp.size}, {"spec_hex", hexdump(spec, specp.size)}, {"tag", tagname(spec, specp.owner_struct, "m_ItemTag.TagName")} };
    uintptr_t eb; Prop ep;
    if (field(spec, specp.owner_struct, "m_EquipmentItemSubSpec", eb, ep)) {
        const uintptr_t e = eb + ep.offset, est = ep.owner_struct;
        j["corruption_tier"] = (int)num(e, est, "m_nCorruptionTier"); j["reroll_left"] = (int)num(e, est, "m_nRerollLeft");
        Prop ap = sf(est, "m_Addons"); if (ap) { auto arr = ue::array_of(e, ap); Prop ip = ue::array_inner(ap);
            j["addons"] = { {"num", arr.num}, {"inner", ip.owner_struct ? ue::name_of(ip.owner_struct) : ip.type}, {"size", ip.size},
                            {"hex", arr.num > 0 && arr.num < 64 ? hexdump((uintptr_t)arr.data, arr.num * ip.size) : ""} }; }
    }
    return j;
}
static uintptr_t ravenous_table() {
    uintptr_t cdo = ue::find_object("/Script/HnS.Default__HnSInventoryDataInfo"); if (!cdo) return 0;
    Prop tp = ue::find_prop(ue::class_of(cdo), "m_pRavenousTable"); if (!tp) return 0;
    FName pk{}, as{}; mread(cdo + tp.offset + 8, &pk, 8); mread(cdo + tp.offset + 0x10, &as, 8);
    return ue::load_asset(ue::name_str(pk) + "." + ue::name_str(as));
}
// every Ravenous preset: name, effect text, max voracity, item tag per class
static json ravenous_list() {
    const uintptr_t dt = ravenous_table(); if (!dt) return err("ravenous table not loaded");
    uintptr_t rs = 0, data = 0; int32_t num = 0; rd_as(dt + 0x28, rs); rd_as(dt + 0x30, data); rd_as(dt + 0x38, num);
    if (!rs || !data || num <= 0 || num > 200) return err("bad table");
    Prop tp = sf(rs, "m_ItemTagPerClass"), vp = sf(rs, "m_nMaxVoracity"), xp = sf(rs, "m_EffectText"), ep = sf(rs, "m_bDropOnlyInEndgame");
    json out = json::array();
    for (int32_t i = 0; i < num; ++i) {
        FName n{}; uintptr_t row = 0; if (!mread(data + i * 24, &n, 8) || !rd_as(data + i * 24 + 8, row) || !row) continue;
        json tags = json::object();
        if (tp) { Prop kp, vp2; int32_t voff, stride; if (ue::map_kv(tp, kp, vp2, voff, stride)) for (auto& kv : ue::map_entries(row, tp)) {
            uint8_t cls = 0; rd_as(kv.first, cls); FName t{}; mread(kv.second, &t, 8); tags[std::to_string(cls)] = ue::name_str(t); } }
        out.push_back({ {"name", ue::name_str(n)}, {"max_voracity", vp ? (int)ue::get_num(row, vp) : 0},
                        {"endgame", ep ? ue::get_bool(row, ep) : false}, {"text", xp ? ue::ftext(row + xp.offset) : ""}, {"tags", tags} });
    }
    return out;
}
// ---- Ravenous: make / voracity refill / devour (game functions; the item is re-sent with the game's own
// ModifyItem path - ChangeItemSacrificedState with its current value - so the inventory UI shows the change)
// Voracity left = preset m_nMaxVoracity - FHnSEquipmentItemSubSpec.m_nRerollLeft (= devours used on Ravenous items),
// from the game's GetRavenousRerollLeft (exe+0x55541C0).
static int player_class() {
    uintptr_t pw = player_pawn(); Prop pc = pw ? ue::find_prop(ue::class_of(pw), "m_eCharacterType") : Prop();
    return pc ? (int)ue::get_num(pw, pc) : -1;
}
static json g_rav_cache; static ULONGLONG g_rav_at = 0;
static const json& ravenous_presets() {
    if (!g_rav_cache.is_array() || g_rav_cache.empty() || GetTickCount64() - g_rav_at > 300000) { json l = ravenous_list(); if (l.is_array()) { g_rav_cache = l; g_rav_at = GetTickCount64(); } }
    return g_rav_cache;
}
static json ravenous_of_tag(const std::string& tag) {          // preset row for an item tag, or null
    for (auto& r : ravenous_presets()) for (auto& t : r["tags"].items()) if (t.value() == tag) return r;
    return json();
}
static bool equip_sub(const ItemRef& r, uintptr_t& spec, Prop& specp, uintptr_t& e, uintptr_t& est) {
    spec = spec_of(r, specp); uintptr_t eb; Prop ep; if (!field(spec, specp.owner_struct, "m_EquipmentItemSubSpec", eb, ep)) return false;
    e = eb + ep.offset; est = ep.owner_struct; return true;
}
static void item_resend(uintptr_t spec, const Prop& specp) {
    uintptr_t inv = player_inv(); Params c = ue::params_for(inv, "ChangeItemSacrificedState"); if (!c) return;
    uintptr_t hb; Prop hp; if (!field(spec, specp.owner_struct, "m_Handle", hb, hp)) return;
    mread(hb + hp.offset, c.at(arg(c, 0)), arg(c, 0).size);
    const bool cur = num(spec, specp.owner_struct, "m_bMarkedAsSacrificed") != 0; c.put_bool(arg(c, 1).name, cur);
    call_on(inv, c);
}
static int voracity_left(uintptr_t spec, const Prop& specp) {
    uintptr_t L = lib("HnSItemRerollLibrary"); Params p = ue::params_for(L, "GetRavenousRerollLeft"); if (!p) return -1;
    Prop a = arg(p, 0); mread(spec, p.at(a), std::min<size_t>(a.size, specp.size)); return call_on(L, p) ? p.ret<int32_t>() : -1;
}
json ravenous_info(int64_t h) {
    ItemRef r; if (!find_any_item(h, r)) return json();
    Prop specp; uintptr_t spec = spec_of(r, specp); const std::string tag = tagname(spec, specp.owner_struct, "m_ItemTag.TagName");
    json pr = ravenous_of_tag(tag); if (pr.is_null()) return json();
    return { {"name", pr["name"]}, {"text", pr["text"]}, {"max", pr["max_voracity"]}, {"left", voracity_left(spec, specp)} };
}
static json ravenous_make(int64_t h, const std::string& preset) {
    ItemRef r; if (!find_any_item(h, r)) return err("no item");
    if (r.type != 0) return err("take the weapon off first (bag items only)");
    uintptr_t spec, e, est; Prop specp; if (!equip_sub(r, spec, specp, e, est)) return err("not equipment");
    const std::string tag = tagname(spec, specp.owner_struct, "m_ItemTag.TagName");
    const std::string hand = tag.substr(tag.rfind('.') + 1);
    if (hand != "LeftHand" && hand != "RightHand") return err("only weapons (left or right hand) can be Ravenous");
    const int cls = player_class(); json pick;
    for (auto& p : ravenous_presets()) if (p["name"] == preset) pick = p;
    if (pick.is_null()) return err("unknown Ravenous weapon " + preset);
    const std::string want = pick["tags"].value(std::to_string(cls), "");
    if (want.empty()) return err("no " + preset + " for this class");
    if (want.substr(want.rfind('.') + 1) != hand) return err(preset + " is a " + want.substr(want.rfind('.') + 1) + " weapon - pick a " + hand + " Ravenous weapon or another item");
    FName fn{}; if (!ue::name_find(want, fn)) return err("unknown tag " + want);
    uintptr_t b; Prop tp; if (!field(spec, specp.owner_struct, "m_ItemTag.TagName", b, tp)) return err("no tag field");
    ue::set<FName>(b, tp, fn);
    Prop rp = sf(est, "m_nRerollLeft"); if (rp) ue::set_num(e, rp, 0);         // full voracity
    item_resend(spec, specp);
    return { {"ok", true}, {"tag", want}, {"ravenous", ravenous_info(h)} };
}
// Ravenous weapons this item can become: same class, same hand
static json ravenous_choices(int64_t h) {
    ItemRef r; if (!find_any_item(h, r)) return err("no item");
    Prop specp; uintptr_t spec = spec_of(r, specp); const std::string tag = tagname(spec, specp.owner_struct, "m_ItemTag.TagName");
    const std::string hand = tag.substr(tag.rfind('.') + 1); const std::string cls = std::to_string(player_class());
    json out = json::array();
    if (hand != "LeftHand" && hand != "RightHand") return out;
    for (auto& p : ravenous_presets()) { const std::string t = p["tags"].value(cls, "");
        if (!t.empty() && t.substr(t.rfind('.') + 1) == hand) out.push_back({ {"name", p["name"]}, {"text", p["text"]}, {"max", p["max_voracity"]} }); }
    return out;
}
static json voracity_refill(int64_t h) {
    ItemRef r; if (!find_any_item(h, r)) return err("no item");
    uintptr_t spec, e, est; Prop specp; if (!equip_sub(r, spec, specp, e, est)) return err("not equipment");
    if (ravenous_of_tag(tagname(spec, specp.owner_struct, "m_ItemTag.TagName")).is_null()) return err("not a Ravenous weapon");
    Prop rp = sf(est, "m_nRerollLeft"); if (!rp) return err("no counter"); ue::set_num(e, rp, 0);
    item_resend(spec, specp);
    return ravenous_info(h);
}
static std::map<std::string, std::vector<std::vector<uint8_t>>> g_feed_cache;
// options the game offers when the Ravenous weapon (rh) devours another item (oh), replacing affix ai (1-based, 0 = none)
static json feed_options(int64_t rh, int64_t oh, int ai) {
    ItemRef rr, orr; if (!find_any_item(rh, rr) || !find_any_item(oh, orr)) return err("no item");
    if (rh == oh) return err("pick another item to devour");
    uintptr_t rspec, re, rest, ospec, oe, oest; Prop rsp, osp;
    if (!equip_sub(rr, rspec, rsp, re, rest) || !equip_sub(orr, ospec, osp, oe, oest)) return err("both must be equipment");
    uintptr_t L = lib("HnSItemRerollLibrary"); uintptr_t pawn = player_pawn();
    bool select = true;
    { Params q = ue::params_for(L, "ShouldRavenousFeedSelectAnAffixeToReroll"); if (q) { Prop a0 = arg(q, 0), a1 = arg(q, 1);
        mread(rspec, q.at(a0), std::min<size_t>(a0.size, rsp.size)); mread(ospec, q.at(a1), std::min<size_t>(a1.size, osp.size));
        if (call_on(L, q)) select = q.ret<bool>(); } }
    Prop affp = sf(rest, "m_Affixes"); auto arr = ue::array_of(re, affp); Prop ip = ue::array_inner(affp); g_affix_prop = ip;
    Params p = ue::params_for(L, "GetRavenousFeedEquipmentAffixPossibilities"); if (!p) return err("no function");
    memcpy(p.at(arg(p, 0)), &pawn, 8); Prop a1 = arg(p, 1), a2 = arg(p, 2), a3 = arg(p, 3);
    mread(rspec, p.at(a1), std::min<size_t>(a1.size, rsp.size)); mread(ospec, p.at(a2), std::min<size_t>(a2.size, osp.size));
    if (ai >= 1 && ai <= arr.num) mread((uintptr_t)arr.data + (ai - 1) * ip.size, p.at(a3), std::min<size_t>(a3.size, ip.size));
    else if (ai < 0) {                                              // negative = an affix of the item being devoured
        Prop oaff = sf(oest, "m_Affixes"); auto oarr = ue::array_of(oe, oaff);
        if (-ai <= oarr.num) mread((uintptr_t)oarr.data + (-ai - 1) * ip.size, p.at(a3), std::min<size_t>(a3.size, ip.size));
    }
    std::string e; if (!call_on(L, p, &e)) return err(e);
    auto res = *(ue::TArray<uint8_t>*)p.ret_ptr();
    std::vector<std::vector<uint8_t>> got; json list = json::array();
    for (int i = 0; i < res.num && i < 64; ++i) {
        std::vector<uint8_t> raw(ip.size); mread((uintptr_t)res.data + i * ip.size, raw.data(), ip.size);
        got.push_back(raw); list.push_back(affix_json((uintptr_t)raw.data(), ip.owner_struct));
    }
    g_feed_cache[std::to_string(rh) + ":" + std::to_string(oh) + ":" + std::to_string(ai)] = got;
    return { {"select_affix", select}, {"options", list} };
}
// devour: the game's ApplyRavenousFeed with option n1 of feed_options (0 = no affix change); free = refill voracity after
static json feed_apply(int64_t rh, int64_t oh, int ai, int n1, bool free_feed) {
    ItemRef rr, orr; if (!find_any_item(rh, rr) || !find_any_item(oh, orr)) return err("no item");
    uintptr_t rspec, re, rest, ospec, oe, oest; Prop rsp, osp;
    if (!equip_sub(rr, rspec, rsp, re, rest) || !equip_sub(orr, ospec, osp, oe, oest)) return err("both must be equipment");
    if (voracity_left(rspec, rsp) <= 0 && !free_feed) return err("no Voracity left - refill it first");
    // the game indexes m_Affixes[nOriginalAffixIndex] without a check: a weapon with no affixes (or index -1) faults
    // inside ApplyRavenousFeed after the eaten item is already gone (found 2026-11-09) -> refuse before calling.
    { Prop affp = sf(rest, "m_Affixes"); const int na = affp ? ue::array_of(re, affp).num : 0;
      if (na <= 0) return err("this Ravenous weapon has no affixes to swap - give it affixes first (use a magic or rare weapon)");
      if (ai < 1 || ai > na) return err("pick which affix (1-" + std::to_string(na) + ") the devoured item replaces");
      if (n1 < 1) return err("pick the new affix"); }
    if (free_feed) { Prop rp = sf(rest, "m_nRerollLeft"); if (rp) ue::set_num(re, rp, 0); }
    std::vector<uint8_t> affix;
    if (n1 >= 1) { auto it = g_feed_cache.find(std::to_string(rh) + ":" + std::to_string(oh) + ":" + std::to_string(ai));
        if (it == g_feed_cache.end() || n1 > (int)it->second.size()) return err("get the options first"); affix = it->second[n1 - 1]; }
    uintptr_t L = lib("HnSItemRerollLibrary"); uintptr_t pawn = player_pawn(), inv = player_inv();
    Params p = ue::params_for(L, "ApplyRavenousFeed"); if (!p) return err("no function");
    memcpy(p.at(arg(p, 0)), &pawn, 8); Prop a1 = arg(p, 1), a2 = arg(p, 2), a4 = arg(p, 4);
    mread(rspec, p.at(a1), std::min<size_t>(a1.size, rsp.size)); mread(ospec, p.at(a2), std::min<size_t>(a2.size, osp.size));
    const int32_t idx = ai >= 1 ? ai - 1 : -1; memcpy(p.at(arg(p, 3)), &idx, 4);
    if (!affix.empty()) memcpy(p.at(a4), affix.data(), std::min<size_t>(affix.size(), a4.size));
    memcpy(p.at(arg(p, 5)), &inv, 8);
    std::string e; if (!call_on(L, p, &e)) return err(e);
    const bool okv = p.ret<bool>();
    ItemRef now; json out = { {"ok", okv} };
    if (find_any_item(rh, now)) {
        uintptr_t s2, e2, est2; Prop sp2;
        if (free_feed && equip_sub(now, s2, sp2, e2, est2)) { Prop rp = sf(est2, "m_nRerollLeft"); if (rp) ue::set_num(e2, rp, 0); item_resend(s2, sp2); }
        out["item"] = gear_json(now); out["ravenous"] = ravenous_info(rh);
    }
    return out;
}
json editor_call(const std::string& fn, const json& a) {
    auto I = [&](size_t i, double dv = 0) { return (a.is_array() && i < a.size() && a[i].is_number()) ? a[i].get<double>() : dv; };
    auto S = [&](size_t i) { return (a.is_array() && i < a.size() && a[i].is_string()) ? a[i].get<std::string>() : std::string(); };
    auto B = [&](size_t i) { return a.is_array() && i < a.size() && a[i].is_boolean() && a[i].get<bool>(); };
    int rolls = 0;
    if (fn == "DKE.fragments") return fragments(S(0));
    if (fn == "DKE.frag_options") return frag_options((int64_t)I(0), (int)I(1));
    if (fn == "DKE.frag_set") return frag_set((int64_t)I(0), (int)I(1), S(2), (int)I(3), B(4));
    if (fn == "DKE.frag_max") return frag_max((int64_t)I(0));
    if (fn == "DKE.gear") { json g = gear();                // + Ravenous info (name / effect / voracity) on Ravenous weapons
        if (g.is_array()) for (auto& x : g) if (!ravenous_of_tag(x.value("tag", "")).is_null()) { json ri = ravenous_info(x.value("handle", (int64_t)0)); if (!ri.is_null()) x["ravenous"] = ri; }
        return g; }
    if (fn == "DKE.ravenous_choices") return ravenous_choices((int64_t)I(0));
    if (fn == "DKE.gear_options") return gear_roll((int64_t)I(0), (int)I(1), I(2, 5), "", 1, rolls);
    if (fn == "DKE.gear_pick") { json r = gear_roll((int64_t)I(0), (int)I(1), I(3, 5), S(2), (int)I(4, 150), rolls); return r; }
    if (fn == "DKE.gear_attrs") return gear_attrs((int64_t)I(0), (int)I(1));
    if (fn == "DKE.gear_set") return gear_set((int64_t)I(0), (int)I(1), (int)I(2, 1), a.is_array() && a.size() > 3 ? a[3] : json(), a.is_array() && a.size() > 4 ? a[4] : json());
    if (fn == "DKE.spec") return item_spec((int64_t)I(0));
    if (fn == "DKE.ravenous_list") return ravenous_list();
    if (fn == "DKE.ravenous_presets") return ravenous_presets();
    if (fn == "DKE.ravenous_info") return ravenous_info((int64_t)I(0));
    if (fn == "DKE.ravenous_make") return ravenous_make((int64_t)I(0), S(1));
    if (fn == "DKE.voracity_refill") return voracity_refill((int64_t)I(0));
    if (fn == "DKE.feed_options") return feed_options((int64_t)I(0), (int64_t)I(1), (int)I(2));
    if (fn == "DKE.feed_apply") return feed_apply((int64_t)I(0), (int64_t)I(1), (int)I(2), (int)I(3), a.is_array() && a.size() > 4 ? a[4].get<bool>() : true);
    if (fn == "DKE.wyrm") return wyrm();
    if (fn == "DKE.wyrm_level") return wyrm_level((int)I(0, 1));
    if (fn == "DKE.wyrm_level_plus") { json w = wyrm(); if (w.contains("error")) return w; return wyrm_level(std::min(50, w.value("level", 1) + (int)I(0, 10))); }
    if (fn == "DKC.info") return editor_city_info();
    if (fn == "DKC.set_level") return city_set_level((int)I(0, 1));
    if (fn == "DKC.level_up") { json i = editor_city_info(); if (i.contains("error")) return i; return city_set_level(i.value("level", 1) + 1); }
    if (fn == "DKC.add_points") return city_add_points((int)I(0, 5));
    if (fn == "DKC.unlock_all") return city_unlock_all();
    if (fn == "DKH.check") return json{ {"text", "native engine: names are found at runtime"} };
    if (fn == "DKE.containers") {                      // debug: inventory map keys + item counts
        uintptr_t inv = player_inv(); if (!inv) return err("no inventory");
        Prop mc = ue::find_prop(ue::class_of(inv), "m_ItemContainers"); Prop kp, vp; int32_t voff = 0, stride = 0;
        if (!mc || !ue::map_kv(mc, kp, vp, voff, stride)) return err("no map");
        Prop items = sf(vp.owner_struct, "Items");
        ue::TArray<uint8_t> raw; mread(inv + mc.offset, &raw, sizeof raw);
        json out = { {"inv", ue::full_name(inv)}, {"key", kp.type}, {"value", ue::name_of(vp.owner_struct)}, {"voff", voff}, {"stride", stride},
                     {"items_prop", items ? items.offset : -1}, {"raw_num", raw.num}, {"entries", json::array()} };
        for (auto& kv : ue::map_entries(inv, mc)) { uint8_t t = 0; rd_as(kv.first, t); out["entries"].push_back({ {"key", t}, {"items", items ? ue::array_of(kv.second, items).num : -1} }); }
        return out;
    }
    return err("not callable: " + fn);
}

} // namespace dk
