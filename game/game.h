// Kirby and the Forgotten Land: what the engine (LAS Recomp's, docs/UPSTREAM.md) is told about the game. Only what
// names the game: everything else about it is read from the player's own files when they set it up. A new game
// replaces this file, game.mk and runtime/game_addresses/versions.h (the main program's build ID).
#pragma once

#define GAME_NAME "Forgotten Land Recomp"         // the app: window title, message boxes
#define GAME_TITLE "Kirby and the Forgotten Land"  // the game it runs
#define GAME_PROGRAM_ID 0x01004D300C5AE000ull       // the game's title id (svcGetInfo 18; its add-on ids follow it)
#define GAME_UPDATE_PROGRAM_ID (GAME_PROGRAM_ID + 0x800ull)  // the update's own program id: a dump of it is refused (setup "update")
#define GAME_REPO "scarxity/kirby-forgotten-land-recompiled"  // GitHub: the update check
#define GAME_LOG "game"  // the session log next to the exe: game.log, game-backup-*.log (the launcher reads game.log)

// The folders at the top of the game's RomFS: a folder holding all of them is the game's RomFS.
#define GAME_ROMFS_MARKERS "msg", "gfx"

// The program's modules in exefs, main first, then in load order, each at the game code's own address (rt.h
// GUEST_CANON: the window may move). The Makefile's gen and the setup's generator read this list.
#define GAME_MODULES(X) X(main, 0x7000000000) X(sdk, 0x7004000000) X(subsdk0, 0x7006000000)

// nn::init::Start as rtld calls it. 5 arguments (SDK 11 and later): thread handle, argument region, exception handler
// ready, init (every module: the SDK's, then main's), fini. 4 (earlier SDKs): thread handle, argument region, init the
// SDK modules, init main.
#define GAME_START_SYMBOL "_ZN2nn4init5StartEmmPFvvES2_S2_"
#define GAME_START_ARGS 5
