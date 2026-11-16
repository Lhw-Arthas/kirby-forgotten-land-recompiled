# Kirby and the Forgotten Land Recompiled

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>
<a href="https://recompstore.com/dev/scarxity"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recompstore.com/assets/press/verified-light.svg"><img alt="Verified developer on RecompStore" src="https://recompstore.com/assets/press/verified-dark.svg" height="48"></picture></a>

A work-in-progress static recompilation of **Kirby and the Forgotten Land** (Nintendo Switch) for Windows.
The game's ARM64 code is translated to C and compiled for your PC, and a runtime stands in for the Switch. It is not
an emulator.

> [!IMPORTANT]
> **No game files are included, and none may be added.** You need your own legally obtained copy, extracted to
> `exefs` and `romfs` folders. This project provides no game files, keys or dumping tools. It is a fan project, not
> affiliated with Nintendo or HAL Laboratory.

## Status

See [docs/STATUS.md](docs/STATUS.md) for how far the game gets and what is known to be missing. In short: the
game boots, plays its opening and reaches the first stage with correct graphics.

Supported dump: the base game, v1.0.0 (program id `01004D300C5AE000`, `main` build id `d9ba7db72ffafecd`). The
launcher and the runtime accept any version of the game by program id, but only v1.0.0 has been built and run.

## Download

Get `ForgottenLandRecomp-<version>-Windows-x64.zip` from the
[latest release](https://github.com/scarxity/kirby-forgotten-land-recompiled/releases/latest), unzip it, open
`ForgottenLandLauncher.exe`, pick the folder that holds your `exefs` and `romfs`, then Build and Play. The zip holds
tools only: the game code is built on your PC from your own copy, in about 16 minutes on a 16-thread PC. The launcher
wants about 6.5 GB of free disk: your game files are hard-linked into the folder when they are on the same drive (no
extra space) and copied (about 6 GB) when they are not. Its `README.txt` has the details. [CHANGELOG.md](CHANGELOG.md) lists what changed.

The zip also carries a `recomplauncher.json`, so [RecompLauncher](https://recomplauncher.com) can set the game up
from the same folder, show its settings and start it.

## Layout

| Path | What it is |
|---|---|
| `game/` | What names this game: `game.h` (program id, folders, modules) and `game.mk` (exe name, version) |
| `runtime/` | The runtime, from LAS Recomp: kernel, services, NVN graphics on Vulkan, audio, input |
| `tools/` | The generator that lifts the game's NSO modules to C (`gen.py`, `lifter.py`), the packaging and the tests |
| `launcher/` | The launcher (Tauri 2): pick the game folder, build the game code, play |
| `third_party/` | Bundled libraries, each under its own license |
| `docs/` | Status, the upstream record, what the launcher expects from the runtime |

Nothing derived from the game lives in this repository. The extracted game files, the generated C and the build
output all go to a **work folder** outside it (`../work/kfl` by default, `WORK=` to change).

## Build from source (Windows)

Tools:

1. [Git for Windows](https://git-scm.com/download/win), Python 3.12, Node.js 20+, Rust (for the launcher).
2. MSYS2 with the UCRT64 packages, in PowerShell:
   ```powershell
   winget install --id MSYS2.MSYS2 -e
   $env:MSYSTEM='UCRT64'; C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm --disable-download-timeout make mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-spirv-tools mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-gdb mingw-w64-ucrt-x86_64-vulkan-memory-allocator mingw-w64-ucrt-x86_64-vulkan-validation-layers"
   ```
3. The generator's Python environment, in the repository folder:
   ```powershell
   python -m venv .venv; .venv\Scripts\python.exe -m pip install lz4 capstone unicorn
   ```

Then either use the launcher:

```powershell
cd launcher; npm install; npx tauri build
.\src-tauri\target\release\kfl-recomp-launcher.exe
```

Picking the game folder links it as the work folder's `extracted`. Press **Build**, then **Play**. Build builds the
runtime, checks the folder holds a RomFS and builds the game code. No copy of the game's data is made: the game reads
the `romfs` folder as it is.

Or build by hand in Git Bash, with your `exefs` and `romfs` in `../work/kfl/extracted/` (the game reads the `romfs`
folder as it is, so there is no data image to build):

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -j16 ../work/kfl/build/lasrecomp.exe   # the runtime
make gen                                    # translate the game code to C
make -j16                                   # compile it (about 10 minutes on 16 threads)
cd ../work/kfl && LARECOMP_GAME_LIB=build/lasgame.dll ./build/lasrecomp.exe extracted --play
```

## Credits

- [LAS Recomp](https://github.com/peiikai/LASRecomp) by Peiikai (MIT), the Switch recompiler this project is built on:
  the runtime (kernel, services, graphics, audio, input) and the game code generator are LAS Recomp's, copied at the
  commit named in [docs/UPSTREAM.md](docs/UPSTREAM.md) with its own copyright notices kept
- [letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) by raulzrrs (MIT), where this port started: the first
  Kirby port was made from it, and its services for this game are carried over
- The launcher's layout follows [Recompia](https://github.com/Dittobyte/Recompia)'s; its code and art are original

## License

The code in this repository is under the [MIT license](LICENSE); third-party components keep their own licenses. It
grants no rights to the game.
