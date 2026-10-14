# The browser demo defaults to signet; TESTNET=0 selects mainnet.
WEB_TESTNET ?= $(if $(filter command line environment,$(origin TESTNET)),$(TESTNET),1)
WEB_TEST_SEED ?= $(if $(filter command line environment,$(origin TEST_SEED)),$(TEST_SEED),1)

ifeq ($(WEB_TESTNET),1)
WEB_BUILD_DIR := build/web-signet
WEB_EXTRA_TARGET := build/web-signet/airgap-trial.html build/web-signet/jsQR.min.js build/web-signet/jsQR-LICENSE.txt
WEB_TEST_PSBT_H_DIR := $(CURDIR)/build/web-signet/inputs
WEB_TEST_PSBT_H := build/web-signet/inputs/test_psbt.h

build/web-signet/inputs/test_psbt.h: tools/generate/gen_psbt_vectors.py
	mkdir -p build/web-signet/psbt build/web-signet/inputs
	uv run -q $< build/web-signet/psbt 1
	cd build/web-signet/psbt && xxd -i -n test_psbt own_mixed_nwu.psbt \
	  | sed 's/^unsigned char/const unsigned char/' > ../inputs/test_psbt.h
else
WEB_BUILD_DIR := build/web
WEB_EXTRA_TARGET :=
WEB_TEST_PSBT_H_DIR := $(CURDIR)/build
WEB_TEST_PSBT_H := build/test_psbt.h
endif

WEB_EXTRA_TARGET += $(WEB_BUILD_DIR)/html2canvas.min.js $(WEB_BUILD_DIR)/html2canvas-LICENSE.txt
PARSER_SHA256 := $(shell awk '$$2 == "build/parser.wasm" { print $$1 }' checksums.txt)

.PHONY: FORCE_WEB_CONFIG
FORCE_WEB_CONFIG:

web: $(WEB_BUILD_DIR)/signer.html $(WEB_EXTRA_TARGET)

$(WEB_BUILD_DIR)/html2canvas.min.js: web/vendor/html2canvas.min.js
	mkdir -p $(dir $@)
	cp $< $@

$(WEB_BUILD_DIR)/html2canvas-LICENSE.txt: web/vendor/html2canvas-LICENSE.txt
	mkdir -p $(dir $@)
	cp $< $@

$(WEB_BUILD_DIR)/signer.html: FORCE_WEB_CONFIG build/parser_wasm.h build/font8x16.h $(WEB_TEST_PSBT_H) web/CMakeLists.txt web/hal.c web/shell.html \
  web/send_support.c src/main.c src/runtime/parser_host.c src/ui/ui.c $(CORE_SRC) components/parts/parser/c/include/*.h
	emcmake cmake -S web -B $(WEB_BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release \
	  -DTESTNET=$(WEB_TESTNET) -DTEST_SEED=$(WEB_TEST_SEED) -DSIGNER_WASM_H_DIR=$(CURDIR)/build \
	  -DTEST_PSBT_H_DIR=$(WEB_TEST_PSBT_H_DIR) -DCORE_SRC="$(strip $(CORE_SRC))" >/dev/null
	ninja -C $(WEB_BUILD_DIR)

build/web-signet/airgap-trial.html: build/web-signet/signer.html web/airgap-trial.html components/parts/parser/hosts/js/parser.mjs build/parser.wasm checksums.txt
	sed 's/^const PARSER_SHA256 = ".*";/const PARSER_SHA256 = "$(PARSER_SHA256)";/' web/airgap-trial.html > $@
	cp components/parts/parser/hosts/js/parser.mjs build/web-signet/parser.mjs
	cp build/parser.wasm build/web-signet/parser.wasm

build/web-signet/jsQR.min.js: components/parts/examples/viewer/vendor/jsQR.min.js
	cp $< $@

build/web-signet/jsQR-LICENSE.txt: components/parts/examples/viewer/vendor/jsQR-LICENSE.txt
	cp $< $@

.PHONY: web
