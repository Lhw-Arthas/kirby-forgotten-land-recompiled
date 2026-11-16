# The Windows release (included by the Makefile: `include tools/dist.mk`, after GEN_VPY and the other variables it uses).
# Wiring: the Makefile has one line, `include tools/dist.mk`, before its `.PHONY` line, and none of LAS's own Windows `dist`
# (the DIST, WIN_B and WINZIP variables and the `dist:` rule): both define `dist`, and a second recipe overrides the first
# with a warning. LAS's `deps-windows` and `gen-helper` stay as they are.
#
#   make deps-windows    the LGPL FFmpeg the release links (the Makefile's rule: tools/build_deps_windows.sh -> build/deps-win)
#   make gen-helper      the game code generator frozen with PyInstaller (the Makefile's rule -> build/helper/lasgen)
#   make zig             the compiler (tools/fetch_zig.sh -> build/zig)
#   make dist            build/dist/$(EXE_NAME)-<version>-Windows-x64.zip, in that order
#   make test-find-game  the runtime's search for the game files, on synthetic dumps
#
# The zip's top folder $(EXE_NAME)/ holds ForgottenLandLauncher.exe (what the player starts: make launcher), the runtime
# $(EXE_NAME).exe and the DLLs they load (it runs without MSYS2), and what the runtime's setup builds the game code with
# on the player's PC (runtime/gamebuild.c): the frozen generator lasgen/, zig/ (slimmed by tools/zig_slim.sh: its runtime
# built ahead of time), gamelib-src/ and the exe's import library. Also README.txt (docs/release-README.txt),
# recomplauncher.json (RecompLauncher: how it sets the release up, starts it and shows its settings) and licenses/
# (tools/dist_licenses.sh). No game code and no game files: both are checked below. The runtime is a release build in
# $(WIN_B), linked against the LGPL FFmpeg (MSYS2's is a GPLv3 build), without a console window (RELEASE=1) and without
# the iOS export (IOSEXPORT=0: it needs miniz, and the launcher never shows the start screen it lives on). vulkan-1.dll is
# left out: every Vulkan driver installs the loader in System32. Game files, saves, settings and logs go next to the exes
# at run time. x86-64 only: the generator helper and zig are built for it.
DIST    := build/dist/$(EXE_NAME)
WIN_B   := build/win-release
ZIP     := build/dist/$(EXE_NAME)-$(VERSION)-Windows-x64.zip
LAUNCHER_OUT := build/launcher-target

zig:
	tools/fetch_zig.sh

# The launcher: its page built into it (tauri/custom-protocol, what `tauri build` adds: without it the exe loads the dev
# server's URL), the C runtime linked statically (no Visual C++ redistributable needed). TMP: a Windows path for MSVC's
# link.exe (an MSYS shell can hand it one it can't write to).
launcher:
	[ -d launcher/node_modules ] || (cd launcher && npm ci)
	cd launcher && node node_modules/vite/bin/vite.js build --logLevel warn --emptyOutDir --outDir "$(abspath build/launcher-web)"
	mkdir -p build/tmp
	# (relative to src-tauri: an absolute E:/... would be read as a URL)
	cd launcher/src-tauri && TAURI_CONFIG='{"build":{"frontendDist":"../../build/launcher-web"}}' \
	    TMP="$(shell cygpath -w $(abspath build/tmp))" TEMP="$(shell cygpath -w $(abspath build/tmp))" \
	    RUSTFLAGS="-C target-feature=+crt-static --remap-path-prefix=$$(cygpath -w "$${CARGO_HOME:-$$USERPROFILE/.cargo}")=cargo \
	    --remap-path-prefix=$$(cygpath -w "$${RUSTUP_HOME:-$$USERPROFILE/.rustup}")=rustup" \
	    cargo build --release --locked --features tauri/custom-protocol \
	    --target-dir "$(abspath $(LAUNCHER_OUT))"

dist: launcher
	@[ "$(WINARCH)" = x86_64 ] || { echo "the release is Windows x64 only"; exit 1; }
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows first (LGPL FFmpeg)"; exit 1; }
	@[ -d build/helper/lasgen ] || { echo "make gen-helper first (the game code generator)"; exit 1; }
	@[ -x build/zig/zig.exe ] || { echo "make zig first (the compiler)"; exit 1; }
	$(abspath $(GEN_VPY)) tools/check_game_identity.py
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win RELEASE=1 IOSEXPORT=0 $(WIN_B)/$(EXE_NAME).exe
	! nm $(WIN_B)/$(EXE_NAME).exe | grep ' f_70' >/dev/null  # (no game code inside)
	rm -rf build/dist && mkdir -p "$(DIST)/gamelib-src"
	cp $(WIN_B)/$(EXE_NAME).exe $(WIN_B)/lasrecomp.dll.a "$(DIST)/"
	cp $(LAUNCHER_OUT)/release/kfl-recomp-launcher.exe "$(DIST)/ForgottenLandLauncher.exe"
	GAMELIB_MODULES='$(foreach m,$(GAME_MODULE_NAMES),MOD($(m)))' tools/zig_slim.sh $(strip $(filter-out -Iruntime,$(GENFLAGS)))  # (zig with its runtime built ahead of time)
	cp -R build/helper/lasgen "$(DIST)/"
	cp -R build/zig-slim "$(DIST)/zig"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    "$(DIST)/gamelib-src/"
	cp docs/icon/icon_1024.png "$(DIST)/icon.png"  # (window icon)
	cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$(DIST)/"  # (in-game menu font, OFL)
	cp third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$(DIST)/"  # (last-resort fonts, free license)
	cp LICENSE "$(DIST)/LICENSE.txt"
	cp recomplauncher.json "$(DIST)/"
	# (the FFmpeg version and configure line it was built with: what the LGPL asks to make it rebuildable)
	ffconf=$$(sed -n 's/^#define FFMPEG_CONFIGURATION "--prefix=[^ ]* \(.*\)"$$/  .\/configure --prefix=<prefix> \1/p' build/deps-win/src/ffmpeg/config.h); \
	sed -e "s|__FFVER__|$$(cat build/deps-win/src/ffmpeg/RELEASE)|g" -e "s|__FFCONF__|$$ffconf|" docs/release-README.txt \
	    > "$(DIST)/README.txt"
	! grep '__[A-Z]*__' "$(DIST)/README.txt"
	for exe in $(EXE_NAME) ForgottenLandLauncher; do \
	    PATH="$(CURDIR)/build/deps-win/bin:$$PATH" ldd "$(DIST)/$$exe.exe" | awk '(/ucrt64/ || /deps-win/) && !/vulkan-1/ {print $$3}'; \
	done | sort -u | xargs -I{} cp -u {} "$(DIST)/"
	PY=$(abspath $(GEN_VPY)) tools/dist_licenses.sh "$(DIST)"
	! ls "$(DIST)" | grep -iE 'x264|x265|xvid|postproc'  # (no GPL codec libraries in the release)
	! find "$(DIST)" -iname '*.nso' -o -iname '*.nca' -o -iname '*.nsp' -o -iname '*.xci' -o -iname '*.keys' \
	    -o -iname 'romfs*' -o -iname 'exefs' -o -iname 'lasgame.*' -o -iname 'generated' | grep .  # (nothing of the game)
	# (no path of this machine inside: build/deps-win, debug info, zig's runtime. Not on CI: its paths name no one)
	if [ "$${CI:-}" != true ]; then \
	    : "$${USERPROFILE:?empty: an MSYS2 shell started from Git Bash drops it, pass USERPROFILE=... to make}"; \
	    case "$$(basename "$$(cygpath -u "$$USERPROFILE")")" in runner|runneradmin) h= ;; *) h=$$(cygpath -m "$$USERPROFILE") ;; esac; \
	    ! grep -rlaiF -e "$(CURDIR)" -e "$$(cygpath -m '$(CURDIR)')" -e "$$(cygpath -w '$(CURDIR)')" \
	        $${h:+-e "$$h" -e "$$(cygpath -w "$$h")"} "$(DIST)"; fi
	$(abspath $(GEN_VPY)) tools/check_game_identity.py --dist "$(DIST)"
	cd build/dist && $(abspath $(GEN_VPY)) -c "import shutil, sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[2])" \
	    "$(basename $(notdir $(ZIP)))" "$(EXE_NAME)"
	@echo "$(ZIP)"

# The runtime's search for the game files, on synthetic dumps (tools/test_find_game.sh)
test-find-game: $(EXE)
	bash tools/test_find_game.sh $(EXE)

.PHONY: zig launcher test-find-game
