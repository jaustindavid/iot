# Dispatch — split error stream from heartbeep

## Before you start

Read [../AGENTS.md](../AGENTS.md) for project shape, build recipes,
Stra2us conventions, the two-target symmetry rule, comment style, and
handoff conventions. Then read these task-specific files:

- [hal/ErrLog.h](../hal/ErrLog.h) — the producer/consumer contract.
- [hal/particle/src/Stra2usClient.h](../hal/particle/src/Stra2usClient.h)
  — `publish()` shape and tel-thread contract.
- The heartbeat region of both entry-point files (see Step 2 below).

## Context

Critterchron device firmware publishes telemetry heartbeats AND ErrLog
entries on the same Stra2us topic (`critterchron/public/heartbeep`). As
telemetry has grown, the heartbeep stream is overloaded. This task
moves errors to a sibling topic so each stream has one purpose.

Two targets, symmetric edits required (per AGENTS.md):

- `hal/particle/src/critterchron_particle.cpp` — Photon, Photon 2, Argon
- `hal/esp32/src/critterchron_esp32.ino` — ESP32 family

OG Photon (rico_raccoon) is RAM-constrained. This design adds **zero
static/heap/stack** — the heartbeat's existing stack-local `report[384]`
is reused for error publishes after it's dead.

## Step 1 — new topic macro

Add alongside the existing `STRA2US_SNAPSHOT_TOPIC` and
`STRA2US_TRACE_TOPIC` blocks (particle ~L53, esp32 ~L115):

```c
#ifndef STRA2US_ERROR_TOPIC
#define STRA2US_ERROR_TOPIC STRA2US_APP "/public/error"
#endif
```

Singular `error` (not `errors`). Server-side queue is already permitted
for devices.

## Step 2 — remove the `err=` trailer from heartbeats

**In `build_heartbeat_report()`** (both files): the helper itself doesn't
append `err=`, but it reserves an 80-byte tail via the `tail_reserve`
parameter. Cleanest move: drop the parameter entirely since no caller
needs it anymore. If that ripples too far for comfort, leave the
parameter with a default-0 and pass nothing new — the reservation
collapses to 0 and the body has 80B more headroom.

**In `telemetry_cycle()`**: delete the inline error-append block and its
`mark_sent` side effect. Anchor on the call site by grepping for
`peek_oldest_unsent` in each entry-point file — there is one drain
block per file, immediately before the heartbeat `publish()`. Remove:
the `ERR_TAIL_RESERVE` constant, the entire `peek_oldest_unsent` /
`snprintf(" err=...")` block, and the `if (have_err && pub_status ==
200) mark_sent` line that follows the publish.

Heartbeat body is now pure status. Keep `report[384]` at its current
size — we're about to reuse it.

## Step 3 — drain ErrLog to the error topic, after the heartbeat publish

Insert the drain loop **after the trailing `Log.info` / `Serial.printf`
that logs `report` post-publish** and **before** `poll_all()`. Anchor:
search for `publish(STRA2US_TELEMETRY_TOPIC, report)` inside
`telemetry_cycle`; the log line immediately follows. Slotting the
drain before that log line would corrupt the log output because the
drain reuses `report[]` (see next paragraph).

**Reuse the existing `report[]` buffer** — no new allocation. The
buffer is on the tel-thread stack and is dead from the heartbeat's
perspective once the publish and its log line have run.

Particle version:

```c
// Error-stream drain. One publish per pending ErrLog entry on the
// keep-alive socket. Break on non-200 so a transient failure leaves
// the remainder queued for the next cycle (same retry contract as
// the old inline trailer). Ring depth is 4 and tel cadence is
// floor-10s, so peak publish rate is bounded without an explicit
// timer. Buffer is the heartbeat report[], dead after the publish
// above — reuse, no new static.
critterchron::ErrEntry e;
while (critterchron::g_errlog.peek_oldest_unsent(e)) {
    int n = snprintf(report, sizeof(report),
                     "device=%s cat=%s seq=%lu up=%lu wall=%lu msg=%s",
                     DEVICE_NAME,
                     critterchron::err_cat_tag(e.cat),
                     (unsigned long)e.seq,
                     (unsigned long)System.uptime(),
                     (unsigned long)Time.now(),
                     e.msg);
    if (n <= 0 || n >= (int)sizeof(report)) break;
    int s = g_cfg.publish(STRA2US_ERROR_TOPIC, report);
    Log.info("err publish=%d cat=%s seq=%lu",
             s, critterchron::err_cat_tag(e.cat), (unsigned long)e.seq);
    if (s != 200) break;
    critterchron::g_errlog.mark_sent(e.seq);
}
```

ESP32 version is identical in shape; swap:

- `System.uptime()` → `millis() / 1000UL`
- `Time.now()` → `time(nullptr)`
- `Log.info(...)` → `Serial.printf("[err] publish=%d ...\n", ...)`

Match the existing heartbeat builder's idiom in each file for `up=` /
`wall=` exactly — copy, don't re-derive.

## Wire format

`device=<name> cat=<tag> seq=<n> up=<s> wall=<unix> msg=<text>` — k=v
style, matches heartbeep parser conventions. Typical ≤180B; bounded by
ErrLog's 56-byte msg + small fixed fields, well inside `report[384]`.

## Threading / socket invariants

- All publishes stay on the tel thread; `g_errlog.record()` producers
  remain non-blocking.
- The drain reuses the connection from the heartbeat publish — do
  **not** insert `connect()` / `close()` between them, and run it before
  `poll_all()` so the cycle still closes once at the end.
- `peek_oldest_unsent` / `mark_sent` contract and the single mutex in
  `ErrLog.cpp` are unchanged.

## What NOT to change

- Anything outside the scope of this change. If you notice unrelated
  things worth fixing, list them under "Follow-ups" in the closing
  handoff — don't bundle them in.
- ErrLog API, ring depth (4), msg size (56).
- Particle cloud failsafe `Particle.publish("stra2us", …)` — separate
  liveness channel, no errors today, leave it.
- OTA lifecycle publishes (`ota_detected` / `ota_matrix` / `ota_loaded`)
  and the `boot_light` one-shot — all stay on `STRA2US_TELEMETRY_TOPIC`.
  They are not errors.
- Snapshot and trace topics.

## Rate-limit reasoning (so you don't add one)

The drain runs once per tel cycle; tel cadence is floored at 10s. Ring
is 4 deep with overwrite-oldest semantics, so producer rate cannot drive
publish rate above 4 per cycle = 0.4/s peak. At default
`heartbeep=300s` it's ≤4 errors per 5 min. No extra timer needed.

## Verification

See AGENTS.md "Verification expectations" for the general bar. Specific
to this task:

1. **Host build** must pass: `make -C hal host`.
2. **Both target families** must build clean — at minimum one Particle
   device (`make -C hal DEVICE=ricky_raccoon ants`) and one ESP32
   device. Include rico (`DEVICE=rico_raccoon`) — it has `NO_IR_OTA`
   and the tightest budget; removing the trailer is a small stack win,
   no regression expected.
3. **Grep guard:** `grep -rn ' err=' hal/` should show no heartbeat-
   trailer formatters anywhere (only `Log.error` format strings and
   docs).
4. **On-device (best-effort, document what you actually ran):**
   - Force an ErrLog entry — e.g. point a device at a missing script
     to trigger `ErrCat::OtaFetch`:
     `stra2us set <device> ir does-not-exist`
     Within one tel cycle expect: one message on
     `critterchron/public/error` with the k=v fields above; the next
     `critterchron/public/heartbeep` message carries **no** `err=`
     trailer.
   - Black-hole test: brief network drop should leave the entry
     queued; on recovery, next cycle drains it.
   - Burst test: rapidly produce 6 errors. Ring caps at 4; all 4
     publish in one cycle; oldest two are dropped (expected — pre-
     existing ErrLog semantics).

## Out of scope

- Server-side consumer / dashboard panel for the new topic.
- Trimming `ErrCat::Net` reconnect-kick chatter (may want a future pass
  once we see real volume on the dedicated stream).

## Closing handoff

Per AGENTS.md "Dispatch conventions": when done, drop
`dispatch/error-stream-split-handoff.md` next to this brief covering
status, files touched, verification actually run (don't fabricate),
surprises vs. the brief's assumptions, follow-ups, and notes for the
next agent in this area.
