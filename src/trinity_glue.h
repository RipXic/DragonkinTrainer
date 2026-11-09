// DragonkinTheBanished Bridge - glue that lets Trinity's DX12 hook (MIT, XeTrinityz / ReXooGen, see ref\trinity) run
// against our panel. State and Logger are Trinity's real ones (src\trinity\core); this adds the few
// overlay-side symbols the hook calls (style, menu toggle, render, input, icons).
#pragma once
#include <d3d12.h>
#include "trinity/core/logger.h"
#include "trinity/core/state.h"
#include "trinity/gui/icons.h"   // IconsInit / IconsRecordUploads / DrawItemIcon

namespace trinity {
    struct Settings { static void ClaimOwnership() {} };
    namespace ui {
        extern bool g_needFontRebuild;
        void InitStyle(float uiScale);
        bool PollMenuToggle();
    }
    namespace gui { void Render(); bool WantsDraw(); }
    namespace input { void Init(HWND hwnd); void Shutdown(); }
}
