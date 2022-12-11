#ifndef DIGITS_H
#define DIGITS_H

#include "TinyQueue.h"

/*
 * A single "Digit" is a position, color, and a number
 *
 * digits are scheduled to be draw()n; at the appropriate
 * time they are animated with the Turtle.
 *
 */

class Digit {
    public:
        byte position;
        color c;
        byte n;
        
        Digit() {}
        Digit(int) { position = 99; }
};


class Digits {
    private:
        TinyQueue<Digit> *digits;
        byte current[4];
        Turtle *turtle;
        const byte positions[5] = { 3, 9, 18, 24, 15 };
        byte* fonts[11];
        void drawDigit(byte, byte, color, byte);
        void animateColon(byte);
        void animate0(byte, color, byte);
        void animate1(byte, color, byte);
        void animate2(byte, color, byte);
        void animate3(byte, color, byte);
        void animate4(byte, color, byte);
        void animate5(byte, color, byte);
        void animate6(byte, color, byte);
        void animate7(byte, color, byte);
        void animate8(byte, color, byte);
        void animate9(byte, color, byte);
//        void makeFonts();
//        void animate(byte, byte, color, byte);

    public:
        Digits(Turtle*);
        int count();
        void erase(byte);
        void drawColon();
        void draw(byte, byte);
        bool isBusy();
        void go();
        void print();
};


Digits::Digits(Turtle* t) {
    turtle = t;
    digits = new TinyQueue<Digit>(20);
    memset(current, 0, sizeof(current));
    // makeFonts();
} // constructor


int Digits::count() {
    return digits->count();
}


void Digits::erase(byte position) {
    Digit d;
    d.position = position;
    d.c = BLACK;
    d.n = 99;
    digits->enqueue(d);
    Serial.printf("Digit: enq(erase #%d); q#%d\n", d.position, digits->count());
} // erase(position)


void Digits::draw(byte position, byte n) {
    Digit d;
    d.position = position;
    d.c = BLUE;
    d.n = n;
    digits->enqueue(d);
    Serial.printf("Digit: enq(draw %d@#%d); q#%d\n", d.n, d.position, digits->count());
} // draw(position, n)


void Digits::drawColon() {
    Digit d;
    d.position = 5;
    d.c = BLUE;
    d.n = -1;
    digits->enqueue(d);
    Serial.printf("Digit: enq(draw ::); q#%d\n", digits->count());
} // draw(position, n)


bool Digits::isBusy() {
    return turtle->isBusy() || !digits->isEmpty();
} // bool isBusy()


// the driver function
// if my turtle is workin', he should keep doing that
// if I don't have anything to do, don't
// otherwise, dispatch the turtle.
void Digits::go() {
    if (turtle->isBusy()) {
        turtle->go();
        return;
    }
    if (digits->isEmpty()) {
        Serial.println("Digits: no pending tasks");
        return;
    }
    
    Digit d = digits->dequeue();
    if (d.position == 5)  {
        Serial.printf("Digits: draw :: ; %d remain\n", digits->count());
        animateColon(Turtle::slow);
        // animate(5, 10, Turtle::blue, Turtle::slow);
        print();
    } else if (d.c == BLUE) {
        Serial.printf("Digits: draw %d @ #%d; %d remain\n", d.n, d.position, digits->count());
        drawDigit(d.n, d.position, d.c, Turtle::slow);
        current[d.position] = d.n;
        print();
    } else {
        Serial.printf("Digits: erase %d @ #%d; %d remain\n", current[d.position], d.position, digits->count());
        drawDigit(current[d.position], d.position, d.c, Turtle::fast);
        print();
    }
} // go()


void Digits::print() {
    Serial.printf("now: %d%d:%d%d\n", current[0], current[1], current[2], current[3]);
}


/*
void Digits::makeFonts() {
    int8_t zero[] PROGMEM = {
        'a', 1, 0, Turtle::transparent, Turtle::fast,
        'd', 2, 0, Turtle::blue, Turtle::slow,
        'd', 1, 1, Turtle::blue, Turtle::slow,
        'd', 0, 5, Turtle::blue, Turtle::slow,
        'd', -1, 1, Turtle::blue, Turtle::slow,
        'd', -2, 0, Turtle::blue, Turtle::slow,
        'd', -1, -1, Turtle::blue, Turtle::slow,
        'd', 0, -5, Turtle::blue, Turtle::slow,
        'd', 1, -1, Turtle::blue, Turtle::slow,
        Turtle::stop
    };
    fonts[0] = (byte*)malloc(sizeof(zero));
    memcpy(fonts[0], zero, sizeof(zero));

    int8_t one[] PROGMEM = {
        'a', 1, 0, Turtle::transparent, Turtle::fast,
        'd', 2, 0, Turtle::blue, Turtle::slow,
        'd', 1, 1, Turtle::blue, Turtle::slow,
        'd', 0, 5, Turtle::blue, Turtle::slow,
        'd', -1, 1, Turtle::blue, Turtle::slow,
        'd', -2, 0, Turtle::blue, Turtle::slow,
        'd', -1, -1, Turtle::blue, Turtle::slow,
        'd', 0, -5, Turtle::blue, Turtle::slow,
        'd', 1, -1, Turtle::blue, Turtle::slow,
        Turtle::stop
    };
    fonts[1] = (byte*)malloc(sizeof(one));
    memcpy(fonts[0], zero, sizeof(one));
    
    int8_t colon[] PROGMEM = {
        'a', 15, 1, Turtle::transparent, Turtle::fast,
        'd', 1, 0, Turtle::blue, Turtle::slow,
        'd', 0, 1, Turtle::blue, Turtle::slow,
        'd', -1, 0, Turtle::blue, Turtle::slow, 
        'd', 0, 2, Turtle::transparent, Turtle::fast,
        'd', 1, 0, Turtle::blue, Turtle::slow,
        'd', 0, 1, Turtle::blue, Turtle::slow,
        'd', -1, 0, Turtle::blue, Turtle::slow,
        Turtle::stop
    };
    fonts[10] = (byte*)malloc(sizeof(colon));
    memcpy(fonts[10], colon, sizeof(colon));
}


// instruct turtle in animating the specifed font
void Digits::animate(byte position, byte n, color c, byte speed) {
    static const color colors[4] = { TRANSPARENT, BLACK, BLUE, GREEN };
    Serial.printf("starting animate; n=%d\n", n);
    byte* p;
    p = fonts[n];

    while (p[0] != Turtle::stop) {
        Serial.printf("%c: %d %d %s %s\n", p[0], (int8_t)p[1], (int8_t)p[2], 
                p[3] == Turtle::blue ? "blue" : "transparent", 
                p[4] == Turtle::fast ? "fast" : "slow");
        if (p[0] == 'a') {
            turtle->walkTo(p[1], p[2], colors[p[3]], p[4]);
        } else {
            turtle->walk((int8_t)p[1], (int8_t)p[2], colors[p[3]], p[4]);
        }
        p += 5;
    }
} // animate(position, n, color, speed)
*/

void Digits::animateColon(byte speed) {
    turtle->walkTo(15, 1, TRANSPARENT, Turtle::fast);
    turtle->walk(1, 0, BLUE, speed);
    turtle->walk(0, 1, BLUE, speed);
    turtle->walk(-1, 0, BLUE, speed);
    
    turtle->walk(0, 2, TRANSPARENT, Turtle::fast);
    turtle->walk(1, 0, BLUE, speed);
    turtle->walk(0, 1, BLUE, speed);
    turtle->walk(-1, 0, BLUE, speed);
}


void Digits::drawDigit(byte n, byte position, color c, byte speed) {
    Serial.printf("drawing %d at %d\n", n, position);
    byte start = positions[position];
    switch (n) {
        case 1:
            animate1(start, c, speed);
            break;
        case 2:
            animate2(start, c, speed);
            break;
        case 3:
            animate3(start, c, speed);
            break;
        case 4:
            animate4(start, c, speed);
            break;
        case 5:
            animate5(start, c, speed);
            break;
        case 6:
            animate6(start, c, speed);
            break;
        case 7:
            animate7(start, c, speed);
            break;
        case 8:
            animate8(start, c, speed);
            break;
        case 9:
            animate9(start, c, speed);
            break;
        case 0:
        default:
            animate0(start, c, speed);
    }
}


void Digits::animate0(byte startX, color c, byte speed) {
    turtle->walkTo(startX + 1, 0, TRANSPARENT, Turtle::fast);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 5, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(0, -5, c, speed);
    // turtle->walk(1, -1, c, speed);
}


void Digits::animate1(byte startX, color c, byte speed) {
    turtle->walkTo(startX, 3, TRANSPARENT, Turtle::fast);
    
    turtle->walk(3, -3, c, speed);
    turtle->walk(0, 7, c, speed);
}


void Digits::animate2(byte startX, color c, byte speed) {
    turtle->walkTo(startX, 1, TRANSPARENT, Turtle::fast);
    
    turtle->walk(1, -1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(-4, 4, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, -1, c, speed);
}


void Digits::animate3(byte startX, color c, byte speed) {
    turtle->walkTo(startX, 1, TRANSPARENT, Turtle::fast);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, 1, c, speed);
    // turtle->walk(-3, 2, c, speed);
    turtle->walk(-2, 2, c, speed);
    turtle->walk(-1, 0, TRANSPARENT, Turtle::fast);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1,1, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
}


void Digits::animate4(byte startX, color c, byte speed) {
    turtle->walkTo(startX+1, 0, TRANSPARENT, Turtle::fast);
    
    turtle->walk(-1, 3, c, speed);
    turtle->walk(4, 0, c, speed);
    
    turtle->walk(-1, -2, TRANSPARENT, Turtle::fast);
    turtle->walk(0,6, c, speed);
}


void Digits::animate5(byte startX, color c, byte speed) {
    turtle->walkTo(startX + 4, 1, TRANSPARENT, Turtle::fast);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(-4, 0, c, speed);
    turtle->walk(0, 3, c, speed);
    turtle->walk(3, 0, c, speed);
    turtle->walk(1,1, c, speed);
    turtle->walk(0,2, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
}


void Digits::animate6(byte startX, color c, byte speed) {
    turtle->walkTo(startX + 4, 1, TRANSPARENT, Turtle::fast);
    
    turtle->walk(-1, -1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(0, 5, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, -1, c, speed);
    turtle->walk(0, -2, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(-1, 0, c, speed);
}


void Digits::animate7(byte startX, color c, byte speed) {
    turtle->walkTo(startX, 1, TRANSPARENT, Turtle::fast);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(-2, 2, c, speed);
    turtle->walk(0, 3, c, speed);
}


void Digits::animate8(byte startX, color c, byte speed) {
    turtle->walkTo(startX + 1, 0, TRANSPARENT, Turtle::fast);
    
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, -1, c, speed);
    turtle->walk(0, -2, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(0, -1, c, speed);
    turtle->walk(1, -1, c, speed);
    // walk(2, 0, c);
}


void Digits::animate9(byte startX, color c, byte speed) {
    turtle->walkTo(startX + 4, 2, TRANSPARENT, Turtle::fast);
    
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(0, -1, c, speed);
    turtle->walk(1, -1, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 5, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
}
#endif