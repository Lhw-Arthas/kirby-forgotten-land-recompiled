Forgotten Land Recomp
=====================

A native Windows build of Kirby and the Forgotten Land (Nintendo Switch, version 1.0.0) by static recompilation.
Project page, documentation and source code: https://github.com/scarxity/kirby-forgotten-land-recompiled

EARLY AND NOT PLAYABLE YET. It boots and reaches the first stage, but much is missing or broken. See the
repository README for the current state.

NO GAME FILES ARE INCLUDED. You need your own legally obtained copy of the game, extracted (the exefs and romfs
folders, program id 01004D300C5AE000, version 1.0.0). Packed files (.xci, .nsp, .nsz, .nca) must be extracted first.

How to start
------------
1. Unzip this folder anywhere you can write to (not inside Program Files).
2. Open ForgottenLandRecomp.exe. It is not code-signed: if Windows says "Windows protected your PC", click
   "More info" and then "Run anyway".
3. Click "Select game folder..." and pick the folder with your extracted game.
4. Wait while the game files are copied and the game code is recompiled for your PC (once).
5. Click Play.

Everything stays in this folder: settings (larecomp.ini), saves, the game files (extracted\), mods and the log
(lasrecomp.log). If something goes wrong, the log says where.

Legal
-----
Fan project, not affiliated with Nintendo or HAL Laboratory. MIT licensed (LICENSE.txt), based on letsgo-recomp by
raulzrrs (https://github.com/raulzrrs/letsgo-recomp), itself based on LAS Recomp by Peiikai (LICENSE-LASRecomp.txt,
https://github.com/peiikai/LASRecomp).
Bundled: SDL3 (zlib), FFmpeg (LGPL 2.1, built with only the decoders the game needs; source: https://ffmpeg.org),
shaderc (Apache 2.0), libiconv (LGPL), Zig (MIT), the Baloo 2 font (SIL OFL 1.1, Baloo2-OFL.txt),
AMD's FidelityFX SDK runtime for FSR 3.1 (MIT, amd_fidelityfx_vk.dll, FidelityFX-LICENSE.txt), and the
GCC / MinGW-w64 runtime libraries (GPL with the runtime library exception).
