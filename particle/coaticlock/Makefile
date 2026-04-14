PARTICLE = /Users/austin/bin/particle
DEVICE ?= ricky_raccoon

# Gather targets
DEVICES := $(basename $(filter-out device_name.h, $(wildcard *.h)))
SCRIPTS := $(basename $(notdir $(wildcard ../scripts/*.coati)))

# Dynamic Dimension Extraction
GET_HEIGHT = $(shell grep "GRID_HEIGHT" $(DEVICE).h | awk '{print $$3}')
GET_WIDTH  = $(shell grep "GRID_WIDTH" $(DEVICE).h | awk '{print $$3}')

.PHONY: all flash compile clean $(DEVICES) $(SCRIPTS)

default: elk_cheetah

all: $(DEVICES)

# Flashing a specific device (e.g., 'make ricky_raccoon')
$(DEVICES): 
	@$(MAKE) flash DEVICE=$@

# simulate a script
# broken -- need to work with "make tortoise sim"
# sim:
# 	@echo "Simulating $@ on $(DEVICE)"
# 	cd .. && python3 -m coati run scripts/$@.coati \
# 		--grid_width $(GET_WIDTH) \
# 		--grid_height $(GET_HEIGHT)

flash:

# Flash target: Compiles first, then flashes
flash: 
	@echo "Flashing firmware to device: $(DEVICE)"
	cp $(DEVICE).h src/creds.h
	$(PARTICLE) flash $(DEVICE) .

# Code Generation target
$(SCRIPTS):
	@echo "Generating CoatiEngine for $@.coati ($(GET_WIDTH)x$(GET_HEIGHT))"
	cd .. && python3 -m coati codegen scripts/$@.coati \
		--output-dir coaticlock/src/ \
		--grid_width $(GET_WIDTH) \
		--grid_height $(GET_HEIGHT)

clean:
	rm -f src/creds.h
