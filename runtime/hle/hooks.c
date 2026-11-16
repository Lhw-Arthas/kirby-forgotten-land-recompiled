// Hook points: instructions of the game's code that call into the runtime (tools/patches.py HOOKS). The generated
// code at a hook site runs `rt_hook(c, site)` first; what a hook does lives here, so changing it rebuilds the app,
// not the game code.
//
// rt_hook returns 0: run the site's own instruction, 1: the hook did it all, 2..: jump to the site's targets
// (HOOKS[site][1], in order). The game's own; no hook sites yet.
#include "../cpu.h"

int rt_hook(Cpu *c, uint64_t site) {
    (void)c, (void)site;
    return 0;
}
