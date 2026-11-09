// Ported from Trinity (MIT License, Copyright (c) 2026 XeTrinityz; vTweak by ReXooGen/Lian).
// https://github.com/XeTrinityz/Trinity - see ref\trinity\LICENSE. Edited for DragonkinTheBanished Bridge by
// tools\port_sources.py: includes route through trinity_glue.h, teleport hotkey removed.
#pragma once

namespace trinity::hooks
{
    // Grabs the DX12 present/queue vtable pointers from a throwaway device and
    // detours them with MinHook. Must be called after MH_Initialize().
    bool InstallDX12Hooks();
    void RemoveDX12Hooks();
}
