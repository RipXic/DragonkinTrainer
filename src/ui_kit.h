// DragonkinTheBanished Bridge - UI kit (2026-11-04 rework: the game's look - ink, bone text, gold accents, corner-tick frames; shared
// with the Transmog page). Widgets: dark cards, gold accent, status pills, underlined tabs, chips, steppers,
// filled primary buttons. Header-only; render thread only. Used by panel.cpp (via port patch), items_tab, skills_tab.
#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>

namespace wb { extern float g_ui_scale; extern ImFont* g_font_bold; extern ImFont* g_font_title; }

namespace wb::ui {

inline float S(float v) { return v * g_ui_scale; }
inline ImU32 rgb(uint32_t c, float a = 1.f) { return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (int)(a * 255)); }
inline ImVec4 rgbv(uint32_t c, float a = 1.f) { return ImVec4(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, a); }

// palette (2026-11-04 UI rework): the game's own look, shared with the Transmog page - near-black ink, warm dark cards,
// bone text, thin gold accents. The old names are kept so every tab recolors through this one table.
constexpr uint32_t kInk = 0x07080B, kGold = 0xC8A464, kGoldHi = 0xE8CC8C, kBone = 0xD8D0BC, kGoldDeep = 0x3A2E14;
constexpr uint32_t kBg = 0x08090C, kPanel = 0x0E0F12, kCard = 0x15161A, kCardHi = 0x202127, kLine = 0x3A3628,
                   kText = kBone, kDim = 0x8C8676, kAccent = kGold, kAccentHi = kGoldHi, kAccentDim = kGoldDeep,
                   kGreen = 0x7FB069, kAmber = 0xE0A84A, kCyan = 0x9DB8B0, kBlue = 0x7C9CC4, kRed = 0xC0463C;

// Gold selection frame with the game's corner ticks (also used by the Transmog page tiles).
inline void gold_frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float t = 1.5f, float alpha = 1.f) {
    dl->AddRect(a, b, rgb(kGold, alpha), 0, 0, t);
    const float c = S(7);
    dl->AddLine(a, ImVec2(a.x + c, a.y), rgb(kGoldHi, alpha), t + 1); dl->AddLine(a, ImVec2(a.x, a.y + c), rgb(kGoldHi, alpha), t + 1);
    dl->AddLine(b, ImVec2(b.x - c, b.y), rgb(kGoldHi, alpha), t + 1); dl->AddLine(b, ImVec2(b.x, b.y - c), rgb(kGoldHi, alpha), t + 1);
}
// Tile background: the dark vertical gradient + thin warm border every icon tile uses.
inline void tile_bg(ImDrawList* dl, ImVec2 p, ImVec2 q, bool hov) {
    dl->AddRectFilledMultiColor(p, q, rgb(0x1A1C22, .95f), rgb(0x1A1C22, .95f), rgb(0x0C0D10, .95f), rgb(0x0C0D10, .95f));
    dl->AddRect(p, q, rgb(kLine, hov ? 1.f : .8f), 0, 0, 1.f);
}
// Page / section title: big bone caps + a gold rule (bright first part, faint rest).
inline void page_title(const char* text, const char* sub = nullptr) {
    if (g_font_title) ImGui::PushFont(g_font_title);
    ImGui::PushStyleColor(ImGuiCol_Text, rgbv(kBone)); ImGui::TextUnformatted(text); ImGui::PopStyleColor();
    if (g_font_title) ImGui::PopFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(c, ImVec2(c.x + std::min(S(150), w), c.y), rgb(kGold), S(1.5f));
    dl->AddLine(ImVec2(c.x + std::min(S(150), w), c.y), ImVec2(c.x + w, c.y), rgb(kGold, .25f), 1.f);
    ImGui::Dummy(ImVec2(0, S(4)));
    if (sub) { ImGui::PushStyleColor(ImGuiCol_Text, rgbv(kDim)); ImGui::TextUnformatted(sub); ImGui::PopStyleColor(); }
}
// Faint full-width gold rule.
inline void gold_rule(float alpha = .25f) {
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(c, ImVec2(c.x + ImGui::GetContentRegionAvail().x, c.y), rgb(kGold, alpha), 1.f);
    ImGui::Dummy(ImVec2(0, S(3)));
}

// Theme: pushed for the panel window, popped after. Count must match theme_pop.
constexpr int kThemeColors = 26, kThemeVars = 10;
inline void theme_push() {
    using namespace ImGui;
    PushStyleColor(ImGuiCol_WindowBg, rgbv(kInk, 0.90f));    PushStyleColor(ImGuiCol_ChildBg, rgbv(kPanel, 0.55f));
    PushStyleColor(ImGuiCol_PopupBg, rgbv(0x0E0F12, 0.98f)); PushStyleColor(ImGuiCol_Border, rgbv(kLine, 0.9f));
    PushStyleColor(ImGuiCol_Text, rgbv(kText));              PushStyleColor(ImGuiCol_TextDisabled, rgbv(kDim));
    PushStyleColor(ImGuiCol_FrameBg, rgbv(kCard, .9f));      PushStyleColor(ImGuiCol_FrameBgHovered, rgbv(kCardHi, .9f));
    PushStyleColor(ImGuiCol_FrameBgActive, rgbv(0x2A2618));  PushStyleColor(ImGuiCol_Button, rgbv(kCard, .9f));
    PushStyleColor(ImGuiCol_ButtonHovered, rgbv(0x2A2618, .95f)); PushStyleColor(ImGuiCol_ButtonActive, rgbv(kGoldDeep));
    PushStyleColor(ImGuiCol_Header, rgbv(kGold, 0.18f));     PushStyleColor(ImGuiCol_HeaderHovered, rgbv(kGold, 0.28f));
    PushStyleColor(ImGuiCol_HeaderActive, rgbv(kGold, 0.35f)); PushStyleColor(ImGuiCol_CheckMark, rgbv(kGoldHi));
    PushStyleColor(ImGuiCol_SliderGrab, rgbv(kGold));        PushStyleColor(ImGuiCol_SliderGrabActive, rgbv(kGoldHi));
    PushStyleColor(ImGuiCol_Separator, rgbv(kGold, .25f));   PushStyleColor(ImGuiCol_ScrollbarBg, rgbv(kBg, 0.f));
    PushStyleColor(ImGuiCol_ScrollbarGrab, rgbv(kLine));     PushStyleColor(ImGuiCol_ScrollbarGrabHovered, rgbv(kGold, .7f));
    PushStyleColor(ImGuiCol_ScrollbarGrabActive, rgbv(kGoldHi)); PushStyleColor(ImGuiCol_TableRowBgAlt, rgbv(0xFFFFFF, 0.02f));
    PushStyleColor(ImGuiCol_NavHighlight, rgbv(kGold));      PushStyleColor(ImGuiCol_TextSelectedBg, rgbv(kGold, 0.35f));
    PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);         PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    PushStyleVar(ImGuiStyleVar_FrameRounding, S(2));         PushStyleVar(ImGuiStyleVar_PopupRounding, 0.f);
    PushStyleVar(ImGuiStyleVar_GrabRounding, S(2));          PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);        PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(11), S(7)));
    PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(9), S(8))); PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(7));
}
inline void theme_pop() { ImGui::PopStyleVar(kThemeVars); ImGui::PopStyleColor(kThemeColors); }
// Two-tone brand title: "DRAGONKINTHEBANISHED" white + "BRIDGE" accent.
inline void brand(const char* a, const char* b, const char* sub) {
    if (sub) { ImGui::PushStyleColor(ImGuiCol_Text, rgbv(kDim)); ImGui::TextUnformatted(sub); ImGui::PopStyleColor(); }
    if (g_font_title) ImGui::PushFont(g_font_title);
    ImGui::TextUnformatted(a); ImGui::SameLine(0, S(6));
    ImGui::PushStyleColor(ImGuiCol_Text, rgbv(kAccent)); ImGui::TextUnformatted(b); ImGui::PopStyleColor();
    if (g_font_title) ImGui::PopFont();
}

// Status pill: [dot] label    (rounded outline, colored left marker)
inline void pill(const char* label, uint32_t dot, float w = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float h = ImGui::GetFrameHeight(), pw = w > 0 ? w : ts.x + S(34);
    const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + pw, p.y + h);
    dl->AddRectFilled(p, q, rgb(kCard, .85f), S(2));
    dl->AddRect(p, q, rgb(dot, 0.45f), S(2), 0, 1.f);
    dl->AddRectFilled(ImVec2(q.x - S(4), p.y + S(5)), ImVec2(q.x - S(1.5f), q.y - S(5)), rgb(dot), S(2));
    dl->AddCircleFilled(ImVec2(p.x + S(13), p.y + h * 0.5f), S(4), rgb(dot));
    dl->AddText(ImVec2(p.x + S(24), p.y + (h - ts.y) * 0.5f), rgb(kText), label);
    ImGui::Dummy(ImVec2(pw, h));
}

// Underlined top tabs. Returns true when the selection changed.
inline bool tabs(const char* const* names, int n, int* sel) {
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight() + S(6);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine(0, S(4));
        ImGui::PushID(i);
        const ImVec2 ts = ImGui::CalcTextSize(names[i]);
        const float w = ts.x + S(26);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tab", ImVec2(w, h))) { *sel = i; changed = true; }
        const bool hov = ImGui::IsItemHovered(), on = *sel == i;
        if (on) dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), rgb(kGold, 0.f), rgb(kGold, 0.f), rgb(kGold, 0.14f), rgb(kGold, 0.14f));
        else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(0xFFFFFF, 0.04f));
        dl->AddText(ImVec2(p.x + S(13), p.y + (h - ts.y) * 0.5f), on ? rgb(kGoldHi) : hov ? rgb(kText) : rgb(kDim), names[i]);
        if (on) dl->AddRectFilled(ImVec2(p.x + S(6), p.y + h - S(2)), ImVec2(p.x + w - S(6), p.y + h), rgb(kGold));
        ImGui::PopID();
    }
    dl->AddLine(ImVec2(start.x, start.y + h), ImVec2(start.x + full, start.y + h), rgb(kGold, .25f));
    ImGui::Dummy(ImVec2(0, S(4)));
    return changed;
}

// Rounded chip (category filter). Returns true when clicked.
inline bool chip(const char* label, bool on) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float h = ImGui::GetFrameHeight() - S(2), w = ts.x + S(22);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), on ? rgb(kGold, .14f) : hov ? rgb(kCardHi, .9f) : rgb(kCard, .85f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), on ? rgb(kGold, .55f) : rgb(kLine, .8f));
    if (on) dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + w, p.y + h - 1), rgb(kGold), S(1.5f));
    dl->AddText(ImVec2(p.x + S(11), p.y + (h - ts.y) * 0.5f), on ? rgb(kGoldHi) : hov ? rgb(kText) : rgb(kDim), label);
    return clicked;
}
// Lay out chips left to right, wrapping at the region edge. Call chip_wrap() before each chip after the first.
inline void chip_wrap(const char* next_label) {
    const float w = ImGui::CalcTextSize(next_label).x + S(22);
    const float right = ImGui::GetItemRectMax().x + S(6) + w;
    const float edge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (right < edge) ImGui::SameLine(0, S(6));
}

// Filled buttons
inline bool button_fill(const char* label, uint32_t col, uint32_t col_hi, float w = -1, float h = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, rgbv(col)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgbv(col_hi));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgbv(col)); ImGui::PushStyleColor(ImGuiCol_Text, rgbv(kGoldHi));
    const bool r = ImGui::Button(label, ImVec2(w, h > 0 ? h : ImGui::GetFrameHeight() + S(6)));
    ImGui::PopStyleColor(4);
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rgb(kGold, ImGui::IsItemHovered() ? .7f : .35f), S(2));
    return r;
}
inline bool primary(const char* label, float w = -1, float h = 0) { return button_fill(label, 0x3A2E14, 0x5A4720, w, h); }
inline bool danger(const char* label, float w = -1, float h = 0) { return button_fill(label, 0x4A1A16, 0x6A241E, w, h); }
inline bool ghost(const char* label, float w = 0, float h = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, rgbv(kCard)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgbv(kCardHi));
    const bool r = ImGui::Button(label, ImVec2(w, h)); ImGui::PopStyleColor(2); return r;
}

// - [ value ] +   stepper. Returns true on change.
inline bool stepper(const char* id, long long* v, long long mn, long long mx, float w = -1) {
    ImGui::PushID(id);
    const float bw = ImGui::GetFrameHeight();
    const float total = w > 0 ? w : ImGui::GetContentRegionAvail().x;
    bool ch = false;
    if (ghost("-", bw, 0)) { *v = std::max(mn, *v - 1); ch = true; }
    ImGui::SameLine(0, S(4)); ImGui::SetNextItemWidth(total - 2 * bw - S(8));
    if (ImGui::InputScalar("##v", ImGuiDataType_S64, v, nullptr, nullptr, "%lld")) { *v = std::clamp(*v, mn, mx); ch = true; }
    ImGui::SameLine(0, S(4));
    if (ghost("+", bw, 0)) { *v = std::min(mx, *v + 1); ch = true; }
    ImGui::PopID();
    return ch;
}

// Small caps label in the accent color (section label inside a card)
inline void label(const char* text, uint32_t col = kDim) {
    ImGui::PushStyleColor(ImGuiCol_Text, rgbv(col));
    std::string up = text; std::transform(up.begin(), up.end(), up.begin(), ::toupper);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
}

// Card: a rounded child region with the card color.
inline bool card_begin(const char* id, ImVec2 size, bool border = true) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgbv(kPanel, 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(10)));
    return ImGui::BeginChild(id, size, border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None);
}
inline void card_end() { ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(); }

// Sidebar entry (storage list / category list): full-width row, accent fill when selected.
inline bool side_item(const char* label, bool on, const char* right = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight() + S(2), w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (on) { dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), rgb(kGold, .20f), rgb(kGold, .02f), rgb(kGold, .02f), rgb(kGold, .20f));
              dl->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), rgb(kGold)); }
    else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(0xFFFFFF, .04f));
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->PushClipRect(p, ImVec2(p.x + w - S(4), p.y + h), true);
    dl->AddText(ImVec2(p.x + S(12), p.y + (h - ts.y) * 0.5f), on ? rgb(kGoldHi) : hov ? rgb(kText) : rgb(kText, .85f), label);
    if (right) { const ImVec2 rs = ImGui::CalcTextSize(right); dl->AddText(ImVec2(p.x + w - rs.x - S(10), p.y + (h - rs.y) * 0.5f), on ? rgb(kGoldHi, 0.8f) : rgb(kDim), right); }
    dl->PopClipRect();
    return clicked;
}

}  // namespace wb::ui
