#include "matrix.h"

Matrix::Matrix(Adafruit_NeoPixel *newDisplay) {
    display = newDisplay;
    xfader = new SimpleTimer(XFADE);
    memset(fg, 0, sizeof(fg));
    memset(bg, 0, sizeof(bg));
    fgTurt.x = fgTurt.y = 0;
    fgTurt.c = TRANSPARENT;
    bgTurt = fgTurt;
} // constructor


// translate an x/y coordinate to the neopixel address
int Matrix::txlate(byte x, byte y) {
    int pixel = 0;
    
    pixel = x * MATRIX_Y;
    if (x%2 == 0) {
        pixel += y % MATRIX_Y;
    } else {
        pixel += (MATRIX_Y - 1) - (y % MATRIX_Y);
    }
    
    return pixel;
} // int txlate(x, y)


/*    3 -> 25
 *    0 : 3  == (3 * 10-0 + 25*0)/10
 *    10: 25 == 3 * 10-10 + 25*10)/10
 */
// cross-fade one pixel from BG -> FG over N steps
void Matrix::xfade(byte x, byte y, byte step) {
    // https://gist.github.com/Here-Be-Dragons/3909e903c8eda6f15fcb06308a6fefc1
    color startRGB = bg[x][y];
    byte startR = (byte)((startRGB >> 16) & 0xff); 
    byte startG = (byte)((startRGB >> 8) & 0xff);
    byte startB = (byte)(startRGB & 0xff);
    
    color endRGB = fg[x][y];
    byte endR = (byte)((endRGB >> 16) & 0xff); 
    byte endG = (byte)((endRGB >> 8) & 0xff);
    byte endB = (byte)(endRGB & 0xff);

    byte r = (byte)((1.0*startR*(NSTEPS - step) + 1.0*endR*step)/NSTEPS);
    byte g = (byte)((1.0*startG*(NSTEPS - step) + 1.0*endG*step)/NSTEPS);
    byte b = (byte)((1.0*startB*(NSTEPS - step) + 1.0*endB*step)/NSTEPS);

    display->setPixelColor(txlate(x, y), r, g, b);    
} // xfade(x, y, step)


/*
void Matrix::fadeAll() {
    for (byte x = 0; x < MATRIX_X; x++) {
        for (byte y = 0; y < MATRIX_Y; y++) {
            if (fadeable[x][y]) {
                color RGB = fg[x][y];
                byte r = (byte)((RGB >> 16) & 0xff); 
                byte g = (byte)((RGB >> 8) & 0xff);
                byte b = (byte)(RGB & 0xff);
        
                if (r > 0) r -= fadeable[x][y];
                if (g > 0) g -= fadeable[x][y];
                if (b > 0) b -= fadeable[x][y];
                display->setPixelColor(txlate(x, y), r, g, b);
                if (r==0 && g==0 && b==0) {
                    fadeable[x][y] = 0;
                }
            }
        }
    }
}
*/


// "show" the buffer, by slowly cross-fading the (changed) pixels
void Matrix::show() {
    // temporarily overlay the turtle
    if (fgTurt.c != TRANSPARENT) {
        hidden = fg[fgTurt.x][fgTurt.y];
        fg[fgTurt.x][fgTurt.y] = fgTurt.c;
        bg[bgTurt.x][bgTurt.y] = bgTurt.c;
    }
    for (byte step = 0; step <= 10; step ++) {
        for (byte x = 0; x < MATRIX_X; x++) {
            for (byte y = 0; y < MATRIX_Y; y++) {
                if (fg[x][y] != bg[x][y]) {
                    xfade(x, y, step);
                }
            }
        }
        display->show();
        xfader->wait();
    }
    fg[fgTurt.x][fgTurt.y] = hidden;
    // last, flip
    memcpy(bg, fg, sizeof(fg));
    bgTurt = fgTurt;
} // show()


void Matrix::setPixel(byte x, byte y, color c) {
    fg[x][y] = c;
    // fadeable[x][y] = 0;
}


color Matrix::getPixel(byte x, byte y) {
    return fg[x][y];
}


// sets the turtle location, to be drawn in color tC
// this does NOT affect fg/bg
void Matrix::setTurtle(byte tX, byte tY, color tC) {
    fgTurt.x = tX;
    fgTurt.y = tY;
    fgTurt.c = tC;
} // setTurtle(x, y, c)

/*
void Matrix::setFadeable(byte fX, byte fY, byte fadeyness) {
    fadeable[fX][fY] = fadeyness;
}
*/