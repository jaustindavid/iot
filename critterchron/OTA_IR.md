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

| Key                              | Value                              |
|----------------------------------|------------------------------------|
| `critterchron/scripts/<name>`    | Text blob (format below)           |
| `critterchron/<device>/ir`       | String — script name, e.g. `thyme` |

A device polls its pointer key each cycle. On change (or on cold boot if a
pointer is set), it fetches the named blob, validates, and swaps.

No pointer → run compiled-in default. Pointer set but blob missing → keep
running current IR, log `ir_fetch_fail=<name>` in the cloud heartbeat.

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

## Upload tool

```
python3 tools/publish_ir.py agents/thyme.crit [--name thyme]
                                              [--server https://…]
                                              [--dry-run]
                                              [--force]
```

Behavior:

1. Read creds from env: `STRA2US_URL`, `STRA2US_CLIENT_ID`, `STRA2US_SECRET_HEX`.
2. Compute `src_sha256` over the .crit file contents.
3. `GET /kv/critterchron/scripts/<name>` — if present, parse its metadata
   block; if `src_sha256` matches, exit no-op (unless `--force`).
4. Compile .crit → IR dict (reuse `CritCompiler`).
5. Serialize to wire format (new module: `hal/ir/ir_text.py`).
6. `PUT /kv/critterchron/scripts/<name>` with the blob as the msgpack
   string value.
7. Print a short summary: size, sha, key, server.

The shared auth/sign code currently in `stra2us/backend/test_client.py`
moves into a small client library (`tools/s2s_client.py`) so publish and
future CLIs don't duplicate it.

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

## Validation strategy

Round-trip test (pure Python): for every script in `agents/`, compile →
encode → parse → re-encode → compare. Byte-exact on the second encode.

Device-side smoke: flash ricky with `coati` baked in as `DEFAULT_IR_BLOB`.
Publish `thyme` via the upload tool. Set
`critterchron/ricky_raccoon/ir = thyme`. Watch:

- Stra2us heartbeat `s2s=200` and a log line `ir_swap: coati -> thyme`.
- `interp_overruns` counter stays zero at default tick rate.
- Revert the pointer, watch it swap back to the default.

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
3. **Upload tool + shared client lib.** `tools/publish_ir.py` +
   `tools/s2s_client.py`. Usable against today's server immediately.
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
9. **End-to-end test** per "Validation strategy" above.

Steps 1–3 are pure Python and ship the upload tool against today's server
before any firmware changes.
