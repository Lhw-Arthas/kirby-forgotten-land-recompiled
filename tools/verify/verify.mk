# Static checks S1-S4 (tools/verify). The Makefile includes it with: -include tools/verify/verify.mk
# make verify-selftest             every tool's self-test (synthetic input, no game data)
# make build/verify/av_check.exe   the decoder check S4 runs, linked to the FFmpeg the runtime ships
VERIFY_PY   ?= python
VERIFY_DEPS ?= $(or $(DEPS),build/deps-win)

build/verify/av_check.exe: tools/verify/av_check.c
	@mkdir -p $(@D)
	clang -O1 -Wall -o $@ $< -I$(VERIFY_DEPS)/include -L$(VERIFY_DEPS)/lib -lavformat -lavcodec -lavutil

verify-selftest:
	$(VERIFY_PY) tools/verify/gen_gate.py --self-test
	$(VERIFY_PY) tools/verify/log_lint.py --self-test
	$(VERIFY_PY) tools/verify/service_check.py --self-test
	$(VERIFY_PY) tools/verify/asset_check.py --self-test

.PHONY: verify-selftest
