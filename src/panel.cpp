// Dragonkin Trainer - the in-game panel, opened with the SETTINGS hotkey (default INSERT) (rewritten 2026-11-09 for Dragonkin; the Crimson Desert template's

// panel engine is in _crimson_unused\_orig_kept\panel.cpp).
//
// What it shows comes from the same spec the in-game UMG F8 panel uses: F:\DragonkinBridge\panel\panel.json
//   {title, hotkey, tabs:[{name, sections:[{name, items:[{id, type, label, desc, ...}]}]}]}
//   types: toggle | slider {min,max,step,default,off} | button {button, danger} | readout | input | label/note {text}
//   anything else is shown by its label. The spec is re-read when the file changes (checked every second).
//
// The panel runs NO game logic. Everything goes to the Lua side (UE4SS, Mods\DragonkinBridge\Scripts\nativelink.lua)
// through files in F:\DragonkinBridge\ipc\panel\:
//   req.json   we write (req.json.tmp + rename) a JSON array of requests, only when req.json does not exist; otherwise
//              requests wait in memory and go out with the next write. Lua reads it, deletes it, runs every request.
//                {"seq":N,"op":"set","id":"god","v":true}          toggles / sliders (slider: on release or 250 ms idle)
//                {"seq":N,"op":"click","id":"gold10k","confirmed":false}   buttons (danger: confirmed after our own
//                                                                           second click within 4 s)
//                {"seq":N,"op":"all_off"}
//   state.json Lua writes it every ~0.5 s; we read it at 4 Hz when its mtime changes:
//                {t, seq_done, results:{"<seq>":{ok,res}}, values:{id:v}, readouts:{id:"text"}, keeper:{...},
//                 player:{...}, panel_log:[...]}
//              values are the source of truth for toggles/sliders (synced unless the user is dragging / a request is
//              in flight). state.json older than 5 s = "Lua link offline".
// File IO runs on a small worker thread (never on the game or render thread). Paths can be overridden in
// bin\config.json: "dragonkin_root", "panel_spec", "panel_ipc".
#include "common.h"
#include "ui_kit.h"
#include "dk.h"
#include "ue.h"
#include <fstream>
#include <map>
#include <deque>
#include <set>
#include <algorithm>
#include <thread>
#include <cmath>
#include <functional>
#include <ctime>
#include "imgui.h"
#include "imgui_internal.h"
#include "trinity/gui/icons.h"   // DrawItemIcon (painted skin pieces, skin.cpp)
#include <array>

// what the Lua side is called in the UI (public build: plain words, no developer terms)
#define LINK "Game connected"
#define LINK_OFF "Waiting for the game"
#define LINK_FOOT "LIVE"
#define LINK_FOOT_OFF "WAITING"

namespace wb {

extern ImFont* g_font_body;
extern ImFont* g_font_bold;
extern ImFont* g_font_title;
extern float g_ui_scale;
#define S(x) ((x) * g_ui_scale)

struct Item {
    // from the spec
    std::string id, type, label, desc, button, text;
    bool danger = false;
    double minv = 0, maxv = 1, step = 0, defv = 0, offv = 0;
    bool has_off = false;
    // live
    bool on = false;
    double val = 0;
    bool have_value = false;        // the Lua side reported a value for it
    bool dirty = false;             // slider moved, not sent yet
    ULONGLONG last_edit = 0;
    ULONGLONG confirm_until = 0;    // danger button: second click before this = confirmed
    int64_t last_seq = 0;
    ULONGLONG sent_at = 0;
    bool waiting = false;           // request sent, no result yet
    std::string result;
    bool result_ok = true;
    ULONGLONG result_at = 0;
    std::string readout;
    char input[256] = {};
};
struct Section { std::string name; std::vector<Item> items; };
struct Tab { std::string name; std::vector<Section> sections; };

static std::recursive_mutex g_mtx;
static std::vector<Tab> g_tabs;
static std::string g_title = "DRAGONKIN TRAINER";
static std::string g_load_msg;
static int g_tab = 0;
static char g_search[128] = {};
static FILETIME g_spec_mtime{};
static std::string g_proj, g_spec_path, g_ipc_dir;
static std::string g_drag_id;           // slider being dragged this frame (render thread)

// ---- Lua link (state.json)
struct Link {
    bool ever = false;
    FILETIME mtime{};
    int64_t seq_done = 0;
    std::string version, err;
    json keeper = json::object(), player = json::object(), panel_log = json::array(), selfheal = json::object();
    ULONGLONG reads = 0;
};
static Link g_link;
static std::deque<json> g_queue;        // requests waiting for req.json to be free
static int64_t g_seq = 0;
static uint64_t g_written = 0;          // req.json files written this session
static std::string g_status;            // last action result (footer)
static bool g_status_ok = true;
static bool g_skin_classic = false, g_skin_reload = false;   // painted skin: Settings > Display > Classic look
static ULONGLONG g_status_at = 0;
static std::atomic<bool> g_worker_on{false};
static bool g_force_state = false;     // re-read state.json even if unchanged (after a spec reload)

// ---- async Lua calls ({"op":"call","fn":"DKE.x","args":[...]}) - used by the EDITOR page.
// The callback runs with g_mtx held (worker thread when the result arrives / times out, or right away when offline).
using CallCb = std::function<void(bool ok, const json& res)>;
struct PendingCall { std::string fn; ULONGLONG sent_at = 0; CallCb cb; };
static std::map<int64_t, PendingCall> g_calls;
static const ULONGLONG kCallTimeoutMs = 8000;

// ------------------------------------------------------------------ helpers
static double jnum(const json& j, const char* k, double d) {
    auto it = j.find(k); return (it != j.end() && it->is_number()) ? it->get<double>() : d;
}
static std::string jstr(const json& j, const char* k, const std::string& d = std::string()) {
    auto it = j.find(k); return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
}
static bool jbool(const json& j, const char* k, bool d) {
    auto it = j.find(k); if (it == j.end()) return d;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    return d;
}
static std::string as_text(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "-";
    if (v.is_number_float()) { char b[64]; double d = v.get<double>(); if (d == std::floor(d) && std::fabs(d) < 1e15) sprintf_s(b, "%.0f", d); else sprintf_s(b, "%g", d); return b; }
    return v.dump(-1, ' ', false, json::error_handler_t::replace);
}
static bool file_mtime(const std::string& p, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA d; if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) return false;
    ft = d.ftLastWriteTime; return true;
}
static double age_seconds(const FILETIME& ft) {
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b; a.LowPart = ft.dwLowDateTime; a.HighPart = ft.dwHighDateTime; b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return (double)(int64_t)(b.QuadPart - a.QuadPart) / 1e7;
}
static bool lua_online() { return g_link.ever && age_seconds(g_link.mtime) < 5.0; }
static std::string parent_dir(const std::string& p) { size_t k = p.find_last_of("\\/"); return k == std::string::npos ? p : p.substr(0, k); }
static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static void resolve_paths() {
    json cfg = json::object();
    { std::ifstream f(g_cfg.root + "\\config.json"); if (f) { try { cfg = json::parse(f); } catch (...) {} } }
    // everything ships next to native.dll: panel.json + data\ (cheat state, crash journal). No UE4SS, no game-folder files.
    g_proj = g_cfg.root;
    g_spec_path = g_cfg.root + "\\panel.json";
    g_ipc_dir = g_cfg.root + "\\data";
}

// the cheats run in this DLL (dk.cpp on ue.cpp) - PUBLIC always; DEV when bin\config.json says "engine": "native"
// (otherwise the DEV panel keeps using the UE4SS Lua mod through the ipc files)
static void start_engine() {
    dk::init(g_cfg.root + "\\data", g_spec_path);
}

static Item* find_item(const std::string& id) {
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) if (it.id == id) return &it;
    return nullptr;
}

static void set_status(const std::string& s, bool okv) { g_status = s; g_status_ok = okv; g_status_at = GetTickCount64(); }

// ------------------------------------------------------------------ requests
static int64_t next_seq() {
    if (!g_seq) g_seq = (int64_t)(time(nullptr) % 1000000000);   // fresh range per session (Lua keeps the last 20 results)
    if (g_link.seq_done >= g_seq) g_seq = g_link.seq_done;
    return ++g_seq;
}
// set requests for the same id replace the queued one (only the newest value matters)
static int64_t enqueue(json r, const std::string& coalesce_id = std::string()) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    const int64_t s = next_seq();
    r["seq"] = s;
    if (!coalesce_id.empty())
        for (auto q = g_queue.begin(); q != g_queue.end();)
            if (jstr(*q, "op") == "set" && jstr(*q, "id") == coalesce_id) q = g_queue.erase(q); else ++q;
    g_queue.push_back(std::move(r));
    while (g_queue.size() > 64) g_queue.pop_front();
    return s;
}
static void send_set(Item& it, const json& v) {
    it.last_seq = enqueue({ {"op", "set"}, {"id", it.id}, {"v", v} }, it.id);
    it.waiting = true; it.sent_at = GetTickCount64();
}
static bool send_click(Item& it, bool confirmed) {
    if (!lua_online()) {   // never queue one-shot actions for later (5 clicks on +1M gold would all fire when Lua returns)
        it.result = "not sent - " LINK_OFF; it.result_ok = false; it.result_at = GetTickCount64(); it.waiting = false;
        return false;
    }
    it.last_seq = enqueue({ {"op", "click"}, {"id", it.id}, {"confirmed", confirmed} });
    it.waiting = true; it.sent_at = GetTickCount64();
    return true;
}

// worker thread: write req.json when the Lua side consumed the previous one
static void write_requests() {
    std::vector<json> batch;
    { std::lock_guard<std::recursive_mutex> lk(g_mtx); if (g_queue.empty()) return; batch.assign(g_queue.begin(), g_queue.end()); }
    if (dk::native_mode()) {                               // in-process engine: no files
        dk::post(json(batch));
        std::lock_guard<std::recursive_mutex> lk(g_mtx);
        g_queue.clear(); ++g_written;
        return;
    }
}

// worker thread: read state.json when it changed
static void apply_state(const json& j, const FILETIME& ft);
static void read_state() {
    if (dk::native_mode()) {                               // in-process engine: state document straight from memory
        static uint64_t last_v = 0;
        if (!ue::ready()) return;                           // engine core not up yet = offline
        FILETIME now; GetSystemTimeAsFileTime(&now);
        const uint64_t v = dk::state_version();
        { std::lock_guard<std::recursive_mutex> lk(g_mtx);
          const bool force = g_force_state; g_force_state = false;
          if (v == last_v && !force && g_link.ever) { g_link.mtime = now; return; } }
        last_v = v;
        apply_state(dk::state(), now);
        return;
    }
}
static void apply_state(const json& j, const FILETIME& ft) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (j.is_discarded() || !j.is_object()) { g_link.err = "state unreadable"; return; }
    g_link.ever = true; g_link.mtime = ft; g_link.err.clear(); ++g_link.reads;
    g_link.seq_done = (int64_t)jnum(j, "seq_done", (double)g_link.seq_done);
    g_link.version = jstr(j, "version", g_link.version);
    auto obj = [&](const char* k) { auto it = j.find(k); return (it != j.end() && it->is_object()) ? *it : json::object(); };
    g_link.keeper = obj("keeper"); g_link.player = obj("player"); g_link.selfheal = obj("selfheal");
    { auto it = j.find("panel_log"); g_link.panel_log = (it != j.end() && it->is_array()) ? *it : json::array(); }

    std::map<int64_t, std::pair<bool, std::string>> results;
    const json res_obj = obj("results");   // keep it alive: items() of a temporary dangles
    for (auto& kv : res_obj.items()) {
        const int64_t k = (int64_t)strtod(kv.key().c_str(), nullptr);
        const json& r = kv.value();
        if (r.is_object()) results[k] = { jbool(r, "ok", false), r.contains("res") ? as_text(r["res"]) : std::string() };
    }
    const json values = obj("values"), readouts = obj("readouts");
    const ULONGLONG now = GetTickCount64();
    for (auto& t : g_tabs) for (auto& sec : t.sections) for (auto& it : sec.items) {
        if (it.waiting) {
            auto r = results.find(it.last_seq);
            if (r != results.end()) {
                it.waiting = false; it.result_ok = r->second.first; it.result = r->second.second; it.result_at = now;
                set_status(it.label + ": " + (it.result.empty() ? (it.result_ok ? "ok" : "failed") : it.result), it.result_ok);
            } else if (g_link.seq_done >= it.last_seq && now - it.sent_at > 3000) {
                it.waiting = false; it.result_ok = true; it.result = "done"; it.result_at = now;   // result already rotated out
            }
        }
        auto v = values.find(it.id);
        if (v != values.end() && !it.dirty && !it.waiting && g_drag_id != it.id) {
            if (it.type == "toggle") { it.on = v->is_boolean() ? v->get<bool>() : (v->is_number() && v->get<double>() != 0); it.have_value = true; }
            else if (it.type == "slider" && v->is_number()) { it.val = v->get<double>(); it.have_value = true; }
        }
        auto ro = readouts.find(it.id);
        if (ro != readouts.end()) it.readout = as_text(*ro);
    }
    // editor calls: hand the raw result JSON to the callback
    std::vector<std::pair<CallCb, std::pair<bool, json>>> fire;
    for (auto c = g_calls.begin(); c != g_calls.end();) {
        auto r = res_obj.find(std::to_string(c->first));
        if (r != res_obj.end() && r->is_object()) {
            fire.push_back({ std::move(c->second.cb), { jbool(*r, "ok", false), r->contains("res") ? (*r)["res"] : json() } });
            c = g_calls.erase(c);
        } else if (g_link.seq_done >= c->first && now - c->second.sent_at > 3000) {   // done but rotated out of results
            fire.push_back({ std::move(c->second.cb), { false, json("result lost (more than 20 requests since) - refresh") } });
            c = g_calls.erase(c);
        } else ++c;
    }
    for (auto& f : fire) if (f.first) f.first(f.second.first, f.second.second);
}

static int64_t lua_call(const std::string& fn, json args, CallCb cb) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (!lua_online()) { if (cb) cb(false, json(LINK_OFF " - not sent")); return 0; }
    const int64_t s = enqueue({ {"op", "call"}, {"fn", fn}, {"args", std::move(args)} });
    g_calls[s] = PendingCall{ fn, GetTickCount64(), std::move(cb) };
    return s;
}
static bool call_busy(int64_t s) { return s != 0 && g_calls.count(s) != 0; }

static void worker() {
    ULONGLONG last_state = 0, last_spec = 0;
    log("[panel] panel %s, data %s", g_spec_path.c_str(), g_ipc_dir.c_str());
    while (!g_retired && !g_features_off) {
        try {
            write_requests();
            const ULONGLONG now = GetTickCount64();
            if (now - last_state >= 250) { last_state = now; read_state(); }
            {   // a request nobody answers (Lua gone) stops showing "sending..."
                std::lock_guard<std::recursive_mutex> lk(g_mtx);
                for (auto& t : g_tabs) for (auto& sec : t.sections) for (auto& it : sec.items)
                    if (it.waiting && now - it.sent_at > 8000) {
                        it.waiting = false; it.result_ok = false; it.result_at = now;
                        it.result = lua_online() ? "no answer yet (still queued?)" : "queued - " LINK_OFF;
                    }
                std::vector<CallCb> late;
                for (auto c = g_calls.begin(); c != g_calls.end();) {
                    if (now - c->second.sent_at > kCallTimeoutMs) {
                        const int64_t sq = c->first;   // still unsent? drop it so it never fires late
                        for (auto q = g_queue.begin(); q != g_queue.end();) if ((int64_t)jnum(*q, "seq", 0) == sq) q = g_queue.erase(q); else ++q;
                        late.push_back(std::move(c->second.cb)); c = g_calls.erase(c);
                    } else ++c;
                }
                for (auto& cb : late) if (cb) cb(false, json(lua_online() ? "timed out after 8 s (no answer)" : "timed out after 8 s - " LINK_OFF));
            }
        } catch (std::exception& e) { log("[panel] worker: %s", e.what()); Sleep(500); }
        Sleep(50);
    }
    g_worker_on = false;
}

// ------------------------------------------------------------------ spec load
json panel_reload() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (g_spec_path.empty()) resolve_paths();
    file_mtime(g_spec_path, g_spec_mtime);        // also on failure: retry only when the file changes again
    std::ifstream f(g_spec_path, std::ios::binary);
    if (!f) { g_load_msg = "panel spec not found: " + g_spec_path; log("[panel] %s", g_load_msg.c_str()); return fail("NO_SPEC", g_load_msg); }
    json j;
    try { j = json::parse(f); }
    catch (std::exception& e) { g_load_msg = std::string("panel.json error (kept the previous one): ") + e.what(); log("[panel] %s", g_load_msg.c_str()); return fail("BAD_PANEL", g_load_msg); }
    if (!j.is_object()) { g_load_msg = "panel.json is not an object"; return fail("BAD_PANEL", g_load_msg); }

    std::map<std::string, Item> old;
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) old[it.id] = it;

    std::vector<Tab> tabs; int count = 0;
    g_title = jstr(j, "title", "DRAGONKIN TRAINER");
    auto jt_arr = j.find("tabs");
    if (jt_arr != j.end() && jt_arr->is_array()) for (auto& jt : *jt_arr) {
        if (!jt.is_object()) continue;
        Tab t; t.name = jstr(jt, "name", "Tab");
        auto js_arr = jt.find("sections");
        if (js_arr != jt.end() && js_arr->is_array()) for (auto& js : *js_arr) {
            if (!js.is_object()) continue;
            Section s; s.name = jstr(js, "name");
            auto ji_arr = js.find("items");
            if (ji_arr != js.end() && ji_arr->is_array()) for (auto& ji : *ji_arr) {
                if (!ji.is_object()) continue;
                if (WB_PUBLIC && jbool(ji, "dev", false)) continue;   // developer-only item
                Item it;
                it.id = jstr(ji, "id", t.name + "_" + std::to_string(count));
                it.type = jstr(ji, "type", "label");
                it.text = jstr(ji, "text");
                it.label = jstr(ji, "label", it.text.empty() ? it.id : it.text);
                it.desc = jstr(ji, "desc");
                it.button = jstr(ji, "button", "RUN");
                it.danger = jbool(ji, "danger", false);
                it.minv = jnum(ji, "min", 0); it.maxv = jnum(ji, "max", it.minv + 100); it.step = jnum(ji, "step", 0);
                if (it.maxv <= it.minv) it.maxv = it.minv + 1;
                it.defv = jnum(ji, "default", it.minv);
                it.has_off = ji.contains("off") && ji["off"].is_number();
                it.offv = jnum(ji, "off", it.defv);
                it.val = it.defv;
                auto o = old.find(it.id);
                if (o != old.end()) {
                    const Item& p = o->second;
                    it.on = p.on; it.val = p.val; it.have_value = p.have_value; it.dirty = p.dirty; it.last_edit = p.last_edit;
                    it.last_seq = p.last_seq; it.sent_at = p.sent_at; it.waiting = p.waiting;
                    it.result = p.result; it.result_ok = p.result_ok; it.result_at = p.result_at; it.readout = p.readout;
                    memcpy(it.input, p.input, sizeof it.input);
                }
                s.items.push_back(std::move(it)); ++count;
            }
            t.sections.push_back(std::move(s));
        }
        tabs.push_back(std::move(t));
    }
    g_tabs = std::move(tabs);
    if (g_tab > (int)g_tabs.size() + 2) g_tab = 0;   // spec tabs, EDITOR, BRIDGE (DEV), SETTINGS
    g_force_state = true;        // re-read state.json now so new items get their values
    g_load_msg = "Loaded " + std::to_string(count) + " items in " + std::to_string(g_tabs.size()) + " tabs";
    log("[panel] %s from %s", g_load_msg.c_str(), g_spec_path.c_str());
    return ok({ {"items", count}, {"tabs", g_tabs.size()}, {"spec", g_spec_path} }, g_load_msg);
}


void panel_init() {
    resolve_paths();
    start_engine();
    panel_reload();
    if (!g_worker_on.exchange(true)) std::thread(worker).detach();
}

// ------------------------------------------------------------------ widgets
static bool toggle_switch(const char* id, bool* v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight() * 0.78f, w = h * 1.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID sid = ImGui::GetID(id);
    float t = st->GetFloat(sid, *v ? 1.f : 0.f);
    t += (*v ? 1.f : -1.f) * ImGui::GetIO().DeltaTime * 9.f; t = ImClamp(t, 0.f, 1.f);
    st->SetFloat(sid, t);
    ImU32 off = ui::rgb(0x2A2826), on = ui::rgb(ui::kGold);
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(off), b = ImGui::ColorConvertU32ToFloat4(on);
    ImU32 col = ImGui::GetColorU32(ImLerp(a, b, t));
    if (ImGui::IsItemHovered()) col = ImGui::GetColorU32(ImLerp(ImLerp(a, b, t), ImVec4(1, 1, 1, 1), 0.08f));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .35f + .4f * t), h * 0.5f);
    float r = h * 0.5f - 2.5f;
    dl->AddCircleFilled(ImVec2(p.x + r + 2.5f + t * (w - 2 * r - 5), p.y + h * 0.5f), r, ui::rgb(t > .5f ? ui::kInk : ui::kBone));
    return clicked;
}

static void section_header(const char* name) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushFont(g_font_bold);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kGoldHi));
    std::string up = name; std::transform(up.begin(), up.end(), up.begin(), ::toupper);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 3), ImVec2(p.x + std::min(S(120.f), ImGui::GetContentRegionAvail().x), p.y - 3), ui::rgb(ui::kGold), S(1.5f));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + std::min(S(120.f), ImGui::GetContentRegionAvail().x), p.y - 3), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 3), ui::rgb(ui::kGold, .22f));
    ImGui::Dummy(ImVec2(0, 2));
}

static bool matches(const Item& it) {
    if (!g_search[0]) return true;
    std::string a = it.label + " " + it.desc + " " + it.id, b = g_search;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower); std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

static std::string slider_fmt(double step) {
    int d = 2;
    if (step > 0) { d = 0; double s = step; while (d < 4 && std::fabs(s - std::round(s)) > 1e-6) { s *= 10; ++d; } }
    char f[16]; sprintf_s(f, "%%.%df", d); return f;
}

static void text_dim(const std::string& s, uint32_t col = ui::kDim) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(col));
    ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x - S(10));
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
}

static void draw_item(Item& it) {
    const ULONGLONG now = GetTickCount64();
    const bool online = lua_online();
    ImGui::PushID(it.id.c_str());

    if (it.type == "label" || it.type == "note" || it.type == "text" || it.type == "info") {   // plain text rows
        ImGui::Dummy(ImVec2(0, S(2)));
        text_dim(it.text.empty() ? it.label : it.text, ui::kBone);
        if (!it.desc.empty()) text_dim(it.desc);
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::PopID();
        return;
    }

    ImDrawList* row_dl = ImGui::GetWindowDrawList(); row_dl->ChannelsSplit(2); row_dl->ChannelsSetCurrent(1);
    const ImVec2 row_p0 = ImGui::GetCursorScreenPos(); ImGui::Dummy(ImVec2(0, S(1)));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float right = ImGui::GetWindowContentRegionMax().x;
    const bool known = it.type == "toggle" || it.type == "slider" || it.type == "button" || it.type == "readout" || it.type == "input";
    float ctrl_w = it.type == "toggle" ? ImGui::GetFrameHeight() * 0.78f * 1.85f
                 : it.type == "button" ? S(130.f)
                 : it.type == "slider" || it.type == "input" ? std::min(S(320.f), avail * 0.5f) : 0.f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(it.label.c_str());
    if (!it.desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.desc.c_str());
    bool active_mark = false;

    if (it.type == "toggle") {
        ImGui::SameLine(right - ctrl_w);
        bool v = it.on;
        if (toggle_switch("##sw", &v)) { it.on = v; send_set(it, v); }
        active_mark = it.on;
    } else if (it.type == "slider") {
        ImGui::SameLine(right - ctrl_w);
        ImGui::SetNextItemWidth(ctrl_w);
        double v = it.val, mn = it.minv, mx = it.maxv;
        const std::string fmt = slider_fmt(it.step);
        if (ImGui::SliderScalar("##v", ImGuiDataType_Double, &v, &mn, &mx, fmt.c_str())) {
            if (it.step > 0) v = mn + std::round((v - mn) / it.step) * it.step;
            it.val = std::clamp(v, mn, mx); it.dirty = true; it.last_edit = now;
        }
        const bool act = ImGui::IsItemActive();
        if (act) g_drag_id = it.id;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { it.val = it.has_off ? it.offv : it.defv; it.dirty = true; it.last_edit = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%sRight-click: back to %g%s", it.desc.c_str(), it.desc.empty() ? "" : "\n", it.has_off ? it.offv : it.defv, it.has_off ? " (off)" : "");
        if (it.dirty && (!act || now - it.last_edit >= 250)) { send_set(it, it.val); it.dirty = false; }
        active_mark = it.has_off && std::fabs(it.val - it.offv) > 1e-9;
    } else if (it.type == "button") {
        ImGui::SameLine(right - ctrl_w);
        const bool arming = it.danger && now < it.confirm_until;
        const std::string cap = (arming ? std::string("CONFIRM?") : it.button) + "##b";
        const bool pressed = it.danger ? ui::danger(cap.c_str(), ctrl_w, ImGui::GetFrameHeight())
                                       : ui::primary(cap.c_str(), ctrl_w, ImGui::GetFrameHeight());
        if (it.danger && ImGui::IsItemHovered()) ImGui::SetTooltip(arming ? "Click again to confirm" : "Needs a second click within 4 s");
        if (pressed) {
            if (!it.danger) send_click(it, false);
            else if (arming) { it.confirm_until = 0; send_click(it, true); }
            else it.confirm_until = now + 4000;
        }
    } else if (it.type == "readout") {
        const std::string v = it.readout.empty() ? (online ? "-" : "--") : it.readout;
        const float w = ImGui::CalcTextSize(v.c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + S(12), right - w - S(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(online ? ui::kGoldHi : ui::kDim));
        ImGui::TextUnformatted(v.c_str());
        ImGui::PopStyleColor();
    } else if (it.type == "input") {
        ImGui::SameLine(right - ctrl_w);
        ImGui::SetNextItemWidth(ctrl_w - S(62));
        const bool enter = ImGui::InputText("##t", it.input, sizeof it.input, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ui::primary("SET##go", S(54), ImGui::GetFrameHeight()) || enter) send_set(it, std::string(it.input));
    } else if (!known) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", it.type.c_str());
    }

    if (!it.desc.empty()) text_dim(it.desc);
    if (it.waiting) text_dim(online ? "sending..." : "queued - " LINK_OFF, ui::kAmber);
    else if (it.result_at && now - it.result_at < 10000) text_dim(std::string(it.result_ok ? "> " : "! ") + it.result, it.result_ok ? ui::kGreen : ui::kRed);

    ImGui::Dummy(ImVec2(0, S(1)));
    {   const ImVec2 row_p1(ImGui::GetWindowPos().x + right + S(5), ImGui::GetCursorScreenPos().y - S(3));
        row_dl->ChannelsSetCurrent(0);
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1);
        row_dl->AddRectFilledMultiColor(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1, ui::rgb(hov ? 0x22231F : 0x16171A, .92f), ui::rgb(hov ? 0x1A1B1E : 0x101114, .80f), ui::rgb(hov ? 0x1A1B1E : 0x101114, .80f), ui::rgb(hov ? 0x22231F : 0x16171A, .92f));
        row_dl->AddRect(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1, ui::rgb(hov ? ui::kGold : ui::kLine, hov ? .45f : .7f));
        if (active_mark) row_dl->AddRectFilled(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), ImVec2(row_p0.x - S(4), row_p1.y), ui::rgb(ui::kGold));
        row_dl->ChannelsMerge(); }
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::PopID();
}

static void kv_row(const char* k, const std::string& v) {
    ImGui::TextUnformatted(k); ImGui::SameLine(S(190)); ImGui::TextDisabled("%s", v.c_str());
}


// ------------------------------------------------------------------ SETTINGS page (2026-11-09)
// Hotkey capture: "Change..." records which keys are down, then each frame looks for a key that goes down (keyboard
// only: GetAsyncKeyState 0x08..0xFE minus mouse buttons / bare modifiers / Windows keys). Esc cancels, 10 s timeout.
// Ctrl / Shift / Alt held at that moment become part of the hotkey. While waiting, the overlay ignores the panel
// toggle (overlay_hotkey_capture_ping), so pressing the current hotkey here does not close the panel.
static bool g_capturing = false;
static ULONGLONG g_cap_until = 0;
static bool g_cap_prev[256] = {};
static std::string g_set_msg;
static bool g_set_ok = true;
static ULONGLONG g_set_at = 0;
static float g_scale_edit = -1.f;      // slider value while dragging (applied on release, so the slider does not move under the mouse)

static void settings_note(const std::string& m, bool okv) { g_set_msg = m; g_set_ok = okv; g_set_at = GetTickCount64(); }
static void settings_commit(const std::string& what) {
    std::string err;
    if (settings_save(&err)) settings_note(what, true);
    else settings_note("Could not save settings.json: " + err, false);
}
static bool game_focused() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
static bool key_held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void capture_begin() {
    g_capturing = true; g_cap_until = GetTickCount64() + 10000;
    for (int vk = 0; vk < 256; ++vk) g_cap_prev[vk] = key_held(vk);
    overlay_hotkey_capture_ping();
}
static void capture_cancel(const char* why) { if (!g_capturing) return; g_capturing = false; settings_note(why, false); }

static void capture_poll() {
    if (!g_capturing) return;
    overlay_hotkey_capture_ping();
    if (GetTickCount64() > g_cap_until) { capture_cancel("No key pressed in 10 s - hotkey unchanged"); return; }
    const bool focus = game_focused();
    int got = 0;
    for (int vk = 1; vk < 256; ++vk) {
        const bool d = key_held(vk);
        const bool rose = d && !g_cap_prev[vk];
        g_cap_prev[vk] = d;
        if (!rose || !focus || got) continue;
        if (vk == VK_ESCAPE) { got = -1; continue; }
        if (hotkey_vk_allowed(vk)) got = vk;
    }
    if (got < 0) { capture_cancel("Cancelled - hotkey unchanged"); return; }
    if (!got) return;
    Hotkey h; h.vk = got; h.ctrl = key_held(VK_CONTROL); h.shift = key_held(VK_SHIFT); h.alt = key_held(VK_MENU);
    g_capturing = false;
    g_settings.hotkey = h;
    settings_commit("Panel hotkey is now " + hotkey_name(h));
}

static void draw_settings_tab() {
    const float bh = ImGui::GetFrameHeight() + S(4);
    section_header("Panel hotkey");
    kv_row("Keyboard", hotkey_name(g_settings.hotkey));
    if (g_capturing) {
        const int left = (int)((g_cap_until - std::min(g_cap_until, GetTickCount64())) / 1000) + 1;
        text_dim("Press the new key now (optionally with Ctrl / Shift / Alt). Mouse buttons are ignored, Esc cancels. "
                 + std::to_string(left) + " s", ui::kAmber);
        if (ui::danger("Cancel##capx", S(160), bh)) capture_cancel("Cancelled - hotkey unchanged");
    } else {
        if (ui::primary("Change...##hk", S(160), bh)) capture_begin();
        ImGui::SameLine();
        if (ui::primary("Reset to INSERT##hkr", S(180), bh)) { g_settings.hotkey = Hotkey(); settings_commit("Panel hotkey reset to INSERT"); }
    }
    kv_row("Controller", "LB + D-pad Down opens / closes the panel, Start closes it");

    section_header("Display");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Panel scale");
    ImGui::SameLine(S(190));
    ImGui::SetNextItemWidth(std::min(S(320.f), ImGui::GetContentRegionAvail().x));
    float v = g_scale_edit > 0 ? g_scale_edit : g_settings.ui_scale;
    if (ImGui::SliderFloat("##uiscale", &v, 0.75f, 1.5f, "%.2fx")) g_scale_edit = std::round(v * 20.f) / 20.f;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_scale_edit = -1.f; g_settings.ui_scale = 1.f; settings_commit("Panel scale reset to 1.00x"); }
    else if (ImGui::IsItemDeactivatedAfterEdit() && g_scale_edit > 0) {
        g_settings.ui_scale = std::clamp(g_scale_edit, 0.75f, 1.5f); g_scale_edit = -1.f;
        char b[64]; sprintf_s(b, "Panel scale %.2fx", g_settings.ui_scale); settings_commit(b);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Applies when you let go. Right-click: back to 1.00x");
    {   ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Classic look");
        ImGui::SameLine(S(190));
        bool cl = g_skin_classic;
        if (toggle_switch("##classic", &cl)) { g_skin_classic = cl; g_skin_reload = true; settings_commit(cl ? "Classic look on" : "Painted Dragonkin look on"); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the painted Dragonkin panel (skin folder). On: the plain panel. Switching also reloads skin.json."); }


    section_header("About");
    kv_row("Version", kVersion);
    kv_row("Settings file", g_cfg.root + "\\settings.json");
    if (!g_set_msg.empty() && GetTickCount64() - g_set_at < 8000) { ImGui::Dummy(ImVec2(0, S(4))); text_dim(g_set_msg, g_set_ok ? ui::kGreen : ui::kRed); }
}

// panel scale: font scale + the S() layout factor for this panel only (no font atlas rebuild, works in a reloaded copy)
struct PanelScale {
    float base, fgs;
    explicit PanelScale(float k) {
        ImGuiIO& io = ImGui::GetIO();
        base = g_ui_scale; fgs = io.FontGlobalScale;
        g_ui_scale = base * k; io.FontGlobalScale = fgs * k;
    }
    ~PanelScale() {
        ImGui::GetIO().FontGlobalScale = fgs; g_ui_scale = base;
        ImGui::PushFont(ImGui::GetFont()); ImGui::PopFont();   // recompute the current font size for whatever draws next (toasts)
    }
};

// ------------------------------------------------------------------ EDITOR page (fragments + gear)
// Everything goes through lua_call -> DKE.* in Mods\DragonkinBridge\Scripts\editor.lua (the game's own reroll library).
// Lua arrays arrive either as JSON arrays or as objects with keys "1","2",... (WF.serialize) - ed::jlist handles both.
// Never send JSON null in args: the Lua json decoder turns it into nil, which breaks #args / table.unpack.
namespace ed {

static const char* kRar[] = { "Common", "Uncommon", "Rare", "Legendary", "Mythical" };
static uint32_t rar_col(int r) {
    switch (r) { case 0: return 0xA8A294; case 1: return ui::kGreen; case 2: return ui::kBlue; case 3: return ui::kAmber; case 4: return 0xC77DDB; }
    return ui::kDim;
}
static const char* rar_name(int r) { return (r >= 0 && r <= 4) ? kRar[r] : "?"; }
static const char* type_name(int t) { return t == 0 ? "Modifier" : t == 1 ? "Player skill" : t == 2 ? "Dragon skill" : "?"; }

static std::vector<json> jlist(const json& j) {
    std::vector<json> out;
    if (j.is_array()) { for (auto& x : j) out.push_back(x); return out; }
    if (j.is_object()) {
        std::vector<std::pair<long long, json>> v;
        for (auto& kv : j.items()) {
            char* e = nullptr; const long long k = strtoll(kv.key().c_str(), &e, 10);
            if (kv.key().empty() || !e || *e) return {};          // a record, not a list
            v.emplace_back(k, kv.value());
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& x : v) out.push_back(std::move(x.second));
    }
    return out;
}
static std::vector<json> jlist_at(const json& j, const char* k) { auto it = j.find(k); return it == j.end() ? std::vector<json>() : jlist(*it); }
static int jrar(const json& j, const char* k) {
    auto it = j.find(k); if (it == j.end()) return -1;
    if (it->is_number()) return (int)it->get<double>();
    if (it->is_string()) {
        std::string s = it->get<std::string>();
        if (s.find("Uncommon") != std::string::npos) return 1;
        for (int i = 4; i >= 0; --i) if (s.find(kRar[i]) != std::string::npos) return i;
    }
    return -1;
}
static std::string fnum(double d) {
    char b[64];
    if (std::fabs(d - std::round(d)) < 1e-6 && std::fabs(d) < 1e15) sprintf_s(b, "%.0f", d);
    else { sprintf_s(b, "%.3f", d); size_t n = strlen(b); while (n && b[n - 1] == '0') b[--n] = 0; }
    return b;
}
static std::string pretty_tag(std::string t) {
    if (t.empty() || t == "None") return "(empty)";
    for (const char* p : { "GameplayCue.PlayerSkill.", "AbilityModifiers." })
        if (t.rfind(p, 0) == 0) { t = t.substr(strlen(p)); break; }
    if (t.size() > 6 && t.compare(t.size() - 6, 6, ".Skill") == 0) t.resize(t.size() - 6);
    return t;
}
static std::string gear_name(const std::string& tag) {   // last 2-3 tag parts
    std::vector<std::string> parts; size_t a = 0;
    while (a <= tag.size()) { size_t b = tag.find('.', a); if (b == std::string::npos) b = tag.size(); if (b > a) parts.push_back(tag.substr(a, b - a)); a = b + 1; }
    if (parts.empty()) return tag;
    if (parts.size() >= 6 && parts[0] == "Items" && parts[1] == "Equipment") {   // Items.Equipment.Harrower.01.01.RightHand
        std::string slot;
        for (char c : parts.back()) { if (isupper((unsigned char)c) && !slot.empty()) slot += ' '; slot += c; }
        return parts[2] + " " + slot + " (" + parts[3] + "." + parts[4] + ")";
    }
    const size_t take = parts.size() >= 4 ? 3 : std::min<size_t>(2, parts.size());
    std::string out;
    for (size_t i = parts.size() - take; i < parts.size(); ++i) out += (out.empty() ? "" : " ") + parts[i];
    return out;
}
static std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

struct Slot { std::string tag, name, desc; int rarity = 0, type = 0; bool divine = false; };
// game name when the Lua side found one (DKE.tag_name), else the shortened tag
static std::string slot_label(const std::string& tag, const std::string& name) { return name.empty() ? pretty_tag(tag) : name; }
// "HealthMaxMultiplier" -> "Health Max Multiplier" (gear attributes: the game's own names need an FGameplayAttribute
// call, which crashed from Lua on 2026-11-09 04:27 - so this stays text-only)
static std::string pretty_attr(const std::string& a) {
    std::string out;
    for (size_t i = 0; i < a.size(); ++i) {
        const char c = a[i];
        if (i && isupper((unsigned char)c) && (islower((unsigned char)a[i - 1]) || (i + 1 < a.size() && islower((unsigned char)a[i + 1]))))
            out += ' ';
        out += c;
    }
    return out;
}
struct Frag { int64_t handle = 0; int rarity = 0; bool onGrid = false, floating = false; int size = 0; std::vector<Slot> slots; };
struct Affix { std::string attr; double value = 0, eff = 0; bool divine = false, primary = false, corrupted = false; };
struct Gear { int64_t handle = 0; bool equipped = false; std::string tag, name; int level = 0, rarity = -1, rerolls = -1; std::vector<Affix> affixes;
              bool rav = false; std::string rav_name, rav_text; int rav_left = -1, rav_max = 0; };

struct SlotUI {          // key "handle:slot"
    std::vector<std::string> opts, opt_names, opt_desc; bool loaded = false; int64_t opts_seq = 0;
    int sel = -1;        // index into opts, -1 = keep the current tag
    int rarity = -1;     // -1 = not initialised from the slot yet
    char filter[64] = {};
    int64_t apply_seq = 0;
};
struct AttrOpt { std::string attr; double mn = 0, mx = 0; bool has_range = false; };
struct AffixUI {         // key "handle:affix"
    float quality = 5.f; std::vector<Affix> opts; bool rolled = false; int64_t opts_seq = 0;
    int sel = -1; double value = 0; bool divine = false; int64_t apply_seq = 0;
    std::vector<AttrOpt> attrs; bool attrs_loaded = false; int64_t attrs_seq = 0; int attr_sel = -1; int64_t pick_seq = 0;
};
struct Msg { std::string text; bool ok = true; };

static int g_sub = 0;                       // 0 FRAGMENTS, 1 GEAR
static std::vector<Frag> g_frags; static bool g_frags_loaded = false; static int64_t g_frags_seq = 0;
static char g_ffilter[96] = {}; static int64_t g_fsel = 0; static std::map<int64_t, int64_t> g_fmax_seq;
static std::vector<Gear> g_gear; static bool g_gear_loaded = false; static int64_t g_gear_seq = 0;
static char g_gfilter[96] = {}; static int64_t g_gsel = 0;
static std::map<std::string, SlotUI> g_slot_ui;
static std::map<std::string, AffixUI> g_aff_ui;
static std::map<std::string, Msg> g_msgs;

static std::string key(int64_t h, int i) { return std::to_string(h) + ":" + std::to_string(i); }
static void set_msg(const std::string& k, const std::string& t, bool okv) { g_msgs[k] = Msg{ t, okv }; }
static void show_msg(const std::string& k) {
    auto m = g_msgs.find(k); if (m == g_msgs.end() || m->second.text.empty()) return;
    text_dim((m->second.ok ? "> " : "! ") + m->second.text, m->second.ok ? ui::kGreen : ui::kRed);
}
static void loading_line(const char* what) { text_dim(std::string(what) + " loading...", ui::kAmber); }

static Frag parse_frag(const json& j) {
    Frag f; f.handle = (int64_t)jnum(j, "handle", 0); f.rarity = jrar(j, "rarity");
    f.onGrid = jbool(j, "onGrid", false); f.floating = jbool(j, "floating", false);
    for (auto& s : jlist_at(j, "slots")) if (s.is_object()) {
        Slot x; x.tag = jstr(s, "tag"); x.name = jstr(s, "name"); x.desc = jstr(s, "desc"); x.rarity = jrar(s, "rarity"); x.type = (int)jnum(s, "type", -1); x.divine = jbool(s, "divine", false);
        f.slots.push_back(x);
    }
    f.size = (int)jnum(j, "size", (double)f.slots.size());
    return f;
}
static Affix parse_affix(const json& j) {
    Affix a; a.attr = jstr(j, "attr", "?"); a.value = jnum(j, "value", 0); a.eff = jnum(j, "eff", 0);
    a.divine = jbool(j, "divine", false); a.primary = jbool(j, "primary", false); a.corrupted = jbool(j, "corrupted", false);
    return a;
}
static Gear parse_gear(const json& j) {
    Gear g; g.handle = (int64_t)jnum(j, "handle", 0); g.equipped = jbool(j, "equipped", false); g.tag = jstr(j, "tag"); g.name = jstr(j, "name");
    g.level = (int)jnum(j, "level", 0); g.rarity = jrar(j, "rarity"); g.rerolls = (int)jnum(j, "rerollsLeft", -1);
    for (auto& a : jlist_at(j, "affixes")) if (a.is_object()) g.affixes.push_back(parse_affix(a));
    auto rv = j.find("ravenous");
    if (rv != j.end() && rv->is_object()) { g.rav = true; g.rav_name = jstr(*rv, "name"); g.rav_text = jstr(*rv, "text");
        g.rav_left = (int)jnum(*rv, "left", -1); g.rav_max = (int)jnum(*rv, "max", 0); }
    return g;
}
static Frag* find_frag(int64_t h) { for (auto& f : g_frags) if (f.handle == h) return &f; return nullptr; }
static Gear* find_gear(int64_t h) { for (auto& g : g_gear) if (g.handle == h) return &g; return nullptr; }
static std::string err_text(const json& res) { std::string s = as_text(res); return s.empty() ? "failed" : s; }

// ---- requests
static void refresh_frags() {
    g_frags_seq = lua_call("DKE.fragments", json::array(), [](bool okv, const json& res) {
        g_frags_seq = 0;
        if (!okv) { set_msg("frags", "Refresh failed: " + err_text(res), false); return; }
        std::vector<Frag> v;
        for (auto& x : jlist(res)) if (x.is_object()) v.push_back(parse_frag(x));
        g_frags = std::move(v); g_frags_loaded = true;
        set_msg("frags", std::to_string(g_frags.size()) + " fragments", true);
    });
}
static void refresh_gear() {
    g_gear_seq = lua_call("DKE.gear", json::array(), [](bool okv, const json& res) {
        g_gear_seq = 0;
        if (!okv) { set_msg("gear", "Refresh failed: " + err_text(res), false); return; }
        std::vector<Gear> v;
        for (auto& x : jlist(res)) if (x.is_object()) v.push_back(parse_gear(x));
        std::stable_sort(v.begin(), v.end(), [](const Gear& a, const Gear& b) { return a.equipped && !b.equipped; });
        g_gear = std::move(v); g_gear_loaded = true;
        set_msg("gear", std::to_string(g_gear.size()) + " items", true);
    });
}
static void fetch_frag_options(int64_t h, int slot) {
    const std::string k = key(h, slot);
    g_slot_ui[k].opts_seq = lua_call("DKE.frag_options", json::array({ h, slot }), [k, h, slot](bool okv, const json& res) {
        SlotUI& u = g_slot_ui[k]; u.opts_seq = 0; u.loaded = true;
        if (!okv) { set_msg("fs:" + k, "Options failed: " + err_text(res), false); return; }
        u.opts.clear(); u.opt_names.clear(); u.opt_desc.clear();
        for (auto& x : jlist(res)) {
            if (x.is_string()) { u.opts.push_back(x.get<std::string>()); u.opt_names.emplace_back(); u.opt_desc.emplace_back(); }
            else if (x.is_object() && !jstr(x, "tag").empty()) {
                u.opts.push_back(jstr(x, "tag")); u.opt_names.push_back(jstr(x, "name")); u.opt_desc.push_back(jstr(x, "desc"));
            }
        }
        u.sel = -1;
        if (Frag* f = find_frag(h)) if (slot >= 1 && slot <= (int)f->slots.size())
            for (int i = 0; i < (int)u.opts.size(); ++i) if (u.opts[i] == f->slots[slot - 1].tag) u.sel = i;
    });
}
static void apply_frag_slot(int64_t h, int slot, const std::string& tag, int rarity, bool divine) {
    const std::string k = key(h, slot);
    set_msg("fs:" + k, "", true);
    g_slot_ui[k].apply_seq = lua_call("DKE.frag_set", json::array({ h, slot, tag, rarity, divine }), [k, h, slot, tag](bool okv, const json& res) {
        SlotUI& u = g_slot_ui[k]; u.apply_seq = 0;
        if (!okv) { set_msg("fs:" + k, "Apply failed: " + err_text(res), false); return; }
        const bool inner = res.is_object() ? jbool(res, "ok", false) : false;
        auto fr = res.is_object() ? res.find("fragment") : res.end();
        if (res.is_object() && fr != res.end() && fr->is_object()) {
            Frag nf = parse_frag(*fr);
            if (Frag* f = find_frag(h)) *f = nf; else g_frags.push_back(nf);
        }
        u.loaded = false; u.opts.clear(); u.sel = -1; u.rarity = -1;   // re-read from the updated slot
        std::string now_label = pretty_tag(tag);
        if (Frag* f = find_frag(h)) if (slot >= 1 && slot <= (int)f->slots.size()) now_label = slot_label(f->slots[slot - 1].tag, f->slots[slot - 1].name);
        if (!inner) set_msg("fs:" + k, "Game call returned false - slot unchanged? (" + as_text(res) + ")", false);
        else set_msg("fs:" + k, "Applied: " + now_label, true);
    });
}
static void frag_max(int64_t h) {
    set_msg("fmax:" + std::to_string(h), "", true);
    g_fmax_seq[h] = lua_call("DKE.frag_max", json::array({ h }), [h](bool okv, const json& res) {
        g_fmax_seq[h] = 0;
        const std::string mk = "fmax:" + std::to_string(h);
        if (!okv) { set_msg(mk, "Failed: " + err_text(res), false); return; }
        const json* fj = nullptr;
        if (res.is_object() && res.contains("slots")) fj = &res;
        else if (res.is_object() && res.contains("fragment") && res["fragment"].is_object()) fj = &res["fragment"];
        if (fj) { Frag nf = parse_frag(*fj); if (Frag* f = find_frag(h)) *f = nf; else g_frags.push_back(nf); }
        for (auto& kv : g_slot_ui) if (kv.first.rfind(std::to_string(h) + ":", 0) == 0) kv.second.rarity = -1;
        set_msg(mk, fj ? "Every slot is now Mythical" : "Done (no fragment data returned - refresh)", true);
    });
}
static void roll_gear_options(int64_t h, int ai, float quality) {
    const std::string k = key(h, ai);
    set_msg("ga:" + k, "", true);
    const double q = std::round(quality * 10.0) / 10.0;
    g_aff_ui[k].opts_seq = lua_call("DKE.gear_options", json::array({ h, ai, q }), [k](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.opts_seq = 0;
        if (!okv) { set_msg("ga:" + k, "Roll failed: " + err_text(res), false); return; }
        u.opts.clear(); u.sel = -1; u.rolled = true;
        for (auto& x : jlist(res)) if (x.is_object()) u.opts.push_back(parse_affix(x));
        set_msg("ga:" + k, u.opts.empty() ? "The game offered no options for this affix" : std::to_string(u.opts.size()) + " options - pick one", !u.opts.empty());
    });
}
// every attribute the game allows on this affix slot (DKE.gear_attrs), with the value range for this item
static void fetch_gear_attrs(int64_t h, int ai) {
    const std::string k = key(h, ai);
    g_aff_ui[k].attrs_seq = lua_call("DKE.gear_attrs", json::array({ h, ai }), [k](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.attrs_seq = 0; u.attrs_loaded = true;
        if (!okv) { set_msg("ga:" + k, "Attribute list failed: " + err_text(res), false); return; }
        u.attrs.clear(); u.attr_sel = -1;
        for (auto& x : jlist(res)) if (x.is_object()) {
            AttrOpt o; o.attr = jstr(x, "attr", "?");
            o.has_range = x.contains("min") && x.contains("max") && x["min"].is_number() && x["max"].is_number();
            if (o.has_range) { o.mn = x["min"].get<double>(); o.mx = x["max"].get<double>(); }
            u.attrs.push_back(o);
        }
    });
}
// roll the game's generator until it offers the chosen attribute (DKE.gear_pick); the hit becomes option 1
static void pick_gear_attr(int64_t h, int ai, const std::string& attr, float quality) {
    const std::string k = key(h, ai);
    set_msg("ga:" + k, "", true);
    const double q = std::round(quality * 10.0) / 10.0;
    g_aff_ui[k].pick_seq = lua_call("DKE.gear_pick", json::array({ h, ai, attr, q, 150 }), [k](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.pick_seq = 0;
        if (!okv || !res.is_object()) { set_msg("ga:" + k, "Not offered: " + err_text(res), false); return; }
        Affix a = parse_affix(res);
        u.opts.clear(); u.opts.push_back(a); u.rolled = true;
        u.sel = 0; u.value = a.value; u.divine = a.divine;
        set_msg("ga:" + k, "Got " + pretty_attr(a.attr) + " " + fnum(a.value) + " after " + fnum(jnum(res, "rolls", 0)) + " roll(s) - set the value, then APPLY", true);
    });
}
static void apply_gear_affix(int64_t h, int ai, int opt1, double value, bool divine) {
    const std::string k = key(h, ai);
    set_msg("ga:" + k, "", true);
    g_aff_ui[k].apply_seq = lua_call("DKE.gear_set", json::array({ h, ai, opt1, value, divine }), [k, h, ai](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.apply_seq = 0;
        if (!okv) { set_msg("ga:" + k, "Apply failed: " + err_text(res), false); return; }
        const bool inner = res.is_object() ? jbool(res, "ok", false) : false;
        auto it = res.is_object() ? res.find("item") : res.end();
        if (res.is_object() && it != res.end() && it->is_object()) {
            Gear ng = parse_gear(*it);
            if (Gear* g = find_gear(h)) { ng.equipped = g->equipped; *g = ng; }   // gear_set always reports equipped=true
            else g_gear.push_back(ng);
        }
        u.opts.clear(); u.sel = -1; u.rolled = false;       // roll again for the next change
        u.attrs_loaded = false;                             // ranges / allowed list can change with the item
        std::string now;
        if (Gear* g = find_gear(h)) if (ai >= 1 && ai <= (int)g->affixes.size()) now = pretty_attr(g->affixes[ai - 1].attr) + " " + fnum(g->affixes[ai - 1].value);
        if (!inner) set_msg("ga:" + k, "Game call returned false - affix unchanged? (" + as_text(res) + ")", false);
        else set_msg("ga:" + k, "Applied: " + now, true);
    });
}

// ---- widgets
static bool list_row(const char* id, const std::string& left, const std::string& right, uint32_t col, bool sel) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight() + S(4), w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (sel) { dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .20f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .20f));
               ui::gold_frame(dl, p, ImVec2(p.x + w - 1, p.y + h - 1), 1.f, .5f); }
    else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::rgb(0xFFFFFF, .04f));
    dl->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), ui::rgb(col));
    const ImVec2 ts = ImGui::CalcTextSize(left.c_str()), rs = ImGui::CalcTextSize(right.c_str());
    dl->PushClipRect(p, ImVec2(p.x + w - rs.x - S(14), p.y + h), true);
    dl->AddText(ImVec2(p.x + S(10), p.y + (h - ts.y) * .5f), ui::rgb(col, sel || hov ? 1.f : .9f), left.c_str());
    dl->PopClipRect();
    dl->AddText(ImVec2(p.x + w - rs.x - S(8), p.y + (h - rs.y) * .5f), ui::rgb(sel ? ui::kGoldHi : ui::kDim), right.c_str());
    return clicked;
}
static void colored(const std::string& s, uint32_t c) { ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(c)); ImGui::TextUnformatted(s.c_str()); ImGui::PopStyleColor(); }

static void toolbar(const char* id, bool online, bool loading, char* filter, size_t fsz, const char* hint, void (*refresh)()) {
    ImGui::PushID(id);
    ImGui::BeginDisabled(!online || loading);
    if (ui::primary(loading ? "LOADING...##r" : "REFRESH##r", S(130), ImGui::GetFrameHeight())) refresh();
    ImGui::EndDisabled();
    if (!online && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(LINK_OFF);
    ImGui::SameLine(0, S(8));
    ImGui::SetNextItemWidth(std::min(S(320), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##f", hint, filter, fsz);
    ImGui::PopID();
}

// ---- FRAGMENTS
static bool frag_matches(const Frag& f, const std::string& flt) {
    if (flt.empty()) return true;
    for (auto& s : f.slots) if (lower(s.tag).find(flt) != std::string::npos || lower(s.name).find(flt) != std::string::npos) return true;
    return false;
}
static void draw_frag_slot(Frag& f, int si, bool online) {
    Slot& s = f.slots[si - 1];
    const std::string k = key(f.handle, si);
    SlotUI& u = g_slot_ui[k];
    if (u.rarity < 0) u.rarity = std::clamp(s.rarity, 0, 4);
    ImGui::PushID(si);
    char head[300];
    sprintf_s(head, "Slot %d   %s###slot", si, slot_label(s.tag, s.name).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(rar_col(s.rarity)));
    const bool open = ImGui::CollapsingHeader(head);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s", s.tag.c_str(), s.desc.empty() ? "" : "\n", s.desc.c_str());
    {   // right-hand info: fixed column, pushed right when the name is long (never over the name)
        const char* vis_end = strstr(head, "###");
        const float name_w = ImGui::CalcTextSize(head, vis_end).x;
        const float name_end = ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x + ImGui::GetFontSize()
                             + ImGui::GetStyle().FramePadding.x * 3 + name_w + S(14);
        ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - S(230), name_end));
    }
    ImGui::TextDisabled("%s  %s%s", rar_name(s.rarity), type_name(s.type), s.divine ? "  Divine" : "");
    if (open) {
        ImGui::Indent(S(12));
        if (!s.desc.empty()) ImGui::TextWrapped("%s", s.desc.c_str());
        text_dim("Tag: " + (s.tag.empty() ? std::string("(empty)") : s.tag));
        if (!u.loaded && !call_busy(u.opts_seq) && online) fetch_frag_options(f.handle, si);   // fetched when expanded
        const bool busy_opts = call_busy(u.opts_seq), busy_apply = call_busy(u.apply_seq);
        const float lw = S(70);
        // tag combo (with a filter field inside)
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Tag"); ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(std::min(S(440), ImGui::GetContentRegionAvail().x - S(80)));
        auto opt_label = [&u](int i) { return slot_label(u.opts[i], i < (int)u.opt_names.size() ? u.opt_names[i] : std::string()); };
        const std::string preview = u.sel >= 0 && u.sel < (int)u.opts.size() ? opt_label(u.sel) : busy_opts ? std::string("loading...") : "(keep current) " + slot_label(s.tag, s.name);
        ImGui::BeginDisabled(busy_opts || u.opts.empty());
        if (ImGui::BeginCombo("##tag", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##flt", "Filter...", u.filter, sizeof u.filter);
            const std::string fl = lower(u.filter);
            if (ImGui::Selectable("(keep current)", u.sel < 0)) u.sel = -1;
            for (int i = 0; i < (int)u.opts.size(); ++i) {
                const std::string lab = opt_label(i);
                if (!fl.empty() && lower(u.opts[i]).find(fl) == std::string::npos && lower(lab).find(fl) == std::string::npos) continue;
                ImGui::PushID(i);
                const bool cur = u.opts[i] == s.tag;
                if (ImGui::Selectable((lab + (cur ? "   (current)" : "")).c_str(), u.sel == i)) u.sel = i;
                if (ImGui::IsItemHovered()) {
                    const std::string d = i < (int)u.opt_desc.size() ? u.opt_desc[i] : std::string();
                    ImGui::SetTooltip("%s%s%s", u.opts[i].c_str(), d.empty() ? "" : "\n", d.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!online || busy_opts);
        if (ImGui::SmallButton("refresh")) { u.loaded = false; fetch_frag_options(f.handle, si); }
        ImGui::EndDisabled();
        if (busy_opts) loading_line("Options");
        else if (u.loaded && u.opts.empty()) text_dim("No options from the game for this slot", ui::kAmber);
        // rarity
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Rarity"); ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(S(180));
        ImGui::Combo("##rar", &u.rarity, kRar, 5);
        // divine: does not stick on fragments
        ImGui::SameLine(0, S(16));
        ImGui::BeginDisabled(true); bool dv = s.divine; ImGui::Checkbox("Divine", &dv); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("(does not stick on fragments)");
        // apply
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::BeginDisabled(!online || busy_apply || busy_opts);
        if (ui::primary(busy_apply ? "APPLYING...##ap" : "APPLY##ap", S(140), ImGui::GetFrameHeight() + S(2))) {
            const std::string tag = u.sel >= 0 && u.sel < (int)u.opts.size() ? u.opts[u.sel] : s.tag;   // never null
            apply_frag_slot(f.handle, si, tag, u.rarity, s.divine);
        }
        ImGui::EndDisabled();
        if (busy_apply) loading_line("Apply");
        show_msg("fs:" + k);
        ImGui::Unindent(S(12));
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    ImGui::PopID();
}
static void draw_fragments(bool online) {
    toolbar("frag", online, call_busy(g_frags_seq), g_ffilter, sizeof g_ffilter, "Filter by any slot tag...", refresh_frags);
    show_msg("frags");
    if (!g_frags_loaded) { if (!call_busy(g_frags_seq)) text_dim(online ? "Press REFRESH to read your fragments from the game." : LINK_OFF ".", ui::kDim); else loading_line("Fragments"); return; }
    const std::string flt = lower(g_ffilter);
    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##flist", ImVec2(std::min(S(330), ImGui::GetContentRegionAvail().x * 0.42f), avail_h), ImGuiChildFlags_Borders);
    int shown = 0;
    for (auto& f : g_frags) {
        if (!frag_matches(f, flt)) continue;
        ++shown;
        ImGui::PushID((int)(f.handle & 0x7FFFFFFF));
        const std::string name = f.slots.empty() ? "(no slots)" : slot_label(f.slots[0].tag, f.slots[0].name);
        const std::string right = std::to_string(f.size) + (f.onGrid ? "  GRID" : f.floating ? "  FLOAT" : "  STASH");
        if (list_row("##r", name, right, rar_col(f.rarity), g_fsel == f.handle)) g_fsel = f.handle;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  -  %s, %d slots, handle %lld", f.onGrid ? "on the grid" : f.floating ? "floating" : "in the stash", rar_name(f.rarity), f.size, (long long)f.handle);
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled(g_frags.empty() ? "No fragments." : "Nothing matches the filter.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##fdet", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
    Frag* f = find_frag(g_fsel);
    if (!f) text_dim("Select a fragment on the left.");
    else {
        const Frag& cf = *f;
        ImGui::PushFont(g_font_bold);
        colored(cf.slots.empty() ? "(no slots)" : slot_label(cf.slots[0].tag, cf.slots[0].name), rar_col(cf.rarity));
        ImGui::PopFont();
        ImGui::TextDisabled("%s  |  %d slots  |  %s  |  handle %lld", rar_name(cf.rarity), cf.size,
                            cf.onGrid ? "on the grid" : cf.floating ? "floating" : "in the stash", (long long)cf.handle);
        const int64_t h = cf.handle;
        const bool busy_max = call_busy(g_fmax_seq[h]);
        ImGui::BeginDisabled(!online || busy_max);
        if (ui::primary(busy_max ? "WORKING...##max" : "MAKE ALL SLOTS MYTHICAL##max", S(260), ImGui::GetFrameHeight() + S(2))) frag_max(h);
        ImGui::EndDisabled();
        if (busy_max) loading_line("Make Mythical");
        show_msg("fmax:" + std::to_string(h));
        section_header("Slots");
        f = find_frag(h);   // a callback may have replaced it (same lock, but keep it obviously safe)
        if (f) for (int si = 1; si <= (int)f->slots.size(); ++si) draw_frag_slot(*f, si, online);
    }
    ImGui::EndChild();
}

// ---- GEAR
static bool gear_matches(const Gear& g, const std::string& flt) {
    if (flt.empty()) return true;
    if (lower(g.tag).find(flt) != std::string::npos || lower(g.name).find(flt) != std::string::npos) return true;
    for (auto& a : g.affixes) if (lower(a.attr).find(flt) != std::string::npos) return true;
    return false;
}
static void draw_affix(Gear& g, int ai, bool online) {
    Affix& a = g.affixes[ai - 1];
    const std::string k = key(g.handle, ai);
    AffixUI& u = g_aff_ui[k];
    ImGui::PushID(ai);
    char head[300];
    sprintf_s(head, "%d.  %s  %s%s%s%s###aff", ai, pretty_attr(a.attr).c_str(), fnum(a.value).c_str(),
              a.primary ? "   [primary]" : "", a.divine ? "   [divine]" : "", a.corrupted ? "   [corrupted]" : "");
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(a.divine ? ui::kGoldHi : a.corrupted ? ui::kRed : ui::kBone));
    const bool open = ImGui::CollapsingHeader(head);
    ImGui::PopStyleColor();
    if (open) {
        ImGui::Indent(S(12));
        if (a.eff != 0) text_dim("Effectiveness " + fnum(a.eff));
        const bool busy_roll = call_busy(u.opts_seq), busy_apply = call_busy(u.apply_seq);
        const bool busy_attrs = call_busy(u.attrs_seq), busy_pick = call_busy(u.pick_seq);
        const bool busy_any = busy_roll || busy_apply || busy_pick;
        if (!u.attrs_loaded && !busy_attrs && online) fetch_gear_attrs(g.handle, ai);   // fetched when expanded
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Quality"); ImGui::SameLine(S(80));
        ImGui::SetNextItemWidth(S(220));
        ImGui::SliderFloat("##q", &u.quality, 0.f, 10.f, "%.1f");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) u.quality = 5.f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quality passed to the game's reroll generator (0..10). Right-click: 5");
        // pick any attribute the game allows on this slot
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Attribute"); ImGui::SameLine(S(80));
        ImGui::SetNextItemWidth(std::min(S(300), ImGui::GetContentRegionAvail().x - S(200)));
        auto attr_label = [](const AttrOpt& o) {
            return o.has_range ? pretty_attr(o.attr) + "   (" + fnum(o.mn) + " - " + fnum(o.mx) + ")" : pretty_attr(o.attr);
        };
        const std::string apreview = u.attr_sel >= 0 && u.attr_sel < (int)u.attrs.size() ? attr_label(u.attrs[u.attr_sel])
                                   : busy_attrs ? std::string("loading...") : u.attrs.empty() ? std::string("(none)") : std::string("choose...");
        ImGui::BeginDisabled(busy_attrs || u.attrs.empty());
        if (ImGui::BeginCombo("##attr", apreview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < (int)u.attrs.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(attr_label(u.attrs[i]).c_str(), u.attr_sel == i)) u.attr_sel = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Every attribute the game allows on this slot. The range is the normal roll range for this item level.");
        ImGui::SameLine(0, S(10));
        ImGui::BeginDisabled(!online || u.attr_sel < 0 || busy_any);
        if (ui::primary(busy_pick ? "ROLLING...##pick" : "GET THIS AFFIX##pick", S(170), ImGui::GetFrameHeight()))
            pick_gear_attr(g.handle, ai, u.attrs[u.attr_sel].attr, u.quality);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Rolls the game's own generator until it offers this attribute (up to 150 rolls, free)");
        // or a plain random roll
        ImGui::Dummy(ImVec2(0, 0)); ImGui::SameLine(S(80));
        ImGui::BeginDisabled(!online || busy_any);
        if (ui::primary(busy_roll ? "ROLLING...##roll" : "RANDOM OPTIONS##roll", S(170), ImGui::GetFrameHeight())) roll_gear_options(g.handle, ai, u.quality);
        ImGui::EndDisabled();
        if (busy_roll || busy_pick) loading_line("Options");
        if (!u.opts.empty()) {
            ImGui::Dummy(ImVec2(0, S(2)));
            for (int i = 0; i < (int)u.opts.size(); ++i) {
                const Affix& o = u.opts[i];
                char lab[300];
                sprintf_s(lab, "%s  %s%s%s##o%d", pretty_attr(o.attr).c_str(), fnum(o.value).c_str(), o.divine ? "   [divine]" : "", o.primary ? "   [primary]" : "", i);
                if (ImGui::RadioButton(lab, u.sel == i)) { u.sel = i; u.value = o.value; u.divine = o.divine; }
            }
            ImGui::Dummy(ImVec2(0, S(2)));
            ImGui::BeginDisabled(u.sel < 0);
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Value"); ImGui::SameLine(S(80));
            ImGui::SetNextItemWidth(S(160));
            ImGui::InputDouble("##val", &u.value, 0, 0, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Prefilled with the option's value; any number works (e.g. 500)");
            ImGui::SameLine(0, S(16));
            ImGui::Checkbox("Divine", &u.divine);
            ImGui::SameLine(0, S(16));
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!online || u.sel < 0 || busy_any);
            if (ui::primary(busy_apply ? "APPLYING...##ap" : "APPLY##ap", S(140), ImGui::GetFrameHeight()))
                apply_gear_affix(g.handle, ai, u.sel + 1, u.value, u.divine);
            ImGui::EndDisabled();
            if (u.sel < 0) text_dim("Pick one of the game's options above, then adjust value / divine.");
            if (busy_apply) loading_line("Apply");
        } else if (u.rolled && !busy_roll) text_dim("No options - try another quality.", ui::kAmber);
        show_msg("ga:" + k);
        ImGui::Unindent(S(12));
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    ImGui::PopID();
}
// ---- Ravenous weapons (Editor > GEAR, 2026-11-09): make a weapon Ravenous, refill Voracity, devour with affix choice
struct RavUI {
    int64_t h = 0;                                   // item these choices belong to
    std::vector<std::pair<std::string, std::string>> choices; int64_t choices_seq = 0; bool choices_loaded = false; int csel = -1;
    int64_t make_seq = 0, refill_seq = 0, offer_seq = 0, eat_seq = 0; bool make_arm = false, eat_arm = false;
    int64_t food = 0; int slot = 1; std::vector<Affix> offer; int osel = -1; bool offered = false;
};
static RavUI g_rav;
static std::string rav_text(const std::string& t) {        // game rich text -> plain: <Tag>x</> -> x, {Value0} -> N
    std::string o;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '<') { size_t e = t.find('>', i); if (e != std::string::npos) { i = e; continue; } }
        if (t[i] == '{') { size_t e = t.find('}', i); if (e != std::string::npos) { o += t.compare(i, 13, "{ValuePercent") == 0 ? "N%" : "N"; i = e; continue; } }
        o += t[i];
    }
    return o;
}
static void rav_reset(int64_t h) { if (g_rav.h != h) { g_rav = RavUI(); g_rav.h = h; } }
static void rav_fetch_choices(int64_t h) {
    g_rav.choices_seq = lua_call("DKE.ravenous_choices", json::array({ h }), [h](bool okv, const json& res) {
        if (g_rav.h != h) return;
        g_rav.choices_seq = 0; g_rav.choices_loaded = true;
        if (!okv) { set_msg("rav", "Ravenous list failed: " + err_text(res), false); return; }
        for (auto& x : jlist(res)) if (x.is_object()) g_rav.choices.push_back({ jstr(x, "name"), rav_text(jstr(x, "text")) });
    });
}
static void rav_make(int64_t h, const std::string& name) {
    set_msg("rav", "", true);
    g_rav.make_seq = lua_call("DKE.ravenous_make", json::array({ h, name }), [name](bool okv, const json& res) {
        g_rav.make_seq = 0;
        if (!okv) { set_msg("rav", "Make failed: " + err_text(res), false); return; }
        set_msg("rav", "Now a Ravenous weapon: " + name + ". Re-open the inventory to see it.", true);
        refresh_gear();
    });
}
static void rav_refill(int64_t h) {
    g_rav.refill_seq = lua_call("DKE.voracity_refill", json::array({ h }), [h](bool okv, const json& res) {
        g_rav.refill_seq = 0;
        if (!okv) { set_msg("rav", "Refill failed: " + err_text(res), false); return; }
        if (Gear* g = find_gear(h)) { g->rav_left = (int)jnum(res, "left", g->rav_left); g->rav_max = (int)jnum(res, "max", g->rav_max); }
        set_msg("rav", "Voracity refilled", true);
    });
}
static void rav_offer(int64_t h) {
    g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; set_msg("rav", "", true);
    g_rav.offer_seq = lua_call("DKE.feed_options", json::array({ h, g_rav.food, g_rav.slot }), [h](bool okv, const json& res) {
        if (g_rav.h != h) return;
        g_rav.offer_seq = 0; g_rav.offered = true;
        if (!okv) { set_msg("rav", "Offer failed: " + err_text(res), false); return; }
        for (auto& a : jlist_at(res, "options")) if (a.is_object()) g_rav.offer.push_back(parse_affix(a));
        if (g_rav.offer.size() == 1) g_rav.osel = 0;
    });
}
static void rav_eat(int64_t h) {
    const int64_t food = g_rav.food; const int slot = g_rav.slot, opt = g_rav.osel + 1;
    g_rav.eat_seq = lua_call("DKE.feed_apply", json::array({ h, food, slot, opt, true }), [h, food](bool okv, const json& res) {
        g_rav.eat_seq = 0; g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; g_rav.food = 0;
        if (!okv) { set_msg("rav", "Devour failed: " + err_text(res), false); refresh_gear(); return; }
        auto it = res.is_object() ? res.find("item") : res.end();
        if (it != res.end() && it->is_object()) { Gear ng = parse_gear(*it); if (Gear* g = find_gear(h)) { ng.equipped = g->equipped; *g = ng; } }
        auto rv = res.is_object() ? res.find("ravenous") : res.end();
        if (Gear* g = find_gear(h)) if (rv != res.end() && rv->is_object()) {
            g->rav = true; g->rav_name = jstr(*rv, "name"); g->rav_text = jstr(*rv, "text"); g->rav_left = (int)jnum(*rv, "left", -1); g->rav_max = (int)jnum(*rv, "max", 0); }
        g_gear.erase(std::remove_if(g_gear.begin(), g_gear.end(), [food](const Gear& x) { return x.handle == food; }), g_gear.end());
        set_msg("rav", "Devoured - Voracity kept full. Re-open the inventory to see the new affix.", true);
    });
}
static bool is_weapon(const Gear& g) { const std::string t = g.tag.substr(g.tag.rfind('.') + 1); return t == "LeftHand" || t == "RightHand"; }
static std::string gear_label(const Gear& x) { return x.name.empty() ? gear_name(x.tag) : x.name; }
static std::string affix_label(const Gear& g, int i) { return std::to_string(i) + ".  " + pretty_attr(g.affixes[i - 1].attr) + " " + fnum(g.affixes[i - 1].value); }
static void draw_ravenous(Gear& g, bool online) {
    if (!is_weapon(g)) return;
    rav_reset(g.handle);
    section_header("Ravenous");
    const float bh = ImGui::GetFrameHeight();
    if (!g.rav) {
        if (g.equipped) { text_dim("Take this weapon off to turn it into a Ravenous weapon (bag items only)."); show_msg("rav"); return; }
        if (!g_rav.choices_loaded && !call_busy(g_rav.choices_seq) && online) rav_fetch_choices(g.handle);
        if (call_busy(g_rav.choices_seq)) { loading_line("Ravenous weapons"); return; }
        if (g_rav.choices.empty()) { text_dim("No Ravenous weapon for this class and hand."); show_msg("rav"); return; }
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Become"); ImGui::SameLine(S(80));
        ImGui::SetNextItemWidth(std::max(S(120), std::min(S(260), ImGui::GetContentRegionAvail().x - S(190))));   // room for the button
        const char* prev = g_rav.csel >= 0 && g_rav.csel < (int)g_rav.choices.size() ? g_rav.choices[g_rav.csel].first.c_str() : "choose...";
        if (ImGui::BeginCombo("##ravc", prev, ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < (int)g_rav.choices.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(g_rav.choices[i].first.c_str(), g_rav.csel == i)) { g_rav.csel = i; g_rav.make_arm = false; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_rav.choices[i].second.c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, S(10));
        ImGui::BeginDisabled(!online || g_rav.csel < 0 || call_busy(g_rav.make_seq));
        if (ui::danger(g_rav.make_arm ? "CONFIRM?##ravm" : "MAKE RAVENOUS##ravm", S(170), bh)) {
            if (!g_rav.make_arm) g_rav.make_arm = true; else { g_rav.make_arm = false; rav_make(g.handle, g_rav.choices[g_rav.csel].first); }
        }
        ImGui::EndDisabled();
        if (g_rav.csel >= 0 && g_rav.csel < (int)g_rav.choices.size()) {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(8)); text_dim(g_rav.choices[g_rav.csel].second, ui::kDim); ImGui::PopTextWrapPos(); }
        text_dim("Changes this item for good (keeps its level and affixes). Use a magic or rare weapon: it needs affixes to devour.", ui::kAmber);
        show_msg("rav");
        return;
    }
    // already Ravenous
    ImGui::PushFont(g_font_bold); colored(g.rav_name, ui::kRed); ImGui::PopFont();
    ImGui::SameLine(S(200)); ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Voracity %s / %d", g.rav_left < 0 ? "?" : std::to_string(g.rav_left).c_str(), g.rav_max);
    ImGui::SameLine(0, S(12));
    ImGui::BeginDisabled(!online || call_busy(g_rav.refill_seq));
    if (ui::primary("REFILL VORACITY##ravr", S(170), bh)) rav_refill(g.handle);
    ImGui::EndDisabled();
    if (!g.rav_text.empty()) { ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(8)); text_dim(rav_text(g.rav_text), ui::kDim); ImGui::PopTextWrapPos(); }
    // devour
    ImGui::Dummy(ImVec2(0, S(4))); ui::label("Devour (free, Voracity stays full)", ui::kGold);
    if (g.affixes.empty()) { text_dim("This weapon has no affixes - devouring would fail. Give it affixes first.", ui::kAmber); show_msg("rav"); return; }
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Eat"); ImGui::SameLine(S(80));
    ImGui::SetNextItemWidth(std::min(S(320), ImGui::GetContentRegionAvail().x - S(10)));
    Gear* food = g_rav.food ? find_gear(g_rav.food) : nullptr;
    const std::string fprev = food ? gear_label(*food) + "  (Lv " + std::to_string(food->level) + ")" : std::string("choose an item from the bag...");
    if (ImGui::BeginCombo("##ravf", fprev.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (auto& x : g_gear) {
            if (x.equipped || x.handle == g.handle || x.rav) continue;
            ImGui::PushID((int)(x.handle & 0x7FFFFFFF));
            const std::string lab = gear_label(x) + "  (" + rar_name(x.rarity) + ", Lv " + std::to_string(x.level) + ")";
            if (ImGui::Selectable(lab.c_str(), g_rav.food == x.handle)) { g_rav.food = x.handle; g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; g_rav.eat_arm = false; }
            if (ImGui::IsItemHovered()) { std::string tt = x.tag; for (auto& a : x.affixes) tt += "\n" + pretty_attr(a.attr) + " " + fnum(a.value); ImGui::SetTooltip("%s", tt.c_str()); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Replace"); ImGui::SameLine(S(80));
    ImGui::SetNextItemWidth(std::min(S(320), ImGui::GetContentRegionAvail().x - S(10)));
    if (g_rav.slot < 1 || g_rav.slot > (int)g.affixes.size()) g_rav.slot = 1;
    if (ImGui::BeginCombo("##ravs", affix_label(g, g_rav.slot).c_str())) {
        for (int i = 1; i <= (int)g.affixes.size(); ++i)
            if (ImGui::Selectable(affix_label(g, i).c_str(), g_rav.slot == i)) { g_rav.slot = i; g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; g_rav.eat_arm = false; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which of this weapon's affixes the devoured item replaces");
    ImGui::Dummy(ImVec2(0, 0)); ImGui::SameLine(S(80));
    ImGui::BeginDisabled(!online || !food || call_busy(g_rav.offer_seq) || call_busy(g_rav.eat_seq));
    if (ui::primary(call_busy(g_rav.offer_seq) ? "ASKING...##ravo" : "SEE NEW AFFIX##ravo", S(170), bh)) rav_offer(g.handle);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Asks the game which affix this item would give (nothing is eaten yet)");
    if (!g_rav.offer.empty()) {
        for (int i = 0; i < (int)g_rav.offer.size(); ++i) {
            const Affix& o = g_rav.offer[i]; char lab[300];
            sprintf_s(lab, "%s  %s%s##ro%d", pretty_attr(o.attr).c_str(), fnum(o.value).c_str(), o.divine ? "   [divine]" : "", i);
            if (ImGui::RadioButton(lab, g_rav.osel == i)) { g_rav.osel = i; g_rav.eat_arm = false; }
        }
        ImGui::BeginDisabled(!online || g_rav.osel < 0 || call_busy(g_rav.eat_seq));
        if (ui::danger(call_busy(g_rav.eat_seq) ? "DEVOURING...##rave" : g_rav.eat_arm ? "CONFIRM? (item is destroyed)##rave" : "DEVOUR##rave", S(260), bh)) {
            if (!g_rav.eat_arm) g_rav.eat_arm = true; else { g_rav.eat_arm = false; rav_eat(g.handle); }
        }
        ImGui::EndDisabled();
    } else if (g_rav.offered && !call_busy(g_rav.offer_seq)) text_dim("The game offered nothing for this item.", ui::kAmber);
    text_dim("The eaten item is destroyed, like in the game. Voracity is refilled after each devour.", ui::kDim);
    show_msg("rav");
}
static void draw_gear(bool online) {
    toolbar("gear", online, call_busy(g_gear_seq), g_gfilter, sizeof g_gfilter, "Filter by tag or affix...", refresh_gear);
    show_msg("gear");
    if (!g_gear_loaded) { if (!call_busy(g_gear_seq)) text_dim(online ? "Press REFRESH to read your gear from the game." : LINK_OFF ".", ui::kDim); else loading_line("Gear"); return; }
    const std::string flt = lower(g_gfilter);
    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##glist", ImVec2(std::min(S(330), ImGui::GetContentRegionAvail().x * 0.42f), avail_h), ImGuiChildFlags_Borders);
    int shown = 0, group = -1;
    for (auto& g : g_gear) {
        if (!gear_matches(g, flt)) continue;
        const int grp = g.equipped ? 1 : 0;
        if (grp != group) { group = grp; if (shown) ImGui::Dummy(ImVec2(0, S(4))); ui::label(g.equipped ? "Equipped" : "Bag", ui::kGold); }
        ++shown;
        ImGui::PushID((int)(g.handle & 0x7FFFFFFF));
        const std::string right = "Lv " + std::to_string(g.level);
        if (list_row("##r", g.name.empty() ? gear_name(g.tag) : g.name, right, rar_col(g.rarity), g_gsel == g.handle)) g_gsel = g.handle;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s, level %d, handle %lld", g.tag.c_str(), rar_name(g.rarity), g.level, (long long)g.handle);
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled(g_gear.empty() ? "No gear." : "Nothing matches the filter.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##gdet", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
    Gear* g = find_gear(g_gsel);
    if (!g) text_dim("Select an item on the left.");
    else {
        ImGui::PushFont(g_font_bold);
        colored(g->name.empty() ? gear_name(g->tag) : g->name, rar_col(g->rarity));
        ImGui::PopFont();
        text_dim(g->tag);
        ImGui::TextDisabled("%s  |  level %d  |  %s  |  rerolls left %s  |  handle %lld", rar_name(g->rarity), g->level,
                            g->equipped ? "equipped" : "in the bag", g->rerolls < 0 ? "-" : std::to_string(g->rerolls).c_str(), (long long)g->handle);
        section_header("Affixes");
        if (g->affixes.empty()) text_dim("This item has no affixes.");
        for (int ai = 1; ai <= (int)g->affixes.size(); ++ai) draw_affix(*g, ai, online);
        text_dim("Pick an attribute and press GET THIS AFFIX (or take RANDOM OPTIONS). The affix always comes from the game's own "
                 "reroll generator; APPLY passes it back with your value / divine.");
        draw_ravenous(*g, online);
    }
    ImGui::EndChild();
}

// ---- talents (Editor > TALENTS): extra points per talent on top of what gear gives (DKT.* in dk.cpp)
struct Talent { std::string tag, name, desc; std::vector<int> floors; int total = 0, gear = 0, extra = 0; };
static std::vector<Talent> g_talents; static bool g_tal_loaded = false; static int64_t g_tal_seq = 0; static char g_tfilter[64] = {};
static void parse_talents(const json& res) {
    std::vector<Talent> v;
    for (auto& x : jlist(res)) if (x.is_object()) {
        Talent t; t.tag = jstr(x, "tag"); t.name = jstr(x, "name"); t.desc = jstr(x, "desc");
        t.total = (int)jnum(x, "total", 0); t.gear = (int)jnum(x, "gear", 0); t.extra = (int)jnum(x, "extra", 0);
        if (x.contains("floors") && x["floors"].is_array()) for (auto& f : x["floors"]) if (f.is_number()) t.floors.push_back(f.get<int>());
        if (t.name.empty()) t.name = t.tag.substr(t.tag.rfind('.') == std::string::npos ? 0 : t.tag.rfind("Talent.") == std::string::npos ? 0 : t.tag.rfind("Talent.") + 7);
        v.push_back(t);
    }
    // class talents first, then common, then wyrmling; by name inside
    auto grp = [](const Talent& t) { return t.tag.find(".Common.") != std::string::npos ? 1 : t.tag.find(".Wyrmling.") != std::string::npos ? 2 : 0; };
    std::stable_sort(v.begin(), v.end(), [&](const Talent& a, const Talent& b) { return grp(a) != grp(b) ? grp(a) < grp(b) : a.name < b.name; });
    g_talents = std::move(v); g_tal_loaded = true;
}
static void refresh_talents() {
    g_tal_seq = lua_call("DKT.list", json::array(), [](bool okv, const json& res) {
        g_tal_seq = 0;
        if (!okv) { set_msg("talents", "Refresh failed: " + err_text(res), false); return; }
        parse_talents(res); set_msg("talents", std::to_string(g_talents.size()) + " talents", true);
    });
}
static void set_talent(const std::string& tag, int extra) {
    g_tal_seq = lua_call("DKT.set", json::array({ tag, extra }), [](bool okv, const json& res) {
        g_tal_seq = 0;
        if (!okv) { set_msg("talents", "Set failed: " + err_text(res), false); return; }
        parse_talents(res);
    });
}
static int talent_rank(const Talent& t) { int r = 0; for (int f : t.floors) if (t.total >= f) ++r; return r; }
// TALENTS page: one card per talent with the game's own description visible (some talents can hurt the player when
// used wrong - the author 2026-11-09), so every talent is set on its own: OFF / I / II / III. Points go on top of gear.
static void draw_talents(bool online) {
    toolbar("talents", online, call_busy(g_tal_seq), g_tfilter, sizeof g_tfilter, "Filter by name or effect...", refresh_talents);
    show_msg("talents");
    if (!g_tal_loaded) {
        if (!call_busy(g_tal_seq)) { if (online) refresh_talents(); else text_dim(LINK_OFF " - load into a level with your character.", ui::kDim); }
        else loading_line("Talents");
        return;
    }
    text_dim("Read what a talent does before you raise it - some effects can hurt you. Ranks unlock at 1 / 3 / 6 points; "
             "the trainer adds points on top of what your gear gives and keeps them after zone changes.", ui::kDim);
    const std::string flt = lower(g_tfilter);
    ImGui::BeginChild("##tlist", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_None);
    int group = -1;
    for (auto& t : g_talents) {
        if (!flt.empty() && lower(t.name).find(flt) == std::string::npos && lower(t.desc).find(flt) == std::string::npos) continue;
        const int grp = t.tag.find(".Common.") != std::string::npos ? 1 : t.tag.find(".Wyrmling.") != std::string::npos ? 2 : 0;
        if (grp != group) { group = grp; section_header(grp == 0 ? "Class talents" : grp == 1 ? "Common talents" : "Wyrmling talents"); }
        ImGui::PushID(t.tag.c_str());
        const int rank = talent_rank(t), nr = (int)t.floors.size();
        ImGui::PushFont(g_font_bold);
        colored(t.name, rank >= nr && nr ? ui::kGold : rank > 0 ? ui::kGreen : ui::kBone);
        ImGui::PopFont();
        // OFF / I / II / III: the trainer's extra points needed to reach that rank's threshold on top of the gear.
        // Buttons sit on the name line, the rank text on its own line below (the skin's content area is narrow).
        static const char* roman[] = { "I", "II", "III", "IV", "V" };
        const float bx = ImGui::GetContentRegionMax().x - S(54) * (nr + 1);
        if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + S(8) < bx) ImGui::SameLine(bx);
        else ImGui::SetCursorPosX(bx);
        const bool off_sel = t.extra == 0;
        if (off_sel) ImGui::BeginDisabled();
        if (ImGui::Button("OFF", ImVec2(S(50), 0)) && online) set_talent(t.tag, 0);
        if (off_sel) ImGui::EndDisabled();
        for (int k = 0; k < nr && k < 5; ++k) {
            ImGui::SameLine(0, S(4));
            const int want = std::max(0, t.floors[k] - t.gear);
            const bool cur = rank == k + 1;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ui::rgbv(ui::kGold, .45f));
            if (ImGui::Button(roman[k], ImVec2(S(50), 0)) && online) set_talent(t.tag, want);
            if (cur) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rank %s needs %d points (gear gives %d, the trainer adds %d)", roman[k], t.floors[k], t.gear, want);
        }
        ImGui::TextDisabled("rank %d / %d  |  %d pts  (gear %d%s)", rank, nr, t.total, t.gear, t.extra ? (", trainer +" + std::to_string(t.extra)).c_str() : "");
        if (!t.desc.empty()) { ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(8)); text_dim(t.desc, ui::kDim); ImGui::PopTextWrapPos(); }
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void draw_editor(bool online) {
    static const char* subs[] = { "FRAGMENTS", "GEAR" };
    ui::tabs(subs, 2, &g_sub);
    if (!online) text_dim(LINK_OFF " - load into a level; the buttons work once it is running.", ui::kAmber);
    if (g_sub == 0) draw_fragments(online); else draw_gear(online);
}

} // namespace ed

#include "panel_skin.inc"   // painted Dragonkin panel (skin_src -> tools\make_skin_dragon.py -> bin\skin)

void panel_draw(bool* visible) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_drag_id.clear();
    const bool online = lua_online();
    const bool dev = developer_mode();
    PanelScale scale_scope(std::clamp(g_settings.ui_scale, 0.75f, 1.5f));
    if (sk::on()) { panel_draw_skin(visible, online, dev); return; }
    { const ImVec2 ds = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(std::max(S(980), std::min(ds.x * 0.55f, S(1700))), std::max(S(700), ds.y * 0.82f)), ImGuiCond_Once); }
    ImGui::SetNextWindowPos(ImVec2(S(60), S(70)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(460), S(360)), ImVec2(S(2600), S(1600)));
    ui::theme_push();
    ImGui::PushFont(g_font_body);
    if (!ImGui::Begin("##dragonkin_bridge", visible, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); ImGui::PopFont(); ui::theme_pop(); return; }

    // ---- header: two-tone title + status pills
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ui::rgb(ui::kInk, .55f), ui::rgb(ui::kInk, .25f), ui::rgb(ui::kInk, .25f), ui::rgb(ui::kInk, .55f));
    dl->AddLine(ImVec2(wp.x, wp.y + 1), ImVec2(wp.x + ws.x, wp.y + 1), ui::rgb(ui::kGold, .8f), S(1.5f));
    ui::gold_frame(dl, ImVec2(wp.x + 1, wp.y + 1), ImVec2(wp.x + ws.x - 1, wp.y + ws.y - 1), 1.f, .5f);
    {
        std::string t1 = g_title, t2; size_t sp = g_title.find(' '); if (sp != std::string::npos) { t1 = g_title.substr(0, sp); t2 = g_title.substr(sp + 1); }
        ui::brand(t1.c_str(), t2.c_str(), "DRAGONKIN TRAINER");
        ui::pill(game_base() ? "Game linked" : "Game not found", game_base() ? ui::kGreen : ui::kAmber); ImGui::SameLine(0, S(6));
        ui::pill(online ? LINK : LINK_OFF, online ? ui::kGreen : g_link.ever ? ui::kRed : ui::kAmber); ImGui::SameLine(0, S(6));
        const json& k = g_link.keeper;
        const bool ready = jbool(k, "ready", false);
        ui::pill(!online ? "Keeper ?" : ready ? "Keeper ready" : "Keeper waiting", !online ? ui::kDim : ready ? ui::kGreen : ui::kAmber); ImGui::SameLine(0, S(6));
        const std::string zone = jstr(k, "world", jstr(g_link.player, "world"));
        ui::pill(("Zone  " + (zone.empty() ? std::string("-") : zone)).c_str(), ui::kCyan);
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(28));
    if (ImGui::Button("X", ImVec2(S(28), 0))) *visible = false;
    ImGui::Dummy(ImVec2(0, 2));

    // ---- body: left nav (spec tabs + EDITOR + BRIDGE (DEV, developer mode) + SETTINGS) + content
    const int ntab = (int)g_tabs.size(), kEditor = ntab, kBridge = ntab + 1, kSettings = ntab + 2, kTalents = ntab + 3;
    std::vector<int> pages;
    for (int i = 0; i < ntab; ++i) pages.push_back(i);
    pages.push_back(kEditor);
    pages.push_back(kTalents);
    if (dev) pages.push_back(kBridge);
    pages.push_back(kSettings);
    if (g_tab == kBridge && !dev) g_tab = kSettings;
    if (g_tab < 0 || g_tab > kTalents) g_tab = 0;
    if (g_capturing && g_tab != kSettings) capture_cancel("Cancelled - hotkey unchanged");
    capture_poll();
    if (!ImGui::IsAnyItemActive()) {   // controller: LB / RB switch pages
        const int n = (int)pages.size();
        int cur = 0; for (int i = 0; i < n; ++i) if (pages[i] == g_tab) cur = i;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_tab = pages[(cur + n - 1) % n];
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_tab = pages[(cur + 1) % n];
    }
    const float rail = S(176.f), footer = ImGui::GetFrameHeightWithSpacing() + 6;
    ImGui::BeginChild("##rail", ImVec2(rail, -footer), ImGuiChildFlags_Borders);
    for (int i : pages) {
        const std::string name = i < ntab ? g_tabs[i].name : i == kEditor ? "Editor" : i == kTalents ? "Talents" : i == kBridge ? "Bridge" : "Settings";
        const bool sel = g_tab == i;
        if (i == ntab) { ImGui::Dummy(ImVec2(0, S(4))); ui::gold_rule(.3f); }
        ImGui::PushID(i);
        {   const ImVec2 p = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() + S(8);
            if (ImGui::InvisibleButton("##nav", ImVec2(w, h))) g_tab = i;
            const bool hov = ImGui::IsItemHovered(); ImDrawList* nd = ImGui::GetWindowDrawList();
            if (sel) { nd->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .22f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .22f));
                       nd->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), ui::rgb(ui::kGold));
                       ui::gold_frame(nd, p, ImVec2(p.x + w - 1, p.y + h - 1), 1.f, .55f); }
            else if (hov) nd->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::rgb(0xFFFFFF, .04f));
            std::string up = name; std::transform(up.begin(), up.end(), up.begin(), ::toupper);
            const ImVec2 ts = ImGui::CalcTextSize(up.c_str());
            nd->AddText(ImVec2(p.x + S(14), p.y + (h - ts.y) * .5f), sel ? ui::rgb(ui::kGoldHi) : hov ? ui::rgb(ui::kBone) : ui::rgb(ui::kDim), up.c_str()); }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##content", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (g_tab < ntab) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof g_search);
        ui::page_title(g_tabs[g_tab].name.c_str());
        if (!online) {
            text_dim(LINK_OFF " - load into a level with your character.", ui::kAmber);
        }
        for (auto& s : g_tabs[g_tab].sections) {
            bool any = false; for (auto& it : s.items) if (matches(it)) { any = true; break; }
            if (!any) continue;
            if (!s.name.empty()) section_header(s.name.c_str());
            for (auto& it : s.items) if (matches(it)) draw_item(it);
        }
        if (g_tabs.empty()) text_dim(g_load_msg, ui::kAmber);
    } else if (g_tab == kEditor) {
        ui::page_title("Editor", "Fragments and gear, rerolled with the game's own rules");
        ed::draw_editor(online);
    } else if (g_tab == kTalents) {
        ui::page_title("Talents", "Raise any talent on its own, on top of what your gear gives");
        ed::draw_talents(online);
    }
    else {
        ui::page_title("Settings");
        draw_settings_tab();
    }
    ImGui::EndChild();

    // ---- footer
    auto dot = [](uint32_t col) {
        ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5, p.y + h * 0.5f), 4.f, ui::rgb(col));
        ImGui::Dummy(ImVec2(12, h)); ImGui::SameLine();
    };
    ImGui::AlignTextToFramePadding();
    dot(online ? ui::kGreen : ui::kAmber); ImGui::TextDisabled(online ? LINK_FOOT : LINK_FOOT_OFF); ImGui::SameLine(0, 18);
    if (!g_status.empty() && GetTickCount64() - g_status_at < 15000) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(g_status_ok ? ui::kBone : ui::kRed));
        std::string st = g_status; if (st.size() > 90) st = st.substr(0, 87) + "...";
        ImGui::TextUnformatted(st.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    {   const std::string hide = hotkey_name(g_settings.hotkey) + "  hide";
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(hide.c_str()).x - S(4));
        ImGui::TextDisabled("%s", hide.c_str()); }
    ImGui::End();
    ImGui::PopFont();
    ui::theme_pop();
}


} // namespace wb
