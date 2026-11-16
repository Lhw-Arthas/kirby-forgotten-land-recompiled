# What the launcher expects from the runtime

The Tauri launcher (`launcher/`) and RecompLauncher (`recomplauncher.json`) drive `ForgottenLandRecomp.exe` from outside.
This page lists everything they rely on, so the runtime can change without breaking them. The values that name the game
(program ids, folder names, file counts) come from `game/game.h`. The launcher keeps its own copy of them, checked by
`tools/check_game_identity.py`.

v0.2.0 implemented all of it in `runtime/main.c` (`setup_headless`) and `runtime/gamesetup.c`. The LAS engine has no
headless setup (its setup lives in `runtime/ui/launcher.mm`), so the runtime side is rebuilt over LAS's
`gamebuild_run`, `romfs_find_game` and `romfs_build`, and tested against this page and `tools/test_find_game.sh`.

## Process rules for every command below

- Started with the working directory set to the data root and `LARECOMP_DATA_ROOT=<data root>`. The data root is the
  install folder in the release zip. Without that variable `--prepare` must use the exe's own folder.
- The release exe is a GUI-subsystem program. Output the launcher reads goes to **stdout** as whole lines, flushed
  after each one. The launcher starts these commands with stdout piped, stdin null, and stderr either null
  (`--check`, `--status`) or the file `setup.log` (`--setup`, `--rebuild-code`). LAS's `plat_cli_*` channel writes to
  stderr, so it is not the channel for these lines. Lines that are not in the formats below are ignored, but a line
  that starts with `phase`, `done`, `ok` or `failed` is always read as a protocol line.
- `--check` and `--status` change nothing, open no window, write no session log (`game.log` and
  `game-backup-*.log` belong to a game run) and answer in well under a second on a folder of thousands of files.
- A command's stderr is free text for people. It is never parsed.

## Commands

| Command | stdout | Exit code |
|---|---|---|
| `extracted --play` | none (see "A game run") | see "A game run" |
| `--check <folder>` | one line, `ok` or `failed <reason>` | 0 for `ok`, 1 for `failed` |
| `--status` | one line, `ready`, `build` or `none` | 0 |
| `--setup <folder>` | `phase <name> <0..1> [(<slow file>, <n> min)]` lines, then `done` or `failed <reason>` | 0 for `done`, 1 for `failed` |
| `--rebuild-code` | the same lines as `--setup` | the same |
| `--prepare <folder>` | `progress <0..1> <text>` lines, ending with `progress 1 Ready` | 0, or the failure code of the table below |
| `extracted --build-romfs` | none | 0 when `extracted/romfs.bin` is a good image (it returns at once if one exists), else 1 and `romfs image: <why>` on stderr |

`--build-romfs` is used only by the launcher's developer mode, which builds from a source checkout.

### `--check <folder>`
The setup's search and program id check, without copying. A folder holds the game when the search finds:
- an `exefs` folder (any name, any depth up to the search limit) that holds `main` and `main.npdm`;
- a RomFS: a folder holding every name of `GAME_ROMFS_MARKERS` (`msg` and `gfx`), or a `romfs.bin` image;
- a program id in `main.npdm` equal to `GAME_PROGRAM_ID`. The id is at `ACI0 + 0x10`, where the `ACI0` offset is the
  32-bit value at `META + 0x70`.

Reasons, in the order they are decided:

| Reason | When |
|---|---|
| `not_found` | no exefs, or neither a RomFS folder nor a `romfs.bin`. Also when the exefs found is inside the data root's own `extracted` folder |
| `package` | as `not_found`, and the search met a `.xci`, `.nsp`, `.nca`, `.nsz` or `.xcz` file |
| `update` | the program id is the update's: `GAME_PROGRAM_ID + 0x800` (`01004D300C5AE800`). `game/game.h` has no define for it yet, so the engine side adds one |
| `version` | any other program id, or an unreadable `main.npdm` |

`update` is new in 0.3.0. `tools/test_find_game.sh` has a case for it (a `main.npdm` with `...E800`).
The launcher and `--setup` must use one search, so they cannot disagree about a folder (0.2.0 fixed exactly that).

### `--status`
What is set up in the data root:
- `ready`: `extracted/exefs` has the game's program id, `extracted/romfs.bin` is a readable image
  (`romfs_image_ok`), and `extracted/lasgame.dll` exists with a `lasgame.dll.id` equal to this runtime's id for it
  (the library id and the game's build id, `gamelib_full_id`).
- `build`: the same exefs and image, but the library is missing or its id differs. The game code needs building again
  from the files already in `extracted` (a new version unzipped over an old folder). `--rebuild-code` does it.
- `none`: anything else.

The launcher runs it once per change of these four files' modification times: the runtime exe, `extracted/lasgame.dll`,
`extracted/lasgame.dll.id` and `extracted/romfs.bin`. So the setup must write `lasgame.dll.id` last, after
`lasgame.dll`.

### `--setup <folder>` and `--rebuild-code`
`--setup` finds the game as `--check` does, replaces everything in `<data root>/extracted` that the setup made
(never the folder the player picked, never a link, nothing outside `<data root>/extracted`), copies `exefs`, copies or
builds `romfs.bin`, checks the image, then builds the game code. Hard links are used where the file system allows.
Killing it at any point is safe: the next run starts over. `--rebuild-code` does only the last step, from the files in
`extracted` (no folder argument; it reads the exefs there).

Phase names in `phase` lines, the fraction of the whole setup (copying and the image are the first 15%):

| Name | Meaning | Fraction |
|---|---|---|
| `looking` | searching the folder | 0 |
| `copying` | copying exefs and the image | up to 0.15 |
| `image` | building `romfs.bin` from a RomFS folder | up to 0.15 |
| `reading` | the generator is lifting the NSOs | from 0.15 |
| `compiling` | compiling the game code | to 0.99 |
| `finishing` | linking | 0.99 and over |

A line is printed only when the phase, the fraction (in thousandths) or the step text changed. A step text ends with
`(<file>, <n> min)` once one file has taken more than a minute (the file the compiler has been on longest), and the
launcher shows that text as it is. The compiler's step texts begin `Reading`, `Compiling` or `Finishing`.
`--rebuild-code` starts at `reading`.

Failure reasons after `failed`: the `--check` ones, and

| Reason | Meaning |
|---|---|
| `copy` | the files could not be copied (disk full, unreadable) |
| `build` | the game code did not build (or the build left no usable library) |
| `tools` | the generator or the compiler is missing from this install |
| `bad_image` | the `romfs.bin` (the picked one or the built one) is not a RomFS the game can read |

### `--prepare <folder>` (RecompLauncher)
The same setup as `--setup`, with the output RecompLauncher parses: `progress <0..1> <text>` where the text is the step
text, or one of `Looking for the game files`, `Copying the game files`, `Writing the game data image`,
`Reading the game code`, `Compiling the game code`, `Finishing`. Success ends with `progress 1 Ready` and exit 0.
A failure exits with its code and prints nothing more:

| Code | Reason | Code | Reason |
|---|---|---|---|
| 8 | `not_found` | 12 | `build` |
| 9 | `package` | 13 | `tools` |
| 10 | `version` | 14 | `bad_image` |
| 11 | `copy` | 15 | `update` |

The codes are the setup's enum values (`SETUP_FAIL_NOT_FOUND` = 8 in order). `recomplauncher.json` `exit_codes` words
each one for the player: it must have all eight.

## A game run
Both launchers start `ForgottenLandRecomp.exe extracted --play` (RecompLauncher adds nothing; the Tauri launcher
adds the environment below). `--play` goes straight to the game, never the LAS start screen.

- Working directory and `LARECOMP_DATA_ROOT`: the data root.
- `LARECOMP_GAME_LIB=<data root>\extracted\lasgame.dll` (Tauri launcher; RecompLauncher relies on the default place).
  The runtime loads that file as given and refuses a library whose id is not its own.
- The Tauri launcher truncates `game.log` in the data root and gives it as stdout and stderr, so the runtime must keep
  an inherited file or pipe as it is and not start a log of its own (LAS's `log_to_file_without_console` already does:
  windows.c, the `fresh && !g_cli` test). Started on its own, the runtime logs to `game.log` with
  `game-backup-*.log` copies (`GAME_LOG` in `game/game.h`), and the README and the launcher name `game.log`.
- The launcher puts each setup step it starts (`--setup`, `--rebuild-code`, the developer mode's make) in a
  kill-on-close job object, so closing the launcher ends a build and the compilers under it. The game it started is
  not in that job and keeps running.

Exit code and crash report, read by the launcher after the process ends (`classify` in `launcher/src-tauri/src/main.rs`):
- 0 is a normal close.
- A line of `game.log` that starts (after trimming) with `FATAL:` or `*** exception` is a crash, and that line is shown
  to the player (300 characters). `rt_fatal_msg` prints the first, `rt_fault_report` the second, then the process
  aborts, which exits with code 3. Code 3 without such a line is "aborted without a report".
- An exit code from 0xC0000000 up (an NTSTATUS) is "Windows ended the game with error ...".
- When the crash line holds `Vulkan` or `SDL_CreateWindow`, the player is told to update the graphics driver and the
  line follows in parentheses. A start-up failure that wants that advice must say one of those words.
- Any run that ends within 15 seconds of its start is reported as "closed N seconds after it started", crash line or not.
- The launcher copies `game.log` to `crash-<yyyymmdd>-<hhmmss>.log` for every reported run and keeps the last five.

## Files and names

In the data root (the install folder):

| Path | Who | What |
|---|---|---|
| `extracted\exefs\` | setup | `main`, `main.npdm`, `sdk`, `subsdk0`, and `rtld` when the dump has it |
| `extracted\romfs.bin` | setup | the game's data as one image, about 6 GB |
| `extracted\lasgame.dll`, `extracted\lasgame.dll.id` | setup | the game code and the id `--status` compares |
| `extracted\.gamebuild\` | setup | the generated C and the compiler's files, deleted when the build ends |
| `larecomp.ini` | runtime, launcher | settings, `key = value` lines (`SETTINGS_FILE` in `runtime/settings.c`) |
| `save\` | runtime | saves (`save\save_0\file0` and the rest) |
| `cache\` | runtime | shader and pipeline caches |
| `game.log` | launcher, or the runtime on its own | the log of the last run |
| `setup.log` | launcher | stdout and stderr of the last Build |
| `crash-*.log` | launcher | copies of `game.log` after crashes |
| `game-folder.txt` | launcher | the folder the player picked, until the build reads it |

The setup only ever deletes entries it made in `extracted` (the list `rt_remove_game_files` holds) and refuses when
`extracted` is a link or not `<data root>\extracted`. The launcher's developer mode links `extracted` to the player's
dump on purpose, so a runtime started by it must never run `--setup`.

Settings the launcher writes and RecompLauncher sets (`recomplauncher.json` `settings`), all of which the runtime's
`settings_load` must read: `fullscreen` (`0` or `1`), `resolution` (`match`, `75%`, `50%`, `25%`, `1x`, `2x`) and
`volume` (`0`..`1`). `tools/check_game_identity.py` checks that each name is in `runtime/settings.c`.

Beside the runtime in the release folder (found with `rt_resource`; the launcher reports each as a missing build
tool when it is absent):

| Path | What |
|---|---|
| `ForgottenLandRecomp.exe` | the runtime (`EXE_NAME` in `game/game.mk`) |
| `lasrecomp.dll.a` | the exe's import library, which the game code links against |
| `lasgen\lasgen.exe` | the frozen generator |
| `zig\zig.exe`, `zig\rt\link.rsp` | the compiler and its prebuilt runtime |
| `gamelib-src\` | `cpu.h`, `simd.h`, `simd_fp.h`, `gamelib.h`, `gamelib_entry.c`, `hle_symbols.txt`, plus every header they include |
| `icon.png`, `*.ttf` | window icon and fonts |

The launcher's own copy of the game's values: `PROGRAM_ID` and `UPDATE_ID` (program ids), `MODULES` (`main`, `sdk`,
`subsdk0`: the exefs files its developer-mode check requires), the RomFS marker folders `msg` and `gfx`, the
runtime's file name, `BUILD_BYTES` (6.5 GB free disk before a build) and `CODE_BYTES` (1 GB, a code-only rebuild).
`MAX_WORK_PATH` is 200 characters, measured with the bundled zig: the build adds about 45 below the install folder and
Windows stops at 260.

## Developer mode (source checkout)

The launcher beside a checkout (a `Makefile` and `runtime\` above the exe, none of the bundled tools) builds with
MSYS2 `make` and works in `KFL_WORK` (default `..\work\kfl`). It runs, from the repository, with `WORK=<work relative to
the repo>`:

```
make WORK=<w> -j<n> <w>/build/ForgottenLandRecomp.exe
<work>\build\ForgottenLandRecomp.exe extracted --build-romfs        (cwd <work>)
make WORK=<w> gen
make WORK=<w> -j<n>
```

and counts `*.c` in `<work>\generated` against `*.o` in `<work>\build\obj` for the progress bar. So the Makefile must
take `WORK` (the game files in `$(WORK)\extracted`, the generated C in `$(WORK)\generated`, the build in
`$(WORK)\build`), and keep the object paths short enough to link (a response file does that). It also needs
`.venv\Scripts\python.exe` in the repository. If the LAS Makefile keeps `build/` and `generated/` in the repository,
developer mode does not work, and `Mode::Dev` can be deleted instead: the release folder mode is the one players use.
