// Dst.cpp — see Dst.h for design notes.
//
// Pure portable C++. We roll our own UTC-tm-to-time_t conversion
// rather than calling `timegm` because arduino-esp32's newlib doesn't
// ship that symbol despite it being newlib-derived elsewhere. The
// manual implementation is ~20 lines, exact, and avoids per-platform
// surprises (host tests + ESP32 firmware share one code path).

#include "Dst.h"

#include <stddef.h>


namespace critterchron {
namespace dst {

namespace {

// Days from Jan 1 to the start of each month in a NON-leap year.
// (Index 0 unused; months are 1-based to match callers.)
static const int DAYS_TO_MONTH[13] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365
};

static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// UTC `struct tm` → UTC time_t. Mirrors the contract of `timegm` but
// portable everywhere. Accepts tm_year/mon/mday/hour/min/sec; ignores
// tm_wday/yday/isdst (computed values, not inputs).
static time_t tm_to_utc_(struct tm* t) {
    int year = t->tm_year + 1900;
    int month = t->tm_mon;          // 0..11
    if (month < 0 || month > 11) return (time_t)-1;
    long days = 0;
    for (int y = 1970; y < year; ++y) {
        days += is_leap(y) ? 366 : 365;
    }
    days += DAYS_TO_MONTH[month];
    if (month >= 2 && is_leap(year)) ++days;
    days += t->tm_mday - 1;
    return (time_t)((days * 86400L) + (t->tm_hour * 3600L) +
                    (t->tm_min * 60L) + t->tm_sec);
}

}  // anonymous namespace


time_t nth_dow_of_month_utc(int year, int month_1_12, int dow_0_6,
                            int nth, int seconds_into_day_utc) {
    if (month_1_12 < 1 || month_1_12 > 12) return 0;
    if (dow_0_6   < 0 || dow_0_6   > 6)   return 0;
    if (nth == 0) return 0;

    // Build midnight UTC of the 1st of the month.
    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month_1_12 - 1;
    tm.tm_mday = 1;
    time_t first_of_month = tm_to_utc_(&tm);
    if (first_of_month == (time_t)-1) return 0;

    // gmtime tells us the dow of the 1st.
    struct tm first_tm;
    gmtime_r(&first_of_month, &first_tm);
    int first_dow = first_tm.tm_wday;     // 0=Sun

    // Days from the 1st to the FIRST matching dow.
    int days_to_first = (dow_0_6 - first_dow + 7) % 7;

    // Days-in-month table with Feb adjusted for leap years. Used to
    // reject impossible nth (e.g. "5th Sunday of February") and to
    // find the last matching dow when nth==-1.
    static const int DAYS_IN[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    int days_in_month = DAYS_IN[month_1_12];
    if (month_1_12 == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) days_in_month = 29;
    }

    int target_day;
    if (nth > 0) {
        target_day = 1 + days_to_first + (nth - 1) * 7;
        if (target_day > days_in_month) return 0;
    } else {
        // nth == -1 → LAST matching dow. Find by overshooting and
        // backing off by a week if needed.
        // (Not used by the US rule today but useful for EU/AU later.)
        int last_candidate = 1 + days_to_first + 4 * 7;  // 5th occurrence (if it exists)
        while (last_candidate > days_in_month) last_candidate -= 7;
        target_day = last_candidate;
        if (target_day < 1) return 0;
    }

    // Build the target moment.
    tm.tm_mday = target_day;
    tm.tm_hour = seconds_into_day_utc / 3600;
    tm.tm_min  = (seconds_into_day_utc % 3600) / 60;
    tm.tm_sec  = seconds_into_day_utc % 60;
    // tm_year/tm_mon already set above.
    return tm_to_utc_(&tm);
}


bool is_us_dst_active(time_t now_utc, float base_offset_hours) {
    // Read the year from `now_utc`. Each year's DST window is
    // self-contained — never crosses Jan 1.
    struct tm now_tm;
    gmtime_r(&now_utc, &now_tm);
    int year = now_tm.tm_year + 1900;

    // The two transitions are defined against different local clocks:
    //
    //   Spring forward — at 02:00 STANDARD local time. Clocks jump
    //     forward to 03:00 daylight. UTC moment = 02:00 - base_offset.
    //     For US-Mountain (base=-7): 09:00 UTC.
    //
    //   Fall back     — at 02:00 DAYLIGHT local time. Clocks jump
    //     back to 01:00 standard. Daylight = base + 1, so the UTC
    //     moment is one hour earlier than the spring formula yields.
    //     For US-Mountain: 08:00 UTC. Subtle but real — this is the
    //     bug that bit test_us_dst_fall_back_boundary_mountain on
    //     first run.
    int spring_seconds = (int)((2.0f - base_offset_hours) * 3600.0f);
    int fall_seconds   = spring_seconds - 3600;

    // Sunday = 0.
    time_t spring = nth_dow_of_month_utc(year,  3, /*Sun*/0, /*2nd*/ 2,
                                         spring_seconds);
    time_t fall   = nth_dow_of_month_utc(year, 11, /*Sun*/0, /*1st*/ 1,
                                         fall_seconds);
    if (spring == 0 || fall == 0) return false;  // pathological inputs

    return now_utc >= spring && now_utc < fall;
}


float dst_adjustment_hours(Rule rule, time_t now_utc,
                           float base_offset_hours) {
    switch (rule) {
        case Rule::Disabled:
            return 0.0f;
        case Rule::US:
            return is_us_dst_active(now_utc, base_offset_hours) ? 1.0f : 0.0f;
        default:
            // Unknown rule — treat as disabled. Future EU/AU/etc.
            // values land as new cases here.
            return 0.0f;
    }
}

}  // namespace dst
}  // namespace critterchron
