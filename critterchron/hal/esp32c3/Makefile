# hal/esp32c3/Makefile — ESP32-C3 build orchestration (arduino-cli).
#
# A parallel hal/esp32s3/ Makefile will be a near-clone when that
# hardware lands — only FQBN changes. The shim code in hal/esp32/src/
# is chip-agnostic within the ESP32 family (Arduino-ESP32 core wraps
# both C3-RISC-V and S3-Xtensa behind the same WiFi.h / FastLED API).
#
# Usage:
#   make DEVICE=timmy_tanuki swarm         # encode IR + compile (no flash)
#   make DEVICE=timmy_tanuki flash PORT=/dev/cu.usbmodem101
#   make DEVICE=timmy_tanuki swarm flash PORT=/dev/cu.usbmodem101
#   make DEVICE=timmy_tanuki flash-ota     # push OTA to $(DEVICE).local
#   make DEVICE=timmy_tanuki swarm flash-ota
#   make DEVICE=timmy_tanuki flash-ota OTA_HOST=192.168.1.42  # IP override
#   make clean
#
# Flashing is opt-in — bare `make <script>` only compiles. Add `flash` plus
# a PORT= value on the command line when you actually want to push to
# hardware. For OTA push over LAN use `flash-ota` — hostname defaults to
# `$(DEVICE).local` (the ArduinoOTA listener's mDNS name, set in
# critterchron_esp32.ino's setup()).
#
# Assembly model (parallels hal/photon2/Makefile):
#   1. Copy the cross-platform engine from ../ (CritterEngine, interface/, ir/)
#   2. Copy the ESP32 shim sources from ../esp32/src/
#   3. Copy chosen device header into build/critterchron/creds.h
#   4. Rename critterchron_esp32.ino → critterchron.ino in the build dir,
#      because arduino-cli requires the sketch folder name and the .ino
#      stem to match.
#   5. Run ir_encoder.py on agents/<script>.crit → build/critterchron/ir/critter_ir.h
#   6. `arduino-cli compile --fqbn esp32:esp32:esp32c3 build/critterchron`
#
# Prerequisites (install once per host):
#   brew install arduino-cli
#   arduino-cli config init
#   arduino-cli core update-index --additional-urls \
#     https://espressif.github.io/arduino-esp32/package_esp32_index.json
#   arduino-cli core install esp32:esp32 --additional-urls \
#     https://espressif.github.io/arduino-esp32/package_esp32_index.json
#   arduino-cli lib install FastLED

ARDUINO_CLI ?= arduino-cli
PYTHON      ?= python3
PLATFORM    := esp32c3
FQBN        ?= esp32:esp32:esp32c3
DEVICE      ?= timmy_tanuki
PORT        ?=

REPO_ROOT   := $(abspath ../..)
HAL_ROOT    := $(abspath ..)
ESP32_SRC   := $(HAL_ROOT)/esp32/src
DEVICES_DIR := $(HAL_ROOT)/devices
AGENTS_DIR  := $(REPO_ROOT)/agents
IR_ENCODER  := $(REPO_ROOT)/hal/ir/ir_encoder.py

# arduino-cli insists: <sketch_dir>/<sketch_dir_basename>.ino. Pick a
# short name so the dir-basename dance doesn't sprawl. The canonical
# .ino in hal/esp32/src/ is critterchron_esp32.ino — it gets copied
# under the shorter name here.
#
# arduino-cli's sketch-layout rules (important): the sketch folder may
# contain the .ino plus any top-level .cpp/.h/.c files. It recursively
# compiles everything under a `src/` subdirectory, but IGNORES any other
# subdirectory. So engine sources (which live under interface/, ir/)
# must sit under src/ or the linker silently drops them — observed as
# `undefined reference to critter_ir::RUNTIME_TICK_MS` on the first
# build of this scaffold. SRC_DIR is therefore `<sketch>/src`, and only
# the .ino + creds.h live at the sketch root.
SKETCH_NAME := critterchron
SKETCH_DIR  := build/$(SKETCH_NAME)
SRC_DIR     := $(SKETCH_DIR)/src

DEVICE_HEADER := $(DEVICES_DIR)/$(DEVICE).h
GRID_WIDTH  = $(shell grep GRID_WIDTH  $(DEVICE_HEADER) | awk '{print $$3}')
GRID_HEIGHT = $(shell grep GRID_HEIGHT $(DEVICE_HEADER) | awk '{print $$3}')

DEVICES := $(basename $(filter-out device_name.h, $(notdir $(wildcard $(DEVICES_DIR)/*.h))))
SCRIPTS := $(basename $(notdir $(wildcard $(AGENTS_DIR)/*.crit)))

# Cross-platform HAL copies (engine + abstract interfaces + IR runtime).
# Identical set to hal/photon2/Makefile — the engine and interfaces are
# platform-neutral by contract.
ENGINE_COPIES := \
	$(SRC_DIR)/CritterEngine.h \
	$(SRC_DIR)/CritterEngine.cpp \
	$(SRC_DIR)/interface/Config.h \
	$(SRC_DIR)/interface/LedSink.h \
	$(SRC_DIR)/interface/TimeSource.h \
	$(SRC_DIR)/ir/IrRuntime.h \
	$(SRC_DIR)/ir/IrRuntime.cpp

# ESP32-common shim copies. Wildcarded so new additions to hal/esp32/src/
# pick up automatically. The one special-case rename is the .ino: the
# canonical name includes the platform (critterchron_esp32.ino) for
# clarity in source control, but arduino-cli forces it to match the
# sketch folder (critterchron.ino) in the build dir.
ESP32_FILES    := $(filter-out critterchron_esp32.ino, \
                      $(notdir $(wildcard $(ESP32_SRC)/*)))
ESP32_COPIES   := $(addprefix $(SRC_DIR)/,$(ESP32_FILES)) \
                  $(SKETCH_DIR)/$(SKETCH_NAME).ino

.PHONY: all flash flash-ota clean list $(DEVICES) $(SCRIPTS) compile assemble

default: list

list:
	@echo "Usage: make DEVICE=<device> <script>"
	@echo "  platform:  $(PLATFORM) ($(FQBN))"
	@echo "  devices:   $(DEVICES)"
	@echo "  scripts:   $(SCRIPTS)"
	@echo "  flash:     make DEVICE=<device> flash PORT=/dev/cu.usbmodemXXX"
	@echo "  flash-ota: make DEVICE=<device> flash-ota  (pushes to <device>.local)"

# `make echo_ermine` shorthand for `make flash DEVICE=echo_ermine`.
$(DEVICES):
	@$(MAKE) flash DEVICE=$@

$(SCRIPTS): assemble $(SKETCH_DIR)/creds.h | $(SRC_DIR)/ir
	@echo "== Encoding IR: $@.crit ($(GRID_WIDTH)x$(GRID_HEIGHT)) =="
	@$(PYTHON) $(IR_ENCODER) $(AGENTS_DIR)/$@.crit -o $(SRC_DIR)/ir/critter_ir.h.tmp \
		--width $(GRID_WIDTH) --height $(GRID_HEIGHT)
	@if cmp -s $(SRC_DIR)/ir/critter_ir.h.tmp $(SRC_DIR)/ir/critter_ir.h 2>/dev/null; then \
		rm $(SRC_DIR)/ir/critter_ir.h.tmp; \
	else \
		mv $(SRC_DIR)/ir/critter_ir.h.tmp $(SRC_DIR)/ir/critter_ir.h; \
	fi
	@$(MAKE) compile DEVICE=$(DEVICE)

FIRMWARE_BIN := build/firmware.bin

compile: $(FIRMWARE_BIN)

# Depend on real files, not the phony `assemble`, so make can skip
# arduino-cli when nothing actually changed. `$(SCRIPTS)` regenerates
# critter_ir.h via cmp-and-move so its mtime only bumps on real content
# changes — identical to the Photon 2 flow.
$(FIRMWARE_BIN): $(ENGINE_COPIES) $(ESP32_COPIES) $(SKETCH_DIR)/creds.h $(SRC_DIR)/ir/critter_ir.h
	$(ARDUINO_CLI) compile --fqbn $(FQBN) \
		--output-dir $(dir $(FIRMWARE_BIN)) \
		$(SKETCH_DIR)
	@# arduino-cli drops the binary as <sketch>.ino.bin — normalize the
	@# name so `flash` can point at a stable path, matching Photon 2's
	@# firmware.bin convention.
	@cp -f $(dir $(FIRMWARE_BIN))$(SKETCH_NAME).ino.bin $(FIRMWARE_BIN)

$(SRC_DIR)/ir/critter_ir.h: ;

flash: $(FIRMWARE_BIN)
ifeq ($(PORT),)
	@echo "error: set PORT=/dev/cu.usbmodemXXX to flash (try: ls /dev/cu.*)" >&2
	@exit 1
endif
	@echo "== Flashing $(DEVICE) via $(PORT) =="
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $(PORT) \
		--input-dir $(dir $(FIRMWARE_BIN)) \
		$(SKETCH_DIR)

# OTA push over LAN. Default hostname = $(DEVICE).local — matches the
# ArduinoOTA listener's mDNS name in critterchron_esp32.ino's setup().
# Override with `OTA_HOST=<ip>` if mDNS isn't resolving (some networks
# block multicast) or if you want to target a specific device by IP.
#
# Calls espota.py directly rather than going through `arduino-cli upload
# --protocol network`. arduino-cli's network-port discovery requires the
# port to be registered via its mDNS discovery protocol BEFORE upload,
# which is a separate step that often lags reality ("port not found:
# <host> network" even when the device is pingable on the same name).
# espota.py has no such coupling — it just does the UDP handshake and
# TCP push. It ships inside the arduino-esp32 core at
# `tools/espota.py` and is what `arduino-cli upload --protocol network`
# ultimately shells out to anyway; we just skip the middleman.
#
# Discover dynamically so a core upgrade (3.3.8 → 3.4.x) doesn't require
# a Makefile edit. Fails loudly if the core isn't installed — better
# than a cryptic "file not found" later.
ESP32_CORE_ROOT := $(HOME)/Library/Arduino15/packages/esp32/hardware/esp32
ESPOTA_PY       := $(firstword $(wildcard $(ESP32_CORE_ROOT)/*/tools/espota.py))

OTA_HOST ?= $(DEVICE).local
OTA_PORT ?= 3232

# espota.py auto-detects the host's LAN IP and sends it to the device in
# the UDP invitation — the device then opens a TCP callback to that
# address for the firmware stream. Auto-detection fails (shows
# `host_ip='0.0.0.0'` in --debug output) on multi-interface macs and
# multi-subnet LANs, and the device silently can't call back. Symptom:
# device LED blinks when invitation arrives, then "No response from
# device" ten seconds later.
#
# Auto-pick the route'd interface's IP — whichever NIC macOS would use
# to reach the device. Override with `OTA_HOST_IP=<ip>` if needed.
OTA_HOST_IP ?= $(shell route -n get $(OTA_HOST) 2>/dev/null | awk '/interface:/ {print $$2}' | xargs -I{} ipconfig getifaddr {} 2>/dev/null)

flash-ota: $(FIRMWARE_BIN)
ifeq ($(ESPOTA_PY),)
	@echo "error: espota.py not found under $(ESP32_CORE_ROOT); is the esp32 core installed?" >&2
	@exit 1
endif
	@echo "== OTA flashing $(DEVICE) via $(OTA_HOST):$(OTA_PORT) (host_ip=$(OTA_HOST_IP)) =="
	$(PYTHON) $(ESPOTA_PY) \
		--ip=$(OTA_HOST) --port=$(OTA_PORT) \
		$(if $(OTA_HOST_IP),--host_ip=$(OTA_HOST_IP),) \
		--file=$(FIRMWARE_BIN) \
		--progress --debug

# --- Source assembly ---
assemble: $(ENGINE_COPIES) $(ESP32_COPIES)

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

# Special-case rename: the canonical .ino keeps its platform suffix in
# source control (critterchron_esp32.ino), arduino-cli forces the
# folder-basename match in the build dir (critterchron.ino). Also the
# .ino lives at the sketch root, NOT under src/ — the sketch-layout
# rule is: .ino at top, everything else (recursively) under src/.
$(SKETCH_DIR)/$(SKETCH_NAME).ino: $(ESP32_SRC)/critterchron_esp32.ino | $(SKETCH_DIR)
	cp $< $@

# Generic copy rule for every other file in hal/esp32/src/.
$(SRC_DIR)/%: $(ESP32_SRC)/% | $(SRC_DIR)
	cp $< $@

$(SKETCH_DIR):
	mkdir -p $@
$(SRC_DIR):
	mkdir -p $@
$(SRC_DIR)/interface:
	mkdir -p $@
$(SRC_DIR)/ir:
	mkdir -p $@

# --- Device-specific creds.h ---
# FORCE so switching DEVICE between builds doesn't leave a stale
# creds.h. cmp short-circuits when contents match.
.PHONY: FORCE
FORCE:

# creds.h lives at the sketch root, not under src/, so the .ino can
# reference it with a bare `#include "creds.h"` (matching the Particle
# shim's spelling — the engine headers under src/interface/ and src/ir/
# never touch it, so its location has no reach beyond the .ino).
$(SKETCH_DIR)/creds.h: FORCE | $(SKETCH_DIR)
	@if ! cmp -s $(DEVICES_DIR)/$(DEVICE).h $@ 2>/dev/null; then \
		echo "== Updating creds.h → $(DEVICE) =="; \
		cp $(DEVICES_DIR)/$(DEVICE).h $@; \
	fi

clean:
	rm -rf build
