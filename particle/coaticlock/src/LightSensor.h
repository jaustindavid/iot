#pragma once

class LightSensor {
public:
    float exponent = 2.5f;

    // Calibration bounds — start conservative, expand as extremes are observed
    int cal_dark  = 3500;  // raw value that maps to "fully dark"
    int cal_bright = 2000; // raw value that maps to "fully bright"

    float update(int raw) {
        // Expand calibration bounds as new extremes are observed
        if (raw > cal_dark)   cal_dark  = raw;
        if (raw < cal_bright) cal_bright = raw;

        // Normalize: dark end → 0.0, bright end → 1.0 (inverted, higher raw = darker)
        float range = (float)(cal_dark - cal_bright);
        if (range < 1.0f) return 0.5f; // guard against division by zero at startup

        float normalized = (float)(cal_dark - raw) / range;  // 0..1, 0=dark, 1=bright
        normalized = constrain(normalized, 0.0f, 1.0f);

        // Power curve: exponent > 1 gives fine control at dark end, compression at bright
        return powf(normalized, exponent);
    }
};
