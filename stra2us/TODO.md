# TODO

## Near-term

- ~~**Draft a "Stra2us client implementor's guide" / spec.**~~ Landed
  2026-05-03 as [`docs/client_spec.md`](docs/client_spec.md). Covers
  wire basics, request signing, response verification, msgpack value
  shapes (str/bin parity, absent-key signals, ext family for
  encrypted), HMAC-keystream cipher, Connection: close discipline,
  streaming-fetch chunk-callback pattern, tel-thread sizing, errlog
  surfacing convention, catalog `ops_only`/`encrypted` distinctions,
  drift-test patterns, pointers into the three reference impls, and a
  10-item validation checklist for new clients. Intended to be the
  starting point for any fourth-platform port (nRF52, RP2040, etc.)
  before they have to re-derive things from scattered prose.

  *Original scoping notes (kept for context):*

  Sections worth covering, drawn from what bit us during the existing
  implementations:

  - **Wire basics.** HTTP/1.1, msgpack body format. Endpoints
    (`GET/POST /kv/{key}`, `POST /q/{topic}`, etc.). Required request
    headers (`X-Client-ID`, `X-Timestamp`, `X-Signature`); response
    headers (`X-Response-Timestamp`, `X-Response-Signature`).
  - **Request signing.** HMAC-SHA256 over `URI || body || timestamp`.
    Body is empty bytes for GET. Timestamp is ASCII decimal.
    Signature hex-encoded.
  - **Response verification.** Streaming HMAC update during body read
    (don't require buffering the whole body — relevant for ~1MB OTA
    fetches). Drift window (±300s). Constant-time hex compare. Fail
    closed: a 2xx response without the signing headers MUST be
    rejected.
  - **msgpack value shapes.** str family (`0xa0-0xbf` fixstr, `0xd9`
    str8, `0xda` str16, `0xdb` str32) and bin family (`0xc4` bin8,
    `0xc5` bin16, `0xc6` bin32) — server may emit either for the same
    logical string value, so client must accept both. Numeric types
    for int/float values. The "absent key" signals: nil (`0xc0`) and
    fixmap envelope (`0x80-0x8f`) — both must be handled silently as
    "not found," not as protocol errors. Ext family (`0xd4-0xd8`
    fixext, `0xc7-0xc9` ext8/16/32) for encrypted values
    (per `fr_encrypted_values.md`).
  - **Encryption.** HMAC-keystream cipher: `keystream =
    HMAC-SHA256(secret, "stra2us-kvenc-v1" || nonce_BE || counter)`,
    nonce = response timestamp uint32, counter increments per 32-byte
    block. Marker: msgpack ext type 0x21. Decryption transparently
    feeds the plaintext to the existing str/bin parser; client
    callers don't need to know it was encrypted.
  - **Connection lifecycle.** `Connection: close` honored — server
    FINs the socket after every response; client must close its end
    too or risk reusing a half-closed socket. ESP32-specific: bug
    where `WiFiClient::connected()` doesn't see the FIN until
    propagated; force-close after body read regardless.
  - **Streaming fetches** for large values (~1MB OTA blobs).
    Chunk-callback pattern (see `kv_fetch_stream_` in critterchron's
    `Stra2usClient.cpp`). HMAC over streamed bytes, not buffered.
  - **Error categorization & operator surfacing.** Recommended
    pattern: errlog ring buffer + heartbeat surfacing
    (`err=cat:detail`). Categories observed in critterchron: Net,
    OtaFetch, Boot, Other — useful starting set.
  - **Catalog interplay.** `ops_only`, `encrypted` flags are
    consumer-side hints, not server-enforced (with the exception of
    `encrypted` per-record flag in storage, which IS server-driven —
    distinguish carefully). Drift-test pattern enforcing call-site /
    catalog-entry agreement is recommended for the consumer's CI.
  - **Threading model.** Telemetry runs on its own thread/task,
    network I/O isolated from render path. Stack sizing matters
    (≥8KB for IR-OTA-enabled critterchron tel thread on Particle —
    see `debug_ota_hardfault_stack` memory note).
  - **Worked examples.** Pointers into the three reference
    implementations: `tools/stra2us_cli/client.py` (Python),
    `critterchron/hal/particle/src/Stra2usClient.{h,cpp}` (Particle
    C++), `critterchron/hal/esp32/src/Stra2usClient.{h,cpp}` (ESP32
    C++).

  Worth doing before any *fourth* client implementation. Not urgent
  while just the existing three are in play.

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
