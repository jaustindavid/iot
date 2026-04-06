a vibecoded ESP32 / ESPHome-based "clock"

* includes several "guides" for future "engineers"
* leverages the ESPHome stack for full OTA deployment, debugging, etc

# Hardware

* 8x32 WS2812b display.  There are many options on Amazong or Alibabra, go 
nuts
* ESP32-C3 or better.
* Data pin -> D10 or something (ask a robot), GND + VCC pin to GND and VUSB.
* The secret innovation here is powering the display directly from USB (find
any USB-C or USB-Micro adapter) do the center power line.  This injects the
full 5v power to the whole display, does NOT require dragging an amp or more
across the ESP32's fusing, and the ESP32 can pick up its meager power 
requirement off to the side.  And you're tying ground, always a good idea.
* A custom laser-cut wood case.  You'll have to ask me about this.
