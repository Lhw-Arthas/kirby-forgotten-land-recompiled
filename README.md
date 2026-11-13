# Kirby and the Forgotten Land Recompiled

An early, work-in-progress static recompilation of **Kirby and the Forgotten Land** (Nintendo Switch) for Windows.
The game's ARM64 code is translated to C and compiled for your PC, and a runtime stands in for the Switch. It is not
an emulator.

> [!IMPORTANT]
> **No game files are included, and none may be added.** You need your own legally obtained copy, extracted to
> `exefs` and `romfs` folders. This project provides no game files, keys or dumping tools. It is a fan project, not
> affiliated with Nintendo or HAL Laboratory.

## Status

See [docs/STATUS.md](docs/STATUS.md) for how far the game gets and what is known to be missing. In short: the
game boots, plays its opening and reaches the first stage with correct graphics, but it is **not tested beyond
that and not playable yet**.

Supported dump: the base game, v1.0.0 (program id `01004D300C5AE000`, `main` build id `d9ba7db72ffafecd`).

## Layout

| Path | What it is |
|---|---|
| `tools/` | The generator: lifts the game's NSO modules to C (`gen.py`, `lifter.py`) |
| `runtime/` | The runtime: kernel, services, NVN graphics on Vulkan, audio, input |
| `launcher/` | The launcher (Tauri 2): pick the game folder, build the game code, play |
| `third_party/` | Bundled libraries, each under its own license |

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

Pick your game folder, press **Build**, then **Play**.

Or build by hand in Git Bash, with your `exefs` and `romfs` in `../work/kfl/extracted/`:

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -j16 ../work/kfl/build/lasrecomp.exe   # the runtime
make gen                                    # translate the game code to C
make -j16                                   # compile it (about 10 minutes on 16 threads)
cd ../work/kfl && LARECOMP_GAME_LIB=build/lasgame.dll ./build/lasrecomp.exe extracted --play
```

## Credits

- [letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) by raulzrrs, the runtime and generator this port starts from
- [LAS Recomp](https://github.com/peiikai/LASRecomp) by Peiikai, which letsgo-recomp is built on
- The launcher's layout follows [Recompia](https://github.com/Dittobyte/Recompia)'s; its code and art are original

## License

The code in this repository is under the [MIT license](LICENSE); third-party components keep their own licenses. It
grants no rights to the game.
