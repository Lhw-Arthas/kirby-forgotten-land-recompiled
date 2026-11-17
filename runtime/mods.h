// Player mods: <data root>/mods/<mod>/romfs/<path> replaces that RomFS file (services/fs.c), the layout Ryujinx and
// yuzu use. mods/mods.txt, written by the launcher, lists mods in order, one a line, "-<mod>" for one switched off.
#pragma once
#include <stdbool.h>

typedef char ModName[256];

// The mods in dir that are switched on, in order: those mods.txt lists first, then the rest in name order. A folder
// is a mod when it holds romfs/. *out: malloc'd (free it), the count returned.
int mods_order(const char *dir, ModName **out);
