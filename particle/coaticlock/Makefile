PARTICLE = /Users/austin/bin/particle
DEVICE ?= ricky_raccoon

# Automatically gather all config header files dynamically into targets
DEVICES := $(basename $(filter-out device_name.h, $(wildcard *.h)))

# Automatically gather all .coati scripts as codegen targets
SCRIPTS := $(basename $(notdir $(wildcard ../scripts/*.coati)))

.PHONY: all flash clean $(DEVICES) $(SCRIPTS)

default: elk_cheetah

all: $(DEVICES)

$(DEVICES):
	@echo "\n=============================================="
	@echo " Flashing firmware for device: $@"
	@echo "=============================================="
	cp $@.h src/creds.h
	$(PARTICLE) flash $@ .

flash:
	cp $(DEVICE).h src/creds.h
	$(PARTICLE) flash $(DEVICE) .

$(SCRIPTS):
	@echo "\n=============================================="
	@echo " Generating CoatiEngine from script: $@.coati"
	@echo "=============================================="
	cd .. && python3 -m coati codegen scripts/$@.coati --output-dir coaticlock/src/

clean:
	rm -f src/creds.h
