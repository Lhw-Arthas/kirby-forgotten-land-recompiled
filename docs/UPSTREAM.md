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
Link's Awakening feature or its data, takes a game value from `game/game.h` / `game/game.mk` (the game layer), or
carries one of this repo's fixes; the engine stays as LAS has it otherwise.

- `Makefile`: no randomizer helper, venv or smoke test; the generator helper's venv is `build/gen-venv`. Includes
  `game/game.mk` (exe name, version); `make gen` lifts every module `game/game.h` lists (`GAME=` names the game
  files); the game code links through a linker response file.
- `runtime/settings.c`, `runtime/settings.h`: no settings of the removed features.
- `runtime/main.c`: no seed at boot; `LARECOMP_REBOOT_AT` takes `game` or `title`. nn::init::Start's symbol and
  argument count come from `game.h` (5 arguments: every module initialised at x3, the SDK's first).
- `runtime/game_addresses.c`, `runtime/game_addresses.h`: no version tables of their game; the address table has a
  placeholder field (C has no empty struct).
- `runtime/gamelib.h`, `gamelib.c`, `dispatch.c`, `loader.c`, `rt.h`, `gamebuild.c`, `tools/gamelib_entry.c`: any
  number of modules (`game.h` GAME_MODULES; GAMELIB_ABI 2), where LAS has main and sdk.
- `runtime/kernel.c`: the program id from `game.h`; `kernel_run_main` passes x4; a condition variable wait sets its
  key before it releases the mutex (`tools/test_condvar.c` has the case).
- `runtime/services/misc.c`: the add-on base id from `game.h`.
- `runtime/romfs_build.c`, `romfs_build.h`, `services/fs.c`: `romfs_is_game()` (the marker folders in `game.h`) in
  place of the `region_common` checks.
- `runtime/paths.c`, `runtime/platform/windows.c`: message boxes named after the game; Windows: the session log is
  `game.log` (`game.h` GAME_LOG).
- `runtime/nvn/objects.c`: a packaged 3D texture's layout word gives its block depth too (bits 4-7).
- `runtime/nvn/maxwell/translate.c`: VOTE.VTG is a no-op, EXIT honours its flow test (EXIT.FCSM_TR goes on), LEA's
  lo forms.
- `tools/game_addresses.py`: an empty address table is allowed. `tools/gen.py`: an ADR whose register a BR uses soon
  after is a jump table, not a function start.
- `runtime/hle/input.c`: no controller light (it showed Link's health), d-pad items, mirror mode, warps or test hooks
  of the removed features; `g_window_shot_request` is defined here (it was in `hle/hookshot.c`).
- `runtime/hle/movie.c`: no run-timer calls. Extractor slot 2 SetDataSource(path), MediaData::FindInt64, decoded
  frames in tight NV12 and copied into a frame buffer the game passes.
- `runtime/services/fs.c`: the RomFS overlay has no source (it served randomizer seeds); no per-seed save folder or
  save hooks of the removed features.
- `runtime/nvn/pace.c`: no "30 fps outdoors" by level, no frame-work object (it came from their game's addresses).
- `runtime/nvn/vulkan/exec.c`, `device.c`, `shaders.c`, `textures.c`: no weather, snow, mirror mode, recolouring,
  texture patches, screen filter, phone HUD corners, pause backdrop or room widening; the present crop is the whole
  image and the wide view is off. `device.c`: the window title from `game.h`.
- `runtime/crash_explain.c`: a comment points at `hle/game_state.c`; no RomFS file count (the incomplete-copy
  explanation is off).
- `runtime/ui/menu.mm`, `launcher.mm`, `controls_ui.mm`, `difficulty.mm`, `option_rows.mm`, `option_rows.h`: no
  Randomizer, Warp, Save Editor, Cheats, Cosmetics or Gameplay pages, overlays or rows; the start screen has no
  Randomizer row. `launcher.mm`: version, update check repository, picker text and RomFS check from the game layer;
  no RomFS file count.
- `tools/gen.py`, `tools/lasgen.spec`: no randomizer patch variants.
- `tools/menu_strings.py`: only the menus' own text (no warp, spoiler or tracker names).
- `tools/build_deps_windows.sh`: FFmpeg keeps swresample (its Opus decoder needs it, for `services/hwopus.c`) and the
  build checks that every decoder was built.
- `tools/test_settings.c`, `tools/test_settings.py`: no cases for the removed settings.

## This repo's own files among them

- The game layer, outside the copied paths: `game/game.h` (name, program id, log name, RomFS markers, modules,
  nn::init::Start) and `game/game.mk` (exe name, version).
- The game's slots: `runtime/game_addresses/versions.h` (Kirby 1.0.0's main build ID, its one line); empty:
  `patches_v1_0_0.h`, `runtime_v1_0_0.h`, `romfs_files.h`, `runtime/hle/hooks.c`, `runtime/nvn/shader_variants.txt`,
  `tools/patches.py`, `tools/chunk_bounds.py`, and `runtime/menu_sfx.c` (silent).
- `runtime/services/hwopus.c` (Opus decoding) and `network.c` (an offline console's nifm, ssl, bsd, ldn), from
  letsgo-recomp (MIT) by way of v0.2.0.
- `runtime/hle/game_state.c`: what the engine asks the game (in play, level name, world on screen, camera zone, RomFS
  read hook), answered neutrally, and the `__cxa_throw` and `vector::at` diagnostics moved out of `hle/warp.c`.
- `tools/upstream/`: the copy's lists and scripts.
- The test harness and packaging scripts in `tools/` that LAS doesn't have (`run_scripted.sh`, `boot_check.sh`,
  `test_find_game.sh`, `dist_licenses.sh`, `dist.mk`, `launcher_notice.py`, `check_game_identity.py`,
  `audio_check.py`, `input/`, `frametime_summary.py`, `perf_summary.py`, `winshot.ps1`).

## Bringing a LAS fix over

1. `tools/upstream/sync.sh <LAS checkout> [rev]` lists every taken file LAS changed since the commit above, and says
   for each whether this repo changed it too.
2. Copy the files unchanged here; merge the others by hand; put a new LAS file in `removed.txt` if it belongs to their
   game.
3. Set the Commit line above to the new commit, and update the lists in this file.
