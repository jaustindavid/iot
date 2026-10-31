#pragma once
// Sunrise / sunset, pure math (no Particle or FastLED). Accurate to a minute
// or two, which is plenty for porch lights. Source: "Almanac for Computers",
// US Naval Observatory 1990, the same algorithm most hobby libraries use.
//
// NOTE for future maintainers: the Photon 1 (STM32F205, Cortex-M3) has no FPU,
// so this is all software double math. It runs once every few seconds, which
// is fine. Do not call it per frame.
#include <math.h>
#include "calendar.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ZENITH_OFFICIAL 90.833   // sun's upper limb touches the horizon
#define ZENITH_CIVIL    96.0     // civil twilight: enough light to see by

static inline double sunDeg2Rad(double d) { return d * M_PI / 180.0; }
static inline double sunRad2Deg(double r) { return r * 180.0 / M_PI; }
static inline double sunNorm360(double x) { x = fmod(x, 360.0); return x < 0 ? x + 360.0 : x; }
static inline double sunNorm24(double x)  { x = fmod(x, 24.0);  return x < 0 ? x + 24.0  : x; }

// Returns false when the sun never rises/sets that day (polar regions),
// otherwise sets utHours to the event time in UTC as fractional hours [0, 24).
static inline bool sunEventUtc(int y, int m, int d, double lat, double lon,
                               bool rising, double zenith, double &utHours) {
    const int N = dayOfYear(y, m, d);
    const double lngHour = lon / 15.0;
    const double t = N + (((rising ? 6.0 : 18.0) - lngHour) / 24.0);

    // Sun's mean anomaly, then true longitude.
    const double M = (0.9856 * t) - 3.289;
    double L = M + (1.916 * sin(sunDeg2Rad(M))) + (0.020 * sin(sunDeg2Rad(2 * M))) + 282.634;
    L = sunNorm360(L);

    // Right ascension, forced into the same quadrant as L, then in hours.
    double RA = sunNorm360(sunRad2Deg(atan(0.91764 * tan(sunDeg2Rad(L)))));
    const double Lq  = floor(L / 90.0) * 90.0;
    const double RAq = floor(RA / 90.0) * 90.0;
    RA = (RA + (Lq - RAq)) / 15.0;

    // Declination.
    const double sinDec = 0.39782 * sin(sunDeg2Rad(L));
    const double cosDec = cos(asin(sinDec));

    // Local hour angle.
    const double cosH = (cos(sunDeg2Rad(zenith)) - (sinDec * sin(sunDeg2Rad(lat))))
                        / (cosDec * cos(sunDeg2Rad(lat)));
    if (cosH > 1.0 || cosH < -1.0) return false;

    double H = rising ? 360.0 - sunRad2Deg(acos(cosH)) : sunRad2Deg(acos(cosH));
    H /= 15.0;

    const double T = H + RA - (0.06571 * t) - 6.622;
    utHours = sunNorm24(T - lngHour);
    return true;
}

// Convenience: both events for one local calendar date, as minutes after
// LOCAL midnight, given the local UTC offset in seconds for that day.
// Returns false if either event does not occur.
static inline bool sunTimesLocalMin(int y, int m, int d, double lat, double lon,
                                    int32_t localOffsetSec, double zenith,
                                    int &riseMin, int &setMin) {
    double r, s;
    if (!sunEventUtc(y, m, d, lat, lon, true,  zenith, r)) return false;
    if (!sunEventUtc(y, m, d, lat, lon, false, zenith, s)) return false;
    riseMin = (int)((lround(r * 60.0) + localOffsetSec / 60 + 1440) % 1440);
    setMin  = (int)((lround(s * 60.0) + localOffsetSec / 60 + 1440) % 1440);
    return true;
}
