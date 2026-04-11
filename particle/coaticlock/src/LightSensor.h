#pragma once

/*
  ## Brightness Curve

  **KV key:** `brightness_curve` | **Default:** `2.5` | **Range:** `1.0–5.0`

  Controls how the display brightness responds to ambient light. Think of it as
  the "sensitivity shape" — how eager the display is to brighten up as the room
  gets lighter.

  | Value | Behavior |
  |-------|----------|
  | `0.3–0.7` | Pops to bright very quickly — display is nearly full brightness in a moderately lit room. Good for bright environments where you always want a vivid display. |
  | `1.0` | Linear — brightness tracks light level evenly across the whole range |
  | `2.0–3.0` | Display stays dim until the room gets noticeably bright, then ramps up. Good for mixed-use rooms. |
  | `4.0+` | Display spends most of its time very dim and only brightens in a well-lit room. Good for bedrooms. |

  > **Below 1.0** = jumps to bright quickly, compressed at the top end.  
  > **1.0** = perfectly linear.  
  > **Above 1.0** = stays dim longer, only brightens in a well-lit room.  
  >
  > A value of `0.5` is equivalent to a square-root curve; `2.0` is equivalent to squaring.

*/

class LightSensor {
public:
    float exponent = 2.5f;

    // Calibration bounds — start conservative, expand as extremes are observed
    int cal_dark  = 3500;  // raw value that maps to "fully dark"
    int cal_bright = 2000; // raw value that maps to "fully bright"

    void set_exponent(float v) {
        exponent = constrain(v, 0.1f, 10.0f);  // hard safety rails
    }

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
