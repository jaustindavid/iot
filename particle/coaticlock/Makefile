PARTICLE = /Users/austin/bin/particle
DEVICE = ricky_raccoon

.PHONY: flash clean

flash:
	cp $(DEVICE).h src/creds.h
	$(PARTICLE) flash $(DEVICE) .

clean:
	rm -f src/creds.h
