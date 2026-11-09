// DragonkinTheBanished Bridge - hot reload: the first copy loaded into the game is the HOST for the whole session
// (DX12 overlay, ImGui context, fonts, item icons, input capture, panel toggle key). Every later copy
// loaded by "Reload trainer" is a CLIENT: it runs all features, the AI link and the panel contents,
// and draws them through the host's ImGui context.
#pragma once
#include <cstdint>
#include "imgui.h"

namespace wb {

struct HostIcons {
    bool (*item)(ImDrawList*, const char*, ImVec2, ImVec2, ImU32);
    void (*icon)(ImDrawList*, int, ImVec2, ImVec2, ImU32);
    ImVec2 (*size)(int, float);
    bool (*ready)();
};

struct HostApi {
    uint32_t magic;            // 'CBHA'
    uint32_t abi;              // bump when this struct changes
    int imgui_version;
    uint32_t io_size, style_size, drawlist_size, font_size;
    ImGuiContext* (*context)();
    void (*allocators)(ImGuiMemAllocFunc*, ImGuiMemFreeFunc*, void**);
    ImFont* (*font)(int which);          // 0 body, 1 bold, 2 title
    float (*ui_scale)();
    bool (*visible)();
    void (*set_visible)(bool);
    void (*toast)(const char*);
    void (*set_client)(void (*draw)(bool*));   // nullptr = stop drawing the client
    HostIcons icons;
};

constexpr uint32_t kHostMagic = 0x41484243;   // "CBHA"
constexpr uint32_t kHostAbi = 1;

const HostApi* host_api();      // the host this copy draws through; nullptr in the host itself
const HostIcons* host_icons();  // icon routing for the engine layer's icons.cpp (nullptr in the host)
bool host_connect();            // client: find the host and share its ImGui context
void host_drop_client();        // a retiring client: the host stops drawing it
void host_attach_panel();       // a started client: the host draws its panel from now on

} // namespace wb
