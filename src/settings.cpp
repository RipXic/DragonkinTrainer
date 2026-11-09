// Dragonkin Trainer - user settings (2026-11-09, release prep).
// bin\settings.json is the single source of truth for the panel hotkey, developer mode and the panel scale
// (panel\panel.json's "hotkey" is the UMG F8 panel's and is ignored here; config.json no longer has a toggle key).
//   {"hotkey": {"vk": 45, "ctrl": false, "shift": false, "alt": false}, "developer_mode": false, "ui_scale": 1.0}
// Missing / invalid file or field = default. Written atomically (settings.json.tmp + MoveFileEx replace).
// The hotkey is also published to the shared input mapping (overlay.cpp) so the copy that owns the overlay - the first
// copy of the session, which may be an older build after a hot reload - polls the key the newest copy saved.
#include "common.h"
#include <fstream>
#include <algorithm>
#include <cmath>

namespace wb {

Settings g_settings;

static std::string settings_path() { return g_cfg.root + "\\settings.json"; }

int hotkey_pack(const Hotkey& h) {
    return (h.vk & 0xFF) | (h.ctrl ? 0x100 : 0) | (h.shift ? 0x200 : 0) | (h.alt ? 0x400 : 0) | 0x8000;
}

Hotkey hotkey_unpack(int p) {
    Hotkey h; h.vk = p & 0xFF; h.ctrl = (p & 0x100) != 0; h.shift = (p & 0x200) != 0; h.alt = (p & 0x400) != 0;
    return h;
}

// keys a panel hotkey may use: no mouse buttons, no bare modifiers, no Escape (cancels the capture), no Windows keys
bool hotkey_vk_allowed(int vk) {
    if (vk <= 0x07 || vk > 0xFE) return false;                   // 1..6 = mouse buttons
    switch (vk) {
    case VK_ESCAPE: case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
    case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_CAPITAL:
    case VK_NUMLOCK: case VK_SCROLL: case VK_PACKET: case 0xFF:
        return false;
    }
    return true;
}

std::string key_name(int vk) {
    struct N { int vk; const char* n; };
    static const N names[] = {
        { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" }, { VK_HOME, "HOME" }, { VK_END, "END" }, { VK_PRIOR, "PAGE UP" },
        { VK_NEXT, "PAGE DOWN" }, { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
        { VK_BACK, "BACKSPACE" }, { VK_TAB, "TAB" }, { VK_RETURN, "ENTER" }, { VK_SPACE, "SPACE" }, { VK_PAUSE, "PAUSE" },
        { VK_SNAPSHOT, "PRINT SCREEN" }, { VK_ESCAPE, "ESC" }, { VK_MULTIPLY, "NUM *" }, { VK_ADD, "NUM +" },
        { VK_SUBTRACT, "NUM -" }, { VK_DECIMAL, "NUM ." }, { VK_DIVIDE, "NUM /" }, { VK_CLEAR, "CLEAR" },
        { VK_OEM_3, "`" }, { VK_OEM_MINUS, "-" }, { VK_OEM_PLUS, "=" }, { VK_OEM_4, "[" }, { VK_OEM_6, "]" },
        { VK_OEM_5, "\\" }, { VK_OEM_1, ";" }, { VK_OEM_7, "'" }, { VK_OEM_COMMA, "," }, { VK_OEM_PERIOD, "." },
        { VK_OEM_2, "/" }, { VK_OEM_102, "<>" },
    };
    for (auto& n : names) if (n.vk == vk) return n.n;
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM " + std::to_string(vk - VK_NUMPAD0);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    char buf[64] = {};   // anything else: the keyboard layout's own name
    const UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof buf) > 0) {
        std::string s = buf; std::transform(s.begin(), s.end(), s.begin(), ::toupper); return s;
    }
    char hx[16]; sprintf_s(hx, "KEY 0x%02X", vk); return hx;
}

std::string hotkey_name(const Hotkey& h) {
    std::string s;
    if (h.ctrl) s += "CTRL + ";
    if (h.shift) s += "SHIFT + ";
    if (h.alt) s += "ALT + ";
    return s + key_name(h.vk);
}

void settings_load() {
    Settings s;   // defaults
    std::ifstream f(settings_path(), std::ios::binary);
    if (f) {
        try {
            json j = json::parse(f);
            if (j.is_object()) {
                auto hk = j.find("hotkey");
                if (hk != j.end() && hk->is_object()) {
                    const int vk = hk->value("vk", VK_INSERT);
                    if (hotkey_vk_allowed(vk)) {
                        s.hotkey.vk = vk;
                        s.hotkey.ctrl = hk->value("ctrl", false); s.hotkey.shift = hk->value("shift", false); s.hotkey.alt = hk->value("alt", false);
                    } else log("[settings] hotkey vk %d is not allowed - using INSERT", vk);
                }
                auto us = j.find("ui_scale");
                if (us != j.end() && us->is_number()) { const double v = us->get<double>(); if (std::isfinite(v)) s.ui_scale = (float)std::clamp(v, 0.75, 1.5); }
            }
        } catch (std::exception& e) {
            log("[settings] settings.json is not valid (%s) - using defaults", e.what());
            s = Settings();
        }
    }
    g_settings = s;
    overlay_publish_hotkey(hotkey_pack(g_settings.hotkey));
    log("[settings] panel hotkey %s, ui scale %.2f%s", hotkey_name(g_settings.hotkey).c_str(),
        g_settings.ui_scale, f ? "" : " (no settings.json - defaults)");
}

bool settings_save(std::string* err) {
    json j = {
        {"hotkey", { {"vk", g_settings.hotkey.vk}, {"ctrl", g_settings.hotkey.ctrl}, {"shift", g_settings.hotkey.shift}, {"alt", g_settings.hotkey.alt} }},
        {"ui_scale", std::round(g_settings.ui_scale * 100.0) / 100.0},
    };
    overlay_publish_hotkey(hotkey_pack(g_settings.hotkey));
    const std::string path = settings_path(), tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp; log("[settings] cannot write %s", tmp.c_str()); return false; }
        f << j.dump(2) << "\n";
        f.flush();
        if (!f) { if (err) *err = "write failed: " + tmp; return false; }
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileA(tmp.c_str());
        if (err) *err = "cannot replace settings.json (error " + std::to_string(e) + ")";
        log("[settings] cannot replace %s (error %lu)", path.c_str(), e);
        return false;
    }
    return true;
}

} // namespace wb
