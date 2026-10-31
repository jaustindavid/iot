#ifndef PARTICLE  // keep the Particle cloud compiler from building this desktop test
// Desktop test for calendar.h and sun.h. No Particle, no FastLED.
//   clang++ -std=c++11 -I src test/host_test.cpp -o /tmp/porch_test && /tmp/porch_test
// Expected values come from published calendars / timeanddate.com for Charleston.
#include <cstdio>
#include <cstdlib>
#include "calendar.h"
#include "sun.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const int32_t EST = -5 * 3600;

static void testCivil() {
    Civil c = civilFromEpoch(0);
    CHECK(c.year == 1970 && c.month == 1 && c.day == 1 && c.hour == 0 && c.weekday == 4, "epoch 0");
    CHECK(weekdayOf(2026, 10, 30) == 5, "2026-10-30 should be Friday");
    CHECK(dayOfYear(2026, 12, 31) == 365 && dayOfYear(2024, 12, 31) == 366, "day of year");
    int y, m, d; civilFromDays(daysFromCivil(2026, 10, 30), y, m, d);
    CHECK(y == 2026 && m == 10 && d == 30, "round trip");
}

static void testHolidays() {
    struct { int y, m, d; } easters[] = { {2024,3,31}, {2025,4,20}, {2026,4,5}, {2027,3,28}, {2038,4,25} };
    for (auto &e : easters) { int m, d; easterDate(e.y, m, d); CHECK(m == e.m && d == e.d, "Easter %d got %d/%d", e.y, m, d); }
    CHECK(nthWeekdayOfMonth(2026, 11, 4, 4) == 26, "Thanksgiving 2026");
    CHECK(nthWeekdayOfMonth(2025, 11, 4, 4) == 27, "Thanksgiving 2025");
    CHECK(lastWeekdayOfMonth(2026, 5, 1) == 25, "Memorial Day 2026");
    CHECK(nthWeekdayOfMonth(2026, 9, 1, 1) == 7, "Labor Day 2026");
}

static int64_t utcOf(int y, int m, int d, int h, int mi) { return (int64_t)daysFromCivil(y, m, d) * 86400 + h * 3600 + mi * 60; }

static void testDst() {
    // 2026: DST starts Mar 8 02:00 EST = 07:00 UTC, ends Nov 1 02:00 EDT = 06:00 UTC.
    CHECK(!usDstActive(utcOf(2026, 3, 8, 6, 59), EST), "just before DST start");
    CHECK( usDstActive(utcOf(2026, 3, 8, 7, 0),  EST), "at DST start");
    CHECK( usDstActive(utcOf(2026, 11, 1, 5, 59), EST), "just before DST end");
    CHECK(!usDstActive(utcOf(2026, 11, 1, 6, 0),  EST), "at DST end");
    CHECK( usDstActive(utcOf(2026, 10, 30, 12, 0), EST), "Oct 30 2026 is EDT");
    CHECK(!usDstActive(utcOf(2026, 1, 15, 12, 0), EST), "January is EST");
}

static void testSeasons() {
    struct { int y, m, d; Season want; } cases[] = {
        {2026,10,30, SEASON_HALLOWEEN}, {2026,10,1, SEASON_HALLOWEEN}, {2026,11,1, SEASON_HALLOWEEN},
        {2026,11,2, SEASON_THANKSGIVING}, {2026,11,26, SEASON_THANKSGIVING},
        {2026,11,27, SEASON_XMAS}, {2026,12,25, SEASON_XMAS}, {2026,1,6, SEASON_XMAS},
        {2026,1,7, SEASON_DEFAULT}, {2026,8,15, SEASON_DEFAULT}, {2026,2,14, SEASON_DEFAULT},
        {2026,3,17, SEASON_STPATTY}, {2026,3,10, SEASON_STPATTY}, {2026,3,19, SEASON_DEFAULT},
        {2026,3,29, SEASON_EASTER}, {2026,4,5, SEASON_EASTER}, {2026,4,6, SEASON_EASTER}, {2026,4,7, SEASON_DEFAULT},
        {2026,5,22, SEASON_MEMORIAL}, {2026,5,26, SEASON_MEMORIAL}, {2026,5,21, SEASON_DEFAULT},
        {2026,6,27, SEASON_JULY4}, {2026,7,4, SEASON_JULY4}, {2026,7,11, SEASON_JULY4}, {2026,7,12, SEASON_DEFAULT},
        {2026,9,4, SEASON_LABOR}, {2026,9,8, SEASON_LABOR}, {2026,9,9, SEASON_DEFAULT},
        {2027,3,18, SEASON_STPATTY}, {2027,3,21, SEASON_EASTER}, // 2027 Easter Mar 28: Palm Sunday Mar 21
    };
    for (auto &c : cases) {
        Season got = seasonForDate(c.y, c.m, c.d);
        CHECK(got == c.want, "%04d-%02d-%02d want %s got %s", c.y, c.m, c.d, seasonName(c.want), seasonName(got));
    }
}

static void testSun() {
    const double lat = 32.7765, lon = -79.9311;
    // Approximate published Charleston times (local). Tolerance 8 minutes.
    struct { int y, m, d; bool dst; int rise, set; } cases[] = {
        {2026,10,30, true,  7*60+32, 18*60+29},
        {2026, 6,21, true,  6*60+13, 20*60+30},
        {2026,12,21, false, 7*60+17, 17*60+18},
        {2026, 3, 1, false, 6*60+52, 18*60+16},
    };
    for (auto &c : cases) {
        int rise, set;
        int32_t off = EST + (c.dst ? 3600 : 0);
        bool ok = sunTimesLocalMin(c.y, c.m, c.d, lat, lon, off, ZENITH_OFFICIAL, rise, set);
        printf("  %04d-%02d-%02d rise %02d:%02d set %02d:%02d  (expected ~%02d:%02d / %02d:%02d)\n",
               c.y, c.m, c.d, rise/60, rise%60, set/60, set%60, c.rise/60, c.rise%60, c.set/60, c.set%60);
        CHECK(ok, "sun event exists");
        CHECK(abs(rise - c.rise) <= 8, "sunrise %04d-%02d-%02d off by %d min", c.y, c.m, c.d, rise - c.rise);
        CHECK(abs(set - c.set) <= 8,   "sunset %04d-%02d-%02d off by %d min",  c.y, c.m, c.d, set - c.set);
    }
}

// Print a year of season boundaries so a human can eyeball the calendar.
static void printYear(int y) {
    printf("\nSeason calendar for %d:\n", y);
    Season prev = SEASON_COUNT;
    for (int32_t D = daysFromCivil(y, 1, 1); D <= daysFromCivil(y, 12, 31); D++) {
        int yy, m, d; civilFromDays(D, yy, m, d);
        Season s = seasonForDate(yy, m, d);
        if (s != prev) { printf("  %02d/%02d  %s\n", m, d, seasonName(s)); prev = s; }
    }
}

int main() {
    testCivil(); testHolidays(); testDst(); testSeasons();
    printf("Charleston sun times:\n"); testSun();
    printYear(2026); printYear(2027);
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
#endif // PARTICLE
