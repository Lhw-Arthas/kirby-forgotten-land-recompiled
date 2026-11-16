// The General tab. (LAS Recomp's Gameplay tab, with its difficulty levers, was its game's own.)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "i18n.h"
#include "option_rows.h"

extern "C" {
#include "../hle.h"
#include "../settings.h"
}

// General tab: the menus' and the game's language and the window's focus - settings that change how the game is
// shown, not how it plays (user, 2026-11-11). The start screen's System tab stays apart.
extern "C" void input_apply_background(void);  // hle/input.c

extern "C" void general_draw(float scale) {
    opt_begin("##general", scale);
    opt_section("General");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c), and the menus
        // at once. Each language by its own name (the row leaves names it has no translation for as they are). The
        // menus' own languages (pt-BR) have no game text: the game stays English.
        const char *code = settings_menu_language(), *auto_name = "English";
        for (int i = 1; i < N_LANGUAGES; i++)
            if (!strcmp(code, g_language_codes[i])) auto_name = g_language_endonyms[i];
        char autol[64];
        snprintf(autol, sizeof autol, L("Auto (%s)"), auto_name);
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_endonyms[i];
        bool menus_only = strcmp(code, "en-US") && !strcmp(settings_language(), "en-US");
        if (opt_choice("Language", &g_settings.language, names, N_LANGUAGES, LANG_AUTO,
                       menus_only ? "The language of the menus. The game has no text in this language, so it stays English."
                                  : "The language of the menus and the game's text. The game's text changes after a restart."))
            settings_save();
    }
    // (from Video, user 2026-11-12; a phone's window is the screen, and the app in the background is paused anyway)
    if (!PLAT_TOUCH && opt_toggle("Pause on focus lost", &g_settings.pause_unfocused, false,
                                  "The game pauses while its window isn't in front."))
        settings_save();
    // (from Controls, user 2026-11-12)
    if (!PLAT_TOUCH && opt_toggle("Allow background inputs", &g_settings.bg_input, false,
                                  "Controllers keep working while another window is in front. macOS may ask for Input "
                                  "Monitoring permission."))
        input_apply_background(), settings_save();
    opt_end();
}
