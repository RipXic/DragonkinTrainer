# Dragonkin Trainer — source

Source code of **Dragonkin Trainer 1.0.0**, an in-game trainer for *Dragonkin: The Banished* (Steam, game version 1.6.75).
This is exactly the code of the release on Nexus Mods: `DragonkinTrainer.exe` (the loader) and `native.dll` (the trainer).

Single-player / offline only. Fan-made, not affiliated with Eko Software or Nacon.

## What the two files do
- **DragonkinTrainer.exe** (`loader/loader.cpp`) starts the game through Steam (`steam://rungameid/1863430`), or finds the
  running game, and loads `native.dll` into it with `LoadLibraryW` (CreateRemoteThread). Nothing is written to the game folder.
- **native.dll** (`src/`) draws the INSERT panel with Dear ImGui over the game's DirectX 12 swapchain and applies the cheats
  through the game's own Unreal Engine functions (reflection + ProcessEvent, `src/ue.cpp`, `src/dk.cpp`, `src/dk_editor.cpp`).
  `src/dk_gridhook.cpp` hooks one game function with MinHook for the Ancestral Grid feature.
  It makes no network connections.

## Build
Requirements: Windows 10/11 x64, Visual Studio 2022 or newer with **Desktop development with C++**.

```
build.bat
```
Output: `bin\DragonkinTrainer.exe`, `bin\native.dll`, `bin\panel.json` (the panel layout). Run `bin\DragonkinTrainer.exe`.

## Third-party code (in `third_party/`, each under its own license)
- [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b — MIT
- [MinHook](https://github.com/TsudaKageyu/minhook) — BSD 2-Clause
- [nlohmann/json](https://github.com/nlohmann/json) — MIT
