// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden. GAMELIB_MODULES (-D, from
// the Makefile or the setup's build: game/game.h GAME_MODULES) names the modules, MOD(<name>) each, main first.
#include "gamelib.h"

#define MOD(n) extern const uint64_t n##_fn_addrs[], n##_fn_count; extern const GuestFn n##_fn_ptrs[];
GAMELIB_MODULES
#undef MOD

GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) {
#define MOD(n) +1
    enum { N = 0 GAMELIB_MODULES };
#undef MOD
#define MOD(n) {#n, n##_fn_addrs, n##_fn_ptrs, &n##_fn_count},
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, N, {GAMELIB_MODULES}};
#undef MOD
    return &t;
}
