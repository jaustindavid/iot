#include "turtle.h"


Turtle::Turtle(Matrix* newMatrix, byte qsize) {
    matrix = newMatrix;
    x = y = angle = 0;
    _hidden = 0;
    ntasks = qsize;
    tasks = new TinyQueue<Task>(ntasks);
} // Turtle(Adafruit_NeoPixel*)


/*
 *        360
 *    315     45
 *  270         90
 *    225     135
 *        180
 */
void Turtle::step() {
    // matrix->setPixel(x, y, _hidden);

    if (angle > 337 || angle < 23) { // N
        y -= 1;    
    } else if (angle < 45+23) {      // NE
        x += 1;
        y -= 1;   
    } else if (angle < 90+23) {      // E
        x += 1;
    } else if (angle < 135+23) {     // SE
        x += 1;
        y += 1;
    } else if (angle < 180+23) {     // S
        y += 1;
    } else if (angle < 225+23) {     // SW
        x -= 1;
        y += 1;
    } else if (angle < 170 + 23) {   // W
        x -= 1;
    } else {                         // NW
        x -= 1; 
        y -= 1;
    }
    if (x < 0) { 
        x += MATRIX_X; 
    } else {
        x = x % MATRIX_X;
    }
    if (y < 0) { 
        y += MATRIX_Y; 
    } else {
        y = y % MATRIX_Y;
    }
    
    matrix->setTurtle(x, y, GREEN);
    return;
    // draw my GREEN shell
    _hidden = matrix->getPixel(x, y);
    matrix->setPixel(x, y, GREEN);
} // step()


void Turtle::turn(int turnAngle) {
    angle += turnAngle;
    if (angle < 0) { 
        angle += 360;
    } else {
        angle = angle % 360;
    }
} // turn()


void Turtle::print() {
    Serial.printf("Turtle (%02d, %02d) @ %3d", x, y, angle);
} // print()


// Austin's Slow Line Algorithm
// Breshenham is great and all, but an MCU mostly sleeps
/*
 * INCREMENTAL VERSION
 * start: drawLine(x', y')
 * iterate: while (state != STATE_IDLE) stepLine()
 *
 */


void Turtle::stepLine() {
    if (false && _delay != 0) {
        _delay -= 1;
        return;
    } 

    // leave a trail?
    if (_trail != TRANSPARENT) {
        matrix->setPixel(x, y, _trail);
    } 

    if (state == STATE_HLINE) {
        _x += _xi;
        _y += _dy;
    } else {
        _x += _dx;
        _y += _yi;
    }
    
    x = round(_x);
    y = round(_y);

    matrix->setTurtle(x, y, GREEN, _speed);
    
    -- _i;
    _delay = _speed;
    if (_i == 0) {
        if (_trail != TRANSPARENT) {
            _hidden = _trail;  
            matrix->setPixel(x, y, _trail);
        }   
        // Serial.printf("end state: (%5.2f, %5.2f)\n", _x, _y);
        state = STATE_IDLE;
    }
} // stepLine()


void Turtle::drawHLine(byte x1, byte y1) {
    _xi = _Dx > 0 ? 1 : -1;
    _dy = _Dy/abs(_Dx);
    _i = abs(_Dx);
    
    // print();
    // Serial.printf("HLine: xi=%d, dy=%5.2f\n", _xi, _dy);
    state = STATE_HLINE;
} // drawHLine(x0, y0, x1, y1, color, speed)


void Turtle::drawVLine(byte x1, byte y1) {
    _yi = _Dy > 0 ? 1 : -1;
    _dx = _Dx/abs(_Dy);
    _i = abs(_Dy);

    // print();
    // Serial.printf("VLine: yi=%d, i=%d, dx=%5.2f\n", _yi, _i, _dx);
    state = STATE_VLINE;
} // drawVLine(x0, y0, x1, y1, color, speed)


void Turtle::drawLine(byte x1, byte y1, color c, byte speed) {
    _Dy = y1 - y;
    _Dx = x1 - x;
    if (_Dy == 0 && _Dx == 0) {
        Serial.printf("discarding null move to (%d, %d)\n", x1, y1);
        return;
    }
    _x = x;
    _y = y;
    _delay = _speed = speed;
    _trail = c;

    // Serial.printf("Line: Dy = %5.2f, Dx = %5.2f\n", _Dy, _Dx);
    if (c == TRANSPARENT) {
        Serial.print("moving ("); 
    } else {
        Serial.print("drawing ("); 
    }
    Serial.print(x); 
    Serial.print(","); 
    Serial.print(y);
    Serial.print(") -> (");
    Serial.print(x1);
    Serial.print(",");
    Serial.print(y1);
    Serial.println(")");
    if (abs(_Dy) > abs(_Dx)) {
        drawVLine(x1, y1);
    } else {
        drawHLine(x1, y1);
    }
} // drawLine(x1, y1, color, speed)


// walk to a location; drag a color (or TRANSPARENT)
// returns quickly, but leaves turtle in a busy state;
// while (isBusy()) go();
void Turtle::walkTo(byte newX, byte newY, color c, byte speed) {
    // ignore trivial walks
    if (newX == x && newY == y) {
        if (c != TRANSPARENT) {
            _hidden = c;
        }   
        return;
    }

    Task t;
    t.instruction = 'a';
    t.x = newX;
    t.y = newY;
    t.color = c;
    t.speed = speed;
    tasks->enqueue(t);
    print();
    Serial.printf("; enq a->(%0d,%0d)", t.x, t.y);
    Serial.printf("; %d tasks in queue\n", tasks->count());
    // Serial.println(s);
} // walkTo(x, y, color, speed)


// walk in a direction; drag a color
void Turtle::walk(int dx, int dy, color c, byte speed) {
    // if the queue is full, we have to consume 1 task
    if (tasks->count() >= ntasks) {
        // finish anything running;
        while (isBusy()) go();
        // start a new one, to consume 1 task slot
        startATask();
    }
    
    Task t;
    t.instruction = 'd';
    t.x = dx;
    t.y = dy;
    t.color = c;
    t.speed = speed;
    tasks->enqueue(t);
    print(); 
    Serial.printf("; enq d->(%0d,%0d)", t.x, t.y);
    Serial.printf("; %d tasks in queue\n", tasks->count());
    //Serial.printf(": d (%d,%d)\n", t.x, t.y);
} // walk(dx, dy, color, speed)


void Turtle::teleport(byte newX, byte newY) {
    // TURTLE matrix->setPixel(x, y, _hidden);
    x = newX;
    y = newY;
    matrix->setTurtle(x, y, GREEN);
}


void Turtle::startATask() {
    if (! tasks->isEmpty()) {
        Task task = tasks->dequeue();
        Serial.printf("q=%d\n", tasks->count());
        switch (task.instruction) {
            case 'a': // absolute
                drawLine(task.x, task.y, task.color, task.speed);
                break;
            case 'd': // delta
                drawLine(x + task.x, y + task.y, task.color, task.speed);
                break;
        }
    }
}


bool Turtle::isBusy() {
    // Serial.printf("Busy?  state=%d", state);
    if (state == STATE_IDLE) {
        startATask();
    }
    return state != STATE_IDLE;
} // bool isBusy();



void Turtle::go() {
    switch (state) {
        case STATE_HLINE:
            stepLine();
            break;
        case STATE_VLINE:
            stepLine();
            break;
        default:
            if (tasks->isEmpty()) {
                state = STATE_IDLE;
            } else {
                startATask();
            }
    }
} // go()


void Turtle::abandon() {
    state = STATE_IDLE;
    tasks->reset();
}
