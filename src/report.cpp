// report.cpp - "trainer report" for support (SETTINGS > Support), both builds. 2026-11-14.
// A user presses one button and gets a text file (data\reports\trainer_report_<time>.txt, also on the clipboard) to post
// in the Discord support channel. The bot there reads the verdict and the BOT DATA block at the end; a person reads the
// rest. The verdict says whether the trainer itself has a problem:
//   OK      - every trainer check passed: the issue is most likely something else on the PC
//   CHECK   - nothing broken right now, but something to look at (an earlier session ended unexpectedly, co-op, ...)
//   PROBLEM - a trainer check failed (engine core, overlay, a cheat the game keeps undoing, errors in the log,
//             a game build the trainer was not made for) -> flagged for a fix
// Session tracking: data\session.json is written at start and marked clean when the game closes normally; a start that
// finds it unclean records the previous session (its log in logs\) in data\incidents.jsonl - the public build has no
// crash dumps, this is how a crash or a killed game shows up in the report.
// Privacy: the Windows user name, the user profile path, the trainer and game folders are replaced before writing.
#include "common.h"
#include "dk.h"
#include "ue.h"
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace wb {

namespace fs = std::filesystem;

// The game build this trainer release was made and tested for (exe size in bytes -> version shown in the main menu).
// Update both on update day (tools/update_day.py prints the new exe size).
static const uint64_t kMadeForExe = 185137736ULL;
static const struct { uint64_t size; const char* version; } kKnownBuilds[] = {
    { 185137736ULL, "1.7.79.58651 (Harrower update)" },
    { 184976456ULL, "1.6.75.56961" },
};

static std::string data_dir() { return g_cfg.root + "\\data"; }
static std::string now_text() {
    SYSTEMTIME t; GetLocalTime(&t); char b[40];
    sprintf_s(b, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b;
}
static std::string now_stamp() {
    SYSTEMTIME t; GetLocalTime(&t); char b[40];
    sprintf_s(b, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b;
}

// ------------------------------------------------------------------ session tracking
static std::atomic<bool> g_session_closed{ false };

void session_begin() {
    std::error_code ec; fs::create_directories(data_dir(), ec);
    const std::string sp = data_dir() + "\\session.json";
    try {
        std::ifstream f(sp);
        if (f) {
            json prev = json::parse(f, nullptr, false);
            // same process = a dev hot reload (a new copy of the trainer in the running game), not a crash
            if (prev.is_object() && !prev.value("clean", true) && prev.value("pid", 0UL) != (unsigned long)GetCurrentProcessId()) {
                // the previous session never reached a normal close: its log is the newest file in logs\ (just rotated)
                std::string last_log;
                fs::path dir = g_cfg.root + "\\logs"; fs::file_time_type best{};
                for (auto& e : fs::directory_iterator(dir, ec)) {
                    auto t = fs::last_write_time(e.path(), ec);
                    if (last_log.empty() || t > best) { best = t; last_log = e.path().filename().string(); }
                }
                json inc = { {"recorded", now_text()}, {"session_start", prev.value("start", "")}, {"trainer", prev.value("trainer", "")},
                             {"log", last_log}, {"what", "the previous session ended without a normal game close (crash, freeze or the game was killed)"} };
                std::vector<std::string> keep;
                { std::ifstream in(data_dir() + "\\incidents.jsonl"); std::string l; while (std::getline(in, l)) if (!l.empty()) keep.push_back(l); }
                keep.push_back(inc.dump());
                while (keep.size() > 20) keep.erase(keep.begin());
                std::ofstream out(data_dir() + "\\incidents.jsonl", std::ios::trunc); for (auto& l : keep) out << l << "\n";
                log("[report] the previous session ended unexpectedly (log %s) - noted for the trainer report", last_log.c_str());
            }
        }
    } catch (...) {}
    std::ofstream(sp, std::ios::trunc) << json{ {"start", now_text()}, {"trainer", kVersion}, {"pid", (unsigned long)GetCurrentProcessId()}, {"clean", false} }.dump();
}

void session_clean_exit() {
    if (g_session_closed.exchange(true)) return;
    try { std::ofstream(data_dir() + "\\session.json", std::ios::trunc) << json{ {"end", now_text()}, {"trainer", kVersion}, {"clean", true} }.dump(); } catch (...) {}
}

// ------------------------------------------------------------------ helpers
static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static void replace_all_ci(std::string& s, const std::string& what, const std::string& with) {
    if (what.size() < 3) return;
    const std::string lw = lower(what);
    std::string ls = lower(s); size_t p = 0;
    while ((p = ls.find(lw, p)) != std::string::npos) { s.replace(p, what.size(), with); ls.replace(p, what.size(), lower(with)); p += with.size(); }
}
static std::string scrub(std::string s) {
    char buf[MAX_PATH] = {};
    std::string game_dir; if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) { game_dir = buf; for (int i = 0; i < 4 && !game_dir.empty(); ++i) game_dir = game_dir.substr(0, game_dir.find_last_of("\\/")); }
    replace_all_ci(s, g_cfg.root, "<trainer folder>");
    if (game_dir.size() > 3) replace_all_ci(s, game_dir, "<game folder>");
    if (const char* up = getenv("USERPROFILE")) replace_all_ci(s, up, "<user folder>");
    DWORD n = MAX_PATH; if (GetUserNameA(buf, &n) && strlen(buf) >= 3) replace_all_ci(s, buf, "<user>");
    n = MAX_PATH; if (GetComputerNameA(buf, &n) && strlen(buf) >= 3) replace_all_ci(s, buf, "<pc>");
    return s;
}
static std::string windows_version() {
    typedef LONG(WINAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v = { sizeof v };
    if (auto f = (RtlGetVersion_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) f(&v);
    char b[96]; sprintf_s(b, "Windows %s (build %lu)", v.dwBuildNumber >= 22000 ? "11" : "10", v.dwBuildNumber); return b;
}
static void game_exe(uint64_t& size, std::string& file_version) {
    size = 0; file_version = "?";
    char p[MAX_PATH]; if (!GetModuleFileNameA(nullptr, p, MAX_PATH)) return;
    WIN32_FILE_ATTRIBUTE_DATA a; if (GetFileAttributesExA(p, GetFileExInfoStandard, &a)) size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    DWORD h = 0, n = GetFileVersionInfoSizeA(p, &h); if (!n) return;
    std::vector<char> buf(n); VS_FIXEDFILEINFO* fi = nullptr; UINT fl = 0;
    if (GetFileVersionInfoA(p, 0, n, buf.data()) && VerQueryValueA(buf.data(), "\\", (void**)&fi, &fl) && fi) {
        char b[48]; sprintf_s(b, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS)); file_version = b;
    }
}
static bool noteworthy(const std::string& line) {   // log lines that point at a trainer problem
    const std::string l = lower(line);
    if (l.find("[perf]") != std::string::npos || l.find("[report]") != std::string::npos || l.rfind("crash diagnostics on", 0) != std::string::npos
        || l.find("] crash diagnostics on") != std::string::npos) return false;
    static const char* keys[] = { "failed", "error", "exception", "crashed", "gave up", "keeps undoing", "refused", "blocked",
                                  "not found", "did not validate", "cannot", "can't", "invalid", "unexpectedly", "timed out" };
    for (auto k : keys) if (l.find(k) != std::string::npos) return true;
    return false;
}
static std::vector<std::string> read_tail(const fs::path& p, size_t n) {
    std::vector<std::string> v; std::ifstream f(p); std::string l;
    while (std::getline(f, l)) { v.push_back(l); if (v.size() > n) v.erase(v.begin()); }
    return v;
}

// ------------------------------------------------------------------ the report
std::string report_create(std::string* out_path) {
    json problems = json::array(), checks = json::array(), notes = json::array();
    std::ostringstream body;
    auto check = [&](const std::string& name, bool ok, const std::string& detail) {
        checks.push_back({ {"check", name}, {"ok", ok}, {"detail", detail} });
        body << "  [" << (ok ? " OK " : "FAIL") << "] " << name << (detail.empty() ? "" : " - " + detail) << "\n";
        if (!ok) problems.push_back(name + (detail.empty() ? "" : ": " + detail));
    };

    // versions
    uint64_t exe_size = 0; std::string exe_ver; game_exe(exe_size, exe_ver);
    std::string game_version = "unknown build (" + std::to_string(exe_size) + " bytes)";
    for (auto& b : kKnownBuilds) if (b.size == exe_size) game_version = b.version;
    std::string made_for = "?"; for (auto& b : kKnownBuilds) if (b.size == kMadeForExe) made_for = b.version;

    // live state
    json ue_st = json::object(), ov = json::object(), dk_st = json::object();
    try { ue_st = ue::status(); } catch (...) {}
    try { ov = overlay_status(); } catch (...) {}
    try { dk_st = dk::state(); } catch (...) {}
    const json keeper = dk_st.value("keeper", json::object());
    const json player = dk_st.value("player", json::object());
    const json readouts = dk_st.value("readouts", json::object());
    const bool in_game = player.is_object() && !player.value("pawn", std::string()).empty();
    const std::string solo = readouts.is_object() ? readouts.value("r_solo", std::string("?")) : "?";

    body << "CHECKS\n";
    check("Game build", exe_size == kMadeForExe, exe_size == kMadeForExe ? game_version
          : "this game build (" + game_version + ") is not the one this trainer was made for (" + made_for + ") - the trainer may need an update");
    { const bool ok = ue_st.value("ready", false);
      check("Engine core (game internals found)", ok, ok ? std::to_string(ue_st.value("objects", 0)) + " game objects" : ue_st.value("error", std::string("not ready yet"))); }
    const bool second_copy = ov.value("role", std::string()).find("client") != std::string::npos;   // dev reload: the first copy draws + owns input
    if (second_copy) check("Overlay and input", true, "handled by the first trainer copy in this game (dev reload)");
    else {
        const bool hooks = ov.value("hooks", false); const uint64_t frames = ov.value("frames_drawn", (uint64_t)0);
        check("Overlay (panel drawing)", hooks && frames > 0, hooks ? std::to_string(frames) + " frames drawn" : "graphics hooks failed");
        check("Input", ov.value("input_attached", false), ov.value("input_attached", false) ? "keyboard and mouse reach the panel" : "the panel is not attached to the game window");
    }
    if (in_game) {
        const bool ready = keeper.value("ready", false);
        check("Cheat keeper on this character", ready, ready ? std::to_string(keeper.value("reapplied", 0)) + " features applied, " + std::to_string(keeper.value("zone_changes", 0)) + " zone changes handled" : "not ready on this character");
        const json errs = keeper.value("errors", json::object());
        if (errs.is_object() && !errs.empty())
            for (auto it = errs.begin(); it != errs.end(); ++it) check("Cheat '" + it.key() + "'", false, it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
        else check("Cheats switched on", true, "no cheat reports a problem");
    } else {
        notes.push_back("No character was loaded when the report was made - load your character first for a complete report.");
        body << "  [ -- ] Cheat keeper - no character loaded (load one and make the report again for the full check)\n";
    }
    if (lower(solo) != "yes" && solo != "?") notes.push_back("Not a solo session: every cheat switches itself off in co-op (that is by design).");

    // log of this session
    std::vector<std::string> warn;
    for (auto& l : log_tail(800)) if (noteworthy(l)) { std::string s = scrub(l); if (std::find(warn.begin(), warn.end(), s) == warn.end()) warn.push_back(s); }
    if (warn.size() > 30) warn.erase(warn.begin(), warn.end() - 30);
    if (!warn.empty()) problems.push_back(std::to_string(warn.size()) + " warning/error line(s) in this session's trainer log");

    // earlier sessions that ended unexpectedly
    std::vector<json> incidents;
    { std::ifstream f(data_dir() + "\\incidents.jsonl"); std::string l;
      // only the last 7 days: an old crash that was already looked at should not mark every later report
      SYSTEMTIME st; GetLocalTime(&st); std::tm tmv = {}; tmv.tm_year = st.wYear - 1900; tmv.tm_mon = st.wMonth - 1; tmv.tm_mday = st.wDay - 7; std::mktime(&tmv);
      char cutoff[16]; sprintf_s(cutoff, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
      while (std::getline(f, l)) { json j = json::parse(l, nullptr, false); if (j.is_object() && j.value("recorded", std::string()) >= cutoff) incidents.push_back(j); } }
    if (incidents.size() > 5) incidents.erase(incidents.begin(), incidents.end() - 5);
    if (!incidents.empty()) notes.push_back(std::to_string(incidents.size()) + " earlier session(s) ended without a normal game close (crash, freeze or killed) - see EARLIER SESSIONS.");

    // verdict
    std::string verdict = problems.empty() ? (incidents.empty() && notes.empty() ? "OK" : "CHECK") : "PROBLEM";
    if (verdict == "CHECK" && incidents.empty()) verdict = "OK";   // notes alone (no character / co-op) are advice, not a problem
    std::string verdict_text =
        verdict == "OK" ? "TRAINER OK - every trainer check passed. If something is wrong, it is most likely not the trainer." :
        verdict == "CHECK" ? "TRAINER CHECK - the trainer works right now, but an earlier session ended unexpectedly. Someone will look at it." :
                             "TRAINER PROBLEM - a trainer check failed (see below). This is flagged for a fix.";

    // active cheats
    std::string active;
    { const json act = keeper.value("active", json::object());
      int talents = 0;
      if (act.is_object()) for (auto it = act.begin(); it != act.end(); ++it) {
          if (it.key().rfind("talent:", 0) == 0) { ++talents; continue; }
          if (!active.empty()) active += ", "; active += it.key();
          if (it.value().is_number() && it.value().get<double>() != 1.0) { char b[32]; sprintf_s(b, "=%g", it.value().get<double>()); active += b; } }
      if (talents) { if (!active.empty()) active += ", "; active += std::to_string(talents) + " talent(s) raised"; } }

    std::ostringstream out;
    out << "=== DRAGONKIN TRAINER REPORT ===\n"
        << verdict_text << "\n\n"
        << "Trainer:   Dragonkin Trainer " << kVersion
        << "\n"
        << "Game:      Dragonkin: The Banished " << game_version << " (engine " << exe_ver << ")\n"
        << "Made for:  " << made_for << "\n"
        << "System:    " << windows_version() << "\n"
        << "Session:   " << (solo == "yes" ? "solo" : solo == "?" ? "unknown" : "co-op") << ", "
        << (in_game ? "character loaded (" + player.value("pawn", std::string()) + " in " + player.value("world", std::string()) + ")" : "no character loaded") << "\n"
        << "Made:      " << now_text() << "\n\n"
        << body.str() << "\n";
    if (!notes.empty()) { out << "NOTES\n"; for (auto& n : notes) out << "  - " << n.get<std::string>() << "\n"; out << "\n"; }
    out << "CHEATS SWITCHED ON\n  " << (active.empty() ? "(none)" : active) << "\n\n";
    out << "WARNINGS AND ERRORS IN THIS SESSION'S LOG\n";
    if (warn.empty()) out << "  (none)\n"; else for (auto& w : warn) out << "  " << w << "\n";
    out << "\n";
    if (!incidents.empty()) {
        out << "EARLIER SESSIONS THAT ENDED UNEXPECTEDLY\n";
        for (auto& inc : incidents) {
            out << "  * session started " << inc.value("session_start", std::string("?")) << " (trainer " << inc.value("trainer", std::string("?")) << ")\n";
            const std::string lf = inc.value("log", std::string());
            if (!lf.empty()) { auto tail = read_tail(fs::path(g_cfg.root) / "logs" / lf, 25); out << "    last lines of its log:\n"; for (auto& l : tail) out << "      " << scrub(l) << "\n"; }
        }
        out << "\n";
    }
    out << "LAST LINES OF THIS SESSION'S LOG\n";
    { auto t = log_tail(40); for (auto& l : t) out << "  " << scrub(l) << "\n"; }
    out << "\n";

    json bot = { {"kind", "trainer_report"}, {"format", 1}, {"game", "dragonkin"}, {"trainer", "Dragonkin Trainer"},
                 {"trainer_version", kVersion}, {"build", WB_PUBLIC ? "public" : "dev"}, {"verdict", verdict},
                 {"game_version", game_version}, {"made_for", made_for}, {"game_build_match", exe_size == kMadeForExe},
                 {"exe_size", exe_size}, {"windows", windows_version()}, {"character_loaded", in_game}, {"solo", solo},
                 {"problems", problems}, {"notes", notes}, {"checks", checks}, {"log_warnings", (int)warn.size()},
                 {"unexpected_ends", (int)incidents.size()}, {"made", now_text()} };
    out << "----- BOT DATA (please do not edit) -----\n" << scrub(bot.dump()) << "\n----- END BOT DATA -----\n";

    const std::string text = scrub(out.str());
    std::error_code ec; fs::create_directories(data_dir() + "\\reports", ec);
    const std::string path = data_dir() + "\\reports\\trainer_report_" + now_stamp() + ".txt";
    { std::ofstream f(path, std::ios::binary); f << text; }
    // newest 10 kept
    { std::vector<fs::path> all; for (auto& e : fs::directory_iterator(data_dir() + "\\reports", ec)) all.push_back(e.path());
      std::sort(all.begin(), all.end()); while (all.size() > 10) { fs::remove(all.front(), ec); all.erase(all.begin()); } }
    log("[report] trainer report written: %s (verdict %s)", fs::path(path).filename().string().c_str(), verdict.c_str());
    if (out_path) *out_path = path;
    return text;
}

bool clipboard_set(const std::string& utf8) {
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0); if (wn <= 0) return false;
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)wn * sizeof(wchar_t)); bool ok = false;
    if (h) { if (void* p = GlobalLock(h)) { MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, (wchar_t*)p, wn); GlobalUnlock(h); ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr; } if (!ok) GlobalFree(h); }
    CloseClipboard(); return ok;
}

void show_in_explorer(const std::string& path) {
    const std::string args = "/select,\"" + path + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace wb
