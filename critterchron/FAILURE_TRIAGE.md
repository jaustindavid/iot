# Failure-triage instrumentation

Design for catching the rare, hard-to-reproduce agent failures in
critterchron's deployed fleet, using the new staging system as the
proving ground.

## The problem

The failures we care about — agents that get stuck, tiles that
never converge, slow-creep memory leaks, mysterious watchdog resets —
share a profile that defeats traditional debugging:

- **Rare** — minutes to days between occurrences, not seconds.
- **Hard to reproduce** — non-deterministic, depends on accumulated
  state, sensor drift, network conditions, agent-position interactions.
- **Often invisible to current metrics** — the heartbeat captures
  aggregate counts and timings, but a stuck-tile bug with no metric
  delta walks past unnoticed.

The traditional "add a print, reflash, wait" loop has poor return
because the failure may not happen for hours — and when it does, the
print might be in the wrong place.

This document proposes five complementary instrumentation angles
designed to either *catch* the failure as it happens (so we have full
state to look at) or *make it reproducible* (so we can debug at
leisure).  They compose; some are small (a Python script polling the
queue) and some are larger (engine refactor for determinism).

## Architectural framing: external analyzer over standard feeds

A key affordance of Stra2us: queue-tail and KV-read are part of the
public client API. Anything reachable by `stra2us` from the operator
side is reachable by an automated process running anywhere — laptop,
tiny VM, Raspberry Pi in a closet. **No server-side changes are
required to layer analysis on top of the existing telemetry.** The
analyzer presented in §3 is just a Python service that authenticates
the same way operators do.

This shapes the design:

- All five angles are additive — none changes the existing protocol or
  forces a server-side deploy.
- The same analyzer can monitor staging *and* prod streams, with
  different alert thresholds for each.
- Headless staging devices can run alongside production devices on the
  same Stra2us instance, separated only by topic prefix
  (`<app>/staging/...`) and ACL.

## Angle 1: On-device ring buffer + anomaly-triggered snapshot dump

A small RAM-resident ring buffer captures the last N seconds of
detailed engine state on every device. When a *trigger* fires — a
metric crosses a threshold, an explicit assertion fails, or an
operator manually flips a KV flag — the device dumps the buffer to a
dedicated cloud topic. Operators replay the snapshot offline to see
what the device was doing leading up to the failure.

### What's in a frame

One frame per physics tick (or per N ticks, configurable). Each
frame carries the *delta* from the previous frame plus a few
absolutes:

```
{
  "tick": 12345,
  "millis": 9876543,
  "agents": [
    {"id": 0, "name": "ant", "pos": [12, 8], "state": "wander", "pc": 7},
    ...
  ],
  "markers": {"trail": [{"pos": [10, 5], "count": 12}, ...]},
  "heap_free": 85432,
  "seeks_fail_delta": 0,
  "recent_log": "ir_apply: loaded swarm@8cd2777d"
}
```

Frame size ~200–800 bytes depending on agent count. At 8Hz × 32
frames = 4 seconds of history in ~16KB of RAM. Tunable via a
`snapshot_buffer_frames` KV knob.

### Triggers

A trigger evaluator runs each tick and decides whether to *dump* the
ring. Initial set:

| Trigger | Condition |
|---|---|
| `seeks_fail_spike` | seeks_fail counter advances by >5 in a single second |
| `agent_count_drop` | live agent count drops by >50% inter-tick (excluding intentional despawns) |
| `heap_low` | heap_free below per-device threshold |
| `assertion` | engine internal `assert` (we don't have many today; they get added with this work) |
| `unconverged_long` | `extra` or `missing` count stays nonzero for >5 minutes |
| `manual` | operator sets `dump_now: 1` — useful for "just send me what you've got, I'll look" |

### Wire format

Snapshot publish to `q:<app>/staging/snapshots` (or `prod/snapshots`):

```
{
  "device": "rachel_raccoon",
  "timestamp": 1777952751,
  "trigger": "seeks_fail_spike",
  "trigger_detail": "delta=8 in tick 12345",
  "frames": [<frame>, <frame>, ...]
}
```

### Success criteria

- ✅ Manual-trigger path works: setting KV `dump_now: 1` on a staging
  device causes a snapshot to land on the cloud topic within one
  heartbeat cycle.
- ✅ Snapshot is parseable msgpack offline; contents include ≥30
  consecutive frames with monotonic ticks; agent state across frames
  reconstructs a continuous timeline.
- ✅ Auto-trigger fires within 1 second of `seeks_fail` advancing past
  the threshold (tested by spawning a deliberately A*-failing target).
- ✅ RAM cost <8KB measured against pre-change firmware; no
  measurable render-rate regression (50fps → 50fps).
- ✅ `dump_now` clears itself after a successful publish so it's
  edge-triggered, not level-triggered.

### Tradeoffs

- *Ring fills only with "bad" events* — needs reasonable trigger
  thresholds. Too sensitive → constant snapshots, signal lost in
  noise. Too lax → never catches anything. Tune in staging first.
- *Triggers we don't yet know about* — the bug class we can't catch
  with this design is "the heartbeat says everything's fine but the
  panel is visibly wrong." See "where the gap is" below.

## Angle 2: KV-driven trace mode for hands-on debugging

A `trace_mode: 1` KV knob makes the device emit *every* physics tick
as a frame to a per-device trace topic. Off by default everywhere;
flipped on for a single device when you want to watch closely. Same
frame shape as §1, just streaming continuously instead of buffered.

### Wire format

Topic: `q:<app>/staging/trace/<device>` (per-device so analyzers can
tail individual streams without filtering out 4 other devices).
Payload: same frame format as §1.

### Bandwidth budget

- Frame ~500 bytes typical
- Physics tick at 8 Hz (default `RUNTIME_TICK_MS=125`) → ~4 KB/s/device
- Real-world with msgpack overhead and small messages: ~6–8 KB/s
- Acceptable for ≤2 simultaneous staging devices on a single Stra2us
  instance. Beyond that, throttle the publish rate (every Nth tick)
  or rely on §1 instead.

### Success criteria

- ✅ Flipping `trace_mode: 1` on a staging device starts the trace
  stream within one heartbeat cycle (≤30s typical).
- ✅ Trace frames arrive at the cloud topic at ≥7Hz (allowing
  occasional drops).
- ✅ Flipping `trace_mode: 0` stops the stream within one heartbeat
  cycle; no leftover frames after flip.
- ✅ A 60-second trace can be captured and post-processed offline
  to produce a per-tick timeline of all live agents.
- ✅ At `trace_mode: 1`, render rate stays at the device's normal
  budget (no rendering regression from the publish path).

### Tradeoffs

- *Bandwidth-heavy*: this is the single most expensive thing in this
  design. Budget for staging only; do not enable in production.
- *Easy to forget on*: a device left at `trace_mode: 1` indefinitely
  will spam its topic forever. Auto-disable after N hours? KV-driven
  with no expiry is the simplest design today; an `expire_at`
  timestamp is a future tightening.

## Angle 3: External analyzer client (soft client)

A Python service that polls staging and prod heartbeat queues,
maintains rolling per-device statistics, detects outliers, and
emits alerts. Pure soft client: authenticates via `stra2us_cli`,
no server-side code.

This is the *first* thing to build because:

- No firmware changes required — works against the existing fleet today.
- Establishes the alerting/triggering pipeline that the other angles
  feed into and consume.
- Can monitor production immediately, not just staging.

### Architecture

```
┌─────────────┐     ┌──────────────┐     ┌─────────────────┐
│  Stra2us    │ ←── │   Analyzer   │ ──→ │  Alert sinks    │
│  q: heartbeep│     │              │     │  (log/Slack/email)│
│  q: snapshots│ ←── │  - polls     │     └─────────────────┘
└─────────────┘     │  - aggregates│
                    │  - detects   │     ┌─────────────────┐
                    │  - alerts    │ ──→ │  KV: dump_now   │
                    │              │     │  trace_mode     │
                    └──────────────┘     └─────────────────┘
                       │                  (closes the loop:
                       ↓                   alert triggers
                  ┌────────────┐           on-device action)
                  │ Local DB   │
                  │ (sqlite or │
                  │  parquet)  │
                  └────────────┘
```

### Detection methods

Three classes of detector, increasing in sophistication:

1. **Threshold rules** — `if heap_free < 5000: alert`. Static
   per-metric. Catches obvious things; needs hand-tuning per device
   class.
2. **Robust outliers (median + MAD)** — within a fleet of similar
   devices, flag any whose metric is >K MAD from the fleet median.
   Catches "one device is acting weird" without per-device tuning.
3. **Per-device temporal outliers** — model each device's metric as a
   rolling distribution; flag deviations from its own history.
   Catches "this device's seeks_fail used to be 0–2 per minute, now
   it's 50 per minute, even though the fleet hasn't changed."

Start with (1) and (2); (3) is a later refinement once we know which
metrics are worth temporally tracking.

### Alert payload

```
{
  "device": "rachel_raccoon",
  "metric": "seeks_fail",
  "rule": "fleet_outlier_mad",
  "value": 47,
  "fleet_median": 2,
  "fleet_mad": 1,
  "deviation_mads": 22.5,
  "first_seen": "2026-05-05T18:32:00Z",
  "context": "<last 5 heartbeats from this device>",
  "suggested_action": "set dump_now=1 on this device",
}
```

### Closing the loop

When an alert fires, the analyzer can *automatically* set
`dump_now: 1` on the affected device (or `trace_mode: 1` for sustained
issues). This turns the analyzer into the trigger source for §1 and
§2 — no human in the loop for the common "weird metric → grab a
snapshot" path.

### Use beyond staging

The user explicitly noted: this analyzer should also watch production.
Lower alert thresholds for prod (don't get paged every time a device's
WiFi blips), but the same machinery applies. Production alerting can
be passive (a daily summary of fleet health) or active (page when
multiple devices fail simultaneously, suggesting a server problem).

### Success criteria

- ✅ Service runs continuously for ≥7 days against staging without
  auth/restart issues.
- ✅ Within 5 minutes of seeing ≥3 staging devices, computes
  per-metric medians and MADs.
- ✅ Detects an injected anomaly (manually crash a device, hold
  seeks_fail at 50/min, drop heap below floor) within ≤2 heartbeat
  cycles.
- ✅ False-positive rate <1 alert/device/day in steady-state staging.
- ✅ Auto-triggered `dump_now` correctly causes a snapshot to land
  for the alerted device (proving §1 + §3 closed loop).
- ✅ Configurable via a single YAML file: which queues to tail,
  metric thresholds, alert sinks, MAD multipliers per metric.
- ✅ Storage: persists alerts + last N hours of metrics to a local
  database (sqlite is fine) so post-hoc "when did this start?"
  queries work.

### Tradeoffs

- *No protection against analyzer downtime* — if the analyzer dies
  while a device is misbehaving, we miss the window to set
  `dump_now`. Mitigation: §1's manual-trigger path and §1's
  auto-trigger thresholds remain available without the analyzer.
  Belt-and-suspenders.
- *Metric blind spots* — see "where the gap is" below; this is the
  fundamental tradeoff of metric-driven alerting.

## Angle 4: Differential staging fleet

Run 3–5 headless devices on staging with identical scripts and KV
configuration. Their metrics should cluster tightly under normal
operation; outliers are bugs.

### Hardware

ESP32-C3 dev boards work fine without LEDs attached — the engine
runs unchanged, FastLEDSink just emits to nothing. Five boards at
~$5 each, USB-powered, sitting on a shelf with WiFi. Provision once,
flash with the latest staging firmware, leave them running.

### Why this works

Critterchron failures are typically *device-specific* — accumulated
memory pressure, peculiar A* call sequences, sensor calibration
drift. The fleet median provides a built-in baseline:

- All 5 devices show seeks_fail ~2/min → fleet is healthy.
- Device 3 shows seeks_fail = 50/min, others still at 2 → device 3 has
  the bug, fleet median holds the baseline. Snapshot device 3.
- All 5 devices show seeks_fail = 50/min → likely a server-side or
  script-level change. Different remediation path.

### Determinism — yes or no?

**Don't make the fleet bit-for-bit deterministic.** RNG should still
differ per device. If we did force determinism, the fleet would
diverge only when there's a non-determinism bug — useful but narrow.
Letting RNG differ exposes a wider class: any bug whose probability
is amplified by a particular state sequence becomes visible as one
device hitting it more often.

(The exception is §5's reproducer harness, which *does* want
determinism. Different goal, different mechanism.)

### Success criteria

- ✅ ≥3 headless staging devices running for ≥48h continuously.
- ✅ Their per-metric distributions cluster within ±2 MADs of each
  other in steady state (no outliers ≥4 MADs).
- ✅ Introducing a deliberate bug on one device — e.g., flashing it
  with a slightly different IR script — causes the analyzer to flag
  that device specifically within ≤10 minutes.
- ✅ Easy reset: a single command can re-flash the fleet to current
  staging firmware and restart their telemetry baselines.

### Tradeoffs

- *Hardware overhead* — five extra boards to provision, monitor, and
  not lose track of. Modest cost; one bad sensor on a sibling device
  could mask its outlier-detection contribution.
- *Doesn't catch fleet-wide bugs* — if all 5 devices have the bug
  identically (e.g., a regression in shared engine code), the fleet
  median is wrong-but-consistent and no individual outlier is flagged.
  Pair with §3's static thresholds (which don't depend on fleet
  comparison) to cover this.

## Angle 5: Reproducer harness — record and replay

The deepest debug primitive: capture a device's seed + tick-by-tick
input stream into a `.replay` file, ship to staging, replay on the
host harness bit-for-bit. Once you can reproduce, you can step through
in a debugger.

### Required: engine determinism

The engine must produce identical state for identical (seed, IR,
input stream). Today it probably doesn't, because:

- `esp_random()` / `random()` calls scattered through agent behavior
  pull from device-state-dependent RNG.
- `millis()` is consumed in some agent decisions.
- Sensor reads (light sensor, A* heuristic occasionally) inject
  device-specific values.
- Any uninitialized memory in agent struct allocations.

The work here is to route every such non-deterministic source through
an injectable interface — `RngSource`, `TimeSource` (we have this for
WobblyTime), `SensorSource`. In recording mode the source captures
each value; in replay mode the source returns recorded values
verbatim.

This is the heaviest lift in the document. Don't start until §1–§4
have proven the metric-driven approach has gaps that only deep replay
can fill.

### File format

```
header: { magic, version, device_id, ir_sha, start_seed, start_tick }
inputs: [
  { tick: N, kind: "rng", values: [<u32>, <u32>, ...] },
  { tick: N, kind: "time", millis: <u32> },
  { tick: N, kind: "sensor", source: "light", value: 4029 },
  { tick: N, kind: "kv", key: "max_brightness", value: 64 },
  ...
]
```

Stream is append-only on the device (RAM ring → ship on snapshot
trigger), then a host harness consumes it and re-runs the engine with
the recorded inputs as the only nondeterminism source.

### Capture path

Each input source's recorder writes a `(tick, kind, value)` record to
a ring buffer. On a §1-style trigger (or KV `record_dump: 1`), the
ring is shipped via the snapshot topic — but with replay-format
content instead of human-readable frames.

### Success criteria

- ✅ Engine refactored to take all RNG/time/sensor sources as
  injected dependencies.
- ✅ `make test-determinism` passes: a recorded session, replayed,
  produces identical engine state at each tick.
- ✅ `make replay file=<bug.replay>` on the host harness loads a
  replay file and runs to completion without divergence.
- ✅ A real captured failure (snapshot from prod, converted to
  replay) reproduces the bug on the host. This is the "we can debug
  prod failures from a laptop" milestone.
- ✅ Deliberately introducing a determinism break (e.g., calling
  `rand()` directly bypassing the source) is detected by
  test-determinism, not silently masked.

### Tradeoffs

- *Substantial engine surgery* — touching every nondeterminism source
  is risky for current functionality. Should only be undertaken with
  good test coverage.
- *Replay file size* — at 8 Hz, with ~5 RNG calls per tick + sensor +
  occasional KV, ~50 bytes/tick → ~25KB for a 60-second window. Fits
  in the snapshot path easily.
- *Heap-corruption-class bugs won't replay* — if the bug is in a
  third-party lib's heap behavior, the replay sees the same inputs
  but the resulting heap state diverges, and the bug doesn't
  reproduce. This is a real limitation; no instrumentation can fully
  fix it. Reduces to "static analysis + careful code review" for
  that class.

## Suggested phasing

The angles compose, and their prerequisites form a natural ordering:

| Phase | Angle | Why this order |
|-------|-------|----------------|
| 1     | §3 analyzer | No firmware needed; immediately useful; sets up the trigger pipeline. |
| 2     | §1 ring buffer + dump | Once analyzer is alerting, dumps answer "what was happening." Cheap firmware change. |
| 3     | §4 headless fleet | Hardware provisioning is independent of firmware; pairs with §3 for differential analysis. |
| 4     | §2 trace mode | Useful for hands-on investigation once §1/§3 has localized to a specific device. |
| 5     | §5 reproducer | Heaviest lift; only undertake if §1–§4 surface a class of bug that needs deeper investigation than metrics+snapshots can provide. |

Phases 1–3 are all "small" by reasonable standards (each ≤2 weeks of
focused work) and self-contained. Phase 5 is the only multi-month
commitment; defer until proven necessary.

## Cross-cutting concerns

### Topic / ACL hygiene

- Staging traffic stays under `<app>/staging/...` topics; production
  under `<app>/public/...`. The analyzer reads both; alerting rules
  differ by source.
- New per-device topics (`<app>/staging/trace/<device>`) need ACL
  grants. Pattern: every staging device gets `<app>/staging/*:rw`.
- Snapshot retention server-side: 7 days for staging, 24 hours for
  prod (snapshots may include device-specific state worth not
  hoarding).

### Privacy / secret leakage

Snapshots and traces could capture sensitive state: encrypted KV
values shouldn't be dumped in plaintext just because the device has
them. Audit the frame format before deploying — don't include
`wifi_password` or other encrypted KV values in the dump.

### Where the gap is

This whole design is metric-driven. Failures whose footprint never
touches a metric — silent visual artifacts, audio glitches in a
hypothetical future audio path, anything we don't yet capture in the
heartbeat — won't be caught by §3's outlier detection or §1's
auto-triggers.

**Use the analyzer's record of "we didn't catch this" as the signal
to add new metrics.** When a known failure is reported (operator
emails "rachel was stuck for 2 hours") and the analyzer's history
shows nothing flagged in that window, that's evidence that we need
new heartbeat fields covering whatever subsystem misbehaved. Over
time, this loop naturally extends the metric coverage to where
failures actually live.

The corollary: don't try to design exhaustive metrics up-front.
Start with what the heartbeat already has, build §3 to catch what's
catchable, and let real failures dictate the next instrumentation
target.

## Reference

- `STRA2US_CATALOG_FR.md` — catalog conventions for the new
  `trace_mode`, `dump_now`, `snapshot_buffer_frames` knobs.
- `PUBLIC_NAMESPACE.md` — staging vs prod topic separation.
- `client_spec.md` (in stra2us repo) — soft-client primer; the
  analyzer is just a particularly busy soft client.
- `debug_*.md` memory notes — historical record of the failure
  classes this design is trying to catch faster.

## Revision history

- 2026-05-05 — Initial draft.
