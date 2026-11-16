// The settings menu's sounds (menu_sfx.h). LAS Recomp played its game's own menu sounds from the player's files; no
// game layer yet, so the menus are silent. The frame hooks (menu_sfx_frame, menu_sfx_tab_item, menu_sfx_click_silent)
// live in runtime/ui/option_rows.mm.
#include "menu_sfx.h"

void menu_sfx_play(int id) { (void)id; }
void menu_sfx_play_gain(int id, float gain) { (void)id, (void)gain; }
void menu_sfx_play_ex(int id, float gain, float rate, float lowpass, float from, float len, float delay) {
    (void)id, (void)gain, (void)rate, (void)lowpass, (void)from, (void)len, (void)delay;
}
void menu_sfx_set_game_dir(const char *dir) { (void)dir; }
void menu_sfx_idle_check(void) {}
void menu_sfx_load_now(void) {}
