# TODO

## Near-term

- **Catalog edit UI: prefill input with current value.** When the
  operator clicks to edit a key in the catalog UI, the input box opens
  empty regardless of whether the key already has a value set. Should
  prefill with the existing value (per-scope: device-scope value if
  present, else app-scope, else blank) so an operator tweaking — e.g.
  bumping `heartbeep` from 300 to 600, or appending a new segment to
  `brightness_schedule` — doesn't have to re-type from scratch or
  paste from a separate `stra2us get` invocation.

  Edge case: long string values (`brightness_schedule`,
  `wifi_password`) need an input wide enough to show the full value
  without truncation, or a textarea-style editor for the multi-segment
  schedules.

  Edge case: secrets (`wifi_password`) — the current value should
  prefill but the field should also support "show/hide" toggling so an
  operator doing a quick edit doesn't have the password sitting on
  screen.
