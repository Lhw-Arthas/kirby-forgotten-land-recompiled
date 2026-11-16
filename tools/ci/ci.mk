# The checks CI runs and the ones that need the game, as make targets. Add to the end of the Makefile:
#   -include tools/ci/ci.mk
# make ci-test                 build the runtime, then tools/ci/run.sh engine repo (what CI runs; no game files)
# make verify GAME=<dump>      tools/ci/verify.sh: the checks that need the player's extracted game files (a folder
#                              holding exefs/ and romfs/); results go to <dump>/../verify-out, outside the repo
.PHONY: ci-test verify
ci-test:
	$(MAKE) $(EXE)
	EXE=$(EXE) bash tools/ci/run.sh engine repo

verify:
	bash tools/ci/verify.sh "$(GAME)"
