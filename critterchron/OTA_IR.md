# OTA IR — design notes

Phase 5 decouples the agent script from the firmware image. A compiled .crit
ships as a text blob in Stra2us KV; devices fetch, validate, and load it at
runtime. A full firmware reflash is no longer required to change what an
agent does.

This doc covers the wire format, metadata, loader behavior, the upload tool,
and how we'll watch interpreter cost as scripts grow. Format v1 — bump
`CRIT` version on any incompatible change.

## Why text

The C++ engine is already a small VM. It interprets instruction text
(`seek flag_1 timeout 500`, `if on flag_1:`, `done`) at runtime, maintains a
per-agent PC, walks indentation + `done` as implicit branches, schedules
yields. What we call "IR" is really a structured bundle of that instruction
text plus the scenery (colors, landmarks, agent types, spawn rules).

Since the payload is already strings, encoding them as bytes just interposes
a pack/unpack step with no semantic payoff. The wire format is line-oriented
plain text:

- `curl | less` on a KV value shows exactly what a device is running.
- Encoder / loader are both cursor-through-a-buffer, no byte packing.
- Truncation or wrong-kind value is visible by eye.
- Parser null-terminates in place and points runtime structs at the original
  buffer — no copies, no allocator churn.

Cost: ~2× size vs binary (decimal ints only, instruction text is identical
either way). At our scale that's a few KB — well within any budget.

## Goals

- **Grid-agnostic IR.** Grid dimensions live in the device header (creds.h)
  and are injected at engine init, not baked into the IR. One blob, every
  device that speaks the opcodes.
- **Self-describing.** Magic + version + checksum so a device rejects a
  truncated or wrong-kind value before it tries to execute it.
- **Fail-safe.** A missing, stale, or garbage blob never bricks a running
  device. The compiled-in default IR is the floor.
- **Upload-side idempotent.** Re-publishing an unchanged script is a no-op.
- **Single format.** The compiled-in default IR uses the same wire format,
  hex-escaped into a C header — same parser, no forked code path.

## Key layout

Stra2us KV shape (under app `critterchron`):

| Key                                 | Value                                             |
|-------------------------------------|---------------------------------------------------|
| `critterchron/scripts/<name>`       | Text blob (format below). Multi-KB.               |
| `critterchron/scripts/<name>/sha`   | `<64-hex content_sha>:<decimal size_bytes>`. Sidecar. |
| `critterchron/<device>/ir`          | String — script name, e.g. `thyme`                |

A device polls its pointer key on its own timer (default 1200s, decoupled
from the heartbeat cadence so a stalled OTA fetch can't silence telemetry).
On change (or cold boot with a pointer set), it fetches the sidecar first
(~100B), compares against the loaded sha, and only pulls the blob if they
differ. The blob embeds its own `content_sha` (implicit — recomputed on
the device); the device verifies recomputed-sha == sidecar-sha before
applying, which catches torn uploads (sidecar updated but blob stale). The
size suffix on the sidecar lets the device refuse an oversized blob before
the fetch even starts, closing the crash path from an OTA that would
overrun `IR_OTA_BUFFER_BYTES`.

**Legacy sidecar format** (pre-2026-04-22): bare 64-char hex sha, no size
suffix. Both the device and `set_ir_pointer.py` accept either format —
rolling a fleet through the update is graceful, though devices on old
firmware will fall back to fetching the full blob every poll cycle (since
their strict `sha_len == 64` gate rejects the extended format) until
reflashed.

No pointer → run compiled-in default. Pointer set but blob missing → keep
running current IR, log `ir_fetch_fail=<name>` in the cloud heartbeat.

### Why the sidecar

The obvious design is to fetch the blob and compare its embedded sha. That
works, but it pulls multi-KB every poll cycle just to discover "no change"
99% of the time. The sidecar is a 64-byte fetch that answers the same
question. Upload order is **blob first, sidecar second** — if the publish
process dies between writes, the sidecar still points at the previous sha,
devices see no change, and no stale blob gets applied. The reverse tear
(sidecar updated but blob stale) is caught by the blob-sha verification on
the device side.

## Wire format

UTF-8, LF-terminated lines. Leading indentation on record lines is
cosmetic — the parser treats any whitespace run as a separator.

```
CRIT 1
name thyme
src_sha256 a1b2c3...
encoded_at 2026-04-19T22:00:00Z
encoder_version 1
ir_version 1
---
TICK 50

COLORS 3
static white 255 255 255
static red 255 0 0
cycle heartbeat 3 1 5 2 3 0 5

LANDMARKS 2
flag_1 red 1
  5 5
clock_bar white 4
  0 0
  1 0
  1 1
  0 1

AGENTS 1
coati 1 3
  idle
  wander
  seek

INITIAL 1
coati 0 0 idle

SPAWNS 0

PF_TOP 1 1 1.4

PF 0

BEHAVIORS 1
coati 3
  0 seek flag_1 timeout 500
  0 if on flag_1:
  1 done

END 1f4a
```

### Header

Everything from `CRIT 1` through the `---` separator is the metadata block.
One key per line, first token is the key, rest of line is the value (so
timestamps with colons pass through cleanly).

| key               | required | notes                                         |
|-------------------|----------|-----------------------------------------------|
| `name`            | yes      | Script name, matches pointer value.           |
| `src_sha256`      | yes      | Hex SHA-256 of the .crit source. Upload tool uses this to skip no-op reposts. |
| `encoded_at`      | yes      | ISO-8601 UTC timestamp at encode time.        |
| `encoder_version` | yes      | Bumps when encoder semantics change but `CRIT` version doesn't. |
| `ir_version`      | yes      | From `ir["ir_version"]` — existing compiler field. |

Devices MAY read `name` as a sanity check against the pointer. Other fields
are debug/ops; the loader doesn't depend on them.

### Sections

Each section starts with `<KIND> <COUNT>` on a line of its own, followed by
`<COUNT>` records. Records that contain sub-records (landmark points, agent
states, behavior instructions) declare their own count in the header line.

| section     | header line                    | record line                                      |
|-------------|--------------------------------|--------------------------------------------------|
| `TICK`      | `TICK <ms>`                    | none — single-value section                      |
| `COLORS`    | `COLORS <n>`                   | `static <name> <r> <g> <b>` or `cycle <name> <m> <cidx> <frames> …` (m pairs) |
| `LANDMARKS` | `LANDMARKS <n>`                | `<name> <color> <pt_count>` then `<pt_count>` `<x> <y>` lines |
| `AGENTS`    | `AGENTS <n>`                   | `<name> <limit> <state_count>` then `<state_count>` state name lines |
| `INITIAL`   | `INITIAL <n>`                  | `<agent_name> <x> <y> <state>`                   |
| `SPAWNS`    | `SPAWNS <n>`                   | `<agent_type> <landmark> <interval> <cond_rest_of_line>` |
| `PF_TOP`    | `PF_TOP <diag> <has_cost> <cost>` | none                                          |
| `PF`        | `PF <n>`                       | `<agent> <max_nodes> <pen_lit> <pen_occ> <diag> <diag_cost> <step_rate> <has_flags_bitfield>` |
| `BEHAVIORS` | `BEHAVIORS <n>`                | `<agent> <insn_count>` then `<insn_count>` `<indent> <text_rest_of_line>` lines |

Free-form trailing fields (spawn conditions, instruction text) consume the
rest of the line up to `\n` so embedded spaces are fine. The compiler
already rejects instructions containing literal newlines, so no quoting is
needed.

Unknown section kinds are skipped via their `<count>` — reserves room to add
sections without bumping `CRIT` version.

### Terminator

```
END <hex16>
```

`<hex16>` is a 16-bit Fletcher checksum over every byte from the first byte
of `CRIT 1` through the LF before `END`. Catches truncation, flipped bytes,
mid-transmission corruption. Stra2us response signing already protects the
channel; this protects the blob-at-rest in KV and detects upload mistakes.

## Validation

Loader rejects (returns to previous IR, logs) on any of:

- First 6 bytes ≠ `"CRIT 1"`
- `END` line missing or checksum mismatch
- Any section declares a count that would read past `END`
- Required metadata key missing
- Required section missing (at least: `TICK`, `COLORS`, `AGENTS`, `BEHAVIORS`)
- Cross-reference fails: color / landmark / agent name not in its section

## Device-side loader

Load sequence on boot or on pointer change:

1. Read `<device>/ir` pointer. Empty → use compiled-in default, done.
2. Fetch `/kv/critterchron/scripts/<name>`. 4xx / verify-fail → log, keep
   current IR.
3. Copy blob bytes into an owned buffer (large enough for blob + sentinels).
4. Validate per above. Fail → free buffer, log, keep current IR.
5. Parse: scan the buffer, null-terminate tokens in place by flipping
   separators to `\0`, populate runtime IR struct arrays with `const char*`
   pointers into the buffer.
6. Stop engine, swap IR pointer, re-init engine (agent table, RNG seed),
   resume.
7. Free the previous IR buffer after re-init returns cleanly.

The buffer lives exactly as long as the loaded IR — struct pointers are
always valid as long as the runtime IR is.

Swap is a soft reset of engine state, not a device reset. Rescue-hold on
crash-type reset is unchanged.

## Lifecycle publishes

An OTA swap produces three events on the app stream (topic
`critterchron`), in order:

| Event          | Fired at                                     | Payload (text)                                                 |
|----------------|----------------------------------------------|----------------------------------------------------------------|
| `ota_detected` | Sidecar fetched, new sha confirmed, about to pull blob | `ota_detected from=<old_name>@<sha8> to=<new_name>@<sha8> size=<bytes> up=<uptime_s>` |
| `ota_matrix`   | Pending blob staged, device entering "matrix loading" visual indicator | `ota_matrix name=<new_name>@<sha8> up=<uptime_s>`       |
| `ota_loaded`   | `critter_ir::load` + `engine.reinit` returned; new script is live | `ota_loaded name=<new_name>@<sha8> up=<uptime_s>`         |

A complete trio on the stream (detected → matrix → loaded, same
`@<sha8>` across matrix/loaded) confirms the swap landed. Missing
`matrix` after `detected` means the blob fetch or parse failed —
check serial for the breadcrumbs below. Missing `loaded` after
`matrix` means `engine.reinit()` rejected the IR (version gate, etc.),
and the device is still running the previous script.

`detected` is published from the telemetry worker main loop, not
from inside `ir_poll()`. An earlier version fired it inline between
the sidecar GET and the blob GET on the shared keep-alive TCP socket
and deadlocked the telemetry thread — see TODO.md completed entry
2026-04-22. `ir_poll()` now snapshots the from/to identity into
`Stra2usClient` member buffers and flips `ir_detected_flag_`; the
worker loop reads the flag on its next iteration, publishes, and
clears.

### Serial breadcrumbs

Each phase of `ir_poll()` emits a `Log.info` line so a serial trace
names the last phase reached if the device hangs:

```
ir_poll: OTA candidate <name> sidecar=<sha8> (sized) (loaded=<name>@<sha8>)
ir_poll: fetching blob critterchron/scripts/<name>
ir_poll: blob in (<N> bytes); computing content_sha
ir_poll: content_sha=<sha8>...; cross-checking
ir_poll: staged <name> (<N> bytes, sha=<sha8>...)
ir_apply: bytes=<N> preview="CRIT 1|name <name>"
ir_apply: loaded <name> (sha=<sha8>...) colors=... tick=...ms
```

The gap between `staged` and `ir_apply` reflects the matrix-loading
delay (a few seconds by design — lets the operator see the swap
coming on the grid before it takes effect).

All log format strings are ASCII — `test_hal_serial_ascii.py`
enforces this. The serial console doesn't decode UTF-8, so a stray
`…` or smart quote renders as `���` and obscures the diagnostic.

## Compiled-in default

Same wire format, embedded in firmware:

```c++
// Auto-generated by hal/ir/ir_encoder.py
#pragma once
#include <cstdint>
#include <stddef.h>
constexpr uint8_t DEFAULT_IR_BLOB[] = {
    0x43, 0x52, 0x49, 0x54, 0x20, 0x31, 0x0a,   // "CRIT 1\n"
    // ...
};
constexpr size_t DEFAULT_IR_BLOB_LEN = sizeof(DEFAULT_IR_BLOB);
```

On boot with no OTA pointer, the loader runs the exact same parse /
validate / materialize path on `DEFAULT_IR_BLOB`. Loader bugs surface on
every boot, not only when someone pushes OTA.

Grid dims come from `creds.h` (or `-D` on host builds) and are passed into
engine init — they're no longer part of the IR.

## Tools

Two CLIs sit over Stra2us KV: one publishes blobs, one points devices at
them. Both call `stra2us_cli.client_from_env()` for auth — same
resolution path the `stra2us` CLI uses interactively (env vars or a
`~/.stra2us` profile). Neither tool accepts `--server` / `--client-id`
/ `--secret` flags any more; configure those upstream of the call.

### publish_ir

```
python3 tools/publish_ir.py agents/thyme.crit [--name thyme]
                                              [--dry-run]
                                              [--force]
                                              [--source]
```

By default the published blob omits the SOURCE trailer — devices never
read it at runtime, and including it roughly doubles blob size, which
regularly pushes scripts past `IR_OTA_BUFFER_BYTES` on buffer-tight
targets (rico in particular). Pass `--source` when you want the
human-readable trailer on the KV blob for inspection via `stra2us get`.

Behavior:

1. Resolve auth via `stra2us_cli.client_from_env()`.
2. Compile .crit → IR dict, serialize to wire format via `hal/ir/ir_text.py`.
3. Compute the sidecar value `<content_sha>:<size_bytes>` we'd write.
4. `GET critterchron/scripts/<name>/sha` — if it byte-matches the value
   from step 3, exit no-op (unless `--force`). Sidecar comparison is
   cheap (single small value, no full-blob fetch) and sufficient: the
   sidecar IS the content_sha, and tear-safe ordering (step 5 then 6)
   means a matching sidecar implies a matching blob.
5. `PUT critterchron/scripts/<name>` — the blob (bytes).
6. `PUT critterchron/scripts/<name>/sha` — the sidecar (string).
7. Print a short summary: size, sha, key.

Step 5 before 6 is intentional: a mid-publish crash leaves the sidecar
pointing at the previous sha and devices see no change (see "Why the
sidecar" above).

### set_ir_pointer

```
python3 tools/set_ir_pointer.py <device> <name> [--clear] [--force]
```

Writes `critterchron/<device>/ir = <name>` (or empty, with `--clear`).

Before writing, it verifies the target exists by content: the sidecar value
must be a 64-char hex string, or the blob value must start with `CRIT `.
Presence-only checks failed open against typo'd names (some missing keys
come back as empty strings, not nulls); content validation catches them.
`--force` skips the check.

`--clear` writes an empty string rather than DELETE'ing the key (the server
has no DELETE). The device sees len==0 on poll and keeps the currently-loaded
script; a reboot then comes up on the compiled-in default.

## Interpreter cost — monitoring

Today's diag rollup tracks physics and render cost:

```
phys=8(avg=804us max=1410us) rend=48(avg=7358us max=9957us) agents=9 …
```

`phys` currently bundles two very different workloads: pathfinding (A*
seeks, the historically expensive bit) and agent interpretation (walking
each agent's program counter, dispatching opcodes, resolving branches).
Today interpretation is cheap, which is why we haven't split them. OTA IR
makes it easy to ship scripts with thousands of instructions — we need a
metric that goes up *before* the tick budget is breached.

**Plan (implement alongside Phase 5 loader):**

1. Split `phys` into `phys` (world state: movement, collisions, A*) and
   `interp` (program-counter dispatch per agent). Both reported in the diag
   rollup and the Stra2us heartbeat payload, with per-second avg and max.
2. Declare a notional budget: `interp_budget_us` per tick. Starting point:
   25% of a tick at 50 Hz = 5000 µs. Live-tunable via the existing Config
   surface (`get_int("interp_budget_us", 5000)`).
3. When per-tick `interp > interp_budget_us`, log a warning line and
   increment an `interp_overruns` counter visible in telemetry. No throttling
   on the first breach — we want to see the pattern, not mask it.
4. Emit interp-per-agent-type counts periodically so a runaway script is
   attributable to the agent generating the instructions, not just the
   script as a whole.

The same split helps attribute *seeks* and *deadlocks* precisely: a
deadlock that's actually a parse-loop stall shows up as an `interp` spike,
not a `phys` spike. Separating the two metrics is what lets us tell which
subsystem is in trouble.

If interp ever approaches its budget in the field, we flatten the
interpreter: pre-resolve branches (`if on X:` → `if_false_goto <pc>`) at
encode time, opcode-dispatch table instead of string match, etc. That's
strictly an optimization; v1 keeps string-level interpretation for
debuggability.

## Validation

Round-trip test (pure Python): for every script in `agents/`, compile →
encode → parse → re-encode → compare. Byte-exact on the second encode.

## End-to-end walkthrough

Steps from a clean bench to a script swap observed on a live device.

### 1. Flash

Build and flash the current firmware. The compiled-in `DEFAULT_IR_BLOB` is
whatever script the Makefile was pointed at (typically `tortoise.crit`) —
the device will run this until an OTA pointer says otherwise.

```
make -C hal/particle flash DEVICE=ricky_raccoon
```

Confirm heartbeats show the expected bootup:

```
up=12 rssi=… fw=Apr 20 2026 04:48:59 script=default …
```

`script=default` means no OTA pointer is set (or the pointer is empty) and
the device is running the baked-in blob. Any other value means it already
has a pointer and will re-fetch as needed.

### 2. Publish

Push the script you want to run to Stra2us. Idempotent — re-runs with no
source changes are a no-op.

```
python3 tools/publish_ir.py agents/ants.crit
  script:    agents/ants.crit
  name:      ants
  key:       critterchron/scripts/ants
  size:      4812 bytes
  src_sha:   a1b2c3…
  ir_ver:    1
  published: http://…/kv/critterchron/scripts/ants
  sidecar:   http://…/kv/critterchron/scripts/ants/sha
```

Re-running immediately:

```
  up-to-date: remote src_sha matches (use --force to re-upload)
```

### 3. Point

Point the device at the published script.

```
python3 tools/set_ir_pointer.py ricky_raccoon ants
  verified: sidecar critterchron/scripts/ants/sha sha=a1b2c3d4…
  set: http://…/kv/critterchron/ricky_raccoon/ir → ants
```

The `verified:` line confirms `set_ir_pointer` reached the sidecar before
writing. A typo'd name would have aborted with `error: 'antz' not found…`.

### 4. Observe the swap

On the device's next `ir_poll` cycle (≤20 min by default, configurable via
`ir_poll_interval_s`), the device fetches the sidecar, sees a sha mismatch
against `ir_loaded_sha_`, pulls the blob, verifies, and calls
`engine.reinit()`. The subsequent heartbeat reflects the new script:

```
up=1432 … script=ants@a1b2c3d4 bri=(5<14<128) phys=(… agents=18 …
```

`script=<name>@<sha8>` is the content-addressed identity — `name` alone
would miss re-publishes under the same name. Watch for the sha portion to
update every time you push a new version of the same script.

### 5. Revert

Point the device back at a different script, or clear the pointer:

```
python3 tools/set_ir_pointer.py ricky_raccoon tortoise
# or
python3 tools/set_ir_pointer.py ricky_raccoon --clear
```

`--clear` keeps the currently-loaded script live (no DELETE on the server,
empty value = skip), and the next reboot comes up on `DEFAULT_IR_BLOB`.

### What to watch for

- `interp_overruns` stays zero at default tick rate — a nonzero value means
  the new script is too expensive for the budget (see "Interpreter cost").
- Heartbeats continue during/after the swap. `ir_poll` runs on a separate
  timer from heartbeat publish so a stalled fetch can't silence telemetry.
- `script=` field in the heartbeat flips within one poll cycle of the
  pointer change. Sha portion flips on re-publish of the same name.

## Out of scope for v1

- Hot-swap without soft reset.
- Blob compression (payloads are tiny).
- Signed blobs independent of transport — Stra2us response signing already
  covers it; separate signing is a deferred belt-and-suspenders.
- Rollback-on-crash. Rescue-hold gives 60s to push a pointer revert;
  automatic last-good-IR tracking is Phase 5.1 territory.
- Multi-script preloading. One IR at a time.
- Flattened-jump interpreter. Added only if `interp` metric forces it.

## Implementation order

1. **Text serializer** (Python). `hal/ir/ir_text.py` — `encode(ir_dict,
   meta) -> str` and `decode(text) -> (ir_dict, meta)`.
2. **Round-trip test.** Encode every script in `agents/`; decode; verify.
3. **Upload tool.** `tools/publish_ir.py` (originally with a local
   `tools/s2s_client.py` for HTTP/auth; deleted 2026-04-27 when
   `stra2us_cli` shipped a Python client surface usable directly via
   `from stra2us_cli import client_from_env`).
4. **Default blob generation.** `ir_encoder.py` emits `DEFAULT_IR_BLOB[]`
   (hex-escaped text) instead of the constexpr-struct namespace.
5. **C++ loader.** `Stra2usIRLoader` — parse buffer, materialize runtime
   IR structs, same shape the engine already expects.
6. **Engine wiring.** Engine takes a loaded IR at init; the compile-time
   `critter_ir::` namespace goes away entirely.
7. **`interp` metric split.** Separate interp from phys in the diag and
   heartbeat paths, add overrun counter, wire to Config budget knob.
8. **Pointer polling + swap.** Stra2usClient watches `<device>/ir`,
   triggers reload on change.
9. **End-to-end walkthrough** — flash → publish → point → swap → revert,
   per the section above. Verified on ricky (Photon 2) across multiple
   script flips.

Steps 1–3 are pure Python and ship the upload tool against today's server
before any firmware changes.
