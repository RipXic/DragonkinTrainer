// DragonkinTheBanished Bridge - icon API without game assets (2026-11-09).
// The Crimson Desert template drew item / button icons from that game's own pak files (trinity/gui/icons.cpp, now in
// _crimson_unused). Dragonkin has no such paks, so every call here is a no-op: the panel uses text only. The functions stay
// because the DX12 hook calls IconsInit/IconsRecordUploads/IconsShutdown and the hot-reload host table (host_api.h)
// exposes them to reloaded copies - keeping the same symbols keeps the host ABI unchanged.
#include "trinity/gui/icons.h"

namespace trinity::ui {
void IconsInit(ID3D12Device*, ID3D12DescriptorHeap*, unsigned, unsigned, unsigned) {}
void IconsShutdown() {}
void IconsRecordUploads(ID3D12GraphicsCommandList*) {}
bool IconsReady() { return false; }
ImVec2 IconSize(Icon, float height) { return ImVec2(height, height); }
void DrawIcon(ImDrawList*, Icon, ImVec2, ImVec2, ImU32) {}
float DrawIconH(ImDrawList*, Icon, float, float, float h, ImU32) { return h; }
bool DrawItemIcon(ImDrawList*, const char*, ImVec2, ImVec2, ImU32) { return false; }
}
