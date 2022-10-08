#ifndef BUDDY_H
#define BUDDY_H

/*
 * Buddy does 2 things:
 *   It watches buttons and publishes stuff
 *   It subscribes to "glow" events (and reacts if needed)
 *
 * Usage: ClickButton buttonObject(pin [LOW/HIGH, [CLICKBTN_PULLUP]]);
 *
 * where LOW/HIGH denotes active LOW or HIGH button (default is LOW)
 * CLICKBTN_PULLUP is only possible with active low buttons.
 *
 * Returned click counts:
 *   A positive number denotes the number of (short) clicks after a released button
 *   A negative number denotes the number of "long" clicks
 *
 */


ClickButton lButton(LBUTTON_PIN, LOW, CLICKBTN_PULLUP);
ClickButton rButton(RBUTTON_PIN, LOW, CLICKBTN_PULLUP);


class Buddy {
    private:
        
    public:
        Buddy() {
            // pinMode(D2, INPUT_PULLUP);
            pinMode(BUTTON_GND, OUTPUT);
            digitalWrite(BUTTON_GND, LOW);
        } // Constructor


        static void handleGlowEvent(const String event, const String data) {
            Serial.printf("handling glow event: %s\n", data.c_str());
            uint32_t color = stratus.getHex(data + " color", 0x00aaaa);
            blink (color);
            if (data != stratus.getGUID()) {
                glow(color);
            } /* else {
                Serial.println("that was me; signalling different");
                blink(stratus.getHex(stratus.getGUID() + " color", 0x00aaaa));
            } */
        } // handleGlowEvent(data)
        
        
        void setup() {
            Particle.subscribe("glow", Buddy::handleGlowEvent, MY_DEVICES);
            Particle.subscribe("glow-L", Buddy::handleGlowEvent, MY_DEVICES);
        } // setup()
        
        
        void update() {
            lButton.Update();
            rButton.Update();
            if (rButton.clicks != 0) {
                Particle.publish("glow", stratus.getGUID(), PRIVATE);
                Serial.printf("L: %d; R: %d\n", lButton.clicks, rButton.clicks);
                stratus.update();
            }
            if (lButton.clicks != 0) {
                Particle.publish("glow-L", stratus.getGUID(), PRIVATE);
                Serial.printf("L: %d; R: %d\n", lButton.clicks, rButton.clicks);
                stratus.update();
            }
        } // update()
}; // Buddy

#endif