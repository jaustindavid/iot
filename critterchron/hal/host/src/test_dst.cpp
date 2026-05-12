// Tests for hal/Dst.{h,cpp}. Standalone — pure date math, no engine.
// PASS/FAIL idiom mirrors test_snapshot.cpp.
//
// Build & run:
//   make test-dst
//
// Coverage:
//   * nth_dow_of_month_utc — correct date for known anchors
//   * is_us_dst_active — boundary days for 2026 and 2027
//                       (multi-year to catch hardcoded-year regressions)
//   * Spring-forward / fall-back transition moments to the second
//   * dst_adjustment_hours dispatch by rule
//   * Pathological inputs handled

#include "Dst.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

using namespace critterchron::dst;

static int g_pass = 0;
static int g_fail = 0;
static bool g_verbose = false;

static void check(const char* label, bool cond, const std::string& detail = "") {
    if (cond) {
        ++g_pass;
        if (g_verbose) std::printf("  [PASS] %s\n", label);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s%s\n", label,
                    detail.empty() ? "" : ": ", detail.c_str());
    }
}

// Helper: parse "YYYY-MM-DDTHH:MM:SSZ" to UTC time_t. Test convenience.
static time_t parse_utc(const char* s) {
    struct tm t = {};
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) {
        return (time_t)-1;
    }
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    return timegm(&t);
}

// ---- nth_dow_of_month_utc -----------------------------------------

static void test_first_sunday_jan_2026() {
    // 2026-01-04 is the first Sunday of January 2026.
    time_t t = nth_dow_of_month_utc(2026, 1, /*Sun*/0, /*1st*/1, 0);
    time_t expected = parse_utc("2026-01-04T00:00:00Z");
    check("nth_dow: 1st Sun of Jan 2026", t == expected,
          std::string("got=") + std::to_string((long)t) +
          " want=" + std::to_string((long)expected));
}

static void test_second_sunday_march_2026() {
    // 2026-03-08 — second Sunday, US spring-forward.
    time_t t = nth_dow_of_month_utc(2026, 3, 0, 2, 0);
    check("nth_dow: 2nd Sun of Mar 2026",
          t == parse_utc("2026-03-08T00:00:00Z"));
}

static void test_first_sunday_november_2026() {
    // 2026-11-01 — first Sunday, US fall-back.
    time_t t = nth_dow_of_month_utc(2026, 11, 0, 1, 0);
    check("nth_dow: 1st Sun of Nov 2026",
          t == parse_utc("2026-11-01T00:00:00Z"));
}

static void test_last_sunday_october_2026() {
    // 2026-10-25 — last Sunday (used by EU rule, validates -1 path).
    time_t t = nth_dow_of_month_utc(2026, 10, 0, -1, 0);
    check("nth_dow: last Sun of Oct 2026",
          t == parse_utc("2026-10-25T00:00:00Z"));
}

static void test_seconds_into_day() {
    // 1st Sunday of March 2026 at 09:00 UTC = US-Mountain 02:00 standard.
    time_t t = nth_dow_of_month_utc(2026, 3, 0, 1, 9 * 3600);
    check("nth_dow: seconds-into-day applied",
          t == parse_utc("2026-03-01T09:00:00Z"));
}

static void test_pathological_inputs() {
    check("nth_dow: month 0 rejected",
          nth_dow_of_month_utc(2026, 0, 0, 1, 0) == 0);
    check("nth_dow: month 13 rejected",
          nth_dow_of_month_utc(2026, 13, 0, 1, 0) == 0);
    check("nth_dow: dow 7 rejected",
          nth_dow_of_month_utc(2026, 3, 7, 1, 0) == 0);
    check("nth_dow: nth 0 rejected",
          nth_dow_of_month_utc(2026, 3, 0, 0, 0) == 0);
    check("nth_dow: 5th Sun of Feb 2026 (doesn't exist) rejected",
          nth_dow_of_month_utc(2026, 2, 0, 5, 0) == 0);
}

// ---- is_us_dst_active ---------------------------------------------

static void test_us_dst_inactive_jan() {
    // Mid-January, clearly outside DST window.
    time_t t = parse_utc("2026-01-15T12:00:00Z");
    check("is_us_dst: Jan inactive (Mountain)",
          !is_us_dst_active(t, -7.0f));
    check("is_us_dst: Jan inactive (Eastern)",
          !is_us_dst_active(t, -5.0f));
}

static void test_us_dst_active_july() {
    time_t t = parse_utc("2026-07-04T12:00:00Z");
    check("is_us_dst: July active (Mountain)",
          is_us_dst_active(t, -7.0f));
    check("is_us_dst: July active (Eastern)",
          is_us_dst_active(t, -5.0f));
}

static void test_us_dst_inactive_dec() {
    time_t t = parse_utc("2026-12-25T12:00:00Z");
    check("is_us_dst: Dec inactive", !is_us_dst_active(t, -7.0f));
}

static void test_us_dst_spring_forward_boundary_mountain() {
    // 2026 US-Mountain: spring forward at 2nd Sun Mar = 2026-03-08
    // at 02:00 MST = 09:00 UTC.
    time_t one_second_before = parse_utc("2026-03-08T08:59:59Z");
    time_t at_transition     = parse_utc("2026-03-08T09:00:00Z");
    time_t one_second_after  = parse_utc("2026-03-08T09:00:01Z");

    check("is_us_dst: 1s before spring (Mountain): inactive",
          !is_us_dst_active(one_second_before, -7.0f));
    check("is_us_dst: at spring (Mountain): active",
          is_us_dst_active(at_transition, -7.0f));
    check("is_us_dst: 1s after spring (Mountain): active",
          is_us_dst_active(one_second_after, -7.0f));
}

static void test_us_dst_spring_forward_boundary_eastern() {
    // 2026 US-Eastern: spring forward at 2nd Sun Mar = 2026-03-08
    // at 02:00 EST = 07:00 UTC.
    time_t one_second_before = parse_utc("2026-03-08T06:59:59Z");
    time_t at_transition     = parse_utc("2026-03-08T07:00:00Z");

    check("is_us_dst: 1s before spring (Eastern): inactive",
          !is_us_dst_active(one_second_before, -5.0f));
    check("is_us_dst: at spring (Eastern): active",
          is_us_dst_active(at_transition, -5.0f));
}

static void test_us_dst_fall_back_boundary_mountain() {
    // 2026 US-Mountain: fall back at 1st Sun Nov = 2026-11-01
    // at 02:00 MST = 08:00 UTC.
    time_t one_second_before = parse_utc("2026-11-01T07:59:59Z");
    time_t at_transition     = parse_utc("2026-11-01T08:00:00Z");
    time_t one_second_after  = parse_utc("2026-11-01T08:00:01Z");

    check("is_us_dst: 1s before fall (Mountain): active",
          is_us_dst_active(one_second_before, -7.0f));
    check("is_us_dst: at fall (Mountain): inactive",
          !is_us_dst_active(at_transition, -7.0f));
    check("is_us_dst: 1s after fall (Mountain): inactive",
          !is_us_dst_active(one_second_after, -7.0f));
}

static void test_us_dst_year_2027() {
    // Sanity: not hardcoded to 2026. 2027 2nd Sun Mar = 2027-03-14.
    time_t pre  = parse_utc("2027-03-14T08:59:59Z");  // for Mountain
    time_t post = parse_utc("2027-03-14T09:00:00Z");
    check("is_us_dst: 2027 spring (pre) inactive",
          !is_us_dst_active(pre,  -7.0f));
    check("is_us_dst: 2027 spring (post) active",
          is_us_dst_active(post, -7.0f));
}

// ---- dst_adjustment_hours -----------------------------------------

static void test_adjustment_disabled() {
    time_t t = parse_utc("2026-07-04T12:00:00Z");  // mid-DST window
    check("adjustment: Rule::Disabled returns 0 even mid-summer",
          dst_adjustment_hours(Rule::Disabled, t, -7.0f) == 0.0f);
}

static void test_adjustment_us_active() {
    time_t t = parse_utc("2026-07-04T12:00:00Z");
    check("adjustment: US returns 1.0 in summer",
          dst_adjustment_hours(Rule::US, t, -7.0f) == 1.0f);
}

static void test_adjustment_us_inactive() {
    time_t t = parse_utc("2026-01-15T12:00:00Z");
    check("adjustment: US returns 0.0 in winter",
          dst_adjustment_hours(Rule::US, t, -7.0f) == 0.0f);
}

static void test_adjustment_unknown_rule() {
    time_t t = parse_utc("2026-07-04T12:00:00Z");
    // Cast an unknown enum value (future-proofing — operator typo
    // lands here rather than crashing).
    Rule fake = static_cast<Rule>(99);
    check("adjustment: unknown rule treated as 0",
          dst_adjustment_hours(fake, t, -7.0f) == 0.0f);
}

// ---- runner -------------------------------------------------------

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 ||
            std::strcmp(argv[i], "--verbose") == 0) g_verbose = true;
    }

    test_first_sunday_jan_2026();
    test_second_sunday_march_2026();
    test_first_sunday_november_2026();
    test_last_sunday_october_2026();
    test_seconds_into_day();
    test_pathological_inputs();

    test_us_dst_inactive_jan();
    test_us_dst_active_july();
    test_us_dst_inactive_dec();
    test_us_dst_spring_forward_boundary_mountain();
    test_us_dst_spring_forward_boundary_eastern();
    test_us_dst_fall_back_boundary_mountain();
    test_us_dst_year_2027();

    test_adjustment_disabled();
    test_adjustment_us_active();
    test_adjustment_us_inactive();
    test_adjustment_unknown_rule();

    int total = g_pass + g_fail;
    std::printf("\n%d/%d passed", g_pass, total);
    if (g_fail) std::printf(" — %d failed", g_fail);
    std::printf("\n");
    return g_fail ? 1 : 0;
}
