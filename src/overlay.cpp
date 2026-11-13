// DragonkinTheBanished Bridge - overlay layer on top of Trinity's DX12 hook (dx12_hook.cpp).
// Owns: menu toggle, Wildframe theme + fonts, input capture while the panel is open
// (window messages, raw input, cursor clip/recenter), toasts, and the trinity:: glue symbols.
#include "common.h"
#include "trinity_glue.h"
#include "dx12_hook.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"
#include "host_api.h"
#include "trinity/gui/icons.h"
#include "trinity/hooks/xinput_hook.h"
#include <deque>
#ifndef QWORD
typedef unsigned __int64 QWORD;
#endif
#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// fonts/scale shared with panel.cpp (Wolcen panel engine)
namespace wb {
ImFont* g_font_body = nullptr;
ImFont* g_font_bold = nullptr;
ImFont* g_font_title = nullptr;
ImFont* g_font_skin = nullptr;      // painted skin: tabs / headings / row labels (<root>\skin\font.ttf, else Palatino Bold)
ImFont* g_font_skin_big = nullptr;  // painted skin: page title / version
float g_ui_scale = 1.f;
}

// ------------------------------------------------------------------ trinity glue
namespace trinity {
static State& g_state = State::Get();
namespace ui { bool g_needFontRebuild = false; }
}

namespace wb {

static std::atomic<bool> g_hooks_ok{false};
static std::atomic<uint64_t> g_frames{0};
static HWND g_hwnd = nullptr;
static WNDPROC o_WndProc = nullptr;

static const HostApi* g_host = nullptr;               // set when this copy is a client of an earlier copy
const HostApi* host_api() { return g_host; }
const HostIcons* host_icons() { return g_host ? &g_host->icons : nullptr; }

bool overlay_visible() { return g_host ? g_host->visible() : trinity::g_state.menuOpen; }
void overlay_set_visible(bool v) { if (g_host) g_host->set_visible(v); else trinity::g_state.menuOpen = v; }
// 11-06: camera drag while the transmog page is open. The page's right edge lives in a small named mapping so a hot-reloaded
// copy (which only draws) can tell the host copy (which owns the input hooks) where the page ends.
struct InputShare { volatile LONG edge; volatile LONG reserved[7]; };
static InputShare* input_share() {
    static InputShare* p = nullptr;
    if (p) return p;
    char n[64]; sprintf_s(n, "Local\\nativeInput_%lu", GetCurrentProcessId());
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(InputShare), n);   // opens it if it exists
    if (!m) return nullptr;
    p = (InputShare*)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, sizeof(InputShare));   // handle + view kept for the session
    return p;
}
// 11-06 11:05: edge + a heartbeat (reserved[0] = tick). The host honours the edge only while the page keeps redrawing it
// (< 300 ms old), so a page left 'open' by an unloaded copy or a stale value can never steal the mouse from the trainer.
void overlay_set_page_edge(int px) {
    if (InputShare* p = input_share()) { InterlockedExchange(&p->edge, px > 0 ? px : 0); InterlockedExchange(&p->reserved[0], (LONG)GetTickCount()); }
}
// 2026-11-09 settings: reserved[1] = panel hotkey (hotkey_pack, 0 = never published), reserved[2] = tick of the last
// "waiting for a key" ping from the SETTINGS page. The overlay-owning copy polls the key, so a hot-reloaded copy that
// saves a new hotkey reaches it through here (an overlay from a build older than this keeps INSERT until a game restart).
void overlay_publish_hotkey(int packed) {
    if (InputShare* p = input_share()) InterlockedExchange(&p->reserved[1], (LONG)packed);
}
void overlay_hotkey_capture_ping() {
    if (InputShare* p = input_share()) InterlockedExchange(&p->reserved[2], (LONG)GetTickCount());
}
static Hotkey current_hotkey() {
    InputShare* p = input_share();
    const LONG v = p ? p->reserved[1] : 0;
    return (v & 0x8000) ? hotkey_unpack((int)v) : g_settings.hotkey;
}
static bool capture_active() {
    InputShare* p = input_share(); if (!p) return false;
    const LONG t = p->reserved[2]; if (!t) return false;
    return GetTickCount() - (DWORD)t < 400;
}
static LONG page_edge() {
    InputShare* p = input_share(); if (!p) return 0;
    const LONG e = p->edge; if (e <= 0) return 0;
    const DWORD age = GetTickCount() - (DWORD)p->reserved[0];
    return age < 300 ? e : 0;
}
static bool g_camDrag = false;                        // host copy: a mouse button went down right of the page -> movement to the game
static POINT g_camDragPt{};
static bool cam_drag() { return g_camDrag; }
static bool input_blocked() { return trinity::g_state.menuOpen && g_cfg.lock_input && !g_retired && !g_camDrag; }

// ------------------------------------------------------------------ theme
static ImVec4 hex(uint32_t rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

static void apply_wildframe_theme(float k) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 8; s.ChildRounding = 6; s.FrameRounding = 5; s.PopupRounding = 6;
    s.GrabRounding = 5; s.TabRounding = 5; s.ScrollbarRounding = 6;
    s.WindowBorderSize = 1; s.FrameBorderSize = 1; s.ChildBorderSize = 1;
    s.WindowPadding = ImVec2(14, 12); s.FramePadding = ImVec2(9, 5); s.ItemSpacing = ImVec2(9, 7);
    s.ScrollbarSize = 10; s.GrabMinSize = 12;
    ImVec4* c = s.Colors;
    const uint32_t bg = 0x0f0d0e, panel = 0x1a1618, line = 0x2c2427, fg = 0xebe5e5, dim = 0xa99c9e, accent = 0xc21d26;
    c[ImGuiCol_Text] = hex(fg); c[ImGuiCol_TextDisabled] = hex(dim);
    c[ImGuiCol_WindowBg] = hex(bg, 0.97f); c[ImGuiCol_ChildBg] = hex(panel, 0.85f); c[ImGuiCol_PopupBg] = hex(panel, 0.98f);
    c[ImGuiCol_Border] = hex(line); c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = hex(0x231d20); c[ImGuiCol_FrameBgHovered] = hex(0x2f2629); c[ImGuiCol_FrameBgActive] = hex(0x3a2d31);
    c[ImGuiCol_TitleBg] = hex(bg); c[ImGuiCol_TitleBgActive] = hex(panel); c[ImGuiCol_TitleBgCollapsed] = hex(bg);
    c[ImGuiCol_ScrollbarBg] = hex(bg, 0.f); c[ImGuiCol_ScrollbarGrab] = hex(line); c[ImGuiCol_ScrollbarGrabHovered] = hex(0x45383c); c[ImGuiCol_ScrollbarGrabActive] = hex(accent);
    c[ImGuiCol_CheckMark] = hex(accent); c[ImGuiCol_SliderGrab] = hex(accent); c[ImGuiCol_SliderGrabActive] = hex(0xe0303a);
    c[ImGuiCol_Button] = hex(0x2a2225); c[ImGuiCol_ButtonHovered] = hex(0x8f161d); c[ImGuiCol_ButtonActive] = hex(accent);
    c[ImGuiCol_Header] = hex(0x2a2225); c[ImGuiCol_HeaderHovered] = hex(0x3a1c20); c[ImGuiCol_HeaderActive] = hex(0x5a1a20);
    c[ImGuiCol_Separator] = hex(line); c[ImGuiCol_SeparatorHovered] = hex(accent); c[ImGuiCol_SeparatorActive] = hex(accent);
    c[ImGuiCol_ResizeGrip] = hex(line); c[ImGuiCol_ResizeGripHovered] = hex(accent); c[ImGuiCol_ResizeGripActive] = hex(accent);
    c[ImGuiCol_Tab] = hex(panel); c[ImGuiCol_TabHovered] = hex(0x5a1a20); c[ImGuiCol_TabActive] = hex(0x7a161c);
    c[ImGuiCol_TextSelectedBg] = hex(accent, 0.45f); c[ImGuiCol_NavHighlight] = hex(accent);
    c[ImGuiCol_PlotHistogram] = hex(accent); c[ImGuiCol_PlotLines] = hex(accent);
    s.ScaleAllSizes(k);
}

static void load_fonts(float k) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // layout lives in panel.json
    io.Fonts->Clear();
    g_font_body = g_font_bold = g_font_title = g_font_skin = g_font_skin_big = nullptr;
    char win[MAX_PATH]; GetWindowsDirectoryA(win, MAX_PATH);
    std::string f = std::string(win) + "\\Fonts\\";
    auto exists = [](const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    if (exists(f + "segoeui.ttf")) g_font_body = io.Fonts->AddFontFromFileTTF((f + "segoeui.ttf").c_str(), 17.f * k);
    if (exists(f + "seguisb.ttf")) {
        g_font_bold = io.Fonts->AddFontFromFileTTF((f + "seguisb.ttf").c_str(), 16.f * k);
        g_font_title = io.Fonts->AddFontFromFileTTF((f + "seguisb.ttf").c_str(), 24.f * k);
    }
    {   // skin fonts: <root>\skin\font.ttf when present, else Palatino Bold (closest Windows font to the mock-up's serif capitals)
        std::string sk = g_cfg.root + "\\skin\\font.ttf";
        if (!exists(sk)) sk = f + "palab.ttf";
        if (!exists(sk)) sk = f + "georgiab.ttf";
        if (exists(sk)) { g_font_skin = io.Fonts->AddFontFromFileTTF(sk.c_str(), 22.f * k); g_font_skin_big = io.Fonts->AddFontFromFileTTF(sk.c_str(), 30.f * k); }
    }
    if (!g_font_body) g_font_body = io.Fonts->AddFontDefault();
    if (!g_font_bold) g_font_bold = g_font_body;
    if (!g_font_title) g_font_title = g_font_bold;
    if (!g_font_skin) g_font_skin = g_font_bold;
    if (!g_font_skin_big) g_font_skin_big = g_font_title;
}

// ------------------------------------------------------------------ toasts
struct Toast { std::string text; ULONGLONG until; };
static std::mutex g_toast_mtx;
static std::deque<Toast> g_toasts;

void toast(const std::string& text) {
    if (g_host) { g_host->toast(text.c_str()); log("[toast] %s", text.c_str()); return; }
    std::lock_guard<std::mutex> lk(g_toast_mtx);
    g_toasts.push_back({ text, GetTickCount64() + 5000 });
    while (g_toasts.size() > 4) g_toasts.pop_front();
    log("[toast] %s", text.c_str());
}

static bool toasts_live() {
    std::lock_guard<std::mutex> lk(g_toast_mtx);
    ULONGLONG now = GetTickCount64();
    while (!g_toasts.empty() && g_toasts.front().until < now) g_toasts.pop_front();
    return !g_toasts.empty();
}

static void draw_toasts() {
    std::vector<Toast> list;
    { std::lock_guard<std::mutex> lk(g_toast_mtx); list.assign(g_toasts.begin(), g_toasts.end()); }
    if (list.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    float y = io.DisplaySize.y * 0.08f;
    int n = 0;
    for (auto& t : list) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, y), ImGuiCond_Always, ImVec2(0.5f, 0.f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        char id[32]; sprintf_s(id, "##toast%d", n++);
        ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                  ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0xebe5e5));
        ImGui::TextUnformatted(t.text.c_str());
        ImGui::PopStyleColor();
        y += ImGui::GetWindowHeight() + 6;
        ImGui::End();
    }
}

// ------------------------------------------------------------------ input capture
static BOOL(WINAPI* g_scp)(int, int) = nullptr;   // the real SetCursorPos (set when the cursor hook is installed)
static void cam_drag_end(HWND h) {
    if (!g_camDrag) return;
    g_camDrag = false;
    if (GetCapture() == h) ReleaseCapture();
    if (g_scp) g_scp(g_camDragPt.x, g_camDragPt.y); else SetCursorPos(g_camDragPt.x, g_camDragPt.y);   // cursor back where the drag began
}
static LRESULT CALLBACK hk_WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // the game is closing: stop every game call from the trainer (a call during engine shutdown can request an asset
    // load, which is fatal after shutdown - crash at exit, 2026-11-09)
    if (m == WM_CLOSE || m == WM_DESTROY || m == WM_QUIT || m == WM_ENDSESSION) g_game_exiting = true;
    if (g_retired || !trinity::g_state.menuOpen) { if (g_camDrag) cam_drag_end(h); return CallWindowProcW(o_WndProc, h, m, w, l); }
    // 11-06 camera drag: a left/right press RIGHT of the transmog page starts it; the press itself never reaches the game (no
    // attack), raw mouse movement does (the game turns the camera) until the button is released.
    if (g_camDrag) {
        const LONG edge = page_edge();
        if (m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_CAPTURECHANGED || m == WM_ACTIVATEAPP || m == WM_KILLFOCUS || edge <= 0) {
            cam_drag_end(h);
            if (m == WM_LBUTTONUP || m == WM_RBUTTONUP) return 0;
        } else if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) return 0;            // no hover / clicks for ImGui or the game while turning
        else if (m == WM_INPUT) return CallWindowProcW(o_WndProc, h, m, w, l);  // raw movement -> game (presses stripped in the raw hooks)
    } else if ((m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN)) {
        const LONG edge = page_edge();
        const int x = (short)LOWORD(l);
        bool imgui_busy = true;   // 11-06 11:05: never start a camera drag while ImGui wants the mouse (hovered window/widget, active slider)
        __try { const ImGuiIO& io = ImGui::GetIO(); imgui_busy = io.WantCaptureMouse || ImGui::IsAnyItemActive() || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow); }
        __except (EXCEPTION_EXECUTE_HANDLER) { imgui_busy = true; }
        if (edge > 0 && x > edge && !imgui_busy) {
            g_camDrag = true; GetCursorPos(&g_camDragPt); SetCapture(h);
            return 0;
        }
    }
    bool want_mouse = false, want_keys = false;
    __try {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        want_mouse = ImGui::GetIO().WantCaptureMouse;
        want_keys = ImGui::GetIO().WantCaptureKeyboard;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    bool is_mouse = m >= WM_MOUSEFIRST && m <= WM_MOUSELAST;
    bool is_key = m >= WM_KEYFIRST && m <= WM_KEYLAST;
    // key releases always reach the game so nothing stays held after the panel closes
    bool is_release = m == WM_KEYUP || m == WM_SYSKEYUP;
    if (g_cfg.lock_input && (is_mouse || (is_key && !is_release))) return 0;
    if ((is_mouse && want_mouse) || (is_key && want_keys && !is_release)) return 0;
    if (m == WM_INPUT && (g_cfg.lock_input || want_mouse)) return DefWindowProcW(h, m, w, l);
    if (m == WM_SETCURSOR) return TRUE;
    return CallWindowProcW(o_WndProc, h, m, w, l);
}

// The game recenters and clips the cursor for mouse-look; while the panel is open both are ignored
// and raw mouse movement/buttons (plus key presses) are zeroed before the game sees them.
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
static SetCursorPos_t o_SetCursorPos = nullptr;
static ClipCursor_t o_ClipCursor = nullptr;
static GetRawInputData_t o_GetRawInputData = nullptr;
static GetRawInputBuffer_t o_GetRawInputBuffer = nullptr;

static BOOL WINAPI hk_SetCursorPos(int x, int y) { if (input_blocked()) return TRUE; return o_SetCursorPos(x, y); }

static BOOL WINAPI hk_ClipCursor(const RECT* r) { if (input_blocked() && r) return o_ClipCursor(nullptr); return o_ClipCursor(r); }

static void neuter_raw_drag(RAWINPUT* ri) {   // camera drag: keep mouse movement, drop every press (mouse + keys) and the wheel
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        USHORT up = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        ri->data.mouse.usButtonFlags &= up; ri->data.mouse.usButtonData = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK)) { ri->data.keyboard.VKey = 0xFF; ri->data.keyboard.MakeCode = 0; }
    }
}
static bool drag_filter() { return g_camDrag && trinity::g_state.menuOpen && !g_retired; }
static void neuter_raw(RAWINPUT* ri) {
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        ri->data.mouse.lLastX = 0; ri->data.mouse.lLastY = 0;
        // keep button-up flags (no stuck buttons), drop presses and wheel
        USHORT up = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
        ri->data.mouse.usButtonFlags &= up;
        ri->data.mouse.usButtonData = 0;
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK)) { ri->data.keyboard.VKey = 0xFF; ri->data.keyboard.MakeCode = 0; }
    }
}
// 11-12: typing in a trainer text box (search, values) leaked to the game through RAW keyboard input (B toggled the
// damage meter): while a trainer text box is active, key presses are dropped there too (releases pass).
static bool keys_to_panel() {
    if (!trinity::g_state.menuOpen || g_retired) return false;
    bool w = false;
    __try { w = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; } __except (EXCEPTION_EXECUTE_HANDLER) { w = false; }
    return w;
}
static void neuter_raw_keys(RAWINPUT* ri) {
    if (ri->header.dwType == RIM_TYPEKEYBOARD && !(ri->data.keyboard.Flags & RI_KEY_BREAK)) { ri->data.keyboard.VKey = 0xFF; ri->data.keyboard.MakeCode = 0; }
}
static UINT WINAPI hk_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT r = o_GetRawInputData(h, cmd, data, size, hdr);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER)) {
        if (input_blocked()) neuter_raw((RAWINPUT*)data); else if (drag_filter()) neuter_raw_drag((RAWINPUT*)data);
        else if (keys_to_panel()) neuter_raw_keys((RAWINPUT*)data); }
    return r;
}
static UINT WINAPI hk_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdr) {
    UINT n = o_GetRawInputBuffer(data, size, hdr);
    const bool keys = data && n != (UINT)-1 && !input_blocked() && !drag_filter() && keys_to_panel();
    if (data && n != (UINT)-1 && (input_blocked() || drag_filter() || keys)) {
        const bool full = input_blocked(), drag = !full && drag_filter();
        RAWINPUT* ri = data;
        for (UINT i = 0; i < n; ++i) { if (full) neuter_raw(ri); else if (drag) neuter_raw_drag(ri); else neuter_raw_keys(ri); ri = NEXTRAWINPUTBLOCK(ri); }
    }
    return n;
}

static void install_input_hooks() {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    struct H { const char* n; void* d; void** o; } hs[] = {
        { "SetCursorPos", (void*)&hk_SetCursorPos, (void**)&o_SetCursorPos },
        { "ClipCursor", (void*)&hk_ClipCursor, (void**)&o_ClipCursor },
        { "GetRawInputData", (void*)&hk_GetRawInputData, (void**)&o_GetRawInputData },
        { "GetRawInputBuffer", (void*)&hk_GetRawInputBuffer, (void**)&o_GetRawInputBuffer },
    };
    for (auto& h : hs) {
        void* p = GetProcAddress(u32, h.n);
        if (p && MH_CreateHook(p, h.d, h.o) == MH_OK && MH_EnableHook(p) == MH_OK) { MH_MarkPersistent(p); continue; }
        log("[overlay] could not hook %s", h.n);
    }
    g_scp = o_SetCursorPos;
}

} // namespace wb

// ------------------------------------------------------------------ trinity glue implementations
namespace trinity {
namespace input {
void Init(HWND hwnd) {
    using namespace wb;
    if (o_WndProc) return;
    g_hwnd = hwnd;
    o_WndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)hk_WndProc);
    install_input_hooks();
    log("[overlay] input attached to window %p", hwnd);
}
void Shutdown() {
    using namespace wb;
    if (o_WndProc && g_hwnd) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)o_WndProc);
    o_WndProc = nullptr;
}
}

namespace ui {
void InitStyle(float uiScale) {
    float k = std::min(2.5f, std::max(0.9f, uiScale));
    wb::g_ui_scale = k;
    wb::load_fonts(k);
    wb::apply_wildframe_theme(k);
}

// The SETTINGS hotkey (bin\settings.json, default INSERT; optional Ctrl/Shift/Alt) or LB + D-pad Down on a controller -
// rising edge, polled per frame. A hotkey with modifiers needs exactly those modifiers; a plain key ignores them (so it
// still works while Shift = sprint is held). Ignored while the SETTINGS page waits for a new key.
bool PollMenuToggle() {
    static bool was = false, padWas = false;
    const wb::Hotkey hk = wb::current_hotkey();
    auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool down = held(hk.vk);
    if (down && (hk.ctrl || hk.shift || hk.alt))
        down = held(VK_CONTROL) == hk.ctrl && held(VK_SHIFT) == hk.shift && held(VK_MENU) == hk.alt;
    bool edge = down && !was; was = down;
    if (wb::capture_active()) edge = false;   // keep 'was' current so the captured key does not toggle once the wait ends
    // controller: LB + D-pad Down opens/closes the panel; Start also closes it while it is open
    XINPUT_STATE xs{};
    if (hooks::XInputReadReal(0, &xs) == ERROR_SUCCESS) {
        const WORD b = xs.Gamepad.wButtons;
        const bool chord = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) && (b & XINPUT_GAMEPAD_DPAD_DOWN);
        const bool start = State::Get().menuOpen && (b & XINPUT_GAMEPAD_START);
        const bool pad = chord || start;
        if (pad && !padWas) edge = true;
        padWas = pad;
    }
    return edge;
}
}

namespace gui {
// The newest copy's panel. In the host: our own panel until a reload hands over, then the client's.
static bool draw_active_panel(bool* vis) {
    using namespace wb;
    panel_draw(vis);
    return true;
}
bool WantsDraw() { return g_state.menuOpen || wb::toasts_live(); }
// Mouse for the panel, whatever input was used last: while the game is driven by a controller Windows sends
// no mouse messages and the game hides/locks the cursor, so ImGui had no mouse position to draw. Every frame
// the panel is open we free the cursor, read its real position ourselves, and draw it. On open the cursor is
// placed on the panel.
static void panel_mouse(bool opened_now) {
    using namespace wb;
    ImGuiIO& io = ImGui::GetIO();
    if (g_camDrag) { io.MouseDrawCursor = false; return; }       // 11-06: turning the camera - the game owns the cursor
    io.MouseDrawCursor = true;                                    // ImGui draws the cursor (game keeps the OS one hidden)
    if (!g_hwnd) return;
    // only while the game window is the active window - after alt-tab the cursor belongs to Windows
    static bool was_fg = false;
    bool fg = GetForegroundWindow() == g_hwnd;
    if (!fg) { was_fg = false; return; }
    if (!was_fg) opened_now = true;                               // coming back from alt-tab: put it on the panel once
    was_fg = true;
    if (o_ClipCursor) o_ClipCursor(nullptr); else ClipCursor(nullptr);
    RECT rc; GetClientRect(g_hwnd, &rc);
    POINT pt; GetCursorPos(&pt); ScreenToClient(g_hwnd, &pt);
    if (opened_now) {
        // centre of the panel window (largest visible ImGui window), or left third of the screen before it exists
        ImVec2 c(rc.right * 0.2f, rc.bottom * 0.4f);
        float best = 0;
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
            if (!w->WasActive || w->Hidden || (w->Flags & ImGuiWindowFlags_NoInputs)) continue;
            float a = w->Size.x * w->Size.y;
            if (a > best) { best = a; c = ImVec2(w->Pos.x + w->Size.x * 0.5f, w->Pos.y + w->Size.y * 0.35f); }
        }
        POINT sp{ (LONG)c.x, (LONG)c.y }; ClientToScreen(g_hwnd, &sp);
        if (o_SetCursorPos) o_SetCursorPos(sp.x, sp.y); else SetCursorPos(sp.x, sp.y);
        pt = { (LONG)c.x, (LONG)c.y };
    }
    io.AddMousePosEvent((float)pt.x, (float)pt.y);                // no WM_MOUSEMOVE needed
}

// Controller navigation for the panel: the real pad (the game's copy is neutralised while the menu is up) is fed
// to Dear ImGui's gamepad navigation. D-pad / left stick move, A activates, B backs out, X opens text input,
// LT/RT tweak values slow/fast, LB/RB switch tabs (panel.cpp reads GamepadL1/R1). Released cleanly on close.
static void feed_gamepad(bool active) {
    ImGuiIO& io = ImGui::GetIO();
    static bool fed = false;
    XINPUT_STATE xs{};
    const bool have = active && hooks::XInputReadReal(0, &xs) == ERROR_SUCCESS;
    if (!have && !fed) return;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const WORD b = have ? xs.Gamepad.wButtons : 0;
    auto btn = [&](ImGuiKey k, WORD m) { io.AddKeyEvent(k, (b & m) != 0); };
    auto ana = [&](ImGuiKey k, float v, float lo, float hi) { float t = (v - lo) / (hi - lo); t = t < 0 ? 0 : t > 1 ? 1 : t; io.AddKeyAnalogEvent(k, t > 0.10f, t); };
    btn(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);        btn(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
    btn(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);         btn(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    btn(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);         btn(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
    btn(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT); btn(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    btn(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);     btn(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    btn(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);   btn(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    btn(ImGuiKey_GamepadL3, XINPUT_GAMEPAD_LEFT_THUMB);      btn(ImGuiKey_GamepadR3, XINPUT_GAMEPAD_RIGHT_THUMB);
    const float lt = have ? xs.Gamepad.bLeftTrigger : 0, rt = have ? xs.Gamepad.bRightTrigger : 0;
    ana(ImGuiKey_GamepadL2, lt, 30.f, 255.f); ana(ImGuiKey_GamepadR2, rt, 30.f, 255.f);
    const float lx = have ? xs.Gamepad.sThumbLX : 0, ly = have ? xs.Gamepad.sThumbLY : 0;
    const float rx = have ? xs.Gamepad.sThumbRX : 0, ry = have ? xs.Gamepad.sThumbRY : 0;
    const float dz = 8000.f, mx = 32767.f;
    ana(ImGuiKey_GamepadLStickLeft, -lx, dz, mx); ana(ImGuiKey_GamepadLStickRight, lx, dz, mx);
    ana(ImGuiKey_GamepadLStickUp, ly, dz, mx);    ana(ImGuiKey_GamepadLStickDown, -ly, dz, mx);
    ana(ImGuiKey_GamepadRStickLeft, -rx, dz, mx); ana(ImGuiKey_GamepadRStickRight, rx, dz, mx);
    ana(ImGuiKey_GamepadRStickUp, ry, dz, mx);    ana(ImGuiKey_GamepadRStickDown, -ry, dz, mx);
    fed = have;
}

void Render() {
    using namespace wb;
    feed_gamepad(g_state.menuOpen);
    ++g_frames;
    ImGuiIO& io = ImGui::GetIO();
    static bool was_open = false;
    static int place_frames = 0;
    if (g_state.menuOpen) {
        if (!was_open) place_frames = 2;                          // frame 1: panel not laid out yet, frame 2: on the panel
        panel_mouse(place_frames > 0);
        if (place_frames > 0) --place_frames;
        bool vis = true;
        if (!draw_active_panel(&vis)) vis = true;
        if (!vis) g_state.menuOpen = false;
    } else {
        io.MouseDrawCursor = false;
    }
    was_open = g_state.menuOpen;
    draw_toasts();
}
}

} // namespace trinity

namespace wb {


bool install_overlay() {
    bool okh = trinity::hooks::InstallDX12Hooks();
    g_hooks_ok = okh;
    log("[overlay] DX12 hooks %s", okh ? "installed" : "FAILED");
    return okh;
}


void overlay_retire() { trinity::g_state.menuOpen = false; }
void overlay_shutdown() { trinity::hooks::RemoveDX12Hooks(); }


json panel_show(bool visible, const std::string& tab);   // in panel.cpp

} // namespace wb
