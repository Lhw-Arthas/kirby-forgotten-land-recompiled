// What the engine asks the game about its state (the pacer, frame generation, the slow-frame log, the menus). LAS
// Recomp answered from Link's Awakening's own objects (hle/warp.c, widescreen.c, mirror.c); with no game layer yet
// the answers are neutral: never in play, so everything that waits for gameplay stays off.
// Also the game-independent diagnostics that lived in hle/warp.c.
#include <stdbool.h>
#include <stdint.h>

#include "../hle.h"

int g_start_screen;  // runtime/ui/launcher.mm: the start screen is up

int game_in_play(void) { return 0; }
const char *game_level_name(void) { return "-"; }
bool world_on_screen(void) { return false; }
uint64_t game_camera_zone(void) { return 0; }
bool game_camera_zone_id(uint32_t *id) { (void)id; return false; }
void romfs_file_read_hook(const char *path) { (void)path; }  // (services/fs.c: each RomFS file as the game reads it)

// Debug: who calls vector::at() out of range? Log the guest return address and frame chain, then stop.
HLE_FUNC("_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", rt_vector_oor) {
    LOG("[oor] vector::at out of range: lr %llx\n", (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 12 && fp; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, ret = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG("[oor]   frame %d ret %llx\n", i, (unsigned long long)ret);
        fp = next;
    }
    LOG("[oor] regs x0 %llx x1 %llx x2 %llx x8 %llx x9 %llx x19 %llx x20 %llx x21 %llx\n", (unsigned long long)c->x[0],
        (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[8], (unsigned long long)c->x[9],
        (unsigned long long)c->x[19], (unsigned long long)c->x[20], (unsigned long long)c->x[21]);
    FATAL("vector::at out of range (see [oor] lines)\n");
}

// Any C++ exception the game throws ends the game here: recompiled code can't unwind to a catch, so a throw always
// ended in std::terminate - "no function at <heap address> (lr=... get_terminate+20)", which says nothing about what
// was thrown or where (issues #41, #60, #155: an empty event handler, caught by the game on the Switch; hooks.c skips
// that call now). Name the exception (its type_info, x1) and the game code that threw it, then stop as before.
HLE_FUNC("__cxa_throw", rt_cxa_throw) {
    uint64_t ti = c->x[1], nm = ti >= GUEST_LO && ti < GUEST_HI ? LD64(ti + 8) : 0;  // (type_info: vtable, name)
    nm &= ~(1ull << 63);  // (libc++ marks non-unique type names in bit 63)
    const char *type = nm >= GUEST_LO && nm < GUEST_HI ? (const char *)(uintptr_t)nm : "?";
    LOG("[throw] uncaught C++ exception %.80s thrown at %llx\n", type, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 12 && fp >= GUEST_LO && fp < GUEST_HI; i++, fp = LD64(fp))
        LOG("[throw]   frame %d ret %llx\n", i, (unsigned long long)LD64(fp + 8));
    FATAL("uncaught C++ exception %.80s thrown at %llx (see [throw] lines)\n", type, (unsigned long long)c->x[30]);
}
