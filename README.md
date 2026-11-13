# Dragonkin Trainer — source

[![Discord: RipXic Mods](https://img.shields.io/badge/Discord-RipXic%20Mods-5865F2?logo=discord&logoColor=white)](https://discord.gg/ZwrWMdJfFW)
[![Nexus Mods](https://img.shields.io/badge/Nexus%20Mods-Dragonkin%20Trainer-DA8E35)](https://www.nexusmods.com/dragonkinthebanished/mods/10)
[![Latest release](https://img.shields.io/github/v/release/RipXic/DragonkinTrainer?label=latest)](https://github.com/RipXic/DragonkinTrainer/releases/latest)

Source code of **Dragonkin Trainer 1.1.0**, an in-game trainer for *Dragonkin: The Banished* (Steam, game version 1.7.79).
This is exactly the code of the release on Nexus Mods: `DragonkinTrainer.exe` (the loader) and `native.dll` (the trainer).

Single-player / offline only. Fan-made, not affiliated with Eko Software or Nacon.

## Community
Questions, bug reports, feature ideas and update news: join the **[RipXic Mods Discord](https://discord.gg/ZwrWMdJfFW)**.
Pick the **Mod Updates** role there to get a ping for every new version.

## Download
- **Nexus Mods:** https://www.nexusmods.com/dragonkinthebanished/mods/10
- **GitHub releases** (the same zips as on Nexus):

| Version | Game version | Release | SHA-256 of the zip |
|---|---|---|---|
| **1.1.0** (latest) | 1.7.79 (Harrower update) | [v1.1.0](https://github.com/RipXic/DragonkinTrainer/releases/tag/v1.1.0) | `b3afecdc294c5fdac00541e90fb356e777285456faf51142892ecded5bbc6bc6` |
| 1.0.0 | 1.6.75 | [v1.0.0](https://github.com/RipXic/DragonkinTrainer/releases/tag/v1.0.0) | `02842981041f2a8744f2f4096222d3962ce344e54b22de780f31dfe1753201ec` |

Check a zip with `Get-FileHash .\DragonkinTrainer_1.1.0.zip` in PowerShell. This repository holds the latest source;
the 1.0.0 source is at the `v1.0.0` tag.

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

## License
**Source-available, not open source.** You may read the code and build it for your own single-player use.
You may **not** redistribute, re-upload, fork-and-release, sell, or reuse the code or builds without written permission,
and you may **not** use this code to train or build datasets for AI / machine-learning models or to make AI-generated copies of it.
Full terms: [LICENSE](LICENSE). Third-party code in `third_party/` keeps its own licenses (MIT / BSD 2-Clause).
Official download: https://www.nexusmods.com/dragonkinthebanished/mods/10
