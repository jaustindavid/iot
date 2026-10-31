#pragma once
// One palette per Season (see calendar.h). Each is 4 colors repeated across
// a 16-entry FastLED palette so the wave shows 4 soft bands.
// To change a season's colors, edit here and nowhere else.
#include <FastLED.h>
#include "calendar.h"

static inline CRGBPalette16 fourColor(CRGB a, CRGB b, CRGB c, CRGB d) {
    return CRGBPalette16(a, a, a, a, b, b, b, b, c, c, c, c, d, d, d, d);
}

static inline CRGBPalette16 paletteFor(int season) {
    const CRGB purple(160, 0, 255);     // CRGB::Purple is too dim on WS2812
    const CRGB darkOrange(255, 70, 0);
    const CRGB brown(140, 50, 8);
    const CRGB leafGreen(20, 110, 10);
    const CRGB candle(255, 150, 45);    // warm white
    const CRGB ember(255, 110, 20);
    const CRGB pastelGreen(90, 255, 90);
    const CRGB pastelPink(255, 110, 160);

    switch (season) {
        case SEASON_STPATTY:      return fourColor(CRGB::Green, CRGB::DarkOrange, CRGB::Green, CRGB::DarkOrange);
        case SEASON_EASTER:       return fourColor(CRGB::Yellow, pastelGreen, CRGB::Cyan, pastelPink);
        case SEASON_MEMORIAL:
        case SEASON_JULY4:
        case SEASON_LABOR:        return fourColor(CRGB::Red, CRGB::White, CRGB::Blue, CRGB::White);
        case SEASON_HALLOWEEN:    return fourColor(CRGB::Orange, purple, CRGB::Orange, purple);
        case SEASON_THANKSGIVING: return fourColor(darkOrange, brown, leafGreen, brown);
        case SEASON_XMAS:         return fourColor(CRGB::Red, CRGB::Green, CRGB::White, CRGB::Green);
        case SEASON_DEFAULT:
        default:                  return fourColor(candle, ember, candle, ember);
    }
}
