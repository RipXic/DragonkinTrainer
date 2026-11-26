// Dragonkin Trainer drop-in loader (public + dev). Built as dsound.dll and placed next to the game exe
// (<game>\DragonkinTheBanished\Binaries\Win64\dsound.dll) together with the DragonkinTrainer\ folder.
// The game loads dsound.dll by itself at startup (it imports it), so no separate program has to start or attach anything:
//   1. every DirectSound call is passed on unchanged to the real Windows dsound.dll (System32), so game sound works;
//   2. a small thread loads DragonkinTrainer\native.dll once Windows has finished loading the game's DLLs.
// Nothing else is changed. Uninstall: delete dsound.dll and the DragonkinTrainer folder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cwchar>
#define WB_PUBLIC 1

static HMODULE g_self = nullptr;
static HMODULE g_real = nullptr;
static const wchar_t* kGameExe = L"DragonkinTheBanished-Win64-Shipping.exe";

static FARPROC real(const char* name) {
    if (!g_real) {
        wchar_t sys[MAX_PATH]; UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (!n || n >= MAX_PATH) return nullptr;
        g_real = LoadLibraryW((std::wstring(sys) + L"\\dsound.dll").c_str());
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

typedef UINT_PTR U;
#define DSERR_GENERIC_ ((HRESULT)0x80004005L)
#define FWD(name, ...) auto f = (HRESULT(WINAPI*)(__VA_ARGS__))real(name); if (!f) return DSERR_GENERIC_;

extern "C" {
HRESULT WINAPI p_DirectSoundCreate(U a, U b, U c)                { FWD("DirectSoundCreate", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundCreate8(U a, U b, U c)               { FWD("DirectSoundCreate8", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundEnumerateA(U a, U b)                 { FWD("DirectSoundEnumerateA", U, U) return f(a, b); }
HRESULT WINAPI p_DirectSoundEnumerateW(U a, U b)                 { FWD("DirectSoundEnumerateW", U, U) return f(a, b); }
HRESULT WINAPI p_DirectSoundCaptureCreate(U a, U b, U c)         { FWD("DirectSoundCaptureCreate", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundCaptureCreate8(U a, U b, U c)        { FWD("DirectSoundCaptureCreate8", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundCaptureEnumerateA(U a, U b)          { FWD("DirectSoundCaptureEnumerateA", U, U) return f(a, b); }
HRESULT WINAPI p_DirectSoundCaptureEnumerateW(U a, U b)          { FWD("DirectSoundCaptureEnumerateW", U, U) return f(a, b); }
HRESULT WINAPI p_GetDeviceID(U a, U b)                           { FWD("GetDeviceID", U, U) return f(a, b); }
HRESULT WINAPI p_DllGetClassObject(U a, U b, U c)                { FWD("DllGetClassObject", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundFullDuplexCreate(U a, U b, U c, U d, U e, U g, U h, U i, U j, U k) {
    FWD("DirectSoundFullDuplexCreate", U, U, U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i, j, k);
}
HRESULT WINAPI p_DllCanUnloadNow() {
    if (!g_real) return S_FALSE;
    auto f = (HRESULT(WINAPI*)())GetProcAddress(g_real, "DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
}

// Runs after the loader lock is released (a thread created in DllMain starts only then), so LoadLibrary is safe here.
static DWORD WINAPI load_trainer(LPVOID) {
    wchar_t p[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, p, MAX_PATH)) return 0;
    std::wstring exe = p; exe = exe.substr(exe.find_last_of(L"\\/") + 1);
    if (_wcsicmp(exe.c_str(), kGameExe) != 0) return 0;              // only inside the game itself
    if (!GetModuleFileNameW(g_self, p, MAX_PATH)) return 0;
    std::wstring dir = p; dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring dll = dir + L"\\DragonkinTrainer\\native.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    LoadLibraryW(dll.c_str());                                       // native.dll refuses a second copy by itself
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, load_trainer, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        // the process is ending: leave the real dsound.dll to Windows
    }
    return TRUE;
}
