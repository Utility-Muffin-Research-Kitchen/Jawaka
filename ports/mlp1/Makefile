# ports/mlp1/Makefile
# Cross-compile dispatcher for the Miniloong Pocket 1 target.

SHELL := /bin/bash

JAWAKA_DIR ?= /workspace/Jawaka
BUILD ?= build/mlp1
CATASTROPHE_DIR ?= /workspace/Catastrophe
MLP1_BUILD_PROFILE ?= release

.PHONY: all pakrat-smoke clean

all:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		all

pakrat-smoke:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		jawaka-pakrat-smoke

clean:
	$(MAKE) -C "$(JAWAKA_DIR)" BUILD="$(BUILD)" clean
