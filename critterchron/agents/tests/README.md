# `agents/tests/` — compile-time test cases

A catalogue of `.crit` scripts exercising compiler behavior. Each file is an
executable contract: "running `compiler.py` on this file should produce <this
outcome>."

## Filename convention

The first underscore-delimited token in the filename encodes the expected
outcome:

| Prefix    | Meaning                                                             |
|-----------|---------------------------------------------------------------------|
| `ok_`     | Should compile cleanly. No `ValueError`.                            |
| anything else | Should be **rejected** by the compiler. The prefix names the failure category. |

Rejection-category prefixes currently in use / reserved:

| Prefix              | Failure mode                                                           |
|---------------------|------------------------------------------------------------------------|
| `no_yield_`         | A path reaches `done`/`despawn`(bare) without any yield instruction.   |
| `fall_off_`         | Top-level path falls off the bottom of the behavior block.            |
| `unknown_name_`     | `if on X` / `seek X` / `if X` references a name that isn't a landmark, agent, or tile predicate. |
| `unknown_instruction_` | Instruction line doesn't match any known opcode shape (e.g. `state = X` missing the leading `set`). |
| `despawn_landmark_` | `despawn <name>` where `<name>` is a landmark (can't despawn a landmark). |
| `standing_on_name_` | `if standing on <name>` where `<name>` isn't a tile predicate.        |
| `diagonal_`         | `pathfinding` section violates the both-or-neither rule for `diagonal` + `diagonal_cost`. |
| `missing_color_`    | Agent or initial-agent declared without a color mapping.               |

Extend this table when you add a new category. Keep the prefix short and
snake-case.

## Running the tests

(Harness TBD.) The intended contract:

```
# pseudocode
for file in agents/tests/*.crit:
    if file.name.startswith("ok_"):
        compile(file)  # must succeed
    else:
        category = file.name.split("_", 1)[0]  # e.g. "no" from "no_yield_..."
        # first token isn't enough; use the full prefix up to the last underscore before the free-form name
        expect_failure(file, category)
```

A practical implementation should match the prefix against the table above —
`no_yield_foo.crit` looks up `no_yield` — and optionally assert that the raised
`ValueError` message contains a stable token (e.g. `"never yields"` for
`no_yield_*`).

## Authoring notes

- Keep each test **minimal**: exercise exactly one failure mode. If a file
  tickles two problems, split it.
- Prefer the smallest grid and agent cast that triggers the bug.
- Add a comment at the top of the file explaining what the test is proving.
- When the compiler gains a new check, add an `ok_` test that would previously
  have passed but now exercises a benign edge of the rule, in addition to the
  `<category>_` test that triggers the new rejection.

## Current inventory

- `no_yield.crit` — original broken thyme (ladybug "seek, seek, seek, done"
  path with no intervening yield). Exercises the yield-required check.
