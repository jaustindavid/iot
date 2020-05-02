// Example usage for SimpleTimer library by Austin David.

#include "SimpleTimer.h"

SimpleTimer every50(50);
SimpleTimer hourly(3600);

void setup() {
}

void loop() {
  if (hour.isExpired()) {
    Serial.println("Ding!");
  }
  every50.wait();
}
