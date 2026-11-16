Forgotten Land Recomp
=====================

A native Windows build of Kirby and the Forgotten Land (Nintendo Switch, version 1.0.0), made by static
recompilation: the game's code is translated into C and compiled into a program for your PC. It is not an emulator.
Project page and source code: https://github.com/scarxity/kirby-forgotten-land-recompiled

This is a work in progress. See docs/STATUS.md in the repository for what works today.

NO GAME FILES ARE INCLUDED, and no game code: this zip holds only tools. You need your own copy of the game, version
1.0.0, extracted to a folder that holds two folders, exefs and romfs (program id 01004D300C5AE000). Packed files
(.xci, .nsp, .nsz, .nca) must be extracted first. The game code is built on your PC from those files, and stays there.

What you need
-------------
- Windows 10 or 11, 64-bit, and a graphics card with a Vulkan driver.
- The Microsoft Edge WebView2 runtime, which the launcher window uses. Windows 11 has it. Most Windows 10 PCs have it
  too; if the launcher does not open, install it from https://developer.microsoft.com/microsoft-edge/webview2/
- Free disk space: about 6.5 GB (see below), on top of this folder's 280 MB.

How to start
------------
1. Unzip this folder anywhere you can write to (not inside Program Files). Keep the files together.
2. Open ForgottenLandLauncher.exe. It is not code-signed: if Windows says "Windows protected your PC", click
   "More info" and then "Run anyway". Do the same if it asks about ForgottenLandRecomp.exe.
3. Click "Select game folder" and pick the folder that holds your exefs and romfs folders.
4. Click "Build". The launcher copies the game files into this folder, then translates the game code and compiles it
   for your PC. This takes a while, once: about 16 minutes on a 16-thread PC (most of it compiling; the bar slows
   down near the end on two very large files, and names the file it is on). Fewer cores take longer. If your game
   folder has a romfs folder but no romfs.bin, writing that 6 GB image comes first. You can Cancel and Build again
   later; it starts over from the beginning.
5. Click "Play". Next time the launcher shows Play straight away.

Updating: unzip a new version over the old folder. If the new version needs the game code built again, the launcher
says so and offers "Rebuild game code" (about 15 minutes; your game folder is not needed again).

Closing the launcher ends a build in progress (Build again later; it starts over). A game that is running keeps
running.

Keep the path of this folder short, under about 200 characters (for example C:\Games\ForgottenLandRecomp): Windows
stops the build at long paths, and the launcher says so before it starts. It also checks the free disk space first.

If an antivirus removes or blocks lasgen\lasgen.exe or zig\zig.exe, the build cannot run. Restore them (or unzip again)
and allow this folder in the antivirus.

Disk space: the game data image (romfs.bin, 5.9 GB) and the game code (about 115 MB) are kept in the extracted
folder here. A romfs.bin already in your game folder, on the same drive, is hard-linked instead of copied and takes
no extra space. While building, the translated code and the compiler's files take a few hundred MB more, deleted
when the build ends. After a successful build the folder you picked is no longer needed by this copy.

Where things are
----------------
Everything stays in this folder:
- extracted\        the game files and the game code built from them (yours: never share them)
- larecomp.ini      settings (the launcher's Settings page changes some of them)
- save\             saves
- cache\            shader caches
- game.log          the log of the last time the game ran; setup.log, the log of the last Build
- crash-*.log       the logs of the last few crashes (the launcher shows a notice when the game crashes)
- game-folder.txt   the game folder you picked
To start over, delete the extracted folder (and save\ if you also want to drop your saves).

Legal and licences
------------------
Fan project, not affiliated with Nintendo or HAL Laboratory. MIT licensed (LICENSE.txt), based on letsgo-recomp by
raulzrrs (https://github.com/raulzrrs/letsgo-recomp), itself based on LAS Recomp by Peiikai
(https://github.com/peiikai/LASRecomp, licenses\LASRecomp-MIT.txt).

Bundled third-party software, each licence in the licenses folder:
- FFmpeg __FFVER__ (LGPL 2.1): avcodec, avformat, avutil, swresample, swscale DLLs, built with only the decoders the
  game's movies need. Source: https://ffmpeg.org/releases/ffmpeg-__FFVER__.tar.xz, built with MSYS2
  UCRT64 clang (tools/build_deps_windows.sh in the repository) as:
__FFCONF__
  To use another build, replace the DLLs with ones built from the same version.
- SDL3 (zlib), shaderc with glslang and SPIRV-Tools (Apache 2.0 and others), libiconv (LGPL), the GCC runtime
  libgcc and libstdc++ (GPL 3 with the GCC Runtime Library Exception) and winpthreads (MIT): from MSYS2 UCRT64.
- Dear ImGui (MIT), SMAA (MIT), stb (MIT or public domain) and bcdec, compiled into ForgottenLandRecomp.exe.
- AMD FidelityFX SDK runtime for FSR 3.1 (MIT, amd_fidelityfx_vk.dll).
- The Baloo 2 font (SIL Open Font License 1.1).
- zig\: Zig (MIT), which contains LLVM, clang and lld (Apache 2.0 with LLVM exception), with clang's headers and the
  MinGW-w64 headers and runtime (MinGW-w64 licences).
- lasgen\: the project's game code generator frozen with PyInstaller (bootloader: GPL 2 with an exception that
  allows this use), with Python (PSF licence, which also covers the libraries Python bundles), capstone
  (BSD 3-Clause), unicorn (GPL 2) and python-lz4 (BSD 3-Clause). Source of unicorn:
  https://github.com/unicorn-engine/unicorn (version 2.1.4).
- ForgottenLandLauncher.exe: built with Tauri; every Rust crate and JavaScript package in it, with its licence, is in
  licenses\ForgottenLandLauncher-third-party.txt.
