# porch-lights

Permanent, fully automatic seasonal LED string on the front porch. Particle
Photon (gen 1) + 150 WS2812 pixels + FastLED. Charleston, SC.

## Context for future Claude

The owner lost chat context once already, so this section is the handoff.

**What it does.** Off during daylight. From 30 minutes before sunset until
sunrise it shows one very slow color wave (palette drifts one lap of the
string every 12.5 minutes). The season only changes which 4-color palette the
wave uses. Season comes from the date, on/off from sunrise/sunset computed
on the device. No manual state, nothing in EEPROM.

**File map.**

| File | What | Depends on |
|---|---|---|
| `src/config.h` | every tunable: pins, lat/lon, lead time, brightness, speed | nothing |
| `src/calendar.h` | epoch to civil date, US DST, Easter, nth-weekday, season windows | nothing (pure C++) |
| `src/sun.h` | sunrise/sunset, USNO almanac algorithm | `calendar.h`, `<math.h>` |
| `src/palettes.h` | one `CRGBPalette16` per season | FastLED |
| `src/porch-lights.ino` | schedule loop, fades, cloud function, watchdog | Particle, FastLED |
| `test/host_test.cpp` | desktop test of the pure headers | clang++ |

**Season windows** (in `seasonForDate`, `calendar.h`):
St. Patrick's Mar 10-18; Easter Palm Sunday to Easter Monday; Memorial Day
Fri-Tue around the holiday; July 4 Jun 27-Jul 11; Labor Day Fri-Tue;
Halloween Oct 1-Nov 1; Thanksgiving Nov 2 to Thanksgiving; Christmas the day
after Thanksgiving through Jan 6; everything else "default" (warm candle
wave). Memorial, July 4 and Labor Day share the red/white/blue palette.

**Decisions worth knowing before changing things.**

- `SYSTEM_THREAD(ENABLED)` is load-bearing. Without it `loop()` stops while
  the Photon reconnects to the cloud, which froze the old sketch.
- Particle `Time` is kept in UTC (`Time.zone(0)`). All local-time and DST
  math is ours, in `calendar.h`, so it also works on an ESP32 later.
- The CdS photoresistor on A1 has never been proven to work. It is only
  consulted when `Time.isValid()` is false (cold boot with no internet).
  Thresholds `CDS_DARK`/`CDS_LIGHT` in `config.h` are guesses from the old
  code.
- Palette changes and brightness changes both ease toward targets, so
  midnight season rollovers and dusk/dawn are gentle fades.
- `ApplicationWatchdog` resets the device if `loop()` stalls for 60 s.
- The Photon 1 has no FPU. Sun math is double precision in software and
  runs every 10 s, never per frame. Keep it that way.
- Cloud function `command` is for testing only and auto-expires after
  `OVERRIDE_MINUTES`: `season <0..8>`, `on`, `auto`.

**What the previous sketch had that was removed on purpose:** a "motion"
state (drift / fast 4-dot chaser / static), a 0-5 brightness state in EEPROM
with level 5 as a forced-on override, per-season paint functions, fireworks
and spark particle effects, a per-pixel "dumpster fire" flicker, a hardcoded
UTC-4, and a fixed 17:00-01:00 on window. If the chaser or fire effects are
ever wanted again, the owner has the old code.

## Build and flash

Particle Workbench or CLI, target `photon`:

```bash
particle compile photon . --saveTo porch-lights.bin
```

```bash
particle flash <device-name> porch-lights.bin
```

## Test the date and sun math on a Mac

```bash
clang++ -std=c++11 -I src test/host_test.cpp -o /tmp/porch_test && /tmp/porch_test
```

Prints Charleston sun times for a few dates, the full season calendar for
2026 and 2027, and `ALL PASSED`.

## Test on the device

Watch it think:

```bash
particle get <device-name> status
```

Returns a line like `2026-10-30 19:12 EDT | rise 07:36 set 18:31 | night | cds 812`.

Preview a palette for 10 minutes (indices: 0 default, 1 stpatty, 2 easter,
3 memorial, 4 july4, 5 labor, 6 halloween, 7 thanksgiving, 8 xmas):

```bash
particle call <device-name> command "season 8"
```

Force on during daylight for 10 minutes:

```bash
particle call <device-name> command on
```

### Does the CdS work?

Read `cds` a few times across a day and a night:

```bash
particle get <device-name> cds
```

If noon and midnight readings differ by several hundred counts, the sensor
works and the `CDS_DARK`/`CDS_LIGHT` thresholds can be set between them.
If the value barely moves, the divider is miswired or the sensor is dead,
and it does not matter much since it is only a cold-boot fallback.
