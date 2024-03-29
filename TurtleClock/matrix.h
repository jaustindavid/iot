#ifndef MATRIX_H
#define MATRIX_H

#include <neopixel.h>
#include <SimpleTimer.h>

#define RED         (Adafruit_NeoPixel::Color(255, 0, 0))
#define LIGHTRED    (Adafruit_NeoPixel::Color(128, 0, 0))
#define GREEN       (Adafruit_NeoPixel::Color(0, 255, 0))
#define LIGHTGREEN  (Adafruit_NeoPixel::Color(0, 128, 0))
#define BLUE        (Adafruit_NeoPixel::Color(0, 0, 255))
#define YELLOW      (Adafruit_NeoPixel::Color(255, 255, 0))
#define BLACK       (Adafruit_NeoPixel::Color(0, 0, 0))
#define DARKGREY    (Adafruit_NeoPixel::Color(8, 8, 8))
#define LIGHTGREY   (Adafruit_NeoPixel::Color(32, 32, 32))
#define DARKWHITE   (Adafruit_NeoPixel::Color(64, 64, 64))
#define WHITE       (Adafruit_NeoPixel::Color(255, 255, 255))

// weather stuff
#define COLOR_HOT   RED
#define COLOR_WARM  (Adafruit_NeoPixel::Color(128, 64, 0))
#define COLOR_COOL  (Adafruit_NeoPixel::Color(128, 0, 128)) 
#define COLOR_CHILLY (Adafruit_NeoPixel::Color(0, 0, 128))
#define COLOR_COLD  (Adafruit_NeoPixel::Color(128, 0, 255))
#define COLOR_FREEZING (Adafruit_NeoPixel::Color(128, 128, 255))
#define COLOR_FROZE (Adafruit_NeoPixel::Color(192, 192, 192))

#define COLOR_SUNNY YELLOW
#define COLOR_CLOUDY DARKWHITE
#define COLOR_RAINY BLUE

// a magic color, never actually rendered
#define TRANSPARENT (Adafruit_NeoPixel::Color(1, 1, 1))

// a magic color: it fades
#define FOOTPRINT   (0x004000)

#define MATRIX_X 32
#define MATRIX_Y 8
#define NSTEPS 10        // steps in the transition between pallets
#define XFADE  20         // MSPF/10
typedef uint32_t color;

typedef struct Pixel {
    byte x, y;
    color c;
} Pixel;

class Matrix {
    private:
        // TODO: heap
        color fg[MATRIX_X][MATRIX_Y];
        color bg[MATRIX_X][MATRIX_Y];
        // byte fadeable[MATRIX_X][MATRIX_Y];
        Pixel fgTurt, bgTurt;
        color hidden;
        SimpleTimer *mspf, *xfader;             
        Adafruit_NeoPixel *display;
        int txlate(byte, byte);
        void xfade(byte, byte, byte);
        
    public:
        Matrix(Adafruit_NeoPixel*);
        color getPixel(byte, byte);
        void setPixel(byte, byte, color);
        void setTurtle(byte, byte, color);
        void setTurtle(byte, byte, color, byte);
        void fadeSome(byte, byte, byte, byte, byte);
        void show();
};

#endif