#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define MAX_X 32
#define MAX_Y 8

float x = 0.0, y = 0.0;
int angle = 45;


float mod(float v, int base) {
	while (v > base) {
		v -= base;
	}
	return v;
}


// true if v /almost/ eq i
char eq(float v, int i) {
	return fabsf(fabsf(v) - abs(i)) < 0.1;
}


void step() {
	x = mod(x + cos(angle), MAX_X);
	y = mod(y + sin(angle), MAX_Y);
}


int main(void) {
	int target = 3;
	while (!eq(x, target)) {
		step();
		printf("(%05.2f,%05.2f)\n", x, y);
	}
}
