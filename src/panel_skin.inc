// panel_skin.inc - Dragonkin painted panel (included by panel.cpp). Art: a painted mock-up cut into
// <root>\skin\*.png + skin.json. The whole mock-up is ONE background ("bg", transparent outside the dragon frame) drawn
// at one scale K = window width / ref width; the window keeps the picture's aspect. Tabs, page title, row plates,
// check boxes, value boxes, buttons, status and version are drawn live from the pieces.
// No skin folder (or Settings > Display > Classic look) = the old panel. DEV build first (2026-11-10); public after the 12th.
extern ImFont* g_font_skin;       // tabs / headings / row labels: <root>\skin\font.ttf, else Palatino Bold
extern ImFont* g_font_skin_big;   // page title / version
namespace sk {
struct Piece { std::string kind; float w = 0, h = 0, l = 0, t = 0, r = 0, b = 0; };
static bool g_loaded = false, g_ok = false;
static std::map<std::string, Piece> g_p;
static json g_lay = json::object();
static float K = 1.f;      // reference px -> screen px (window width / ref width)
static float T = 1.f;      // text scale that follows K (fonts are built for the default size)
static ImVec2 g_org;       // window origin (screen) = reference (0,0)
static uint32_t C(const char* k, uint32_t d) { auto c = g_lay.find("colors"); return c != g_lay.end() && c->contains(k) ? (*c)[k].get<uint32_t>() : d; }
static bool load() {
    g_loaded = true; g_ok = false; g_p.clear();
    try {
        std::ifstream f(g_cfg.root + "\\skin\\skin.json");
        if (!f) return false;
        json j = json::parse(f, nullptr, false);
        if (!j.is_object() || !j.contains("pieces")) return false;
        for (auto& [n, v] : j["pieces"].items()) {
            Piece p; p.kind = jstr(v, "kind"); p.w = (float)jnum(v, "w", 0); p.h = (float)jnum(v, "h", 0);
            p.l = (float)jnum(v, "l", 0); p.t = (float)jnum(v, "t", 0); p.r = (float)jnum(v, "r", 0); p.b = (float)jnum(v, "b", 0);
            g_p[n] = p;
        }
        if (j.contains("layout") && j["layout"].is_object()) g_lay = j["layout"];
        g_ok = g_p.count("bg") && g_p.count("tab") && g_p.count("row") && g_lay.contains("content");
    } catch (...) { g_ok = false; }
    return g_ok;
}
static bool on() { if (!g_loaded || g_skin_reload) { g_skin_reload = false; load(); } return g_ok && !g_skin_classic; }
static float L(const char* k, float d = 0) { return (float)jnum(g_lay, k, d) * K; }
static ImVec4 box(const char* k) {   // a layout box [x0,y0,x1,y1] in reference px -> screen rect
    auto it = g_lay.find(k); if (it == g_lay.end() || !it->is_array() || it->size() < 4) return ImVec4(0, 0, 0, 0);
    return ImVec4(g_org.x + (*it)[0].get<float>() * K, g_org.y + (*it)[1].get<float>() * K, g_org.x + (*it)[2].get<float>() * K, g_org.y + (*it)[3].get<float>() * K);
}
static ImVec2 size(const char* n) { auto it = g_p.find(n); return it == g_p.end() ? ImVec2(0, 0) : ImVec2(it->second.w * K, it->second.h * K); }
static bool has(const char* n) { return g_p.count(n) != 0; }
static bool draw(ImDrawList* dl, const char* n, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) {
    auto it = g_p.find(n); if (it == g_p.end()) return false;
    const Piece& p = it->second; char spec[160];
    if (p.kind == "3slice") sprintf_s(spec, "%s#3:%g,%g", n, p.l, p.r);
    else if (p.kind == "9slice") sprintf_s(spec, "%s#9:%g,%g,%g,%g@%g", n, p.l, p.t, p.r, p.b, K);
    else sprintf_s(spec, "%s", n);
    return trinity::ui::DrawItemIcon(dl, spec, a, b, tint);
}
static ImU32 rgb_(uint32_t c, float a = 1.f) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, (int)(a * 255)); }
static std::string upper(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::toupper); return s; }
// text in a font, aligned in [a,b] (ax 0 left .5 centre 1 right), vertically centred
static void text(ImDrawList* dl, ImFont* f, ImVec2 a, ImVec2 b, ImU32 col, const char* s, float ax = .5f, bool shadow = true, float scale = 1.f) {
    ImGui::PushFont(f); const float fs = ImGui::GetFontSize() * scale; const ImVec2 t0 = ImGui::CalcTextSize(s); ImGui::PopFont();
    const float base = std::max(1.f, f->FontSize * ImGui::GetIO().FontGlobalScale);
    const ImVec2 ts(t0.x * fs / base, t0.y * fs / base);
    // Palatino (skin font) reserves a deep descent: capitals sit high when the line box is centred - nudge them down
    const float nudge = (f == g_font_skin || f == g_font_skin_big) ? fs * (float)jnum(g_lay, "skin_font_nudge", .1) : 0.f;
    const ImVec2 p(a.x + (b.x - a.x - ts.x) * ax, a.y + (b.y - a.y - ts.y) * .5f + nudge);
    if (shadow) dl->AddText(f, fs, ImVec2(p.x + 1.5f, p.y + 1.5f), IM_COL32(0, 0, 0, 220), s);
    dl->AddText(f, fs, p, col, s);
}
static float text_w(ImFont* f, const char* s, float scale = 1.f) { ImGui::PushFont(f); const float w = ImGui::CalcTextSize(s).x * scale; ImGui::PopFont(); return w; }
// the mock-up's gold "OPEN..." plate button: returns true when clicked
static bool plate_button(const char* id, const char* label, ImVec2 sz, uint32_t txt) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool click = ImGui::InvisibleButton(id, sz);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw(dl, "button", p, ImVec2(p.x + sz.x, p.y + sz.y), act ? IM_COL32(185, 185, 185, 255) : hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(232, 232, 232, 255));
    text(dl, g_font_skin, p, ImVec2(p.x + sz.x, p.y + sz.y), rgb_(txt), label, .5f, true, .82f * T);
    return click;
}
// row icon by label keyword (the mock-up's icons); empty = a small gold diamond
static const char* row_icon(const std::string& label) {
    std::string l = label; std::transform(l.begin(), l.end(), l.begin(), ::tolower);
    static const std::vector<std::pair<const char*, const char*>> kMap = {
        {"god", "ico_row_heart"}, {"invinc", "ico_row_heart"}, {"health", "ico_row_heart"}, {"heal", "ico_row_heart"}, {"barrier", "ico_row_heart"}, {"life", "ico_row_heart"},
        {"mana", "ico_row_flame"}, {"energy", "ico_row_flame"}, {"rage", "ico_row_flame"}, {"fury", "ico_row_flame"},
        {"cooldown", "ico_row_bolt"}, {"ultimate", "ico_row_bolt"}, {"cost", "ico_row_bolt"}, {"speed", "ico_row_runner"}, {"dodge", "ico_row_runner"}, {"move", "ico_row_runner"},
        {"damage", "ico_row_swords2"}, {"crit", "ico_row_swords2"}, {"armor", "ico_row_swords2"}, {"pierce", "ico_row_swords2"}, {"kill", "ico_row_skull"}, {"lifesteal", "ico_row_skull"},
        {"projectile", "ico_row_trident"}, {"area", "ico_row_trident"}, {"skill", "ico_row_trident"}, {"talent", "ico_row_trident"}, {"attribute", "ico_row_trident"}, {"point", "ico_row_trident"},
        {"gold", "ico_row_coins2"}, {"dust", "ico_row_coins2"}, {"echo", "ico_row_coins2"}, {"material", "ico_row_coins2"}, {"resource", "ico_row_coins2"},
        {"loot", "ico_row_magnet"}, {"drop", "ico_row_magnet"}, {"pickup", "ico_row_magnet"}, {"fragment", "ico_row_magnet"}, {"egg", "ico_row_magnet"}, {"rarity", "ico_row_magnet"}, {"divine", "ico_row_magnet"},
        {"level", "ico_row_swirl"}, {"xp", "ico_row_swirl"}, {"grid", "ico_row_swirl"}, {"city", "ico_row_swirl"}, {"upgrade", "ico_row_swirl"}, {"wyrm", "ico_row_swirl"}, {"game speed", "ico_row_swirl"}, {"potion", "ico_row_flame"},
    };
    for (auto& [k, v] : kMap) if (l.find(k) != std::string::npos) return v;
    return "";
}
static void section(const char* name) {
    ImGui::Dummy(ImVec2(0, 4 * K));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x;
    std::string main = name, note; const size_t br = main.find(" (");
    if (br != std::string::npos) { note = main.substr(br + 1); main.resize(br); }
    const std::string up = upper(main);
    const float sc = .9f * T, tw = text_w(g_font_skin, up.c_str(), sc), th = 26 * K;
    text(dl, g_font_skin, ImVec2(p.x + 8 * K, p.y), ImVec2(p.x + 8 * K + tw, p.y + th), rgb_(C("gold", 0xE9B44C)), up.c_str(), 0.f, true, sc);
    if (!note.empty()) text(dl, g_font_body, ImVec2(p.x + 18 * K + tw, p.y + 2 * K), ImVec2(p.x + w, p.y + th), rgb_(C("dim", 0xA8977A)), note.c_str(), 0.f, false, .9f);
    const float y = p.y + th + 3 * K;
    dl->AddRectFilledMultiColor(ImVec2(p.x, y), ImVec2(p.x + w * .7f, y + 1.6f * K), rgb_(C("gold", 0xE9B44C), .85f), rgb_(C("gold", 0xE9B44C), .2f), rgb_(C("gold", 0xE9B44C), .2f), rgb_(C("gold", 0xE9B44C), .85f));
    ImGui::Dummy(ImVec2(w, th + 9 * K));
}
} // namespace sk

// ---- skinned rows: the mock-up's row plate, icon, label, control on the right
static void draw_item_skin(Item& it) {
    using namespace sk;
    const ULONGLONG now = GetTickCount64();
    const bool online = lua_online();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (it.type == "label" || it.type == "note" || it.type == "text" || it.type == "info") {
        ImGui::Dummy(ImVec2(0, 2 * K));
        ImGui::Indent(14 * K); text_dim(it.text.empty() ? it.label : it.text, C("dim", 0xA8977A)); ImGui::Unindent(14 * K);
        ImGui::Dummy(ImVec2(0, 4 * K));
        return;
    }
    const bool two = it.type == "input" && text_w(g_font_skin, it.label.c_str(), .82f * T) > ImGui::GetContentRegionAvail().x * .40f;
    const float rowh = std::max(L("row_h", 62), ImGui::GetFrameHeight() + 14 * K) * (two ? 1.7f : 1.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x - 4 * K;
    const ImVec2 p1(p0.x + w, p0.y + rowh);
    const bool hov = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    draw(dl, "row", p0, p1, hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(236, 236, 236, 255));
    // icon
    {   const char* ic = row_icon(it.label);
        const float isz = 40 * K, cx = p0.x + 43 * K, cy = p0.y + (two ? rowh * .3f : rowh * .5f);
        if (*ic && has(ic)) draw(dl, ic, ImVec2(cx - isz * .5f, cy - isz * .5f), ImVec2(cx + isz * .5f, cy + isz * .5f));
        else { const float r = 6 * K; dl->AddQuadFilled(ImVec2(cx, cy - r), ImVec2(cx + r, cy), ImVec2(cx, cy + r), ImVec2(cx - r, cy), rgb_(C("gold", 0xE9B44C), .85f)); } }
    const float cr = p1.x - 22 * K;      // right edge for the control
    float ctrl_l = cr;
    const float fh = ImGui::GetFrameHeight();
    const float vbh = 47 * K;             // value box / check box height (mock-up px)
    bool lit = false;
    auto transparent_frames = [](bool push) {
        if (push) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 14));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 22)); ImGui::PushStyleColor(ImGuiCol_Text, rgb_(C("text", 0xF3E9D2)));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        } else { ImGui::PopStyleVar(); ImGui::PopStyleColor(4); }
    };

    if (it.type == "toggle") {
        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##row", ImVec2(w, rowh))) { it.on = !it.on; send_set(it, it.on); }
        const ImVec2 s = size("check_on"); const ImVec2 a(cr - s.x - 70 * K, p0.y + (rowh - s.y) * .5f);
        draw(dl, it.on ? "check_on" : "check_off", a, ImVec2(a.x + s.x, a.y + s.y));
        ctrl_l = a.x; lit = it.on;
    } else if (it.type == "button") {
        const bool arming = it.danger && now < it.confirm_until;
        const std::string cap = upper(arming ? std::string("CONFIRM?") : it.button.empty() ? std::string("GO") : it.button);
        const ImVec2 bs(std::max(203 * K, text_w(g_font_skin, cap.c_str(), .82f * T) + 50 * K), std::min(rowh - 8 * K, 49 * K));
        ctrl_l = cr - bs.x;
        ImGui::SetCursorScreenPos(ImVec2(ctrl_l, p0.y + (rowh - bs.y) * .5f));
        const bool pressed = plate_button("##b", cap.c_str(), bs, arming ? 0x5A1208 : 0x2A1606);
        if (it.danger && ImGui::IsItemHovered()) ImGui::SetTooltip(arming ? "Click again to confirm" : "Needs a second click within 4 s");
        else if (!it.desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.desc.c_str());
        if (pressed) {
            if (!it.danger) send_click(it, false);
            else if (arming) { it.confirm_until = 0; send_click(it, true); }
            else it.confirm_until = now + 4000;
        }
    } else if (it.type == "readout") {
        const std::string v = it.readout.empty() ? (online ? "-" : "--") : it.readout;
        const float vw = std::min(std::max(208 * K, text_w(g_font_body, v.c_str(), 1.1f) + 34 * K), w * .55f);
        const ImVec2 a(cr - vw, p0.y + (rowh - vbh) * .5f), b(cr, a.y + vbh);
        draw(dl, has("valbox_plain") ? "valbox_plain" : "valbox", a, b);
        dl->PushClipRect(a, b, true);
        text(dl, g_font_body, ImVec2(a.x + 12 * K, a.y), ImVec2(b.x - 12 * K, b.y), rgb_(online ? C("text", 0xF3E9D2) : C("dim", 0xA8977A)), v.c_str(), .5f, false, 1.1f);
        dl->PopClipRect();
        ctrl_l = a.x;
    } else if (it.type == "slider") {
        // the mock-up's value box: value in the middle (click to type, mouse wheel / drag to change), arrows = one step
        const double mn = it.minv, mx = it.maxv, range = std::max(1e-9, mx - mn);
        const double st = it.step > 0 ? it.step : range / 100.0;
        const std::string fmt = slider_fmt(it.step);
        const float vbw = 208 * K, arrows = 40 * K;
        const ImVec2 a(cr - vbw, p0.y + (rowh - vbh) * .5f), b(cr, a.y + vbh);
        draw(dl, "valbox", a, b); ctrl_l = a.x;
        auto setv = [&](double v) { it.val = std::clamp(v, mn, mx); it.dirty = true; it.last_edit = now; };
        ImGui::SetCursorScreenPos(ImVec2(b.x - arrows, a.y)); if (ImGui::InvisibleButton("##up", ImVec2(arrows, vbh * .5f))) { setv(it.val + st); it.last_edit = 0; }
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(b.x - arrows, a.y), ImVec2(b.x, a.y + vbh * .5f), IM_COL32(255, 220, 140, 30));
        ImGui::SetCursorScreenPos(ImVec2(b.x - arrows, a.y + vbh * .5f)); if (ImGui::InvisibleButton("##dn", ImVec2(arrows, vbh * .5f))) { setv(it.val - st); it.last_edit = 0; }
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(b.x - arrows, a.y + vbh * .5f), ImVec2(b.x, b.y), IM_COL32(255, 220, 140, 30));
        static std::string s_edit;   // the value box being typed in
        const ImVec2 va(a.x, a.y), vb2(b.x - arrows, b.y);
        transparent_frames(true);
        if (s_edit != it.id) {
            char vb[64]; sprintf_s(vb, fmt.c_str(), it.val);
            text(dl, g_font_body, va, vb2, rgb_(C("text", 0xF3E9D2)), vb, .5f, true, 1.15f);
            ImGui::SetCursorScreenPos(va);
            if (ImGui::InvisibleButton("##valb", ImVec2(vb2.x - va.x, vbh))) s_edit = it.id;
            const bool vh = ImGui::IsItemHovered();
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.f)) {   // drag left / right to scrub
                g_drag_id = it.id;
                setv(it.val + ImGui::GetIO().MouseDelta.x / std::max(1.f, vb2.x - va.x) * range);
                if (it.step > 0) it.val = mn + std::round((it.val - mn) / it.step) * it.step;
                s_edit.clear();
            }
            if (vh && ImGui::GetIO().MouseWheel != 0.f) { setv(it.val + st * (ImGui::GetIO().MouseWheel > 0 ? 1 : -1)); }
            if (vh && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { it.val = it.has_off ? it.offv : it.defv; it.dirty = true; it.last_edit = 0; }
            if (vh) ImGui::SetTooltip("%s%sClick: type a value  |  drag or wheel: change  |  right-click: back to %g%s", it.desc.c_str(), it.desc.empty() ? "" : "\n",
                                      it.has_off ? it.offv : it.defv, it.has_off ? " (off)" : "");
        } else {
            ImGui::SetCursorScreenPos(ImVec2(va.x + 10 * K, va.y + (vbh - fh) * .5f));
            ImGui::SetNextItemWidth(vb2.x - va.x - 20 * K);
            if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            double v = it.val;
            if (ImGui::InputDouble("##val", &v, 0, 0, fmt.c_str(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_AutoSelectAll)) {
                it.val = std::clamp(v, mn, mx); send_set(it, it.val); it.dirty = false; s_edit.clear(); }
            else if (ImGui::IsItemDeactivated()) s_edit.clear();
        }
        transparent_frames(false);
        if (it.dirty && (g_drag_id != it.id) && (it.last_edit == 0 || now - it.last_edit >= 250)) { send_set(it, it.val); it.dirty = false; }
        lit = it.has_off && std::fabs(it.val - it.offv) > 1e-9;
    } else if (it.type == "input") {
        const float go = 120 * K, iw = two ? cr - go - 10 * K - (p0.x + 76 * K) : std::min(380 * K, w * .45f);
        const float by = two ? p0.y + rowh * .5f : p0.y + (rowh - vbh) * .5f;
        const ImVec2 a(cr - go - 10 * K - iw, by), b(a.x + iw, a.y + vbh * (two ? .85f : 1.f));
        draw(dl, has("valbox_plain") ? "valbox_plain" : "valbox", a, b); if (!two) ctrl_l = a.x;
        transparent_frames(true);
        ImGui::SetCursorScreenPos(ImVec2(a.x + 12 * K, a.y + (b.y - a.y - fh) * .5f));
        ImGui::SetNextItemWidth(iw - 24 * K);
        const bool enter = ImGui::InputText("##t", it.input, sizeof it.input, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!it.desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.desc.c_str());
        transparent_frames(false);
        ImGui::SetCursorScreenPos(ImVec2(cr - go, by + (b.y - a.y - 44 * K) * .5f));
        if (plate_button("##go", "SET", ImVec2(go, 44 * K), 0x2A1606) || enter) send_set(it, std::string(it.input));
    }

    // label (+ a state line while sending / after a failure)
    {   const ImVec2 la(p0.x + 76 * K, p0.y), lb(two ? p1.x - 22 * K : ctrl_l - 14 * K, two ? p0.y + rowh * .5f : p1.y);
        std::string sub;
        if (it.waiting && it.type != "slider") sub = online ? "sending..." : "queued";
        else if (it.result_at && now - it.result_at < 8000 && !it.result_ok) sub = "! " + it.result;
        else if (it.result_at && now - it.result_at < 4000 && (it.type == "button" || it.type == "input")) sub = "> " + it.result;
        dl->PushClipRect(la, lb, true);
        const ImU32 lc = rgb_(lit ? C("gold_hi", 0xFFE3A0) : C("text", 0xF3E9D2));
        const float ls = .9f * T;
        if (sub.empty()) text(dl, g_font_skin, la, lb, lc, it.label.c_str(), 0.f, true, ls);
        else {
            const float mid = p0.y + (two ? rowh * .25f : rowh * .5f);
            text(dl, g_font_skin, ImVec2(la.x, p0.y + 2 * K), ImVec2(lb.x, mid + 2 * K), lc, it.label.c_str(), 0.f, true, ls * .9f);
            text(dl, g_font_body, ImVec2(la.x, mid), ImVec2(lb.x, two ? lb.y : p1.y - 4 * K), rgb_(it.result_ok ? 0x9FD86A : 0xFF6A5A), sub.c_str(), 0.f, false, .85f);
        }
        dl->PopClipRect();
        if (!it.desc.empty() && ImGui::IsMouseHoveringRect(la, lb) && it.type != "slider" && it.type != "button") ImGui::SetTooltip("%s", it.desc.c_str()); }

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + L("row_gap", 8)));
    ImGui::Dummy(ImVec2(0, 0));
}

// ------------------------------------------------------------------ skinned panel
static void keep_aspect(ImGuiSizeCallbackData* d) {
    const float r = *(float*)d->UserData; d->DesiredSize.y = std::round(d->DesiredSize.x * r);
}
static void panel_draw_skin(bool* visible, bool online, bool dev) {
    using namespace sk;
    const float rw = (float)jnum(g_lay, "ref_w", 1122), rh = (float)jnum(g_lay, "ref_h", 1402);
    static float aspect; aspect = rh / rw;
    {   const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const float w0 = std::min(ds.x * .6f, (ds.y * .93f) / aspect) * std::clamp(g_settings.ui_scale, .75f, 1.f);   // most of the screen height
        ImGui::SetNextWindowSize(ImVec2(w0, w0 * aspect), ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImVec2(S(40), S(20)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(rw * .45f, rh * .45f), ImVec2(rw * 1.4f, rh * 1.4f), keep_aspect, &aspect); }
    ui::theme_push();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushFont(g_font_body);
    const bool open = ImGui::Begin("##dragonkin_skin2", visible, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
                                                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (!open) { ImGui::End(); ImGui::PopFont(); ui::theme_pop(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    K = ws.x / rw; g_org = wp;
    T = std::clamp(K / .78f, .7f, 1.6f);
    draw(dl, "bg", wp, ImVec2(wp.x + ws.x, wp.y + ws.y));

    // pages: spec tabs, EDITOR, TALENTS, BRIDGE (developer mode), SETTINGS
    const int ntab = (int)g_tabs.size(), kEditor = ntab, kBridge = ntab + 1, kSettings = ntab + 2, kTalents = ntab + 3;
    std::vector<int> pages;
    for (int i = 0; i < ntab; ++i) pages.push_back(i);
    pages.push_back(kEditor); pages.push_back(kTalents);
    if (dev) pages.push_back(kBridge);
    pages.push_back(kSettings);
    if (g_tab == kBridge && !dev) g_tab = kSettings;
    if (g_tab < 0 || g_tab > kTalents) g_tab = 0;
    if (g_capturing && g_tab != kSettings) capture_cancel("Cancelled - hotkey unchanged");
    capture_poll();
    if (!ImGui::IsAnyItemActive()) {
        const int n = (int)pages.size(); int cur = 0; for (int i = 0; i < n; ++i) if (pages[i] == g_tab) cur = i;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_tab = pages[(cur + n - 1) % n];
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_tab = pages[(cur + 1) % n];
    }
    auto page_name = [&](int i) -> std::string { return i < ntab ? g_tabs[i].name : i == kEditor ? "Editor" : i == kTalents ? "Talents" : i == kBridge ? "Bridge" : "Settings"; };

    // window buttons baked into the art: minimise / close = hide the panel; anywhere else on the frame drags the window
    if (auto wb = g_lay.find("winbtns"); wb != g_lay.end() && wb->is_array()) {
        for (int i = 0; i < (int)wb->size() && i < 2; ++i) {
            const auto& r = (*wb)[i]; const ImVec2 a(wp.x + r[0].get<float>() * K, wp.y + r[1].get<float>() * K), b(wp.x + r[2].get<float>() * K, wp.y + r[3].get<float>() * K);
            ImGui::SetCursorScreenPos(a); ImGui::PushID(i);
            if (ImGui::InvisibleButton("##wb", ImVec2(b.x - a.x, b.y - a.y))) *visible = false;
            if (ImGui::IsItemHovered()) { dl->AddRectFilled(a, b, IM_COL32(255, 200, 120, 45), 6 * K); ImGui::SetTooltip("Hide (%s)", hotkey_name(g_settings.hotkey).c_str()); }
            ImGui::PopID();
        }
    }

    // tabs (the step shrinks so every page fits above the corner dragon)
    {   const float x = wp.x + L("tabs_x", 40), y0 = wp.y + L("tabs_y", 398), y1 = wp.y + L("tabs_bottom", 968);
        const int n = (int)pages.size();
        const float step = std::min(L("tab_step", 71.4f), (y1 - y0) / std::max(1, n));
        const float tw = L("tab_w", 232), th = std::min(L("tab_h", 65), step - 3 * K);
        const float fit = th / std::max(1.f, L("tab_h", 65));     // smaller tabs -> smaller icon and text
        for (int k = 0; k < n; ++k) {
            const int i = pages[k]; const bool sel = g_tab == i;
            const std::string name = page_name(i);
            const ImVec2 a(x, y0 + k * step), b(x + tw, a.y + th);
            ImGui::SetCursorScreenPos(a); ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ImVec2(tw, th))) g_tab = i;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            draw(dl, sel ? "tab_active" : "tab", a, b, hov && !sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(236, 236, 236, 255));
            std::string l = name; std::transform(l.begin(), l.end(), l.begin(), ::tolower);
            static const std::vector<std::pair<const char*, const char*>> kIco = {
                {"player", "ico_helmet"}, {"progress", "ico_triquetra"}, {"resource", "ico_coins"}, {"loot", "ico_chest"}, {"combat", "ico_swords"},
                {"wyrm", "ico_wing"}, {"city", "ico_globe"}, {"world", "ico_globe"}, {"trainer", "ico_gear"}, {"editor", "ico_chest"},
                {"talent", "ico_triquetra"}, {"bridge", "ico_gear"}, {"setting", "ico_gear"} };
            const char* ic = ""; for (auto& [kk, v] : kIco) if (l.find(kk) != std::string::npos) { ic = v; break; }
            const float isz = th * .86f, ix = a.x + 21 * K * fit;
            if (*ic) draw(dl, ic, ImVec2(ix, a.y + (th - isz) * .5f), ImVec2(ix + isz, a.y + (th + isz) * .5f));
            text(dl, g_font_skin, ImVec2(a.x + 90 * K * std::max(.75f, fit), a.y), ImVec2(b.x - 10 * K, b.y),
                 rgb_(sel ? 0xFFF4DC : hov ? C("gold_hi", 0xFFE3A0) : C("text", 0xF3E9D2)), upper(name).c_str(), 0.f, true, .9f * T * std::max(.72f, fit));
        }
    }

    // page title in the header plate
    {   const ImVec4 h = box("header");
        const std::string title = upper(page_name(g_tab));
        float sc = .95f * T; const float room = (h.z - h.x);
        const float tw = text_w(g_font_skin_big, title.c_str(), sc); if (tw > room) sc *= room / tw;
        text(dl, g_font_skin_big, ImVec2(h.x, h.y), ImVec2(h.z, h.w), rgb_(C("title", 0xF2C46A)), title.c_str(), 0.f, true, sc); }

    // search, right of the title plate (outside the scrolling content, so it is not clipped)
    if (g_tab < ntab) {
        const ImVec4 h = box("header"), c = box("content"); const float sw = 250 * K;
        ImGui::SetCursorScreenPos(ImVec2(c.z - sw - 6 * K, h.y + (h.w - h.y - ImGui::GetFrameHeight()) * .5f));
        ImGui::SetNextItemWidth(sw);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(20, 16, 14, 225)); ImGui::PushStyleColor(ImGuiCol_Border, rgb_(C("gold", 0xE9B44C), .45f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof g_search);
        ImGui::PopStyleVar(); ImGui::PopStyleColor(2);
    }

    // content (scrolls inside the content box)
    {   const ImVec4 c = box("content");
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 50)); ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, rgb_(C("gold", 0xE9B44C), .45f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, rgb_(C("gold", 0xE9B44C), .75f)); ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, rgb_(C("gold", 0xE9B44C)));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10 * K);
        ImGui::BeginChild("##skc", ImVec2(c.z - c.x, c.w - c.y), 0, ImGuiWindowFlags_NoBackground);
        if (g_tab < ntab) {
            if (!online) { ImGui::Dummy(ImVec2(0, 2 * K)); ImGui::Indent(10 * K); text_dim(LINK_OFF " - load into a level with your character.", ui::kAmber); ImGui::Unindent(10 * K); }
            for (auto& s : g_tabs[g_tab].sections) {
                bool any = false; for (auto& it : s.items) if (matches(it)) { any = true; break; }
                if (!any) continue;
                if (!s.name.empty()) section(s.name.c_str());
                for (auto& it : s.items) if (matches(it)) { ImGui::PushID(it.id.c_str()); draw_item_skin(it); ImGui::PopID(); }
            }
            if (g_tabs.empty()) text_dim(g_load_msg, ui::kAmber);
        } else {
            ImGui::Indent(10 * K); ImGui::Dummy(ImVec2(0, 4 * K));
            if (g_tab == kEditor) ed::draw_editor(online);
            else if (g_tab == kTalents) ed::draw_talents(online);
            else draw_settings_tab();
            ImGui::Unindent(10 * K);
        }
        ImGui::Dummy(ImVec2(0, 10 * K));
        ImGui::EndChild();
        ImGui::PopStyleVar(); ImGui::PopStyleColor(4); }

    // status (state + last action) and version, both in the bottom band
    {   const ImVec4 s = box("status");
        const float cy = (s.y + s.w) * .5f, r = 17 * K, cx = s.x + 8 * K + r;
        const ImU32 gc = rgb_(online ? C("green", 0x5BE33A) : 0x9A9A9A);
        dl->AddNgon(ImVec2(cx, cy), r, gc, 8, 3.f * K);
        if (online) dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(cx - 8 * K, cy), ImVec2(cx - 2 * K, cy + 7 * K), ImVec2(cx + 9 * K, cy - 7 * K) }.data(), 3, gc, 0, 3.4f * K);
        else dl->AddLine(ImVec2(cx - 8 * K, cy), ImVec2(cx + 8 * K, cy), gc, 3.f * K);
        // 11-13: the last action's message ("Infinite rage on", errors) takes the whole plate for a few seconds instead of
        // squeezing in after TRAINER ACTIVE (it was cut to "Infinite ra"); then the plate shows the state again.
        const uint64_t age = GetTickCount64() - g_status_at;
        const bool msg = !g_status.empty() && age < (g_status_ok ? 4000u : 15000u);
        const ImVec2 ta(cx + r + 14 * K, s.y), tz(s.z, s.w);
        if (msg) {
            dl->PushClipRect(ta, tz, true);
            text(dl, g_font_skin, ta, tz, rgb_(g_status_ok ? 0xF2E6C8 : 0xFF6A5A), g_status.c_str(), 0.f, true, 1.0f * T);
            dl->PopClipRect();
        } else {
            const char* t = online ? "TRAINER ACTIVE" : "WAITING FOR GAME";
            const float ts = 1.0f * T, tw = text_w(g_font_skin_big, t, ts);
            text(dl, g_font_skin_big, ta, ImVec2(ta.x + tw, s.w), gc, t, 0.f, true, ts);
        } }
    {   const ImVec4 v = box("version");
        text(dl, g_font_skin_big, ImVec2(v.x, v.y), ImVec2(v.z, v.w), rgb_(C("title", 0xF2C46A)), (std::string("v") + kVersion).c_str(), .5f, true, .85f * T); }

    ImGui::End();
    ImGui::PopFont();
    ui::theme_pop();
}
