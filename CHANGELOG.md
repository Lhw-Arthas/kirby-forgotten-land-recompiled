# Changelog

Versions are tagged `vX.Y.Z`. Small fixes bump the last number; features and large fixes bump the middle one.

## 0.1.1

The first version with a download. **The game is still not playable**: it has only been run through the opening and
the start of the first stage.

No game files and no game code are included. The zip holds tools only; the game code is built on your PC from your
own extracted copy (base game, v1.0.0).

### Highlights
- **A download that needs no developer tools.** Unzip `ForgottenLandRecomp-0.1.1-Windows-x64.zip`, open
  `ForgottenLandLauncher.exe`, pick your game folder, Build, Play. The first build takes about 16 minutes on a
  16-thread PC and about 6.5 GB of disk.
- **RecompLauncher support.** The zip carries a `recomplauncher.json`: RecompLauncher can set the game up from your
  folder, show the Window, Resolution and Volume settings, and start it.

### Added
- The release zip: the launcher, the runtime, the code translator and a compiler, with their licences
- `--setup <folder>` and `--prepare <folder>` in the runtime: the setup with no window, for the two launchers
- A notice on the launcher's Home page when the game stops unexpectedly, with the reason and a kept copy of the log
- The build progress names the file being compiled once it has taken more than a minute

### Fixed
- A crash in the first stage when a tutorial clip played: a finished movie's buffers were kept for the next movie
  although the game had already reused that memory

### Known issues
- Each tutorial clip's window stays black for its first few seconds
- Frame generation does not work: leave it off
- Online features do not work: choose "Do Not Use" at the prompt
- Performance: about 30 fps in 3D, with pauses while graphics pipelines compile
- Only extracted `exefs` and `romfs` folders are accepted, not `.nsp` or `.xci` files
- After unzipping a new version over an old one, build the game code again from the Set up page

## 0.1.0

First tagged version, source only. The game boots, plays its opening and reaches the first stage with correct
graphics.

### Added
- The generator and runtime, imported from letsgo-recomp (MIT, built on LAS Recomp) and retargeted to this game
- Shader translator support for `VOTE.VTG`, `EXIT` flow tests, `LEA`, shared-memory loads and stores, and image
  stores with Vulkan storage images
- A launcher: pick your game folder, build the game code, play
