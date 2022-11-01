#include <stdlib.h>
#include <stdio.h>
#include <time.h>


int rndm(int low, int high) {
	float r = 1.0*rand()/RAND_MAX;
	return low + r*(high-low);
}

int r(int rmin, int rmax) {
	float r = 0.000001 * rndm(0, 1000000);
	return rmin + (int)(r*r*r*(rmax - rmin));
}

int main(void) {
	int s, c;
	s = c = 0;
	srand(time(0));
	for (int i = 0; i < 10; i++) {
		c = r(10, 100);
		printf("%d: %d\n", i, c);
		s += c;
	}
	printf("Mean: %5.2f\n", 1.0*(s / 10));
	return 0;
}
