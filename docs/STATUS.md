# Status

This page says how far the game gets and what is known to be wrong. Every fix and the evidence
for it is in [decisions.tsv](decisions.tsv).

Supported dump: the base game, v1.0.0 (program id `01004D300C5AE000`, `main` build id `d9ba7db72ffafecd`).

## What works

| Stage | State |
|---|---|
| Translate and compile the game code | Works: `main`, `sdk` and `subsdk0` lift to C and build into `lasgame.dll` |
| Boot | Works: SDK and game start-up, services, RomFS, threads, Vulkan device and window |
| First screens | Works: the online prompt and the difficulty pick render and take input |
| Opening movie | Plays |
| Opening cutscene | Renders correctly |
| First stage | Reached with scripted input. Kirby walks, the scene is lit and on screen |
| Launcher | Works end to end: pick the game folder, build, cancel, play |

Reproduce the furthest point with `bash tools/run_scripted.sh <name> 170 "3400,5000"` and look at
`<work>/runs/<name>/fin_5000.png`. `tools/boot_check.sh` runs a timed boot without input.

## Known problems

- Nothing past the first seconds of the first stage has been tried. Expect more missing shader instructions,
  services and rendering faults.
- 3D scenes run at about 30 fps, with long stalls the first time a pipeline is compiled.
- Audio has only been checked for not crashing. Nobody has listened to it.
- Input has only been tested through the scripted path, not with a real controller or keyboard.
- One boot in four exited by itself with code 0 in an earlier session. A restart path that did exactly that is
  fixed, but nobody has shown it was the cause, and it did not happen again in 44 later runs.
- The launcher's build can fail to link when the work folder is on another drive or behind a long path.

## Stubs and shortcuts

- `nfp:user` (amiibo) reports no reader.
- `prepo:u`, `bcat:u` and `ectx:aw` log their calls and return success.
- Compute shaders get a fixed 32 KB of shared memory.
- Compute shader image stores (`SUST`) write float data only, and the image handle layout is inferred from the
  game's shaders.
- The shader translator rejects `EXIT` flow tests and `LEA` forms the game has not used so far.

## Inherited from letsgo-recomp, still to sort out

- The frame time handed to the graphics code is 33.3 ms (30 fps). Kirby runs at 60.
- Ambient occlusion runs at half strength, because a title-screen check is always true.
- The built-in start screen and F12 menu are letsgo-recomp's, with its graphics options (tuned for that game and
  untested here). The launcher starts the game with `--play`, which skips the start screen.
- The release packaging in the Makefile (`make dist`) has never been run for this project.
- Many code comments still describe Let's Go behaviour.
