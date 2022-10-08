#ifndef DISPLAY_H
#define DISPLAY_H

// This #include statement was automatically added by the Particle IDE.
#include <Adafruit_SSD1306.h>

// #define OLED_POWER  D3
// #define OLED_GROUND D2
#define OLED_SCL    D1
#define OLED_SCA    D0

Adafruit_SSD1306 display(-1);


/*
void display_power(bool level) {
    if (level == HIGH) {
        digitalWrite(OLED_POWER, HIGH);
        // TODO: refresh the display
    } else {
        digitalWrite(OLED_POWER, LOW);
    }
} // display_power(level)
*/

void setupDisplay() {
    // pinMode(OLED_POWER, OUTPUT);
    // pinMode(OLED_GROUND, OUTPUT);
    // igitalWrite(OLED_GROUND, LOW);
    // display_power(HIGH);
    delay(500);           // NOT a cloud delay
    display.begin();
} // initDisplay()


void banner(byte size, const char * buf) {
    display.clearDisplay();
    display.setTextColor(WHITE);
    // display.setFont(COMICS_8);
    display.setTextSize(size);
    display.setCursor(0,0);
    display.print(buf);
    display.display();
    display.setCursor(0, 32);
    display.setTextSize(1);
} // banner(size, buf)


// show a big clock at x,y
void showClock(byte x, byte y) {
    display.setTextColor(WHITE);
    // display.setFont(TIMESNR_8);
    display.setTextSize(3);
    display.setCursor(x, y);

    if (Time.hour() < 12) {
        display.setTextColor(BLACK);
        display.print("1");
        display.setTextColor(WHITE);
    }
    display.print(Time.hour());
    if (millis()/1000 % 2) {
        display.setTextColor(BLACK);
    }
    display.print(":");
    display.setTextColor(WHITE);
    if (Time.minute() < 10) {
      display.print("0");
    }
    display.print(Time.minute());

    return;
    
    display.setTextSize(1);
    display.setCursor(x+100, y+27);
    display.print(":");
    if (Time.second() < 10) {
      display.print("0");
    }    
    display.print(Time.second());
} // showClock(x, y)


String strangify(const int32_t elapsed) {
    String text;
    text.reserve(16);
    
    // int32_t elapsed = millis()/1000 - lastEvent;
    /*
    if (lastEvent == -1) {
        return String("never");
    } else */
    if (elapsed < 2*60) { // under 2 mins
        return String::format("%ds ago", elapsed);
    } else if (elapsed < 60*60) { // under 1h
        return String::format("%d mins ago", elapsed/60);
    } else if (elapsed < 24*60*60) { // under 1d 
        return String::format("%d hrs ago", elapsed/(60*60));
    } else {
        return String::format("%d days ago", elapsed/(24*60*60));
    }
    
    return "nope";
} // String stringify(elapsed)


String stringle(const time_t lastEvent) {
    String text;
    text.reserve(32);
    
    if (lastEvent == 0) {
        return "never";
    }
    
    text = Time.format(lastEvent, "%R");
    text += "\n   ";
    text += strangify(Time.now() - lastEvent);
    
    return text;
} // String stringle(time_t)


void showStats(const time_t lastEvent, const int intensity) {
    display.clearDisplay();
    
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(0,0);
    display.print("Last event: ");
    // display.setCursor(12,12);
    display.print(stringle(lastEvent));
    display.setCursor(0,24);
    display.print("Intensity: ");
    display.print(intensity);
    
    showClock(16,42);
    display.display();
} // showStats(last, intensity)

#endif