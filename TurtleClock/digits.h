#ifndef DIGITS_H
#define DIGITS_H

#include "TinyQueue.h"

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
        void drawDigit(byte, byte, color, byte);
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

    public:
        Digits(Turtle*);
        void erase(byte);
        void draw(byte, byte);
        bool isBusy();
        void go();
        void print();
};


Digits::Digits(Turtle* t) {
    turtle = t;
    digits = new TinyQueue<Digit>(20);
    memset(current, 0, sizeof(current));
} // constructor


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
    Serial.printf("Digit: enq(draw %d@#%d); q#%d\n", d.position, d.n, digits->count());
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
    if (d.c == BLUE) {
        Serial.printf("Digits: draw %d @ #%d; %d remain\n", d.n, d.position, digits->count());
        drawDigit(d.n, d.position, d.c, SLOW);
        current[d.position] = d.n;
        print();
    } else {
        Serial.printf("Digits: erase %d @ #%d; %d remain\n", current[d.position], d.position, digits->count());
        drawDigit(current[d.position], d.position, d.c, FAST);
        print();
    }
} // go()


void Digits::print() {
    Serial.printf("now: %d%d:%d%d\n", current[0], current[1], current[2], current[3]);
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
    turtle->walkTo(startX + 1, 0, TRANSPARENT, FAST);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1, 1, c, speed);
    turtle->walk(0, 5, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
    turtle->walk(0, -5, c, speed);
    turtle->walk(1, -1, c, speed);
}


void Digits::animate1(byte startX, color c, byte speed) {
    turtle->walkTo(startX, 3, TRANSPARENT, FAST);
    
    turtle->walk(3, -3, c, speed);
    turtle->walk(0, 7, c, speed);
}


void Digits::animate2(byte startX, color c, byte speed) {
    turtle->walkTo(startX, 1, TRANSPARENT, FAST);
    
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
    turtle->walkTo(startX, 1, TRANSPARENT, FAST);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, 1, c, speed);
    turtle->walk(-3, 2, c, speed);
    turtle->walk(2, 0, c, speed);
    turtle->walk(1,1, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(-1, 1, c, speed);
    turtle->walk(-2, 0, c, speed);
    turtle->walk(-1, -1, c, speed);
}


void Digits::animate4(byte startX, color c, byte speed) {
    turtle->walkTo(startX+1, 0, TRANSPARENT, FAST);
    
    turtle->walk(-1, 3, c, speed);
    turtle->walk(4, 0, c, speed);
    
    turtle->walk(-1, -2, TRANSPARENT, FAST);
    turtle->walk(0,6, c, speed);
}


void Digits::animate5(byte startX, color c, byte speed) {
    turtle->walkTo(startX + 4, 1, TRANSPARENT, FAST);
    
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
    turtle->walkTo(startX + 4, 1, TRANSPARENT, FAST);
    
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
    turtle->walkTo(startX, 1, TRANSPARENT, FAST);
    
    turtle->walk(0, -1, c, speed);
    turtle->walk(4, 0, c, speed);
    turtle->walk(0, 2, c, speed);
    turtle->walk(-2, 2, c, speed);
    turtle->walk(0, 3, c, speed);
}


void Digits::animate8(byte startX, color c, byte speed) {
    turtle->walkTo(startX + 1, 0, TRANSPARENT, FAST);
    
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
    turtle->walkTo(startX + 4, 2, TRANSPARENT, FAST);
    
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