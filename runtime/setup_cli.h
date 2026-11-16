// The launchers' command line (docs/launcher-runtime-contract.md): --check <folder>, --status, --setup <folder>,
// --rebuild-code, --prepare <folder> and --build-romfs. Game file setup without a window, over the engine's own pieces:
// romfs_find_game (runtime/romfs_build.c), the copy, and gamebuild_run with its build cache (runtime/gamebuild.c).
// Its lines go to stdout, the channel the launchers read; the details go to stderr.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// First thing in main(), before plat_init: whether argv holds one of the commands. Resolves its folder against the
// directory the command was started in, and keeps plat_init from taking stdout for a session log.
bool setup_cli_parse(int argc, char **argv);
// After rt_paths_init (the data root is the working directory): runs the command, returns the exit code.
int setup_cli_run(void);

#ifdef __cplusplus
}
#endif
