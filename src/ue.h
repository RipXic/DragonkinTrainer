// ue.h - native Unreal Engine 5.5 reflection for Dragonkin (NO UE4SS needed): objects, names, properties, calls.
// Both builds. Locates GUObjectArray / FNamePool / ProcessEvent itself (pattern majority vote + structure checks,
// research\engine_ue4ss_build24478034.txt has what UE4SS measured on this build). Member offsets = UE 5.5 Shipping.
// Everything that touches live game objects must run on the game thread (run_on_main / add_main_tick).
#pragma once
#include "common.h"
#include <unordered_map>

namespace ue {
using namespace ::wb;

// ---- UE 5.5 member offsets (UE4SS "MEMBER OFFSETS", build 24478034)
namespace off {
constexpr int Obj_Flags = 0x08, Obj_Index = 0x0C, Obj_Class = 0x10, Obj_Name = 0x18, Obj_Outer = 0x20;
constexpr int Field_Next = 0x28;                                       // UField::Next
constexpr int Struct_Super = 0x40, Struct_Children = 0x48, Struct_ChildProps = 0x50, Struct_Size = 0x58;
constexpr int Class_CDO = 0x110;
constexpr int Func_Flags = 0xB0, Func_NumParms = 0xB4, Func_ParmsSize = 0xB6, Func_RetOffset = 0xB8, Func_Native = 0xD8;
constexpr int FField_Class = 0x08, FField_Owner = 0x10, FField_Next = 0x18, FField_Name = 0x20;
constexpr int Prop_ArrayDim = 0x30, Prop_ElemSize = 0x34, Prop_Flags = 0x38, Prop_Offset = 0x44;
constexpr int Prop_Struct = 0x70, Prop_ObjClass = 0x70, Prop_ArrayInner = 0x78;
constexpr int Prop_MapKey = 0x70, Prop_MapValue = 0x78, Prop_MapLayout = 0x80;
constexpr int Prop_BoolFieldSize = 0x70, Prop_BoolByteOffset = 0x71, Prop_BoolByteMask = 0x72, Prop_BoolFieldMask = 0x73;
constexpr int Prop_ByteEnum = 0x70, Prop_EnumUnderlying = 0x70, Prop_EnumEnum = 0x78;
}
constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;
constexpr uint32_t FUNC_Static = 0x2000;
constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;

// fast guarded memory access inside the game process (no syscalls; a bad pointer returns false instead of crashing)
bool mread(uintptr_t a, void* out, size_t n);
bool mwrite(uintptr_t a, const void* in, size_t n);
template <class T> bool rd_as(uintptr_t a, T& v) { return mread(a, &v, sizeof(T)); }

struct FName { uint32_t index = 0, number = 0; };
template <class T> struct TArray { T* data = nullptr; int32_t num = 0, max = 0; };

// ---- start-up
bool init();                  // idempotent; true when objects, names and ProcessEvent are usable
bool ready();
json status();                // what was found where (and how many patterns agreed)

// ---- names
std::string name_str(FName n);
bool name_find(const std::string& s, FName& out);   // existing names only (no new entries are created)

// ---- objects (addresses; 0 = none)
int32_t num_objects();
uintptr_t object_at(int32_t i);
bool alive(uintptr_t obj);                           // still in the object array at its index
std::string name_of(uintptr_t obj);
uintptr_t class_of(uintptr_t obj);
uintptr_t outer_of(uintptr_t obj);
std::string path_of(uintptr_t obj);                  // "/Script/HnS.Default__HnSGlobalConfig"
std::string full_name(uintptr_t obj);                // "Class /Script/HnS.HnSGlobalConfig"
bool is_cdo(uintptr_t obj);
bool is_a(uintptr_t obj, uintptr_t cls);
bool is_child_of(uintptr_t cls, uintptr_t parent);
uintptr_t super_of(uintptr_t ustruct);
uintptr_t find_object(const std::string& path, bool allow_full_reindex = true);   // exact path name
// The game's own allocator (GMalloc, found by pattern vote). Memory the game owns and may free/grow later
// (TArray buffers inside game data) must come from here. Game thread only. nullptr on failure.
void* game_realloc(void* ptr, size_t bytes, uint32_t alignment);
std::vector<uintptr_t> find_code(const std::string& pattern, size_t max = 8);
uintptr_t load_asset(const std::string& path);   // "/Game/.../X.X": loaded object, or loads it (LoadAsset_Blocking). GAME THREAD.   // pattern hits in the exe .text
uintptr_t find_class(const std::string& name);       // short ("HnSInventoryComponent", U/A prefix ok) or path
uintptr_t find_struct(const std::string& name);      // UScriptStruct by short name
uintptr_t cdo_of(uintptr_t cls);
std::vector<uintptr_t> find_all(uintptr_t cls, bool include_cdo = false, size_t max = 0);
// cost counters for the expensive paths (read + reset by dk's frame-time monitor)
struct Perf { uint32_t scans = 0; double scan_ms = 0; uint32_t name_builds = 0; double name_ms = 0; uint32_t index_builds = 0; double index_ms = 0; };
Perf& perf();
double now_ms();
uintptr_t find_first(uintptr_t cls);                 // first live non-CDO instance

// ---- properties
struct Prop {
    uintptr_t p = 0; std::string name, type;          // type = FFieldClass name ("IntProperty", "StructProperty"...)
    int32_t offset = 0, size = 0, dim = 1; uint64_t flags = 0;
    uintptr_t owner_struct = 0;                       // struct (StructProperty), class (ObjectProperty), inner (ArrayProperty)
    explicit operator bool() const { return p != 0; }
};
Prop prop_info(uintptr_t fprop);
Prop find_prop(uintptr_t ustruct, const std::string& name);    // walks the super chain
std::vector<Prop> props(uintptr_t ustruct, bool with_super = true);
uintptr_t find_func(uintptr_t cls, const std::string& name);   // walks the super chain
std::vector<uintptr_t> funcs(uintptr_t cls, bool with_super = false);
Prop array_inner(const Prop& arr);
bool map_kv(const Prop& map, Prop& key, Prop& val, int32_t& value_offset, int32_t& stride);

// typed access on raw memory (base = object or struct address)
template <class T> T get(uintptr_t base, const Prop& p) { T v{}; if (p) mread(base + p.offset, &v, sizeof(T)); return v; }
template <class T> bool set(uintptr_t base, const Prop& p, const T& v) { return p && mwrite(base + p.offset, &v, sizeof(T)); }
bool get_bool(uintptr_t base, const Prop& p);
bool set_bool(uintptr_t base, const Prop& p, bool v);
double get_num(uintptr_t base, const Prop& p);        // int/float/double/byte/enum/bool as a number
bool set_num(uintptr_t base, const Prop& p, double v);
// path access: "m_PlayerInventoryCapacityProperties.m_nSkillGridStorageTabCount" (structs only)
bool resolve(uintptr_t obj_or_struct, uintptr_t ustruct, const std::string& dotted, uintptr_t& base, Prop& leaf);
// arrays / maps
TArray<uint8_t> array_of(uintptr_t base, const Prop& arr);
std::vector<std::pair<uintptr_t, uintptr_t>> map_entries(uintptr_t base, const Prop& map);   // (key addr, value addr)

// ---- strings in game memory
std::string fstring(uintptr_t fstring_addr);         // FString -> utf8
std::string ftext(uintptr_t ftext_addr);             // FText -> utf8 (KismetTextLibrary.Conv_TextToString)

// ---- calling game functions (game thread!)
struct Params {
    uintptr_t fn = 0; std::vector<uint8_t> buf;
    Params() = default;
    explicit Params(uintptr_t func);
    explicit operator bool() const { return fn != 0; }
    Prop param(const std::string& name) const;
    uint8_t* at(const Prop& p) { return buf.data() + p.offset; }
    template <class T> bool put(const std::string& name, const T& v) { Prop p = param(name); if (!p) return false; memcpy(at(p), &v, sizeof(T) < (size_t)p.size ? sizeof(T) : p.size); return true; }
    bool put_raw(const std::string& name, const void* src, size_t n);
    bool put_bool(const std::string& name, bool v);
    template <class T> T ret() const { T v{}; uint16_t ro = 0; mread(fn + off::Func_RetOffset, &ro, 2); if (ro != 0xFFFF && ro + sizeof(T) <= buf.size()) memcpy(&v, buf.data() + ro, sizeof(T)); return v; }
    uint8_t* ret_ptr() { uint16_t ro = 0; mread(fn + off::Func_RetOffset, &ro, 2); return ro == 0xFFFF ? nullptr : buf.data() + ro; }
    template <class T> T out(const std::string& name) const { T v{}; Prop p = param(name); if (p) memcpy(&v, buf.data() + p.offset, sizeof(T)); return v; }
};
// ProcessEvent(obj, fn, params). False = not found / crashed inside (caught; the game keeps running) / blocked.
bool call(uintptr_t obj, Params& p, std::string* err = nullptr);
bool call(uintptr_t obj, const std::string& func, Params& p, std::string* err = nullptr);   // looks the function up on obj's class
Params params_for(uintptr_t obj, const std::string& func);

// crash journal (same idea as the Lua PE wrapper): the function in flight is written to <root>\inflight.txt; one that
// killed the game is blocked on the next start (<root>\blocked.txt).
void journal_init(const std::string& dir);
void journal_scope(bool on);     // journal only one-off actions (panel clicks / editor), not the keeper's routine calls
bool blocked(uintptr_t fn);
std::string func_path(uintptr_t fn);

// ---- developer commands (AI link): UE.STATUS / UE.FIND / UE.PROPS / UE.FUNCS / UE.GET / UE.CALL ...
json cmd(const std::string& name, const json& a);

} // namespace ue
