# ports/mlp1/Makefile
# Cross-compile dispatcher for the Miniloong Pocket 1 target.

SHELL := /bin/bash

JAWAKA_DIR ?= /workspace/Jawaka
BUILD ?= build/mlp1
CATASTROPHE_DIR ?= /workspace/Catastrophe

.PHONY: all clean

all:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		CFLAGS_PLATFORM="-DPLATFORM_MLP1" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		all

clean:
	$(MAKE) -C "$(JAWAKA_DIR)" BUILD="$(BUILD)" clean
