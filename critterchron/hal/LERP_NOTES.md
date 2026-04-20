# Render lerp observations — CritterChron hardware

Working notes from 2026-04-19 development session. Intended as input for a
later writeup; not a finished document.

## Terminology

- **lerp** — linear cross-fade between prev_pos and pos during a physics step.
  `prev_w + pos_w = 1` at every `t`.
- **SYRP** — "sinusoidal rainfall"-style dip, `prev_w = cos(π·t)` on the first
  half, `pos_w = cos(π·(1-t))` on the second. Total weight dips to 0 at
  midpoint. Models a point source behind a perforated screen.
- **co-SYRP** — shallower SYRP, `prev_w = sqrt(cos(π·t))`. Agent dwells near
  full brightness for more of the transit; blackout at t=0.5 is briefer.
  Shipped curve (2026-04-19).

All three implementations preserve the `paint` lambda's agent-over-tile
blend, so painter agents (tortoise) transition agent-color → tile-color
smoothly at prev_pos instead of flashing to black mid-step.

## Hardware context

- Panels are perforated with 5mm light pipes on a 10mm pitch. Roughly 50%
  aperture — each LED is fully visible within its 5mm window, then there's a
  5mm gap before the next cell.
- Low-light ambient is the dominant usage regime (bedroom/evening clocks).
- Brightness is clamped `[MIN_BRIGHTNESS, MAX_BRIGHTNESS]` (per-device) with
  a floor-clamp in `NeoPixelSink` that bumps any nonzero channel to 1 so
  dim pixels don't round to black.

## What each curve does at low light

### Linear lerp + floor-clamp (original)

`prev_w + pos_w = 1` for every `t`. At mid-transit (t=0.5), both cells are
at half weight, which at low max-brightness rounds to 1 after floor-clamp,
and both cells sit lit at equal brightness for the *entire* physics frame.
The display reads as a two-pixel smear; the agent looks like it doubled,
not moved. Worst at low ambient because that's where the floor-clamp is
most visible as "this cell should be dark but isn't."

### SYRP (pure cos dip)

Total weight dips to 0 at midpoint. Floor-clamp has nothing to rescue — the
cell actually goes dark. Fast agents (coati) read as "blinky" at low light:
fully visible at t=0 and t=1, dark in between. This is physically accurate
(the agent is behind the mask between pipes) but the perceptual effect is
closer to a strobe than a glide. Slow agents (thyme) look great — the
brightness dip is long enough to read as motion rather than blink.

### co-SYRP (sqrt(cos) dip)

Same true-zero midpoint, but prev_w stays close to 256 for most of the
transit. At t=0.25 prev_w is 220 (vs. 206 for pure cos) and at t=0.4 it's
still 132 (vs. 95). The blackout is real but brief — looks like "agent
dims, blinks, reappears" instead of "agent fades for half a frame, dark
for one instant, fades back." Fast agents regain readability at low light.
Slow agents still look good because the dip is still there and still
centered at the midpoint.

## Key findings

1. **Linear + floor-clamp is wrong for this hardware at low light.** The
   floor-clamp is right for steady-state dim pixels, but when combined with
   a cross-fade that never goes through zero, it defeats the motion cue
   entirely.
2. **True-zero midpoint is load-bearing.** The panel geometry *makes* the
   agent dark mid-transit. Any curve that doesn't honor that drifts away
   from what the eye expects to see.
3. **Dwell matters more than shape depth.** Both SYRP and co-SYRP hit zero
   at the midpoint; the difference is how much of the transit the agent
   spends near full brightness. co-SYRP's longer dwell-high + briefer
   blackout looks better for fast agents without giving up the physics.
4. **Bright light masks everything.** At high ambient, the floor-clamp
   isn't visible and the dip depth isn't visible; all three curves look
   similar. Optimization is exclusively a low-light win.
5. **Agent-over-tile blend is independent but important.** With the
   overwrite form, painter agents (tortoise) would flash the painted tile
   to black through the dip. With the blend form, prev_pos cleanly
   transitions agent-color → tile-color. Works for all three curves;
   just has to happen.

## Implementation notes

- LUT is a hardcoded `constexpr int16_t cos_q8_lut[129]` in
  `CritterEngine.cpp`. 129 entries cover `[0, π/2]`; the second half of
  the curve reuses the table via `cos_q8_lut[256 - alpha_q8]`. Don't pull
  in libm at runtime — it costs ~5.8KB of flash.
- To regenerate for a different curve: `round(256 * <curve>(π·i/256))` for
  i in [0, 128]. co-SYRP uses `sqrt(cos(…))`; pure SYRP is just `cos(…)`.
- Per-pixel math is Q8 fixed-point: `(agent*w + underneath*(256-w) + 128) >> 8`.
  Chosen for consistency across FPU (P2, Argon) and non-FPU (OG Photon)
  chips. FPU doesn't help this path; the add/mul/shift sequence is integer
  anyway.
- The 50Hz render tick at 20ms gives ample headroom on P2/Argon; OG Photon
  headroom is tighter but still positive at 20ms. If a future feature
  squeezes the budget, drop OG Photon to 40ms via per-device
  `RENDER_TICK_MS` override rather than slowing every device down.

## Testing approach

- Rachel (32×8 P2, CDS sensor): primary test rig across ambient levels
  from hand-covered dark to desk-lit bright.
- Ricky / rico: secondary observation rigs; smaller matrices made some
  artifacts easier to spot in peripheral vision.
- Test programs: coati (fast + painter-adjacent), thyme (slow), tortoise
  (explicit painter — caught the tile-flash-to-black bug).
- Each curve change was observed for at least a few minutes at multiple
  ambient settings before moving on. Side-by-side devices with different
  curves would have been ideal; sequential observation is what we had.

## Open questions / future work

- Does the shallower dip change anything perceptually at the very fastest
  step rates? We didn't push coati much past its default tick rate.
- Is there a "too shallow" point — does cos^0.25 or cos^0.1 eventually
  degenerate back to linear's smear? Would be a useful bound to know.
- Night mode palette (warm-white / red-only below an ambient threshold)
  is still a stretch idea — would interact with the dip curve since it
  shifts how the floor-clamp is perceived.
