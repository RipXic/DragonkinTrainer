// DragonkinTheBanished Bridge - core: config, log, memory tools, address expressions,
// main-thread runner, crash diagnostics, guarded calls, DLL entry.
// Derived from the Wolcen Bridge core (Wildframe Forge).
#define _CRT_RAND_S
#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdarg>
#include <deque>
#include <fstream>
#include <share.h>
#include <algorithm>
#include "MinHook.h"
#include "host_api.h"
#include <dbghelp.h>
#include <filesystem>
#pragma comment(lib, "dbghelp.lib")
#include <thread>
#include <csignal>
#include <exception>

namespace wb {

Config g_cfg;
std::atomic<bool> g_retired{false};
std::atomic<bool> g_game_exiting{false};
std::atomic<bool> g_features_off{false};
std::atomic<bool> g_reload_failed{false};
static std::atomic<int> g_drain_active{0};
static HANDLE g_active_marker = nullptr;
static HMODULE g_self = nullptr;

// ------------------------------------------------------------------ util
std::string hexstr(uint64_t v) { char b[32]; sprintf_s(b, "0x%llX", (unsigned long long)v); return b; }

// reply envelope {ok, code, message, data, warnings} (was in server.cpp; the PUBLIC build has no server but the panel uses it)
json ok(json data, const std::string& msg) {
    return { {"ok", true}, {"code", "OK"}, {"message", msg}, {"data", data}, {"warnings", json::array()} };
}
json fail(const std::string& code, const std::string& msg) {
    return { {"ok", false}, {"code", code}, {"message", msg}, {"data", nullptr}, {"warnings", json::array()} };
}

std::string to_hex(const uint8_t* p, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; ++i) { sprintf_s(b, "%02X", p[i]); if (i) s += ' '; s += b; }
    return s;
}

// "48 89 05 ?? ?? ?? ??" (single ? also accepted)
bool parse_pattern(const std::string& s, std::vector<int>& pat) {
    size_t i = 0;
    while (i < s.size()) {
        if (isspace((unsigned char)s[i])) { ++i; continue; }
        if (s[i] == '?') { pat.push_back(-1); while (i < s.size() && s[i] == '?') ++i; continue; }
        if (i + 1 >= s.size() || !isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) return false;
        pat.push_back((int)strtoul(s.substr(i, 2).c_str(), nullptr, 16)); i += 2;
    }
    return !pat.empty();
}

// ------------------------------------------------------------------ log
static std::mutex g_log_mtx;
static std::deque<std::string> g_log_ring;
static FILE* g_log_file = nullptr;

void log(const char* fmt, ...) {
    char msg[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    char line[2200];
    sprintf_s(line, "[%02d:%02d:%02d.%03d] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_log_ring.push_back(line);
    while (g_log_ring.size() > 800) g_log_ring.pop_front();
    if (g_log_file) { fprintf(g_log_file, "%s\n", line); fflush(g_log_file); }
}

HMODULE self_module() { return g_self; }
void release_active_marker() { if (g_active_marker) { CloseHandle(g_active_marker); g_active_marker = nullptr; } }
void log_close() { std::lock_guard<std::mutex> lk(g_log_mtx); if (g_log_file) { fclose(g_log_file); g_log_file = nullptr; } }

std::vector<std::string> log_tail(size_t n) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    size_t start = g_log_ring.size() > n ? g_log_ring.size() - n : 0;
    return std::vector<std::string>(g_log_ring.begin() + start, g_log_ring.end());
}

// ------------------------------------------------------------------ config
void load_config() {
    std::ifstream f(g_cfg.root + "\\config.json");
    if (!f) return;   // optional in the PUBLIC build (only lock_input + the panel path overrides matter); never written
    try {
        json j = json::parse(f);
        g_cfg.port = j.value("port", g_cfg.port);
        g_cfg.allow_raw = j.value("allow_raw", g_cfg.allow_raw);
        g_cfg.lock_input = j.value("lock_input", g_cfg.lock_input);
        g_cfg.backup_minutes = j.value("backup_minutes", g_cfg.backup_minutes);
        g_cfg.tx_cooldown_ms = j.value("tx_cooldown_ms", g_cfg.tx_cooldown_ms);
    } catch (std::exception& e) {
        log("config.json is not valid JSON (%s) - using defaults.", e.what());
    }
}

void save_config() {
    // (the panel hotkey moved to settings.json - see settings.cpp)
    json j = { {"port", g_cfg.port}, {"allow_raw", g_cfg.allow_raw},
               {"lock_input", g_cfg.lock_input}, {"backup_minutes", g_cfg.backup_minutes}, {"tx_cooldown_ms", g_cfg.tx_cooldown_ms} };
    std::ofstream f(g_cfg.root + "\\config.json");
    f << j.dump(2);
}

// ------------------------------------------------------------------ modules / memory
static std::mutex g_mod_mtx;
static std::vector<ModuleInfo> g_mods;

std::vector<ModuleInfo> list_modules(bool refresh) {
    std::lock_guard<std::mutex> lk(g_mod_mtx);
    if (refresh || g_mods.empty()) {
        g_mods.clear();
        HMODULE mods[2048]; DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 2048; ++i) {
                MODULEINFO mi{}; char name[MAX_PATH]{};
                GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
                GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
                g_mods.push_back({ name, (uintptr_t)mi.lpBaseOfDll, (size_t)mi.SizeOfImage });
            }
        }
    }
    return g_mods;
}

std::string module_name_of(uintptr_t a) {
    for (auto& m : list_modules()) if (a >= m.base && a < m.base + m.size) return m.name;
    return "";
}

uintptr_t module_base(const std::string& name, size_t* size) {
    for (int pass = 0; pass < 2; ++pass)
        for (auto& m : list_modules(pass == 1))
            if (_stricmp(m.name.c_str(), name.c_str()) == 0) { if (size) *size = m.size; return m.base; }
    return 0;
}

uintptr_t game_base(size_t* size) {
    static uintptr_t b = 0; static size_t s = 0;
    if (!b) b = module_base(kGameExe, &s);
    if (size) *size = s;
    return b;
}

bool safe_read(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

bool safe_write(uintptr_t addr, const void* in, size_t n) {
    if (addr < 0x10000) return false;
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    SIZE_T put = 0;
    BOOL okw = WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, in, n, &put);
    DWORD tmp; VirtualProtect((LPVOID)addr, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, n);
    return okw && put == n;
}

std::vector<Region> readable_regions(uintptr_t base, size_t size) {
    std::vector<Region> out;
    uintptr_t a = base, end = base + size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = std::min<uintptr_t>(rs + mbi.RegionSize, end);
        DWORD p = mbi.Protect & 0xFF;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && p != PAGE_NOACCESS;
        bool exec = p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY || p == PAGE_EXECUTE;
        if (readable) out.push_back({ std::max(rs, a), (size_t)(re - std::max(rs, a)), exec });
        a = re;
    }
    return out;
}

// ------------------------------------------------------------------ address expressions
namespace {
struct Parser {
    const std::string& s; size_t i = 0; std::string err;
    explicit Parser(const std::string& src) : s(src) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
    bool term(uintptr_t& v) {
        ws();
        if (i >= s.size()) { err = "unexpected end of expression"; return false; }
        if (s[i] == '[') {
            ++i; uintptr_t inner;
            if (!expr(inner)) return false;
            ws(); if (i >= s.size() || s[i] != ']') { err = "missing ']'"; return false; }
            ++i;
            if (!read_as(inner, v)) { err = "cannot read pointer at " + hexstr(inner); return false; }
            return true;
        }
        if (isdigit((unsigned char)s[i])) {
            size_t st = i; int base = 10;
            if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) { base = 16; i += 2; st = i; }
            while (i < s.size() && isxdigit((unsigned char)s[i])) ++i;
            v = (uintptr_t)strtoull(s.substr(st, i - st).c_str(), nullptr, base);
            return true;
        }
        size_t st = i;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == ':' || s[i] == '-')) {
            if (s[i] == '-' && i > st) break;
            ++i;
        }
        std::string sym = s.substr(st, i - st);
        if (sym.empty()) { err = std::string("unexpected character '") + s[i] + "'"; return false; }
        if (_stricmp(sym.c_str(), "game") == 0) { v = game_base(); if (!v) { err = "game module not found"; return false; } return true; }
        v = module_base(sym);
        if (!v) { err = "unknown symbol or module '" + sym + "'"; return false; }
        return true;
    }
    bool expr(uintptr_t& v) {
        if (!term(v)) return false;
        for (;;) {
            ws();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                char op = s[i++]; uintptr_t r;
                if (!term(r)) return false;
                v = op == '+' ? v + r : v - r;
            } else return true;
        }
    }
};
}

bool eval_addr(const std::string& e, uintptr_t& out, std::string* err) {
    Parser p(e);
    bool okp = p.expr(out);
    if (okp) { p.ws(); if (p.i != e.size()) { p.err = "unexpected text at position " + std::to_string(p.i); okp = false; } }
    if (!okp && err) *err = "address '" + e + "': " + p.err;
    return okp;
}

// ------------------------------------------------------------------ main thread runner
static DWORD g_main_tid = 0;
static std::atomic<uint64_t> g_ticks{0};
static std::mutex g_q_mtx;
static std::vector<std::function<void()>> g_queue;
static std::vector<std::function<void()>> g_tick_fns;
static ULONGLONG g_last_tick_ms = 0;
static thread_local bool t_in_drain = false;

DWORD main_thread_id() { return g_main_tid; }
uint64_t main_ticks() { return g_ticks.load(); }

void add_main_tick(std::function<void()> fn) { std::lock_guard<std::mutex> lk(g_q_mtx); g_tick_fns.push_back(std::move(fn)); }

static void drain_main() {
    if (g_retired || g_features_off) return;
    if (t_in_drain || GetCurrentThreadId() != g_main_tid) return;
    t_in_drain = true;
    ++g_drain_active;
    std::vector<std::function<void()>> work, ticks;
    {
        std::lock_guard<std::mutex> lk(g_q_mtx);
        work.swap(g_queue);
        ULONGLONG now = GetTickCount64();
        if (now - g_last_tick_ms >= 15) { g_last_tick_ms = now; ticks = g_tick_fns; }
    }
    for (auto& w : work) { try { w(); } catch (std::exception& e) { log("main-thread task threw: %s", e.what()); } catch (...) { log("main-thread task threw (unknown)"); } }
    if (!ticks.empty()) { ++g_ticks; for (auto& t : ticks) { if (g_features_off) break; try { t(); } catch (std::exception& e) { log("tick threw: %s", e.what()); } catch (...) { log("tick threw (unknown)"); } } }
    --g_drain_active;
    t_in_drain = false;
}

using PeekMessage_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessage_t o_PeekMessageW = nullptr, o_PeekMessageA = nullptr;
static BOOL WINAPI hk_PeekMessageW(LPMSG m, HWND h, UINT a, UINT b, UINT r) { drain_main(); return o_PeekMessageW(m, h, a, b, r); }
static BOOL WINAPI hk_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r) { drain_main(); return o_PeekMessageA(m, h, a, b, r); }

struct FindWnd { DWORD pid; HWND best; LONG area; };
static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    auto* f = (FindWnd*)lp; DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc; GetWindowRect(h, &rc);
    LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > f->area) { f->area = area; f->best = h; }
    return TRUE;
}
HWND find_game_window() { FindWnd f{ GetCurrentProcessId(), nullptr, 0 }; EnumWindows(enum_cb, (LPARAM)&f); return f.best; }

bool install_main_thread_hook() {
    HWND w = nullptr;
    for (int i = 0; i < 1800 && !w; ++i) { w = find_game_window(); if (!w) Sleep(100); }
    if (!w) { log("Main thread: game window not found after 180 s - main-thread commands disabled."); return false; }
    g_main_tid = GetWindowThreadProcessId(w, nullptr);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    void* pw = GetProcAddress(u32, "PeekMessageW");
    void* pa = GetProcAddress(u32, "PeekMessageA");
    bool okh = MH_CreateHook(pw, &hk_PeekMessageW, (void**)&o_PeekMessageW) == MH_OK &&
               MH_CreateHook(pa, &hk_PeekMessageA, (void**)&o_PeekMessageA) == MH_OK &&
               MH_EnableHook(pw) == MH_OK && MH_EnableHook(pa) == MH_OK;
    log("Main thread hook %s (thread %lu)", okh ? "installed" : "FAILED", g_main_tid);
    // The splash screen owns the first window; the game window appears later on another thread.
    // PeekMessage hooks are process-wide, so following the largest window is just a thread-id switch.
    std::thread([]() {
        for (;;) {
            Sleep(1000);
            if (g_retired || g_features_off) return;
            HWND w2 = find_game_window();
            if (!w2) continue;
            DWORD tid = GetWindowThreadProcessId(w2, nullptr);
            if (tid != g_main_tid) { log("Main thread moved to %lu (game window %p replaced the splash screen)", tid, w2); g_main_tid = tid; }
        }
    }).detach();
    return okh;
}

void post_main(std::function<void()> fn) {
    if (GetCurrentThreadId() == g_main_tid) { fn(); return; }
    std::lock_guard<std::mutex> lk(g_q_mtx);
    g_queue.push_back(std::move(fn));
}

json run_on_main(std::function<json()> fn, DWORD timeout_ms) {
    if (GetCurrentThreadId() == g_main_tid) return fn();
    if (!g_main_tid) return fail("NO_MAIN_THREAD", "Main-thread hook is not installed (game window not found). Check the trainer log.");
    struct State { HANDLE ev; json result; };
    auto st = std::make_shared<State>();
    st->ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lk(g_q_mtx);
        g_queue.push_back([st, fn]() {
            try { st->result = fn(); } catch (std::exception& e) { st->result = fail("EXCEPTION", e.what()); }
            SetEvent(st->ev);
        });
    }
    if (WaitForSingleObject(st->ev, timeout_ms) != WAIT_OBJECT_0)
        return fail("TIMEOUT", "Game main thread did not run the command within " + std::to_string(timeout_ms) +
                    " ms (loading screen or minimized?). It may still run later.");
    CloseHandle(st->ev);
    return st->result;
}



} // namespace wb

// ------------------------------------------------------------------ entry
static DWORD WINAPI init_thread(LPVOID) {
    using namespace wb;
    char path[MAX_PATH]; GetModuleFileNameA(g_self, path, MAX_PATH);
    std::string p = path; g_cfg.root = p.substr(0, p.find_last_of("\\/"));
    std::string leaf = g_cfg.root.substr(g_cfg.root.find_last_of("\\/") + 1);
    if (_stricmp(leaf.c_str(), "loaded") == 0) g_cfg.root = g_cfg.root.substr(0, g_cfg.root.find_last_of("\\/"));
    const std::string logname = "trainer";
    {   // keep the previous sessions' logs (<name>.log -> logs\<name>_<time>.log, newest 15 kept)
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path cur = g_cfg.root + "\\" + logname + ".log", dir = g_cfg.root + "\\logs";
        if (fs::exists(cur, ec) && fs::file_size(cur, ec) > 0) {
            fs::create_directories(dir, ec);
            auto t = fs::last_write_time(cur, ec);
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
            fs::rename(cur, dir / (logname + "_" + std::to_string(secs) + ".log"), ec);
            std::vector<fs::path> all; for (auto& e : fs::directory_iterator(dir, ec)) all.push_back(e.path());
            std::sort(all.begin(), all.end());
            while (all.size() > 15) { fs::remove(all.front(), ec); all.erase(all.begin()); }
        }
    }
    g_log_file = _fsopen((g_cfg.root + "\\" + logname + ".log").c_str(), "w", _SH_DENYNO);
    { char nm[64]; sprintf_s(nm, "Local\\nativeActive_%lu", GetCurrentProcessId()); g_active_marker = CreateMutexA(nullptr, FALSE, nm); }
    log("Dragonkin Trainer %s starting in %s", kVersion, g_cfg.root.c_str());
    load_config();
    settings_load();                 // settings.json: panel hotkey + panel scale
    if (MH_Initialize() != MH_OK) { log("MinHook init failed - cannot hook."); return 0; }
    install_overlay();               // DX12 hooks as early as possible (catches swapchain creation on early attach)
    install_main_thread_hook();
    panel_init();                    // the trainer panel: <game>\Binaries\Win64\ue4ss\Mods\DragonkinTrainer (panel.json + ipc)
    log("Ready. Panel hotkey: %s", hotkey_name(g_settings.hotkey).c_str());
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wb::g_self = h;
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
