# hal/Makefile — platform-agnostic entry point.
#
# Usage (from this directory):
#   make DEVICE=rachel_raccoon thyme                 # encode + compile (no flash)
#   make DEVICE=rachel_raccoon thyme flash           # encode + compile + flash
#   make DEVICE=rachel_raccoon flash                 # re-flash last IR
#   make DEVICE=rachel_raccoon PLATFORM=host thyme   # run under host harness
#   make host                                        # cheap sanity gate via host harness
#   make DEVICE=rachel_raccoon clean
#   make list                                        # show devices + scripts
#
# By default, dispatches to hal/<platform>/Makefile where <platform> comes
# from DEVICE_PLATFORM in the chosen device header. PLATFORM=host overrides
# that to run the C++ engine under the host harness at the device's
# geometry — useful for smoke-testing before a flash round-trip.
#
# Adding an ESP32 board is: drop <name>.h in devices/ with
# DEVICE_PLATFORM "esp32", same `make` command works.

DEVICE   ?= rachel_raccoon
PYTHON   ?= python3

# `make host` — cheapest sanity gate. Builds + runs the cross-platform
# engine on the host harness at the default device's geometry. Useful
# as a quick "did I break the engine" check after HAL edits without
# needing flash hardware. Override DEVICE or HOST_SCRIPT to point at
# a different combo; for ad-hoc one-offs, the explicit
# `make PLATFORM=host DEVICE=<dev> <script>` form works through the
# same catch-all path.
HOST_SCRIPT ?= thyme

DEVICES_DIR  := devices
AGENTS_DIR   := ../agents

# Filter out the template
DEVICES := $(basename $(filter-out device_name.h, $(notdir $(wildcard $(DEVICES_DIR)/*.h))))
SCRIPTS := $(basename $(notdir $(wildcard $(AGENTS_DIR)/*.crit)))

DEVICE_HEADER := $(DEVICES_DIR)/$(DEVICE).h

# Extract DEVICE_PLATFORM "foo" → foo. Lazy-eval so `make list` doesn't
# fail when the user hasn't picked a device yet. User-supplied PLATFORM
# (e.g. `PLATFORM=host`) wins.
PLATFORM ?= $(shell test -f $(DEVICE_HEADER) && grep DEVICE_PLATFORM $(DEVICE_HEADER) | cut -d'"' -f2)

.PHONY: default list check-device check-platform host

default: list

list:
	@echo "Usage: make DEVICE=<device> <script>"
	@echo ""
	@echo "  devices: $(DEVICES)"
	@echo "  scripts: $(SCRIPTS)"

# Explicit rule so the catch-all below doesn't try to forward `host` to
# the platform Makefile (which has no such target). Recursive invocation
# overrides PLATFORM so the catch-all routes to hal/host/Makefile via
# the standard dispatch path.
host:
	@$(MAKE) PLATFORM=host DEVICE=$(DEVICE) $(HOST_SCRIPT)

check-device:
	@if [ ! -f $(DEVICE_HEADER) ]; then \
		echo "!! Device header not found: $(DEVICE_HEADER)"; \
		echo "   Available: $(DEVICES)"; \
		exit 1; \
	fi

check-platform: check-device
	@if [ -z "$(PLATFORM)" ]; then \
		echo "!! $(DEVICE_HEADER) is missing #define DEVICE_PLATFORM"; \
		exit 1; \
	fi
	@if [ ! -d $(PLATFORM) ]; then \
		echo "!! $(DEVICE_HEADER) targets platform '$(PLATFORM)' but hal/$(PLATFORM)/ does not exist"; \
		exit 1; \
	fi

# Don't let the catch-all trigger an implicit remake of Makefile itself.
Makefile: ;

# Catch-all: forward any target (script name, flash, compile, clean) to
# the platform-specific Makefile with DEVICE passed through. Skip our own
# meta-targets so they don't recurse.
%: check-platform
	@$(MAKE) -C $(PLATFORM) DEVICE=$(DEVICE) $@
