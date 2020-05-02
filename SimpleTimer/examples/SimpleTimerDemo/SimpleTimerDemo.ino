// Example usage for SimpleTimer library by Austin David.

#include "Particle.h"
#include <SimpleTimer.h>

SimpleTimer every50(50);
SimpleTimer second(1000);
SimpleTimer halfsecond(500);

void setup() {
    pinMode(D7, OUTPUT);
}

void loop() {
  if (second.isExpired()) {
      digitalWrite(D7, !digitalRead(D7));
  }
  every50.wait();
}
