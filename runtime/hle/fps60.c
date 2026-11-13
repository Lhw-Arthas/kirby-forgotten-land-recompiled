// The frame rate the renderer (runtime/nvn/) asks the game side about. No frame-rate mode is hooked into this game, so the
// answers are fixed: nothing runs at a raised rate, no menu gets in-between images, the frame time is the 30 fps one.
// game_title() / game_cutscene() are the renderer's "title screen or cutscene" test (SSAO at half intensity there, no
// contact shadows on the title): tuned for Let's Go's title close-up, so never true here.
#include <stdbool.h>
#include <stdint.h>

long fps60_scene_start(void) { return -1; }
bool fps60_enabled(void) { return false; }
int fps60_rate(void) { return 60; }
uint64_t fps60_period_ns(void) { return 33333333ull; }
bool fps60_world(void) { return true; }
bool fps60_menu(void) { return false; }
bool game_title(void) { return false; }
bool game_cutscene(void) { return false; }

// At each present (runtime/nvn/objects.c)
void fps60_present(void) {
#ifdef _WIN32
    extern void memwatch_present(void);  // (runtime/hle/memwatch.c, research: LARECOMP_WATCH_LIKE)
    memwatch_present();
#endif
}
