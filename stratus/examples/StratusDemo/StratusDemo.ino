// Example usage for stratus library by Austin David.
// austin@austindavid.com

#include "stratus.h"

// Initialize objects from the lib
Stratus stratus;

void setup() {
    stratus.setConfigURL("http://stratus-iot.s3.amazonaws.com/stratus-demo.txt");
    stratus.setSecret("secret passkey");
    stratus.update();
}

void loop() {
  stratus.update();
  delay(stratus.getInt("refresh interval", 60)*1000);
}
