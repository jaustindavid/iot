# hal/argon/Makefile — Argon build orchestration.
#
# Near-clone of hal/photon2/Makefile; only PLATFORM differs. If the pattern
# ever needs a third identical copy (OG Photon, ESP32 via `particle`…),
# factor out a shared hal/particle/common.mk.
#
# Usage:
#   make DEVICE=elk_cheetah thyme        # encode IR for thyme + compile (no flash)
#   make DEVICE=elk_cheetah flash        # flash the last-built binary
#   make DEVICE=elk_cheetah thyme flash  # encode + compile + flash
#   make clean                           # wipe generated src/
#
# Flashing is opt-in — a bare `make <script>` only compiles.

PARTICLE ?= /Users/austin/bin/particle
PYTHON   ?= python3
PLATFORM := argon
DEVICE   ?= elk_cheetah

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

DEVICES := $(basename $(filter-out device_name.h, $(notdir $(wildcard $(DEVICES_DIR)/*.h))))
SCRIPTS := $(basename $(notdir $(wildcard $(AGENTS_DIR)/*.crit)))

ENGINE_COPIES := \
	$(SRC_DIR)/CritterEngine.h \
	$(SRC_DIR)/CritterEngine.cpp \
	$(SRC_DIR)/interface/Config.h \
	$(SRC_DIR)/interface/LedSink.h \
	$(SRC_DIR)/interface/TimeSource.h \
	$(SRC_DIR)/ir/IrRuntime.h \
	$(SRC_DIR)/ir/IrRuntime.cpp

PARTICLE_FILES  := $(notdir $(wildcard $(PARTICLE_SRC)/*))
PARTICLE_COPIES := $(addprefix $(SRC_DIR)/,$(PARTICLE_FILES))

.PHONY: all flash clean list $(DEVICES) $(SCRIPTS) compile assemble

default: list

list:
	@echo "Usage: make DEVICE=<device> <script>"
	@echo "  platform: $(PLATFORM)"
	@echo "  devices:  $(DEVICES)"
	@echo "  scripts:  $(SCRIPTS)"

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

# Same artifact for compile + flash so `make <script> flash` only pays for
# one cloud compile. `particle flash <dev> <bin>` uploads a pre-built binary.
compile: $(FIRMWARE_BIN)

# Depend on real files, not the phony `assemble`, so make skips the cloud
# compile when nothing actually changed. $(SCRIPTS) preserves critter_ir.h's
# mtime via cmp-and-move so re-encoding an unchanged script is a no-op.
$(FIRMWARE_BIN): $(ENGINE_COPIES) $(PARTICLE_COPIES) $(SRC_DIR)/creds.h $(SRC_DIR)/ir/critter_ir.h
	$(PARTICLE) compile $(PLATFORM) . --saveTo $(FIRMWARE_BIN)

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
$(SRC_DIR)/ir/IrRuntime.h: $(HAL_ROOT)/ir/IrRuntime.h | $(SRC_DIR)/ir
	cp $< $@
$(SRC_DIR)/ir/IrRuntime.cpp: $(HAL_ROOT)/ir/IrRuntime.cpp | $(SRC_DIR)/ir
	cp $< $@

$(SRC_DIR)/%: $(PARTICLE_SRC)/% | $(SRC_DIR)
	cp $< $@

$(SRC_DIR):
	mkdir -p $@
$(SRC_DIR)/interface:
	mkdir -p $@
$(SRC_DIR)/ir:
	mkdir -p $@

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
