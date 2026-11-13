# Status

Not playable yet. This page says how far the game gets and what is known to be wrong. Every fix and the evidence
for it is in [decisions.tsv](decisions.tsv).

Supported dump: the base game, v1.0.0 (program id `01004D300C5AE000`, `main` build id `d9ba7db72ffafecd`).

## What works

| Stage | State |
|---|---|
| Translate and compile the game code | Works: `main`, `sdk` and `subsdk0` lift to C and build into `lasgame.dll` |
| Boot | Works: SDK and game start-up, services, RomFS, threads, Vulkan device and window |
| First screens | Works: the online prompt and the difficulty pick render and take input |
| Opening movie | Plays |
| First stage | Reached with scripted input. Kirby moves. The scene is drawn in the internal passes |
| Image on screen in 3D scenes | **Broken: the presented frame is white** |

Reproduce the furthest point with `bash tools/run_scripted.sh <name> 170 "5000" LARECOMP_DEBUG_FRAME=5000`.
`tools/boot_check.sh` runs a timed boot without input.

## Known problems

- 3D scenes are presented white. The g-buffer is correct, so the fault is in lighting, tone mapping or the final
  composite.
- Two compute shaders fail to translate (the Maxwell `SUST` surface store). Storage images are not implemented.
- About one boot in four exits by itself with code 0. Cause unknown.
- A UI shader samples two 3D lookup textures as 2D (`[tex] type mismatch`).
- 3D scenes run at 17 to 30 fps, with long stalls the first time a pipeline is compiled.
- Audio has not been tested.
- Nothing past the start of the first stage has been tried.

## Stubs and shortcuts

- `nfp:user` (amiibo) reports no reader.
- `prepo:u`, `bcat:u` and `ectx:aw` log their calls and return success.
- Compute shaders get a fixed 32 KB of shared memory.
- The shader translator rejects `EXIT` flow tests and `LEA` forms the game has not used so far.

## Inherited from letsgo-recomp, still to sort out

- The frame time handed to the graphics code is 33.3 ms (30 fps). Kirby runs at 60.
- Ambient occlusion runs at half strength, because a title-screen check is always true.
- The built-in start screen and F12 menu still carry Let's Go options and text. The launcher starts the game with
  `--play`, which skips the start screen.
- The release packaging in the Makefile (`make dist`) is still named and laid out for Let's Go Recomp, and it has
  not been run for this project.
