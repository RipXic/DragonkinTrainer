// ue.cpp - native UE 5.5 reflection (see ue.h). No UE4SS: everything is located in the game exe at start-up.
#include "ue.h"
#include <fstream>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace ue {

// ------------------------------------------------------------------ guarded memory
static int seh_copy(void* dst, const void* src, size_t n) { __try { memcpy(dst, src, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
bool mread(uintptr_t a, void* out, size_t n) { if (a < 0x10000 || !out) return false; return seh_copy(out, (const void*)a, n) != 0; }
bool mwrite(uintptr_t a, const void* in, size_t n) { if (a < 0x10000 || !in) return false; return seh_copy((void*)a, in, n) != 0; }

// ------------------------------------------------------------------ state
static std::atomic<bool> g_ready{false};
static std::mutex g_init_mtx;
static uintptr_t g_base = 0; static size_t g_size = 0;
static uintptr_t g_text = 0; static size_t g_text_size = 0;
static uintptr_t g_objobjects = 0;      // GUObjectArray + 0x10 = FChunkedFixedUObjectArray
static uintptr_t g_namepool = 0;        // FNamePool (FNameEntryAllocator: lock, CurrentBlock +8, cursor +0xC, Blocks +0x10)
static uintptr_t g_process_event = 0;   // UObject::ProcessEvent
static int g_pe_slot = -1;              // its vtable slot (fallback when the pattern breaks after an update)
static int g_item_stride = 0x18;        // FUObjectItem size
static uintptr_t g_cls_package = 0, g_cls_class = 0, g_cls_function = 0, g_cls_scriptstruct = 0;
static json g_found = json::object();
static std::string g_journal_dir;
static thread_local bool t_journal = false;
static std::unordered_map<uintptr_t, bool> g_blocked_fn;
static std::vector<std::string> g_blocked_paths;

static constexpr int kChunk = 64 * 1024;

// ------------------------------------------------------------------ pattern scanning (in the loaded exe)
static bool text_section() {
    g_base = game_base(&g_size);
    if (!g_base) return false;
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!rd_as(g_base, dos) || !rd_as(g_base + dos.e_lfanew, nt)) return false;
    uintptr_t sh = g_base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (int i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER s; if (!rd_as(sh + i * sizeof s, s)) return false;
        if (memcmp(s.Name, ".text", 5) == 0) { g_text = g_base + s.VirtualAddress; g_text_size = s.Misc.VirtualSize; return true; }
    }
    return false;
}

// all matches of pattern in .text (direct reads: .text is mapped and readable for the whole game session)
static std::vector<uintptr_t> scan(const std::string& pattern, size_t max) {
    std::vector<int> pat; std::vector<uintptr_t> out;
    if (!parse_pattern(pattern, pat) || pat.empty() || pat[0] < 0 || !g_text) return out;
    const uint8_t* p = (const uint8_t*)g_text; const size_t n = g_text_size, m = pat.size();
    const uint8_t first = (uint8_t)pat[0];
    for (size_t i = 0; i + m <= n; ) {
        const void* hit = memchr(p + i, first, n - m + 1 - i);
        if (!hit) break;
        i = (const uint8_t*)hit - p;
        bool okm = true;
        for (size_t k = 1; k < m; ++k) if (pat[k] >= 0 && p[i + k] != (uint8_t)pat[k]) { okm = false; break; }
        if (okm) { out.push_back(g_text + i); if (max && out.size() >= max) break; }
        ++i;
    }
    return out;
}

// rip-relative target most matches agree on: pattern hit + disp_at -> [disp32] -> next instruction + extra
static uintptr_t vote(const std::vector<std::string>& patterns, int disp_at, int extra, const char* what, size_t max_hits = 400) {
    std::map<uintptr_t, int> votes; int total = 0;
    for (auto& pat : patterns) {
        for (uintptr_t h : scan(pat, max_hits)) {
            int32_t d = 0; if (!rd_as(h + disp_at, d)) continue;
            ++votes[h + disp_at + 4 + extra + d]; ++total;
        }
    }
    uintptr_t best = 0; int bn = 0;
    for (auto& kv : votes) if (kv.second > bn) { bn = kv.second; best = kv.first; }
    g_found[what] = { {"rva", best ? hexstr(best - g_base) : "-"}, {"votes", bn}, {"matches", total}, {"candidates", votes.size()} };
    return best;
}

// ------------------------------------------------------------------ names (FNamePool, UE 5.5 layout)
static std::shared_mutex g_name_mtx;
static std::unordered_map<uint32_t, std::string> g_name_cache;
static std::unordered_map<std::string, uint32_t> g_name_rev;    // built on first name_find
static bool g_rev_built = false;
static uint32_t g_rev_block = 0, g_rev_off = 0;                     // incremental: FNamePool only ever appends
static Perf g_perf;
Perf& perf() { return g_perf; }
double now_ms() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

static bool entry_at(uint32_t index, std::string& out) {
    const uint32_t block = index >> 16, offs = (index & 0xFFFF) * 2;
    uintptr_t blk = 0; if (!rd_as(g_namepool + 0x10 + block * 8, blk) || !blk) return false;
    uint16_t hdr = 0; if (!rd_as(blk + offs, hdr)) return false;
    const bool wide = hdr & 1; const uint32_t len = hdr >> 6;
    if (len == 0 || len > 1024) return false;
    if (!wide) { out.resize(len); return mread(blk + offs + 2, out.data(), len); }
    std::wstring w(len, L'\0'); if (!mread(blk + offs + 2, w.data(), len * 2)) return false;
    int u = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, nullptr, 0, nullptr, nullptr);
    out.resize(u); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, out.data(), u, nullptr, nullptr);
    return true;
}

std::string name_str(FName n) {
    std::string base;
    { std::shared_lock lk(g_name_mtx); auto it = g_name_cache.find(n.index); if (it != g_name_cache.end()) base = it->second; }
    if (base.empty()) {
        if (!g_namepool || !entry_at(n.index, base)) return "";
        std::unique_lock lk(g_name_mtx); if (g_name_cache.size() > 400000) g_name_cache.clear(); g_name_cache[n.index] = base;
    }
    if (n.number) base += "_" + std::to_string(n.number - 1);
    return base;
}

// Reverse name map, built INCREMENTALLY: the pool only appends, so after the first full pass a miss reads just the
// names added since (was: re-read the whole ~1M-name pool on every miss = long game-thread hitch; 2026-11-09).
static void build_rev() {
    const double t0 = now_ms();
    uint32_t cur_block = 0, cursor = 0;
    rd_as(g_namepool + 0x8, cur_block); rd_as(g_namepool + 0xC, cursor);
    std::vector<std::pair<std::string, uint32_t>> add; if (!g_rev_built) add.reserve(1 << 20);
    std::vector<uint8_t> blockbuf(0x20000);
    uint32_t b = g_rev_block, start = g_rev_off, end_b = g_rev_block, end_o = g_rev_off;
    for (; b <= cur_block && b < 8192; ++b, start = 0) {
        uintptr_t blk = 0; if (!rd_as(g_namepool + 0x10 + b * 8, blk) || !blk) break;
        const uint32_t limit = b == cur_block ? cursor : 0x20000;
        if (limit <= start) { end_b = b; end_o = start; continue; }
        if (!mread(blk + start, blockbuf.data() + start, limit - start)) break;
        uint32_t o = start;
        for (; o + 2 <= limit; ) {
            uint16_t hdr; memcpy(&hdr, blockbuf.data() + o, 2);
            const uint32_t len = hdr >> 6; const bool wide = hdr & 1;
            if (len == 0) break;
            const uint32_t bytes = len * (wide ? 2 : 1);
            if (o + 2 + bytes > limit) break;
            if (!wide) add.emplace_back(std::string((const char*)blockbuf.data() + o + 2, len), (b << 16) | (o / 2));
            o += (2 + bytes + 1) & ~1u;
        }
        end_b = b; end_o = o;
        if (b < cur_block) { end_b = b + 1; end_o = 0; }
    }
    std::unique_lock lk(g_name_mtx);
    if (!g_rev_built) g_name_rev.reserve(add.size() + (1 << 16));
    for (auto& kv : add) g_name_rev.emplace(std::move(kv.first), kv.second);
    g_rev_block = end_b; g_rev_off = end_o; g_rev_built = true;
    ++g_perf.name_builds; g_perf.name_ms += now_ms() - t0;
}

bool name_find(const std::string& s, FName& out) {
    std::string base = s; uint32_t number = 0;
    // "Foo_3" is stored as "Foo" + number 4 (only when the suffix has no leading zero)
    size_t us = s.find_last_of('_');
    if (us != std::string::npos && us + 1 < s.size() && s[us + 1] != '0' && s.find_first_not_of("0123456789", us + 1) == std::string::npos && s.size() - us <= 10) {
        std::shared_lock lk(g_name_mtx);
        std::string b2 = s.substr(0, us);
        if (g_rev_built && g_name_rev.count(b2) && !g_name_rev.count(s)) { base = b2; number = (uint32_t)std::stoul(s.substr(us + 1)) + 1; }
    }
    for (int pass = 0; pass < 2; ++pass) {
        { std::shared_lock lk(g_name_mtx);
          auto it = g_name_rev.find(base);
          if (it != g_name_rev.end()) { out.index = it->second; out.number = number; return true; } }
        build_rev();   // new names appear while the game runs: rebuild once and retry
    }
    return false;
}

// ------------------------------------------------------------------ objects
int32_t num_objects() { int32_t n = 0; if (g_objobjects) rd_as(g_objobjects + 0x14, n); return n; }

uintptr_t object_at(int32_t i) {
    if (!g_objobjects || i < 0) return 0;
    uintptr_t chunks = 0; if (!rd_as(g_objobjects, chunks) || !chunks) return 0;
    uintptr_t chunk = 0; if (!rd_as(chunks + (i / kChunk) * 8, chunk) || !chunk) return 0;
    uintptr_t obj = 0; rd_as(chunk + (uintptr_t)(i % kChunk) * g_item_stride, obj);
    return obj;
}

bool alive(uintptr_t obj) {
    if (!obj) return false;
    int32_t idx = -1; if (!rd_as(obj + off::Obj_Index, idx)) return false;
    return object_at(idx) == obj;
}

uintptr_t class_of(uintptr_t obj) { uintptr_t c = 0; if (obj) rd_as(obj + off::Obj_Class, c); return c; }
uintptr_t outer_of(uintptr_t obj) { uintptr_t o = 0; if (obj) rd_as(obj + off::Obj_Outer, o); return o; }
std::string name_of(uintptr_t obj) { FName n; if (!obj || !rd_as(obj + off::Obj_Name, n)) return ""; return name_str(n); }
bool is_cdo(uintptr_t obj) { uint32_t f = 0; rd_as(obj + off::Obj_Flags, f); return (f & (RF_ClassDefaultObject | RF_ArchetypeObject)) != 0; }   // CDOs + their template subobjects
uintptr_t super_of(uintptr_t s) { uintptr_t p = 0; if (s) rd_as(s + off::Struct_Super, p); return p; }

static void path_rec(uintptr_t obj, std::string& out, int depth) {
    uintptr_t outer = outer_of(obj);
    if (outer && depth < 32) {
        path_rec(outer, out, depth + 1);
        const uintptr_t oo = outer_of(outer);
        out += (class_of(outer) != g_cls_package && oo && class_of(oo) == g_cls_package) ? ":" : ".";
    }
    out += name_of(obj);
}
std::string path_of(uintptr_t obj) { std::string s; if (obj) path_rec(obj, s, 0); return s; }
std::string full_name(uintptr_t obj) { return obj ? name_of(class_of(obj)) + " " + path_of(obj) : ""; }

bool is_child_of(uintptr_t cls, uintptr_t parent) {
    for (int i = 0; cls && i < 64; ++i, cls = super_of(cls)) if (cls == parent) return true;
    return false;
}
bool is_a(uintptr_t obj, uintptr_t cls) { return obj && cls && is_child_of(class_of(obj), cls); }

// name -> object indices (rebuilt when a lookup misses; the array only grows / reuses slots)
static std::mutex g_idx_mtx;
static std::unordered_multimap<std::string, int32_t> g_by_name;
static int32_t g_indexed = 0;
static void index_objects(bool full) {
    const int32_t n = num_objects();
    if (full) {                                   // slots get reused after GC: full rebuild, but at most every 15 s
        static double last = -1e9; const double t = now_ms();
        if (t - last < 15000) full = false; else last = t;
    }
    if (!full && n == g_indexed) return;
    const double t0 = now_ms();
    if (full) { g_by_name.clear(); g_indexed = 0; }
    g_by_name.reserve(n + 1024);
    for (int32_t i = g_indexed; i < n; ++i) { uintptr_t o = object_at(i); if (o) g_by_name.emplace(name_of(o), i); }
    g_indexed = n;
    ++g_perf.index_builds; g_perf.index_ms += now_ms() - t0;
}
static std::vector<uintptr_t> objects_named(const std::string& name, bool allow_full = true) {
    std::vector<uintptr_t> out;
    for (int pass = 0; pass < (allow_full ? 2 : 1) && out.empty(); ++pass) {
        std::lock_guard<std::mutex> lk(g_idx_mtx);
        index_objects(pass == 1);
        auto r = g_by_name.equal_range(name);
        for (auto it = r.first; it != r.second; ++it) { uintptr_t o = object_at(it->second); if (o && name_of(o) == name) out.push_back(o); }
    }
    return out;
}

uintptr_t find_object(const std::string& path, bool allow_full_reindex) {
    size_t k = path.find_last_of(".:");
    const std::string leaf = k == std::string::npos ? path : path.substr(k + 1);
    for (uintptr_t o : objects_named(leaf, allow_full_reindex)) if (path_of(o) == path) return o;
    return 0;
}

std::vector<uintptr_t> find_code(const std::string& pattern, size_t max) { init(); return scan(pattern, max); }

// ------------------------------------------------------------------ GMalloc
// Found 2026-11-09 (build 184976456, exe+0xA662FD0): the inlined FMemory pattern "if (!GMalloc) GCreateMalloc();
// GMalloc->vfunc(+0x48)" - 176 sites, one target. Realloc = vtable slot 7 (verified by tools/grid.py through UE4SS).
static uintptr_t gmalloc_global() {
    static uintptr_t g = 0; static bool tried = false;
    if (!tried) { tried = true;
        g = vote({ "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 48" }, 3, 0, "GMalloc", 1000); }
    return g;
}
static void* realloc_seh(void* inst, void* fn, void* ptr, size_t bytes, uint32_t align) {
    using F = void* (*)(void*, void*, size_t, uint32_t);
    __try { return ((F)fn)(inst, ptr, bytes, align); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* game_realloc(void* ptr, size_t bytes, uint32_t alignment) {
    if (GetCurrentThreadId() != main_thread_id()) return nullptr;
    const uintptr_t g = gmalloc_global(); uintptr_t inst = 0, vt = 0, fn = 0;
    if (!g || !rd_as(g, inst) || !inst || !rd_as(inst, vt) || !rd_as(vt + 7 * 8, fn) || !fn) return nullptr;
    if (fn < g_text || fn >= g_text + g_text_size) return nullptr;           // must be game code
    return realloc_seh((void*)inst, (void*)fn, ptr, bytes, alignment);
}

static uintptr_t find_typed(const std::string& name_in, uintptr_t meta_or_0, bool want_class) {
    if (name_in.find('/') != std::string::npos) return find_object(name_in);
    std::vector<std::string> cands = { name_in };
    if (name_in.size() > 2 && (name_in[0] == 'U' || name_in[0] == 'A' || name_in[0] == 'F') && isupper((unsigned char)name_in[1])) cands.push_back(name_in.substr(1));
    uintptr_t fallback = 0;
    for (auto& n : cands) for (uintptr_t o : objects_named(n)) {
        const uintptr_t c = class_of(o);
        const bool ok = want_class ? is_child_of(c, g_cls_class) : (meta_or_0 ? is_child_of(c, meta_or_0) : true);
        if (!ok) continue;
        const std::string p = path_of(o);
        if (p.rfind("/Script/", 0) == 0) return o;          // native first
        if (!fallback) fallback = o;
    }
    return fallback;
}
uintptr_t find_class(const std::string& name) { return find_typed(name, 0, true); }
uintptr_t find_struct(const std::string& name) { return find_typed(name, g_cls_scriptstruct, false); }
uintptr_t cdo_of(uintptr_t cls) { uintptr_t d = 0; if (cls) rd_as(cls + off::Class_CDO, d); return d; }

std::vector<uintptr_t> find_all(uintptr_t cls, bool include_cdo, size_t max) {
    std::vector<uintptr_t> out; if (!cls) return out;
    const double t0 = now_ms();
    struct Done { double t0; ~Done() { ++g_perf.scans; g_perf.scan_ms += now_ms() - t0; } } done{ t0 };
    const int32_t n = num_objects();
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t o = object_at(i); if (!o) continue;
        if (!is_child_of(class_of(o), cls)) continue;
        if (!include_cdo && is_cdo(o)) continue;
        out.push_back(o); if (max && out.size() >= max) break;
    }
    return out;
}
uintptr_t find_first(uintptr_t cls) { auto v = find_all(cls, false, 1); return v.empty() ? 0 : v[0]; }

// ------------------------------------------------------------------ properties
static std::string ffield_class_name(uintptr_t f) {
    uintptr_t fc = 0; if (!rd_as(f + off::FField_Class, fc) || !fc) return "";
    FName n; if (!rd_as(fc, n)) return "";
    return name_str(n);
}
Prop prop_info(uintptr_t f) {
    Prop p; if (!f) return p;
    p.p = f;
    FName n; rd_as(f + off::FField_Name, n); p.name = name_str(n);
    p.type = ffield_class_name(f);
    rd_as(f + off::Prop_Offset, p.offset); rd_as(f + off::Prop_ElemSize, p.size); rd_as(f + off::Prop_ArrayDim, p.dim);
    rd_as(f + off::Prop_Flags, p.flags);
    if (p.type == "StructProperty") rd_as(f + off::Prop_Struct, p.owner_struct);
    else if (p.type == "ObjectProperty" || p.type == "ClassProperty" || p.type == "WeakObjectProperty" || p.type == "SoftObjectProperty") rd_as(f + off::Prop_ObjClass, p.owner_struct);
    else if (p.type == "ArrayProperty") rd_as(f + off::Prop_ArrayInner, p.owner_struct);
    return p;
}
std::vector<Prop> props(uintptr_t s, bool with_super) {
    std::vector<Prop> out;
    for (int depth = 0; s && depth < 64; ++depth, s = with_super ? super_of(s) : 0) {
        uintptr_t f = 0; rd_as(s + off::Struct_ChildProps, f);
        for (int i = 0; f && i < 4096; ++i) { out.push_back(prop_info(f)); rd_as(f + off::FField_Next, f); }
        if (!with_super) break;
    }
    return out;
}
Prop find_prop(uintptr_t s, const std::string& name) {
    for (int depth = 0; s && depth < 64; ++depth, s = super_of(s)) {
        uintptr_t f = 0; rd_as(s + off::Struct_ChildProps, f);
        for (int i = 0; f && i < 4096; ++i) {
            FName n; rd_as(f + off::FField_Name, n);
            if (name_str(n) == name) return prop_info(f);
            rd_as(f + off::FField_Next, f);
        }
    }
    return Prop();
}
std::vector<uintptr_t> funcs(uintptr_t cls, bool with_super) {
    std::vector<uintptr_t> out;
    for (int depth = 0; cls && depth < 64; ++depth, cls = with_super ? super_of(cls) : 0) {
        uintptr_t f = 0; rd_as(cls + off::Struct_Children, f);
        for (int i = 0; f && i < 8192; ++i) { if (is_child_of(class_of(f), g_cls_function)) out.push_back(f); rd_as(f + off::Field_Next, f); }
        if (!with_super) break;
    }
    return out;
}
uintptr_t find_func(uintptr_t cls, const std::string& name) {
    for (int depth = 0; cls && depth < 64; ++depth, cls = super_of(cls)) {
        uintptr_t f = 0; rd_as(cls + off::Struct_Children, f);
        for (int i = 0; f && i < 8192; ++i) {
            if (name_of(f) == name && is_child_of(class_of(f), g_cls_function)) return f;
            rd_as(f + off::Field_Next, f);
        }
    }
    return 0;
}
Prop array_inner(const Prop& arr) { return prop_info(arr.owner_struct); }
bool map_kv(const Prop& map, Prop& key, Prop& val, int32_t& value_offset, int32_t& stride) {
    uintptr_t k = 0, v = 0; if (!rd_as(map.p + off::Prop_MapKey, k) || !rd_as(map.p + off::Prop_MapValue, v)) return false;
    key = prop_info(k); val = prop_info(v);
    int32_t layout[4] = {}; if (!mread(map.p + off::Prop_MapLayout, layout, sizeof layout)) return false;
    value_offset = layout[0]; stride = layout[3];       // FScriptMapLayout { ValueOffset; SetLayout { HashNextIdOffset; HashIndexOffset; Size } }
    return stride > 0 && stride < 0x10000;
}

bool get_bool(uintptr_t base, const Prop& p) {
    uint8_t off_ = 0, mask = 0; rd_as(p.p + off::Prop_BoolByteOffset, off_); rd_as(p.p + off::Prop_BoolFieldMask, mask);
    uint8_t b = 0; rd_as(base + p.offset + off_, b); return (b & mask) != 0;
}
bool set_bool(uintptr_t base, const Prop& p, bool v) {
    uint8_t off_ = 0, mask = 0; rd_as(p.p + off::Prop_BoolByteOffset, off_); rd_as(p.p + off::Prop_BoolFieldMask, mask);
    uint8_t b = 0; if (!rd_as(base + p.offset + off_, b)) return false;
    b = v ? (b | mask) : (b & ~mask); return mwrite(base + p.offset + off_, &b, 1);
}
double get_num(uintptr_t base, const Prop& p) {
    const auto& t = p.type;
    if (t == "BoolProperty") return get_bool(base, p) ? 1 : 0;
    if (t == "FloatProperty") return get<float>(base, p);
    if (t == "DoubleProperty") return get<double>(base, p);
    if (t == "IntProperty") return get<int32_t>(base, p);
    if (t == "Int64Property") return (double)get<int64_t>(base, p);
    if (t == "UInt32Property") return get<uint32_t>(base, p);
    if (t == "Int16Property") return get<int16_t>(base, p);
    if (t == "ByteProperty" || t == "Int8Property") return get<uint8_t>(base, p);
    if (t == "EnumProperty") { if (p.size == 1) return get<uint8_t>(base, p); if (p.size == 2) return get<uint16_t>(base, p); return get<int32_t>(base, p); }
    return 0;
}
bool set_num(uintptr_t base, const Prop& p, double v) {
    const auto& t = p.type;
    if (t == "BoolProperty") return set_bool(base, p, v != 0);
    if (t == "FloatProperty") return set<float>(base, p, (float)v);
    if (t == "DoubleProperty") return set<double>(base, p, v);
    if (t == "IntProperty") return set<int32_t>(base, p, (int32_t)llround(v));
    if (t == "Int64Property") return set<int64_t>(base, p, (int64_t)llround(v));
    if (t == "UInt32Property") return set<uint32_t>(base, p, (uint32_t)llround(v));
    if (t == "Int16Property") return set<int16_t>(base, p, (int16_t)llround(v));
    if (t == "ByteProperty" || t == "Int8Property") return set<uint8_t>(base, p, (uint8_t)llround(v));
    if (t == "EnumProperty") { if (p.size == 1) return set<uint8_t>(base, p, (uint8_t)llround(v)); if (p.size == 2) return set<uint16_t>(base, p, (uint16_t)llround(v)); return set<int32_t>(base, p, (int32_t)llround(v)); }
    return false;
}

bool resolve(uintptr_t base, uintptr_t s, const std::string& dotted, uintptr_t& out_base, Prop& leaf) {
    size_t a = 0;
    while (true) {
        size_t b = dotted.find('.', a);
        const std::string part = dotted.substr(a, b == std::string::npos ? std::string::npos : b - a);
        Prop p = find_prop(s, part); if (!p) return false;
        if (b == std::string::npos) { out_base = base; leaf = p; return true; }
        if (p.type != "StructProperty") return false;
        base += p.offset; s = p.owner_struct; a = b + 1;
    }
}

TArray<uint8_t> array_of(uintptr_t base, const Prop& arr) { TArray<uint8_t> a; if (arr) mread(base + arr.offset, &a, sizeof a); return a; }

std::vector<std::pair<uintptr_t, uintptr_t>> map_entries(uintptr_t base, const Prop& map) {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    Prop k, v; int32_t voff = 0, stride = 0;
    if (!map_kv(map, k, v, voff, stride)) return out;
    // TSparseArray: TArray Data (+0x00), TBitArray AllocationFlags (+0x10: uint32 Inline[4], uint32* Secondary, int NumBits, int MaxBits)
    const uintptr_t m = base + map.offset;
    TArray<uint8_t> data; if (!mread(m, &data, sizeof data) || !data.data || data.num <= 0 || data.num > 1000000) return out;
    uint32_t inl[4] = {}; uintptr_t secondary = 0; int32_t numbits = 0, maxbits = 0;
    mread(m + 0x10, inl, 16); rd_as(m + 0x20, secondary); rd_as(m + 0x28, numbits); rd_as(m + 0x2C, maxbits);
    std::vector<uint32_t> bits((data.num + 31) / 32);
    if (secondary) mread(secondary, bits.data(), bits.size() * 4);
    else for (size_t i = 0; i < bits.size() && i < 4; ++i) bits[i] = inl[i];
    for (int32_t i = 0; i < data.num; ++i) {
        if (!(bits[i / 32] & (1u << (i % 32)))) continue;
        const uintptr_t e = (uintptr_t)data.data + (uintptr_t)i * stride;
        out.push_back({ e, e + voff });
    }
    return out;
}

// ------------------------------------------------------------------ strings
std::string fstring(uintptr_t a) {
    TArray<wchar_t> s; if (!a || !mread(a, &s, sizeof s) || !s.data || s.num <= 0 || s.num > 1 << 20) return "";
    std::wstring w(s.num, L'\0'); if (!mread((uintptr_t)s.data, w.data(), s.num * 2)) return "";
    while (!w.empty() && w.back() == 0) w.pop_back();
    int u = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(u, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), u, nullptr, nullptr);
    return out;
}
std::string ftext(uintptr_t a) {
    static uintptr_t lib = 0, fn = 0;
    if (!fn) { lib = find_object("/Script/Engine.Default__KismetTextLibrary"); fn = find_object("/Script/Engine.KismetTextLibrary:Conv_TextToString"); }
    if (!lib || !fn || !a) return "";
    Params p(fn); if (!p) return "";
    Prop in = p.param("InText"); if (!in) return "";
    mread(a, p.at(in), std::min<size_t>(in.size, 0x18));
    if (!call(lib, p)) return "";
    uint8_t* r = p.ret_ptr(); if (!r) return "";
    return fstring((uintptr_t)r);   // (the returned FString buffer is left to the game's allocator - a few bytes per name)
}

// ------------------------------------------------------------------ calling
Params::Params(uintptr_t func) : fn(func) {
    uint16_t size = 0; if (!func || !rd_as(func + off::Func_ParmsSize, size)) { fn = 0; return; }
    buf.assign(size + 16, 0);
}
Prop Params::param(const std::string& name) const {
    uintptr_t f = 0; rd_as(fn + off::Struct_ChildProps, f);
    for (int i = 0; f && i < 64; ++i) {
        FName n; rd_as(f + off::FField_Name, n);
        if (name_str(n) == name) return prop_info(f);
        rd_as(f + off::FField_Next, f);
    }
    return Prop();
}
bool Params::put_raw(const std::string& name, const void* src, size_t n) {
    Prop p = param(name); if (!p) return false; memcpy(at(p), src, std::min<size_t>(n, p.size)); return true;
}
bool Params::put_bool(const std::string& name, bool v) {
    Prop p = param(name); if (!p) return false;
    uint8_t off_ = 0, mask = 0; rd_as(p.p + off::Prop_BoolByteOffset, off_); rd_as(p.p + off::Prop_BoolFieldMask, mask);
    uint8_t& b = buf[p.offset + off_]; b = v ? (b | mask) : (b & ~mask); return true;
}

typedef void (*ProcessEventFn)(uintptr_t, uintptr_t, void*);
static int seh_pe(uintptr_t pe, uintptr_t obj, uintptr_t fn, void* params, DWORD* code) {
    __try { ((ProcessEventFn)pe)(obj, fn, params); return 1; }
    __except ((*code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

std::string func_path(uintptr_t fn) { return path_of(fn); }

void journal_init(const std::string& dir) {
    g_journal_dir = dir;
    std::ifstream bf(dir + "\\blocked.txt"); std::string line;
    while (std::getline(bf, line)) { while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back(); if (!line.empty()) g_blocked_paths.push_back(line); }
    std::ifstream inf(dir + "\\inflight.txt");
    if (inf && std::getline(inf, line) && !line.empty()) {
        const std::string fnp = line.substr(0, line.find('\t'));
        log("[ue] the game closed during %s last time - that function is now blocked", fnp.c_str());
        g_blocked_paths.push_back(fnp);
        std::ofstream(dir + "\\blocked.txt", std::ios::app) << fnp << "\n";
        inf.close(); DeleteFileA((dir + "\\inflight.txt").c_str());
    }
}
void journal_scope(bool on) { t_journal = on; }
bool blocked(uintptr_t fn) {
    auto it = g_blocked_fn.find(fn); if (it != g_blocked_fn.end()) return it->second;
    const std::string p = func_path(fn); bool b = false;
    for (auto& x : g_blocked_paths) if (x == p) { b = true; break; }
    g_blocked_fn[fn] = b; return b;
}

// A function that returns / fills a STRUCT by value assigns into our zero-initialised buffer. Zero bytes are a valid empty
// FString / TArray / FName / pointer, but not a valid FText / TMap / TSet / delegate: the game's operator= then releases
// references that never existed (corrupted refcounts -> crash at game exit, 2026-11-09). Such calls are refused.
static std::map<uintptr_t, std::string> g_unsafe_cache;
static std::string unsafe_member(uintptr_t ustruct, int depth = 0) {
    if (!ustruct || depth > 6) return "";
    for (auto& m : props(ustruct, true)) {
        if (m.type == "TextProperty" || m.type == "MapProperty" || m.type == "SetProperty" || m.type == "DelegateProperty" ||
            m.type == "MulticastInlineDelegateProperty" || m.type == "MulticastSparseDelegateProperty") return name_of(ustruct) + "." + m.name + " (" + m.type + ")";
        if (m.type == "StructProperty") { std::string u = unsafe_member(m.owner_struct, depth + 1); if (!u.empty()) return u; }
    }
    return "";
}
static std::string unsafe_struct_out(uintptr_t fn) {
    auto it = g_unsafe_cache.find(fn); if (it != g_unsafe_cache.end()) return it->second;
    std::string why;
    for (auto& pp : props(fn, false)) {
        if (!(pp.flags & (CPF_ReturnParm | CPF_OutParm)) || pp.type != "StructProperty") continue;
        if (!(pp.flags & CPF_ReturnParm) && (pp.flags & 0x2)) continue;            // const& input (CPF_ConstParm) - we pass a valid copy
        why = unsafe_member(pp.owner_struct); if (!why.empty()) { why = pp.name + " -> " + why; break; }
    }
    g_unsafe_cache[fn] = why; return why;
}

bool call(uintptr_t obj, Params& p, std::string* err) {
    auto E = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!g_ready) return E("engine core not ready");
    if (g_game_exiting) return E("the game is closing");
    if (!obj || !p.fn) return E("no object / function");
    if (GetCurrentThreadId() != main_thread_id()) return E("not on the game thread");
    if (blocked(p.fn)) return E("blocked: " + func_path(p.fn) + " crashed the game before");
    { const std::string why = unsafe_struct_out(p.fn);
      if (!why.empty()) { log("[ue] refused %s: returns a struct the engine cannot build safely (%s)", func_path(p.fn).c_str(), why.c_str());
                          return E("refused: " + func_path(p.fn) + " returns " + why); } }
    uintptr_t pe = g_process_event;
    if (!pe && g_pe_slot >= 0) { uintptr_t vt = 0; if (rd_as(obj, vt)) rd_as(vt + g_pe_slot * 8, pe); }
    if (!pe) return E("ProcessEvent not found");
    const std::string path = (g_journal_dir.empty() || !t_journal) ? "" : func_path(p.fn);
    if (!path.empty()) { std::ofstream f(g_journal_dir + "\\inflight.txt", std::ios::trunc); f << path << "\t" << GetTickCount64() << "\n"; }
    DWORD code = 0;
    const int r = seh_pe(pe, obj, p.fn, p.buf.data(), &code);
    if (!path.empty()) DeleteFileA((g_journal_dir + "\\inflight.txt").c_str());
    if (!r) { log("[ue] %s raised 0x%08X (caught)", func_path(p.fn).c_str(), code); return E("the game function raised " + hexstr(code) + " (caught)"); }
    return true;
}
bool call(uintptr_t obj, const std::string& func, Params& p, std::string* err) {
    if (!p.fn) { p = Params(find_func(class_of(obj), func)); if (!p) { if (err) *err = "no function " + func + " on " + name_of(class_of(obj)); return false; } }
    return call(obj, p, err);
}
Params params_for(uintptr_t obj, const std::string& func) { return Params(find_func(class_of(obj), func)); }

// ------------------------------------------------------------------ start-up
static bool validate_objects(uintptr_t objobjects) {
    int32_t maxe = 0, nume = 0, numchunks = 0; uintptr_t chunks = 0;
    if (!rd_as(objobjects, chunks) || !rd_as(objobjects + 0x10, maxe) || !rd_as(objobjects + 0x14, nume) || !rd_as(objobjects + 0x1C, numchunks)) return false;
    if (!chunks || nume < 1000 || nume > maxe || maxe > 50000000 || numchunks < 1 || numchunks > 1000) return false;
    // item stride: FUObjectItem 0x18 (UE 5.x); objects know their own index
    for (int stride : { 0x18, 0x10, 0x20 }) {
        g_objobjects = objobjects; g_item_stride = stride;
        int good = 0;
        for (int i = 1; i <= 64; ++i) { uintptr_t o = object_at(i * 7); int32_t idx = -1; if (o && rd_as(o + off::Obj_Index, idx) && idx == i * 7) ++good; }
        if (good >= 48) return true;
    }
    g_objobjects = 0; return false;
}

bool init() {
    if (g_ready) return true;
    std::lock_guard<std::mutex> lk(g_init_mtx);
    if (g_ready) return true;
    if (!text_section()) { g_found["error"] = "game module not found"; return false; }
    // GUObjectArray.ObjObjects: "mov rax,[ObjObjects.Objects]; mov rcx,[rax+rcx*8]; lea rax/r8,[rcx+rdx*8]" (hundreds of sites)
    uintptr_t oo = vote({ "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1", "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 4C 8D 04 D1" }, 3, 0, "GUObjectArray.ObjObjects");
    if (!oo || !validate_objects(oo)) { g_found["error"] = "object array not found / did not validate"; return false; }
    // FNamePool: lazy-init sites "lea rcx,[NamePool]; call Init; mov r8,rax; mov byte [inited],1" (+ variants)
    g_namepool = vote({ "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C0 C6 05 ?? ?? ?? ?? 01",
                        "4C 8D 05 ?? ?? ?? ?? EB ?? 48 8D 0D ?? ?? ?? ?? E8",
                        "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 C6 05 ?? ?? ?? ?? 01" }, 3, 0, "FNamePool");
    // name check: object #0 is a package called "/Script/CoreUObject"
    if (!g_namepool || name_of(object_at(0)) != "/Script/CoreUObject") { g_found["error"] = "name pool did not validate (object 0 = '" + name_of(object_at(0)) + "')"; g_namepool = 0; return false; }
    // core classes
    for (int32_t i = 0; i < 4096 && !(g_cls_package && g_cls_class && g_cls_function && g_cls_scriptstruct); ++i) {
        uintptr_t o = object_at(i); if (!o || name_of(outer_of(o)) != "/Script/CoreUObject") continue;
        const std::string n = name_of(o);
        if (n == "Package") g_cls_package = o; else if (n == "Class") g_cls_class = o;
        else if (n == "Function") g_cls_function = o; else if (n == "ScriptStruct") g_cls_scriptstruct = o;
    }
    if (!g_cls_package || !g_cls_class || !g_cls_function) { g_found["error"] = "core classes not found"; return false; }
    // ProcessEvent: its own prologue (unique on 24478034); the vtable slot is learned now as the update fallback
    auto pe = scan("40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 48 89 9D 28 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33", 4);
    if (pe.size() == 1) g_process_event = pe[0];
    {   uintptr_t probe = object_at(0), vt = 0;
        if (rd_as(probe, vt) && g_process_event) for (int s = 0; s < 200; ++s) { uintptr_t e = 0; if (rd_as(vt + s * 8, e) && e == g_process_event) { g_pe_slot = s; break; } }
    }
    g_found["ProcessEvent"] = { {"rva", g_process_event ? hexstr(g_process_event - g_base) : "-"}, {"vtable_slot", g_pe_slot}, {"pattern_hits", pe.size()} };
    if (!g_process_event && g_pe_slot < 0) { g_found["error"] = "ProcessEvent not found"; return false; }
    g_found["objects"] = num_objects(); g_found["item_stride"] = hexstr(g_item_stride);
    g_ready = true;
    log("[ue] engine core ready: %d objects, names ok, ProcessEvent %s (slot %d)", num_objects(),
        g_process_event ? hexstr(g_process_event - g_base).c_str() : "via vtable", g_pe_slot);
    return true;
}
bool ready() { return g_ready; }
json status() { json j = g_found; j["ready"] = g_ready.load(); return j; }

// ------------------------------------------------------------------ developer commands
static json prop_json(const Prop& p) {
    json j = { {"name", p.name}, {"type", p.type}, {"offset", hexstr(p.offset)}, {"size", p.size} };
    if (p.dim != 1) j["dim"] = p.dim;
    if (p.owner_struct) j["of"] = name_of(p.owner_struct);
    if (p.flags & CPF_ReturnParm) j["ret"] = true; else if (p.flags & CPF_OutParm) j["out"] = true;
    return j;
}
static uintptr_t obj_arg(const json& a) {
    if (a.contains("addr")) { uintptr_t v = 0; std::string e; eval_addr(a["addr"].get<std::string>(), v, &e); return v; }
    if (a.contains("path")) return find_object(a["path"].get<std::string>());
    if (a.contains("class")) { uintptr_t c = find_class(a["class"].get<std::string>()); return a.value("cdo", false) ? cdo_of(c) : find_first(c); }
    return 0;
}

// ------------------------------------------------------------------ UE.DTROWS (research): load a DataTable and dump its rows
// Loads with KismetSystemLibrary::LoadAsset_Blocking and reads everything in the same game-thread call (soft tables
// nobody references are garbage collected right after). Values are summarised (numbers, names, enums, bools, soft
// paths, object names, texts, array sizes, one level of nested structs).
static json prop_value(uintptr_t base, const Prop& p, int depth);
static json struct_value(uintptr_t base, uintptr_t st, int depth) {
    json o = json::object();
    for (auto& c : props(st, true)) o[c.name] = prop_value(base, c, depth + 1);
    return o;
}
static json prop_value(uintptr_t base, const Prop& p, int depth) {
    const std::string& t = p.type; const uintptr_t a = base + p.offset;
    if (t == "BoolProperty") return get_bool(base, p);
    if (t == "NameProperty") { FName n{}; mread(a, &n, sizeof n); return name_str(n); }
    if (t == "StrProperty") return fstring(a);
    if (t == "TextProperty") return ftext(a);
    if (t == "SoftObjectProperty" || t == "SoftClassProperty") { FName pk{}, as{}; mread(a + 8, &pk, 8); mread(a + 0x10, &as, 8);
        const std::string s1 = name_str(pk); return s1 == "None" ? json("") : json(s1 + "." + name_str(as)); }
    if (t == "ObjectProperty" || t == "ClassProperty") { uintptr_t o = 0; rd_as(a, o); return o ? json(path_of(o)) : json(nullptr); }
    if (t == "ArrayProperty") { int32_t n = 0; rd_as(a + 8, n); return json{ {"array", n} }; }
    if (t == "MapProperty" || t == "SetProperty") { int32_t n = 0; rd_as(a + 8, n); return json{ {t == "MapProperty" ? "map" : "set", n} }; }
    if (t == "StructProperty") { if (depth >= 2 || !p.owner_struct) return json(name_of(p.owner_struct)); return struct_value(a, p.owner_struct, depth); }
    return get_num(base, p);
}
uintptr_t load_asset(const std::string& path) {
    const size_t dot = path.rfind('.'); if (dot == std::string::npos) return 0;
    if (uintptr_t o = find_object(path, false)) return o;
    FName pk{}, as{}; if (!name_find(path.substr(0, dot), pk) || !name_find(path.substr(dot + 1), as)) return 0;
    uintptr_t k = cdo_of(find_class("KismetSystemLibrary")); Params p = params_for(k, "LoadAsset_Blocking"); if (!p) return 0;
    Prop sp = p.param("Asset"); std::vector<uint8_t> soft(sp.size, 0); memcpy(soft.data() + 8, &pk, 8); memcpy(soft.data() + 0x10, &as, 8);
    memcpy(p.at(sp), soft.data(), soft.size()); if (!call(k, p)) return 0;
    return p.ret<uintptr_t>();
}

json cmd(const std::string& name, const json& a) {
    if (name == "UE.STATUS") { init(); return ok(status()); }
    if (!init()) return fail("NOT_READY", status().dump());
    return run_on_main([name, a]() -> json {
        if (name == "UE.DTROWS") {                     // {asset:"/Game/.../DT_X.DT_X", max} -> {struct, rows:{name:{...}}}
            const std::string path = a.value("asset", ""); const size_t dot = path.rfind('.');
            if (dot == std::string::npos) return fail("BAD_ARG", "asset = /Game/Path/Name.Name");
            uintptr_t dt = find_object(path, false);
            if (!dt) {
                FName pk{}, as{};
                if (!name_find(path.substr(0, dot), pk) || !name_find(path.substr(dot + 1), as)) return fail("NO_NAME", "asset name not in the name pool");
                uintptr_t k = cdo_of(find_class("KismetSystemLibrary")); Params p = params_for(k, "LoadAsset_Blocking"); if (!p) return fail("NO_FUNC", "LoadAsset_Blocking");
                Prop sp = p.param("Asset"); std::vector<uint8_t> soft(sp.size, 0); memcpy(soft.data() + 8, &pk, 8); memcpy(soft.data() + 0x10, &as, 8);
                memcpy(p.at(sp), soft.data(), soft.size()); std::string e; if (!call(k, p, &e)) return fail("LOAD", e);
                dt = p.ret<uintptr_t>();
            }
            if (!dt) return fail("NOT_FOUND", "asset did not load");
            uintptr_t rs = 0, data = 0; int32_t num = 0; rd_as(dt + 0x28, rs); rd_as(dt + 0x30, data); rd_as(dt + 0x38, num);
            if (!rs || !data || num < 0 || num > 20000) return fail("LAYOUT", "not a data table layout");
            json rows = json::object(); const int mx = a.value("max", 200);
            for (int32_t i = 0; i < num && i < mx; ++i) {
                FName n{}; uintptr_t row = 0; if (!mread(data + i * 24, &n, 8) || !rd_as(data + i * 24 + 8, row) || !row) continue;
                rows[name_str(n)] = struct_value(row, rs, 0);
            }
            return ok({ {"struct", name_of(rs)}, {"count", num}, {"rows", rows} });
        }
        if (name == "UE.NAMEOF") {                     // {index, number} -> text (stored name IDs back to text)
            FName n{}; n.index = (uint32_t)a.value("index", 0); n.number = (uint32_t)a.value("number", 0);
            return ok(name_str(n));
        }
        if (name == "UE.FIND") {                       // {class | name, max}
            json arr = json::array(); const size_t max = a.value("max", 30);
            if (a.contains("class")) { for (uintptr_t o : find_all(find_class(a["class"]), a.value("cdo", false), max)) arr.push_back({ {"addr", hexstr(o)}, {"name", full_name(o)} }); }
            else if (a.contains("name")) { for (uintptr_t o : objects_named(a["name"])) { arr.push_back({ {"addr", hexstr(o)}, {"name", full_name(o)} }); if (arr.size() >= max) break; } }
            return ok(arr);
        }
        uintptr_t o = obj_arg(a);
        if (name == "UE.OBJ") return o ? ok({ {"addr", hexstr(o)}, {"name", full_name(o)}, {"class", path_of(class_of(o))}, {"outer", path_of(outer_of(o))}, {"cdo", is_cdo(o)} }) : fail("NOT_FOUND", "no object");
        if (name == "UE.PROPS") {                      // {class|path|addr, filter, super}
            uintptr_t s = a.contains("class") ? find_class(a["class"]) : (a.contains("struct") ? find_struct(a["struct"]) : class_of(o));
            if (!s) return fail("NOT_FOUND", "no class/struct");
            const std::string flt = a.value("filter", "");
            json arr = json::array(); for (auto& p : props(s, a.value("super", true))) if (flt.empty() || p.name.find(flt) != std::string::npos) arr.push_back(prop_json(p));
            return ok(arr);
        }
        if (name == "UE.FUNCS") {
            uintptr_t c = a.contains("class") ? find_class(a["class"]) : class_of(o); if (!c) return fail("NOT_FOUND", "no class");
            const std::string flt = a.value("filter", "");
            json arr = json::array();
            for (uintptr_t f : funcs(c, a.value("super", false))) {
                const std::string n = name_of(f); if (!flt.empty() && n.find(flt) == std::string::npos) continue;
                json ps = json::array(); for (auto& p : props(f, false)) ps.push_back(prop_json(p));
                arr.push_back({ {"name", n}, {"params", ps} });
            }
            return ok(arr);
        }
        if (name == "UE.GET") {                        // {obj..., field: "a.b.c"}
            if (!o) return fail("NOT_FOUND", "no object");
            uintptr_t base = 0; Prop leaf; if (!resolve(o, class_of(o), a.value("field", ""), base, leaf)) return fail("NO_FIELD", "field not found");
            json r = prop_json(leaf); r["value"] = get_num(base, leaf); r["at"] = hexstr(base + leaf.offset);
            if (leaf.type == "ObjectProperty") r["value_obj"] = full_name(get<uintptr_t>(base, leaf));
            if (leaf.type == "NameProperty") r["value_name"] = name_str(get<FName>(base, leaf));
            if (leaf.type == "StrProperty") r["value_str"] = fstring(base + leaf.offset);
            if (leaf.type == "TextProperty") r["value_text"] = ftext(base + leaf.offset);
            if (leaf.type == "ArrayProperty") r["num"] = array_of(base, leaf).num;
            if (leaf.type == "MapProperty") r["num"] = map_entries(base, leaf).size();
            return ok(r);
        }
        if (name == "UE.CALL") {                       // {obj..., func, args: {param: number | "obj:<path>" | "name:<FName>" | "class:<name>"}}
            if (!o) return fail("NOT_FOUND", "no object");
            Params p = params_for(o, a.value("func", "")); if (!p) return fail("NO_FUNC", "function not found");
            if (a.contains("args")) for (auto& kv : a["args"].items()) {
                Prop pp = p.param(kv.key()); if (!pp) return fail("NO_PARAM", "no parameter " + kv.key());
                const json& v = kv.value();
                if (v.is_number() || v.is_boolean()) {
                    const double d = v.is_boolean() ? (v.get<bool>() ? 1 : 0) : v.get<double>();
                    if (pp.type == "BoolProperty") p.put_bool(kv.key(), d != 0); else set_num((uintptr_t)p.buf.data(), pp, d);
                } else if (v.is_string()) {
                    const std::string s = v.get<std::string>();
                    uintptr_t ob = 0; FName fnm;
                    if (s.rfind("obj:", 0) == 0) ob = find_object(s.substr(4));
                    else if (s.rfind("class:", 0) == 0) ob = find_class(s.substr(6));
                    else if (s.rfind("first:", 0) == 0) ob = find_first(find_class(s.substr(6)));
                    else if (s.rfind("addr:", 0) == 0) { uintptr_t v = 0; std::string e; eval_addr(s.substr(5), v, &e); ob = v; }
                    else if (s.rfind("hex:", 0) == 0) {          // raw parameter bytes (structs), e.g. "hex:A0B1..."
                        const std::string h = s.substr(4); std::vector<uint8_t> b(h.size() / 2);
                        for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)std::stoi(h.substr(i * 2, 2), nullptr, 16);
                        memcpy(p.at(pp), b.data(), std::min<size_t>(b.size(), pp.size)); continue; }
                    else if (s.rfind("name:", 0) == 0) { if (!name_find(s.substr(5), fnm)) return fail("NO_NAME", "name not in the pool: " + s.substr(5)); memcpy(p.at(pp), &fnm, 8); continue; }
                    if (!ob) return fail("NOT_FOUND", "argument object not found: " + s);
                    memcpy(p.at(pp), &ob, 8);
                }
            }
            std::string err;
            if (!call(o, p, &err)) return fail("CALL_FAILED", err);
            json r = { {"called", path_of(p.fn)}, {"on", full_name(o)} };
            for (auto& pp : props(p.fn, false)) {
                if (!(pp.flags & (CPF_ReturnParm | CPF_OutParm))) continue;
                json v = { {"type", pp.type} };
                const uintptr_t b = (uintptr_t)p.buf.data();
                if (pp.type == "ObjectProperty") v["value"] = full_name(get<uintptr_t>(b, pp));
                else if (pp.type == "StrProperty") v["value"] = fstring(b + pp.offset);
                else if (pp.type == "NameProperty") v["value"] = name_str(get<FName>(b, pp));
                else if (pp.type == "ArrayProperty") v["num"] = array_of(b, pp).num;
                else if (pp.type == "StructProperty") v["bytes"] = to_hex(p.at(pp), std::min(pp.size, 64));
                else v["value"] = get_num(b, pp);
                r[pp.name] = v;
            }
            return ok(r);
        }
        if (name == "UE.NAME") { FName n; bool f = name_find(a.value("s", ""), n); return ok({ {"found", f}, {"index", n.index}, {"number", n.number}, {"back", name_str(n)} }); }
        return fail("UNKNOWN", "unknown UE command " + name);
    }, 8000);
}

} // namespace ue
