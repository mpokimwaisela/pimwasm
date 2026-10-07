SHELL := /bin/bash
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
CC := gcc
WASM_CC := clang-15
WASM_LD := /usr/bin/wasm-ld-15
DPU_CC := dpu-upmem-dpurte-clang
HOST_FLAGS := -O2 -g -std=c11 -Wall -Wextra
SDK_FLAGS := $(shell dpu-pkg-config --cflags --libs dpu)

.PHONY: all clean inspect versions
all: examples
build:
	mkdir -p $@

include make/pimwasm.mk

inspect: pimwasm
	@for name in $(EXAMPLE_NAMES); do wasm-objdump -x build/examples/$$name/module.wasm || exit; llvm-readelf -h -S build/examples/$$name/dpu || exit; done
versions:
	$(CC) --version
	$(WASM_CC) --version
	$(WASM_LD) --version
	$(DPU_CC) --version
	wasm2c --version
	wasm-validate --version
	cat /usr/share/upmem/version
clean:
	rm -rf build
