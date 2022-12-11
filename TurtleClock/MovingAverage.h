#ifndef MOVINGAVERAGE_H
#define MOVINGAVERAGE_H

#undef DEBUG_MA



class MovingAverage {
    private:
        int n;
        float sum;

    public:
        MovingAverage(int);
        void prefill(float);
        float average();
        float average(float);
};


MovingAverage::MovingAverage(int newN) {
    n = newN;
    sum = 0.0;
} // constructor


void MovingAverage::prefill(float value) {
    sum = value * n;
}


float MovingAverage::average() {
    return sum / n;
} // float average()


float MovingAverage::average(float value) {
    sum -= average();
    sum += value;
    return average();
}


#endif