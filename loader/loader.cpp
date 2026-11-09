// Dragonkin Trainer loader. DEV build: bin\nativeLoader.exe (+ --watch). PUBLIC build (/DWB_PUBLIC=1):
// bin\public\DragonkinTrainer.exe - same attach logic, no --watch, no AI link line.
// Attaches native.dll to DragonkinTheBanished-Win64-Shipping.exe. Nothing in the game folder is modified (no ASI, no
// mod manager). Two ways in:
//   nativeLoader.exe            game not running: start it through Steam and attach as soon as its
//                                      graphics DLLs load (before the swapchain exists - overlay works with
//                                      DLSS Frame Generation). Game already running: attach now.
//   nativeLoader.exe --attach   only attach to a running game (overlay needs Frame Generation off)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "shell32.lib")

static const wchar_t* kGameExe = L"DragonkinTheBanished-Win64-Shipping.exe";
static const wchar_t* kDllName = L"native.dll";
static const wchar_t* kSteamUrl = L"steam://rungameid/1863430";

static void say(WORD color, const char* tag, const char* msg) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, color); printf("%s", tag);
    SetConsoleTextAttribute(h, 7); printf(" %s\n", msg);
}
#define INFO(m) say(8, "  ..", m)
#define GOOD(m) say(10, "  OK", m)
#define BAD(m)  say(12, "FAIL", m)

static DWORD find_pid() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{ sizeof e }; DWORD pid = 0;
    for (BOOL k = Process32FirstW(s, &e); k; k = Process32NextW(s, &e))
        if (_wcsicmp(e.szExeFile, kGameExe) == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(s);
    return pid;
}

static bool has_module(DWORD pid, const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W m{ sizeof m }; bool found = false;
    for (BOOL k = Module32FirstW(s, &m); k; k = Module32NextW(s, &m))
        if (_wcsicmp(m.szModule, name) == 0) { found = true; break; }
    CloseHandle(s);
    return found;
}

static bool bridge_active(DWORD pid) {
    char nm[64]; sprintf_s(nm, "Local\\nativeActive_%lu", pid);
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, nm);
    if (h) { CloseHandle(h); return true; }
    return false;
}

// Each attach loads its own copy (bin\loaded\native_<time>.dll) so a retired copy and a new one
// never share an address - pointers the game still holds into a retired copy stay valid.
static std::wstring stage_copy(const std::wstring& dir, const std::wstring& dll) {
    std::wstring ld = dir + L"\\loaded";
    CreateDirectoryW(ld.c_str(), nullptr);
    {   // tidy up copies from earlier game sessions (a copy still loaded in a running game cannot be deleted - fine)
        WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((ld + L"\\native_*.dll").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) { do { DeleteFileW((ld + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(fh, &fd)); FindClose(fh); }
    }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"\\native_%04d%02d%02d_%02d%02d%02d_%03d.dll", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::wstring dst = ld + name;
    return CopyFileW(dll.c_str(), dst.c_str(), FALSE) ? dst : L"";
}

static bool inject(DWORD pid, const std::wstring& dll) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { BAD("Cannot open DragonkinTheBanished-Win64-Shipping.exe (run the loader as the same user as the game)."); return false; }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool okw = mem && WriteProcessMemory(p, mem, dll.c_str(), bytes, nullptr);
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = okw ? CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr) : nullptr;
    DWORD code = 0;
    if (t) { WaitForSingleObject(t, 20000); GetExitCodeThread(t, &code); CloseHandle(t); }
    if (mem) VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    return t && code != 0;
}

#define WB_PUBLIC 1



int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"Dragonkin Trainer");
    printf("\n  DRAGONKIN TRAINER\n  -----------------------------------------\n\n");
    bool attach_only = argc > 1 && _wcsicmp(argv[1], L"--attach") == 0;

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self; dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring dll = dir + L"\\" + kDllName;
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) { BAD("native.dll is missing next to the loader."); Sleep(4000); return 2; }

    DWORD pid = find_pid();
    bool early = false;
    if (!pid) {
        if (attach_only) { BAD("Dragonkin: The Banished is not running."); Sleep(3000); return 3; }
        INFO("Dragonkin: The Banished is not running - starting it through Steam...");
        ShellExecuteW(nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
        for (int i = 0; i < 180 * 40 && !pid; ++i) { Sleep(25); pid = find_pid(); }
        if (!pid) { BAD("Dragonkin: The Banished did not start within 3 minutes."); Sleep(4000); return 4; }
        early = true;
    }
    char buf[160]; sprintf_s(buf, "Found DragonkinTheBanished-Win64-Shipping.exe (pid %lu)", pid); GOOD(buf);
    if (bridge_active(pid)) { GOOD("The trainer is already attached. Press your panel hotkey in game (default INSERT)."); Sleep(3000); return 0; }

    if (early) {
        // attach the moment the game loads its graphics stack: the DX12 hook then sees the swapchain being created
        INFO("Waiting for the game to load DirectX...");
        int waited = 0;
        while (!has_module(pid, L"dxgi.dll") && !has_module(pid, L"d3d12.dll") && waited < 120 * 20) { Sleep(50); ++waited; if (!find_pid()) { BAD("Dragonkin: The Banished closed."); return 5; } }
        Sleep(150);
    } else {
        INFO("Game already running - late attach (overlay draws when DLSS Frame Generation is off).");
    }

    std::wstring copy = stage_copy(dir, dll);
    if (copy.empty()) { BAD("Could not copy native.dll (is the trainer folder read-only?)."); Sleep(4000); return 8; }
    if (!inject(pid, copy)) { BAD("Attach failed - see trainer.log next to the DLL."); Sleep(5000); return 7; }
    GOOD(early ? "Trainer attached early (full overlay support)." : "Trainer attached.");
    INFO("Press your panel hotkey in game (default INSERT; change it on the SETTINGS page).");

    Sleep(3000);
    return 0;
}
