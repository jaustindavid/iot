the "particle" directory contains a previous implementation of "critterchron".
The project is a high level language describing "critters", autonomous agents
in a spared space.  The implementation is an LED matrix, current hardware
supports 16x16 or 32x8 and runs on lightweight microcontrollers.  In
critterchron so far we have a python parser / compiler and a simulation engine
with pygame, to troubleshoot the critters.  The underlying engine renders an
image based on the current time -- a simple clock.  The agents follow their
own scripts to move, draw/erase, or otherwise interact to update the displayed
time while also performing this weird simulation.  Your task: review the
contents of particle. That implementation is pretty good, minus the benefits
of the carefully constructed "critter" language.  I'd like you to write a spec
for what has been called the HAL (hardware abstraction layer), or the hardware
engine running on a Particle Photon 2.  On P2 the "SPI" pin is connected to a
NeoPixel WS2812b string with the "zigzag" configuration, with either 16x16 or
32x8 layouts.  The "rotation" parameter is important for the 16x16, and
exposing the "MATRIX_PIN" has also been valuable.  I like the paradigm of a
per-device, general header with hardware and ID / key descriptions.  You'll
also note the presence of a "Stra2us" client -- Stra2us is a very lightweight
IoT cloud service with pub/sub capability and a key/value service.  We can
implement that client later, but when we get there the ID and HMAC key in the
per-device header will be crucial.  I also want to leave room to expand the
hardware to similar devices, specifically ESP32.  A clean implementation of an
enginer for our IR, running on the particle platform, should suffice for now.
You'll also see WobblyTime, we can implement that later; it's just a creative
way to drift time, decoupling "perfect precision" of NTP from the inherent
imprecision of our critter simulation.

Please ask any questions, review the existing codebase in particle, and keep
in mind that the newer sim and behaviors are more "correct", the older base is
more a reference for poorly-documented requirements.  I'm looking forward to
your proposal for a phased approach to implement the CritterChron in hardware!
