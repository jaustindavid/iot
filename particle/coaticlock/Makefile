PARTICLE = /Users/austin/bin/particle
DEVICE ?= ricky_raccoon

# Automatically gather all config header files dynamically into targets
DEVICES := $(basename $(filter-out device_name.h, $(wildcard *.h)))

.PHONY: all flash clean $(DEVICES)

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

clean:
	rm -f src/creds.h
