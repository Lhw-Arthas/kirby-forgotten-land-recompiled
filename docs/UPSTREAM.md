# Upstream: LAS Recomp

The runtime engine, the game-code generator and the build come from LAS Recomp (https://github.com/peiikai/LASRecomp,
MIT, see `LICENSE-LASRecomp`), copied at one commit. They are not merged: a later LAS fix is brought over by hand,
file by file.

Commit: 33784c2ad6ce7f802eb4b78139fb578633e60aa4 (LAS `main`, 2026-11-15)

## What was copied

`tools/upstream/paths.txt` lists the LAS paths this repo takes: `Makefile`, `runtime/`, `tools/`, `third_party/`.
`tools/upstream/removed.txt` lists what is left out of them: Link's Awakening's features (randomizer, cosmetics,
weather, warps, cheats, trackers and the rest) and its game data (addresses, RomFS file hashes, file cuts, hot
functions, shader variants, patch and hook sites). LAS's docs, README, CI workflows and Android project are not taken.
`tools/upstream/import.sh <LAS checkout> <commit>` made the copy. Every copied file keeps LAS's notices as they are.

## Changed here after copying

`tools/upstream/sync.sh <LAS checkout> --local` prints this list from the files themselves. Each change removes a
Link's Awakening feature or its data, and leaves the engine as LAS has it otherwise.

- `Makefile`: no randomizer helper, venv or smoke test; the generator helper's venv is `build/gen-venv`.
- `runtime/settings.c`, `runtime/settings.h`: no settings of the removed features.
- `runtime/main.c`: no seed at boot; `LARECOMP_REBOOT_AT` takes `game` or `title`.
- `runtime/game_addresses.c`, `runtime/game_addresses.h`: no version tables of their game; the address table has a
  placeholder field (C has no empty struct).
- `runtime/hle/input.c`: no controller light (it showed Link's health), d-pad items, mirror mode, warps or test hooks
  of the removed features; `g_window_shot_request` is defined here (it was in `hle/hookshot.c`).
- `runtime/hle/movie.c`: no run-timer calls.
- `runtime/services/fs.c`: the RomFS overlay has no source (it served randomizer seeds); no per-seed save folder or
  save hooks of the removed features.
- `runtime/nvn/pace.c`: no "30 fps outdoors" by level, no frame-work object (it came from their game's addresses).
- `runtime/nvn/vulkan/exec.c`, `device.c`, `shaders.c`, `textures.c`: no weather, snow, mirror mode, recolouring,
  texture patches, screen filter, phone HUD corners, pause backdrop or room widening; the present crop is the whole
  image and the wide view is off.
- `runtime/crash_explain.c`: a comment points at `hle/game_state.c`.
- `runtime/ui/menu.mm`, `launcher.mm`, `controls_ui.mm`, `difficulty.mm`, `option_rows.mm`, `option_rows.h`: no
  Randomizer, Warp, Save Editor, Cheats, Cosmetics or Gameplay pages, overlays or rows; the start screen has no
  Randomizer row.
- `tools/gen.py`, `tools/lasgen.spec`: no randomizer patch variants.
- `tools/menu_strings.py`: only the menus' own text (no warp, spoiler or tracker names).
- `tools/test_settings.c`, `tools/test_settings.py`: no cases for the removed settings.

## This repo's own files among them

- The game's slots, empty: `runtime/game_addresses/versions.h`, `patches_v1_0_0.h`, `runtime_v1_0_0.h`,
  `romfs_files.h`, `runtime/hle/hooks.c`, `runtime/nvn/shader_variants.txt`, `tools/patches.py`,
  `tools/chunk_bounds.py`, and `runtime/menu_sfx.c` (silent).
- `runtime/hle/game_state.c`: what the engine asks the game (in play, level name, world on screen, camera zone, RomFS
  read hook), answered neutrally, and the `__cxa_throw` and `vector::at` diagnostics moved out of `hle/warp.c`.
- `tools/upstream/`: the copy's lists and scripts.
- The test harness and packaging scripts in `tools/` that LAS doesn't have (`run_scripted.sh`, `boot_check.sh`,
  `test_find_game.sh`, `dist_licenses.sh`, `launcher_notice.py`, `frametime_summary.py`, `perf_summary.py`,
  `winshot.ps1`).

## Bringing a LAS fix over

1. `tools/upstream/sync.sh <LAS checkout> [rev]` lists every taken file LAS changed since the commit above, and says
   for each whether this repo changed it too.
2. Copy the files unchanged here; merge the others by hand; put a new LAS file in `removed.txt` if it belongs to their
   game.
3. Set the Commit line above to the new commit, and update the lists in this file.
