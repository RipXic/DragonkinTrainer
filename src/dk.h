// dk.h - the Dragonkin trainer engine in C++ (no UE4SS): game helpers, cheats + keeper, panel items, editor, city.
// Ported from the Lua mod (framework/features/panel/editor/city). Runs on the game thread (main tick + request queue).
// The panel talks to it with the same request / state documents the Lua file link used (engine_post / engine_state).
#pragma once
#include "common.h"

namespace dk {

void init(const std::string& data_dir, const std::string& panel_spec);   // once: loads state, starts the game-thread tick
bool native_mode();                       // true = this engine runs the cheats (PUBLIC always; DEV: config "engine":"native")
void post(const json& requests);          // [{seq, op: set|click|call|all_off, ...}] - processed on the game thread
json state();                             // { seq_done, results, values, readouts, keeper, player, selfheal, panel_log }
uint64_t state_version();                 // changes whenever state() would return something new

// developer access (dev link): DK.STATUS / DK.SET / DK.CALL / DK.ATTR
json cmd(const std::string& name, const json& a);

} // namespace dk
