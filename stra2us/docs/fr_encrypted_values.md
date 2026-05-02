# FR: Per-Key Encrypted KV Values

*Filed 2026-05-02 while scoping confidentiality for critterchron's
procyon rescue WiFi flow.*

## Problem

Stra2us authenticates traffic in both directions (HMAC-SHA256 over
`URI + Body + Timestamp` for requests, response signatures via the
mechanism in `fr_response_signing.md`). Authenticity and integrity
are covered: a passive observer can't forge a value or swap a queue
message under either direction.

**Confidentiality is not.** Response bodies are plaintext msgpack;
the spec explicitly opts out of TLS (`docs/spec.md:8`) to save MCU
resources. For low-stakes tunables (heartbeep cadence, render
budgets, rescue-mode visual thresholds) this is tolerable — there is
nothing to keep secret about a rendering parameter.

The gap becomes meaningful when the KV value is **operator-supplied
secret material** rather than a tunable knob. The motivating example
is critterchron's `wifi_password` key, used by the procyon rescue
flow (see `critterchron/PROCYON.md`):

1. Operator at install site sets `wifi_password` in Stra2us so the
   target device can self-install its primary network credentials
   the first time it joins procyon.
2. Device on procyon (an internet-tethered phone hotspot) fetches
   the value via plaintext HTTP.
3. Anyone with passive sniff capability on the procyon network
   during that fetch reads the wifi_password in cleartext.

The threat is bounded — physical proximity to the procyon hotspot is
required, and procyon-mode use is rare and operator-supervised — but
"don't operate the rescue flow on networks where you wouldn't trust a
passive sniffer" is operational discipline that's easy to forget.

A symmetrically-applied solution (encrypt every KV value) is
unattractive: the dozens of non-sensitive knobs in a typical app
catalog (e.g., critterchron's `heartbeep`, `max_brightness`,
`brightness_schedule`) materially benefit from `curl | less`
debuggability of their wire form, which we leaned on heavily during
testing of OTA IR, firmware OTA, and procyon. We want a per-key
opt-in.

## Proposal

Per-record `encrypted` flag in Stra2us storage. When set, the GET
handler encrypts the value before responding using a stream cipher
keyed by the requesting device's existing shared secret. Wire-format
marker on the response so the client knows when to decrypt.

### Wire format

Two changes to the response body for encrypted values:

1. **Marker.** Wrap the encrypted payload in a msgpack `ext` type
   (e.g., type code `0x21`) so a generic msgpack reader can detect
   "this is an encrypted critterchron value" before attempting to
   parse it as a string. The wrapped payload itself is the raw
   ciphertext bytes; the original msgpack-shape (str8/16/32, bin8/
   16/32) is **not** preserved across the wire — the client knows
   the type from the catalog or context.

   Alternative considered: a fixed prefix byte (e.g., `0xFF`) before
   the existing msgpack str/bin payload. Rejected because it
   collides with valid msgpack negative-fixint encoding (`0xFF =
   -1`), making generic decoders ambiguous.

2. **Nonce.** Reuse the existing `X-Response-Timestamp` header
   (already present per the response-signing FR) as the per-call
   nonce. No new header. The server SHOULD ensure timestamp
   monotonicity within a (device, record) pair so the client can
   detect replays — but this is already a property of the response-
   signing scheme.

### Cipher

HMAC-SHA256 stream cipher (NOT bare XOR with the secret):

```
keystream = HMAC-SHA256(secret, label || nonce || counter)
            // counter increments per 32-byte block until keystream
            // length >= plaintext length
ciphertext = plaintext XOR keystream
```

- `secret` is the per-client 32-byte shared secret already used for
  HMAC request/response signing. **No new key material.**
- `label` is a fixed ASCII string (e.g., `"stra2us-kvenc-v1"`) to
  domain-separate this keystream from any other HMAC use of the
  same secret.
- `nonce` is the response timestamp (uint32 BE, 4 bytes).
- `counter` is a uint8 starting at 0; each subsequent HMAC call
  increments it. For values longer than 32 bytes (e.g., a 63-byte
  WPA2 password), two HMAC calls produce 64 bytes of keystream.

**Why HMAC-keystream, not bare XOR with the secret:** bare XOR is
trivially broken by known-plaintext attacks. WiFi passwords have
known structure (ASCII, length conventions); recovering even one
ciphertext means recovering the secret directly via XOR cancellation.
The HMAC layer's one-way property means a known-plaintext attack
recovers only the keystream — which is itself an HMAC output and
cannot be inverted to the secret.

**Why not AES-128-CTR:** mbedTLS-based AES is a real dependency
addition on both Particle and ESP32. HMAC-SHA256 is already in the
codebase (used for both request signing and response signing), so
HMAC-keystream is "free" in code surface. The marginal security
improvement of AES over HMAC-keystream against the actual threat
model (passive sniffer, no MITM because of response signing) is
small enough that the dep cost wins.

### Storage schema

Each Redis-backed KV record gains an `encrypted: bool` field
(default false). The flag is set by the writer at `set` time and
honored by the GET handler.

### CLI

`stra2us set` gains an `--encrypted` flag:

```
stra2us set <device> wifi_password <pw> --encrypted
```

When set, the CLI marks the record's encrypted flag in the write.
Idempotent: setting `--encrypted` on a record that's already
encrypted leaves it encrypted; setting *without* `--encrypted` on a
previously-encrypted record demotes it to plaintext (which is a
sensible "I changed my mind" semantic). A future `stra2us encrypt
<key>` / `stra2us decrypt <key>` could lift this to a value-
preserving in-place flip if needed.

`stra2us list` should indicate encrypted records visibly (e.g., a
🔒 prefix or `[encrypted]` annotation in the value column) so an
operator scanning the catalog state knows what's confidential.

`stra2us get` of an encrypted record from a CLI session (which
authenticates as the same client_id as the device) returns the
decrypted plaintext, since the CLI holds the same secret. This
preserves operational debuggability: an operator with the right
credentials can still see what's there.

### Catalog hint (consumer-side, e.g., critterchron)

Apps that consume the catalog can declare which keys are expected to
be encrypted via an `encrypted: true` field in the YAML. Stra2us
itself does not enforce this — the per-record flag is what governs
wire behavior — but consumer drift tests can verify "every catalog
entry marked `encrypted: true` is actually stored that way" and
"keys whose names match `password|secret|key` are marked
encrypted." See `critterchron/STRA2US_CATALOG_FR.md` for the
consumer-side spec.

## Work estimate

Server (this repo):

- Schema migration: add `encrypted` field to KV record model
- GET handler: detect flag, compute keystream, XOR, wrap in ext
  type
- Tests for both encrypted-roundtrip and the plaintext fallthrough

CLI:

- `--encrypted` flag on `set`
- `list` formatting to surface encrypted records

Total estimate: ~150 LOC + tests. One sitting if Stra2us internals
are straightforward; two if the Redis schema migration needs care.

## Related gaps surfaced during scoping

- **Catalog-server linkage.** Today the per-app catalog YAML lives
  in the consuming repo (e.g., `critterchron/critterchron.s2s.yaml`)
  and Stra2us has no knowledge of it. An "encrypted" field in the
  catalog is advisory. The per-record flag in Stra2us is what
  actually controls wire behavior. This is the same architectural
  layering as `ops_only` (catalog hint, not server-enforced).
- **Forgot-to-mark-sensitive risk.** Mitigated on the consumer side
  by drift-test name-pattern lints (`password|secret|key` →
  must-be-encrypted). Not Stra2us's problem to enforce, but worth
  flagging here so we don't accidentally bake a bad default into the
  protocol.
- **Encrypted queues.** Out of scope here — this FR is KV-only. If
  encrypted queue messages become useful later, the same wire
  marker + cipher could extend; the GET handler's logic generalizes
  cleanly. File separately when needed.

## What this FR is *not* proposing

- **TLS.** Still opted out per the original spec. This FR adds
  per-key confidentiality without a transport-layer dep.
- **Bulk encryption of every KV value.** Per-key opt-in, by design.
  Operational ergonomics (curl/less debuggability of non-sensitive
  values) is genuinely useful and worth preserving.
- **Authenticated encryption.** The response-signing FR already
  provides authenticity over the entire response body, including
  encrypted-value responses. We don't need a separate AEAD construct.
- **Asymmetric crypto.** Per-client shared secrets are already the
  authn primitive; this FR uses the same. No new key distribution
  story.
- **Forward secrecy.** A device's secret being compromised should be
  treated as a full client compromise — encrypted historical values
  retained on the wire (e.g., in operator captures) become readable.
  This is the same threat model as the existing request/response
  signing.
