// Ported from Trinity (MIT License, Copyright (c) 2026 XeTrinityz; vTweak by ReXooGen/Lian).
// https://github.com/XeTrinityz/Trinity - see ref\trinity\LICENSE. Edited for DragonkinTheBanished Bridge by
// tools\port_sources.py: includes route through trinity_glue.h, teleport hotkey removed.
#pragma once
#include <Windows.h>
#include <Xinput.h>

namespace trinity::hooks::dualsense
{
    // Initializes the background DualSense (DS5) HID monitor thread.
    void Start();

    // Stops the monitor thread and cleans up open device handles.
    void Stop();

    // Returns true if a DualSense (PS5) or DualShock 4 controller is actively connected.
    bool IsConnected();

    // Retrieves the latest synthetic XINPUT_STATE mapped from DualSense inputs.
    bool GetState(XINPUT_STATE* outState);

    // Returns human-readable model and connection string (e.g. "DualSense (USB)").
    const char* GetDeviceName();
}
