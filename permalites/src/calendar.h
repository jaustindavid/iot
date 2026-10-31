#pragma once
// Pure date math: no Particle or FastLED dependencies, so it can be
// unit-tested on a desktop and ported to an ESP32 unchanged.
#include <stdint.h>

// ---------- civil date <-> days since 1970-01-01 (Howard Hinnant) ----------

static inline int32_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static inline void civilFromDays(int32_t z, int &y, int &m, int &d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

// 0 = Sunday .. 6 = Saturday
static inline int weekdayFromDays(int32_t z) {
    return z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6;
}

static inline int weekdayOf(int y, int m, int d) {
    return weekdayFromDays(daysFromCivil(y, m, d));
}

static inline int dayOfYear(int y, int m, int d) {
    return (int)(daysFromCivil(y, m, d) - daysFromCivil(y, 1, 1)) + 1;
}

struct Civil {
    int year, month, day, hour, minute, second;
    int weekday; // 0 = Sunday
};

static inline Civil civilFromEpoch(int64_t t) {
    Civil c;
    int32_t days = (int32_t)(t >= 0 ? t / 86400 : (t - 86399) / 86400);
    int32_t secs = (int32_t)(t - (int64_t)days * 86400);
    civilFromDays(days, c.year, c.month, c.day);
    c.weekday = weekdayFromDays(days);
    c.hour = secs / 3600;
    c.minute = (secs / 60) % 60;
    c.second = secs % 60;
    return c;
}

// ---------- holiday helpers ----------

// Day-of-month of the nth (1-based) given weekday in a month.
static inline int nthWeekdayOfMonth(int y, int m, int weekday, int n) {
    int first = 1 + (weekday - weekdayOf(y, m, 1) + 7) % 7;
    return first + 7 * (n - 1);
}

static inline int daysInMonth(int y, int m) {
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dim[m - 1];
}

static inline int lastWeekdayOfMonth(int y, int m, int weekday) {
    int last = daysInMonth(y, m);
    return last - (weekdayOf(y, m, last) - weekday + 7) % 7;
}

// Gregorian Easter Sunday (Meeus / Jones / Butcher).
static inline void easterDate(int y, int &month, int &day) {
    int a = y % 19, b = y / 100, c = y % 100;
    int d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4, k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    month = (h + l - 7 * m + 114) / 31;
    day = ((h + l - 7 * m + 114) % 31) + 1;
}

// ---------- US daylight saving ----------
// DST runs from 02:00 standard time on the second Sunday of March
// until 02:00 daylight time (= 01:00 standard) on the first Sunday of November.
static inline bool usDstActive(int64_t utc, int32_t stdOffsetSec) {
    int64_t sl = utc + stdOffsetSec; // standard local time
    Civil c = civilFromEpoch(sl);
    int64_t start = (int64_t)daysFromCivil(c.year, 3, nthWeekdayOfMonth(c.year, 3, 0, 2)) * 86400 + 2 * 3600;
    int64_t end   = (int64_t)daysFromCivil(c.year, 11, nthWeekdayOfMonth(c.year, 11, 0, 1)) * 86400 + 1 * 3600;
    return sl >= start && sl < end;
}

// Total local offset (standard + DST) in seconds for a UTC instant.
static inline int32_t localOffsetSec(int64_t utc, int32_t stdOffsetSec) {
    return stdOffsetSec + (usDstActive(utc, stdOffsetSec) ? 3600 : 0);
}

// ---------- seasons ----------

enum Season {
    SEASON_DEFAULT = 0,
    SEASON_STPATTY,
    SEASON_EASTER,
    SEASON_MEMORIAL,
    SEASON_JULY4,
    SEASON_LABOR,
    SEASON_HALLOWEEN,
    SEASON_THANKSGIVING,
    SEASON_XMAS,
    SEASON_COUNT
};

static inline const char *seasonName(int s) {
    switch (s) {
        case SEASON_DEFAULT:      return "default";
        case SEASON_STPATTY:      return "stpatty";
        case SEASON_EASTER:       return "easter";
        case SEASON_MEMORIAL:     return "memorial";
        case SEASON_JULY4:        return "july4";
        case SEASON_LABOR:        return "labor";
        case SEASON_HALLOWEEN:    return "halloween";
        case SEASON_THANKSGIVING: return "thanksgiving";
        case SEASON_XMAS:         return "xmas";
        default:                  return "?";
    }
}

static inline bool between(int32_t D, int32_t a, int32_t b) { return D >= a && D <= b; }

static inline Season seasonForDate(int y, int m, int d) {
    const int32_t D = daysFromCivil(y, m, d);

    // Christmas: day after Thanksgiving through Jan 6. Wraps the year.
    const int32_t tg = daysFromCivil(y, 11, nthWeekdayOfMonth(y, 11, 4, 4));
    if (D >= tg + 1 || D <= daysFromCivil(y, 1, 6)) return SEASON_XMAS;

    // Easter: Palm Sunday through Easter Monday. Checked before St. Patrick's
    // because an early Easter can overlap it.
    int em, ed;
    easterDate(y, em, ed);
    const int32_t E = daysFromCivil(y, em, ed);
    if (between(D, E - 7, E + 1)) return SEASON_EASTER;

    if (between(D, daysFromCivil(y, 3, 10), daysFromCivil(y, 3, 18))) return SEASON_STPATTY;

    const int32_t mem = daysFromCivil(y, 5, lastWeekdayOfMonth(y, 5, 1));
    if (between(D, mem - 3, mem + 1)) return SEASON_MEMORIAL;

    if (between(D, daysFromCivil(y, 6, 27), daysFromCivil(y, 7, 11))) return SEASON_JULY4;

    const int32_t lab = daysFromCivil(y, 9, nthWeekdayOfMonth(y, 9, 1, 1));
    if (between(D, lab - 3, lab + 1)) return SEASON_LABOR;

    if (between(D, daysFromCivil(y, 10, 1), daysFromCivil(y, 11, 1))) return SEASON_HALLOWEEN;

    if (between(D, daysFromCivil(y, 11, 2), tg)) return SEASON_THANKSGIVING;

    return SEASON_DEFAULT;
}
