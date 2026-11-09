#pragma once
// DragonkinTheBanished Bridge shim for Trinity's core/mod.h: the engine modules only ask for the module handle
// (to locate files beside the DLL). Our bridge owns initialisation (see engine.cpp).
#include <Windows.h>

namespace wb { HMODULE self_module(); }

namespace trinity
{
    class Mod
    {
    public:
        static Mod& Get() { static Mod m; return m; }
        HMODULE Module() const { return wb::self_module(); }
    };
}
