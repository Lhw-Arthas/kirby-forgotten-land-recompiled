// LARECOMP_STAGE=<Category>/<Stage>/<StepNN> (the stage tour, tools/verify/stage_tour.sh): a new game starts in that map
// step (a folder under the RomFS's ubin/Step/Map/) instead of the game's first one.
//
// The game's scripts name the step to load as a string and hand it on in fixed 128-byte string fields, copied with
// memcpy from struct to struct until the step loader opens the step's files. The first step name copied after boot is
// the new game's first step; from then on every copy of that name is rewritten to the target before it is made, so
// everything downstream (map data, decor, the loader) sees the target. Nothing about the game is stored here: a step
// name is recognised by its folder existing in the RomFS.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

bool romfs_has_prefix(const char *prefix);  // services/fs.c

volatile int g_stage_jump;  // hle/libc.c: memcpy calls stage_jump_memcpy while it is set
static const char *g_target;
static char g_first[128];  // the new game's own first step, once seen

RT_ON_RESET(stage_jump_reset) { g_first[0] = 0; }

__attribute__((constructor)) static void stage_jump_init(void) {
    g_target = getenv("LARECOMP_STAGE");
    g_stage_jump = g_target && *g_target && strlen(g_target) < sizeof g_first;
}

// memcpy(x0, x1, x2), before the copy
void stage_jump_memcpy(Cpu *c) {
    uint64_t s = c->x[1], n = c->x[2];
    if (n <= strlen(g_target) || n > 0x1000 || s < GUEST_LO || s >= GUEST_HI) return;
    char *src = (char *)(uintptr_t)s;
    size_t len = strnlen(src, sizeof g_first);
    if (len < 8 || len == sizeof g_first || len >= n) return;
    if (g_first[0]) {
        if (strcmp(src, g_first)) return;
    } else {
        int slashes = 0;  // (<Category>/<Stage>/<Step>: cheap to rule out before the RomFS lookup)
        for (size_t i = 0; i < len; i++) slashes += src[i] == '/';
        if (slashes != 2 || !strcmp(src, g_target)) return;
        char dir[sizeof g_first + 24];
        snprintf(dir, sizeof dir, "ubin/Step/Map/%s/", src);
        if (!romfs_has_prefix(dir)) return;
        memcpy(g_first, src, len + 1);
        LOG("[stage] the new game's first step %s: starting in %s\n", g_first, g_target);
    }
    memset(src, 0, len);
    memcpy(src, g_target, strlen(g_target) + 1);
}
