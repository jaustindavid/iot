# hal/host/Makefile — host-side C++ harness.
#
# Usage:
#   make DEVICE=rachel_raccoon thyme          # compile at rachel's geometry, run thyme
#   make DEVICE=rachel_raccoon thyme-run      # re-run without rebuild
#   make DEVICE=rachel_raccoon thyme ARGS="--ticks 500 --seed 42"
#   make clean
#
# Mirrors photon2/Makefile's assembly model. Grid dimensions come from
# the device header's GRID_WIDTH/GRID_HEIGHT (passed via -D on the g++
# command line; no creds.h is copied because host doesn't need the full
# stra2us/pin/pixel-type surface).

CXX      ?= c++
PYTHON   ?= python3
DEVICE   ?= rachel_raccoon
ARGS     ?= --ticks 200 --seed 42 --dump-fake-time 2026-04-19T14:35

REPO_ROOT   := $(abspath ../..)
HAL_ROOT    := $(abspath ..)
DEVICES_DIR := $(HAL_ROOT)/devices
AGENTS_DIR  := $(REPO_ROOT)/agents
IR_ENCODER  := $(REPO_ROOT)/hal/ir/ir_encoder.py

BUILD_DIR    := build
DEVICE_HEADER := $(DEVICES_DIR)/$(DEVICE).h

DEVICES := $(basename $(filter-out device_name.h, $(notdir $(wildcard $(DEVICES_DIR)/*.h))))
SCRIPTS := $(basename $(notdir $(wildcard $(AGENTS_DIR)/*.crit)))

# Pull GRID_WIDTH / GRID_HEIGHT out of the chosen device header.
GRID_WIDTH  = $(shell grep GRID_WIDTH  $(DEVICE_HEADER) | awk '{print $$3}')
GRID_HEIGHT = $(shell grep GRID_HEIGHT $(DEVICE_HEADER) | awk '{print $$3}')

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
            -I $(HAL_ROOT) -I $(BUILD_DIR) -I src

ENGINE_SRCS := $(HAL_ROOT)/CritterEngine.cpp $(HAL_ROOT)/ErrLog.cpp $(HAL_ROOT)/ir/IrRuntime.cpp src/main.cpp

.PHONY: list clean $(SCRIPTS) $(addsuffix -run, $(SCRIPTS))

default: list

list:
	@echo "Host harness. Usage: make DEVICE=<device> <script>"
	@echo "  devices: $(DEVICES)"
	@echo "  scripts: $(SCRIPTS)"

# Compile for a given script and run. Script name → target.
$(SCRIPTS): %: $(BUILD_DIR)/%.bin
	@echo "== Running $@ ($(GRID_WIDTH)x$(GRID_HEIGHT)) =="
	./$< $(ARGS)

# "thyme-run" re-runs without rebuild.
$(addsuffix -run, $(SCRIPTS)): %-run:
	@if [ ! -x $(BUILD_DIR)/$*.bin ]; then \
		echo "!! $(BUILD_DIR)/$*.bin not built yet"; exit 1; \
	fi
	./$(BUILD_DIR)/$*.bin $(ARGS)

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%/ir/critter_ir.h $(ENGINE_SRCS) src/DumpSink.h src/FakeTimeSource.h | $(BUILD_DIR)
	@echo "== Compiling $* at $(GRID_WIDTH)x$(GRID_HEIGHT) =="
	$(CXX) -I $(BUILD_DIR)/$* -I $(BUILD_DIR)/$*/ir $(CXXFLAGS) \
		-DGRID_WIDTH=$(GRID_WIDTH) -DGRID_HEIGHT=$(GRID_HEIGHT) \
		-o $@ $(ENGINE_SRCS)

$(BUILD_DIR)/%/ir/critter_ir.h: $(AGENTS_DIR)/%.crit $(DEVICE_HEADER)
	mkdir -p $(dir $@)
	$(PYTHON) $(IR_ENCODER) $< -o $@ --width $(GRID_WIDTH) --height $(GRID_HEIGHT)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
