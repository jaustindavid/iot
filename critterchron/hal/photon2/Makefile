# hal/photon2/Makefile — Photon 2 build orchestration.
#
# Usage:
#   make DEVICE=rachel_raccoon thyme        # encode IR for thyme + compile (no flash)
#   make DEVICE=rachel_raccoon flash        # flash the last-built binary
#   make DEVICE=rachel_raccoon thyme flash  # encode + compile + flash
#   make clean                              # wipe generated src/
#
# Flashing is opt-in — a bare `make <script>` only compiles. Add `flash` on
# the command line (or use the `<device>` shorthand) when you actually want
# to push to hardware.
#
# Assembly model:
#   1. Copy the cross-platform engine from ../ (CritterEngine, interface/)
#   2. Copy the Particle shim sources from ../particle/src/
#   3. Copy chosen device header into src/creds.h
#   4. Run ir_encoder.py on agents/<script>.crit → src/ir/critter_ir.h
#   5. `particle compile photon2 .` (or flash)
#
# hal/argon/Makefile is a near-clone; the only delta is the platform flag
# on step 5.

PARTICLE ?= /Users/austin/bin/particle
PYTHON   ?= python3
PLATFORM := photon2
DEVICE   ?= rachel_raccoon

REPO_ROOT    := $(abspath ../..)
HAL_ROOT     := $(abspath ..)
PARTICLE_SRC := $(HAL_ROOT)/particle/src
DEVICES_DIR  := $(HAL_ROOT)/devices
AGENTS_DIR   := $(REPO_ROOT)/agents
IR_ENCODER   := $(REPO_ROOT)/hal/ir/ir_encoder.py

SRC_DIR      := src

DEVICE_HEADER := $(DEVICES_DIR)/$(DEVICE).h
GRID_WIDTH  = $(shell grep GRID_WIDTH  $(DEVICE_HEADER) | awk '{print $$3}')
GRID_HEIGHT = $(shell grep GRID_HEIGHT $(DEVICE_HEADER) | awk '{print $$3}')

# Enumerate devices and scripts purely from the filesystem (no manual lists).
DEVICES := $(basename $(filter-out device_name.h, $(notdir $(wildcard $(DEVICES_DIR)/*.h))))
SCRIPTS := $(basename $(notdir $(wildcard $(AGENTS_DIR)/*.crit)))

# Cross-platform HAL copies (engine + abstract interfaces).
ENGINE_COPIES := \
	$(SRC_DIR)/CritterEngine.h \
	$(SRC_DIR)/CritterEngine.cpp \
	$(SRC_DIR)/interface/Config.h \
	$(SRC_DIR)/interface/LedSink.h \
	$(SRC_DIR)/interface/TimeSource.h

# Particle-common shim copies. Wildcarded so new additions to
# hal/particle/src/ pick up automatically.
PARTICLE_FILES  := $(notdir $(wildcard $(PARTICLE_SRC)/*))
PARTICLE_COPIES := $(addprefix $(SRC_DIR)/,$(PARTICLE_FILES))

.PHONY: all flash clean list $(DEVICES) $(SCRIPTS) compile assemble

default: list

list:
	@echo "Usage: make DEVICE=<device> <script>"
	@echo "  platform: $(PLATFORM)"
	@echo "  devices:  $(DEVICES)"
	@echo "  scripts:  $(SCRIPTS)"

# `make rachel_raccoon` shorthand for `make flash DEVICE=rachel_raccoon`.
$(DEVICES):
	@$(MAKE) flash DEVICE=$@

$(SCRIPTS): assemble $(SRC_DIR)/creds.h | $(SRC_DIR)/ir
	@echo "== Encoding IR: $@.crit ($(GRID_WIDTH)x$(GRID_HEIGHT)) =="
	@$(PYTHON) $(IR_ENCODER) $(AGENTS_DIR)/$@.crit -o $(SRC_DIR)/ir/critter_ir.h.tmp \
		--width $(GRID_WIDTH) --height $(GRID_HEIGHT)
	@if cmp -s $(SRC_DIR)/ir/critter_ir.h.tmp $(SRC_DIR)/ir/critter_ir.h 2>/dev/null; then \
		rm $(SRC_DIR)/ir/critter_ir.h.tmp; \
	else \
		mv $(SRC_DIR)/ir/critter_ir.h.tmp $(SRC_DIR)/ir/critter_ir.h; \
	fi
	@$(MAKE) compile DEVICE=$(DEVICE)

FIRMWARE_BIN := firmware.bin

# `compile` and `flash` both go through $(FIRMWARE_BIN) so `make <script>
# flash` doesn't pay for two cloud compiles. `particle flash <dev> <bin>`
# just uploads the pre-built binary; `particle flash <dev> .` would package
# the source tree and compile on the cloud again.
compile: $(FIRMWARE_BIN)

# Depend on real files, not the phony `assemble`, so make can skip the cloud
# compile when nothing actually changed. `$(SCRIPTS)` regenerates
# critter_ir.h via a cmp-and-move so its mtime only bumps on real content
# changes.
$(FIRMWARE_BIN): $(ENGINE_COPIES) $(PARTICLE_COPIES) $(SRC_DIR)/creds.h $(SRC_DIR)/ir/critter_ir.h
	$(PARTICLE) compile $(PLATFORM) . --saveTo $(FIRMWARE_BIN)

# critter_ir.h is produced as a side effect of the $(SCRIPTS) target; this
# empty rule tells make "if it exists, don't try to build it on its own."
$(SRC_DIR)/ir/critter_ir.h: ;

flash: $(FIRMWARE_BIN)
	@echo "== Flashing $(DEVICE) =="
	$(PARTICLE) flash $(DEVICE) $(FIRMWARE_BIN)

# --- Source assembly ---
assemble: $(ENGINE_COPIES) $(PARTICLE_COPIES)

$(SRC_DIR)/CritterEngine.h: $(HAL_ROOT)/CritterEngine.h | $(SRC_DIR)
	cp $< $@
$(SRC_DIR)/CritterEngine.cpp: $(HAL_ROOT)/CritterEngine.cpp | $(SRC_DIR)
	cp $< $@
$(SRC_DIR)/interface/Config.h: $(HAL_ROOT)/interface/Config.h | $(SRC_DIR)/interface
	cp $< $@
$(SRC_DIR)/interface/LedSink.h: $(HAL_ROOT)/interface/LedSink.h | $(SRC_DIR)/interface
	cp $< $@
$(SRC_DIR)/interface/TimeSource.h: $(HAL_ROOT)/interface/TimeSource.h | $(SRC_DIR)/interface
	cp $< $@

# Generic copy rule for every file in hal/particle/src/. Order-only dep on
# $(SRC_DIR) so the copy rule fires before anything tries to read it.
$(SRC_DIR)/%: $(PARTICLE_SRC)/% | $(SRC_DIR)
	cp $< $@

$(SRC_DIR):
	mkdir -p $@
$(SRC_DIR)/interface:
	mkdir -p $@
$(SRC_DIR)/ir:
	mkdir -p $@

# --- Device-specific creds.h ---
# Always consulted via FORCE so switching DEVICE between builds doesn't leave
# a stale creds.h. cmp short-circuits the copy when contents already match.
.PHONY: FORCE
FORCE:

$(SRC_DIR)/creds.h: FORCE | $(SRC_DIR)
	@if ! cmp -s $(DEVICES_DIR)/$(DEVICE).h $@ 2>/dev/null; then \
		echo "== Updating creds.h → $(DEVICE) =="; \
		cp $(DEVICES_DIR)/$(DEVICE).h $@; \
	fi

clean:
	rm -rf $(SRC_DIR)/CritterEngine.h $(SRC_DIR)/CritterEngine.cpp \
	       $(SRC_DIR)/interface $(SRC_DIR)/ir $(SRC_DIR)/creds.h \
	       $(PARTICLE_COPIES) $(FIRMWARE_BIN)
