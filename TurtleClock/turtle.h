#ifndef TURTLE_H
#define TURTLE_H

#include "Particle.h"
#include "matrix.h"

#define fTINYQUEUE_SIZE 100
#include "TinyQueue.h"

#define STATE_IDLE  0
#define STATE_HLINE 1
#define STATE_VLINE 2

#define SLOW 3
#define FAST 0


class Task {
    public:
        char instruction;
        int x;
        int y;
        int color;
        byte speed;
        
        Task() {}
        Task(int) { instruction = '\0'; }
};


/*
 * Angle: rounded to nearest 45.  N = 0, NE = 45, E = 90, etc.
 */

class Turtle {
    private:
        Matrix *matrix;
        // TinyQueue<String> queue;
        TinyQueue<Task> *tasks;
        byte state, ntasks;
        byte _delay, _speed, _i;
        color _trail, _hidden;
        float _x, _y, _Dx, _Dy, _dx, _dy; 
        int _xi, _yi; 
        void drawHLine(byte, byte);
        void drawVLine(byte, byte);
        void drawLine(byte, byte, color, byte);
        void stepLine();
        void decode(String);
        void startATask();
        
        
    public:
        int x, y, angle;
        color hidden;

        Turtle(Matrix*, byte);
        void step();
        void turn(int);
        void print();
        void walkTo(byte, byte, color, byte);
        void walk(int, int, color, byte);
        void teleport(byte, byte);
        bool isBusy();
        void go();
        void abandon();
};


#endif