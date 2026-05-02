# Procyon — rescue WiFi + KV-driven cred install

Recovery flow for a device that can't reach its target WiFi network.
The operator at the install site runs an internet-tethered phone
hotspot named `procyon` with passphrase `horology` (WPA2). The device
joins it as a last-resort credential, fetches new primary creds via
Stra2us KV, persists them, and switches to the target network when
the operator drops the hotspot.

Designed for **remote installs** where physical USB-cable recovery is
not viable: no Particle account, no app, no captive portal, no
operator-side software beyond a phone with hotspot capability.

## Why fleet-shared, not per-device

The procyon SSID/passphrase are compiled into every fleet binary and
recoverable from any flashed device. That's intentional, not a leak:

- Physical access is already root-equivalent (USB cable).
- The point isn't authn against attackers — it's a knowable shared
  channel for recovery.
- Operator carries one phone configuration that works for any device
  in the fleet. No per-device secret to look up.
- Rotate by reflashing the fleet if/when needed.

## Operator workflow

1. **Before shipping** the device, set its target network's credentials
   in Stra2us KV:

   ```
   stra2us set <device> wifi_ssid <target-ssid>
   stra2us set <device> wifi_password <target-password>
   ```

   Both keys are `ops_only` strings, scope `[app, device]`. App-scope
   fallback exists but device-scope is the typical case.

2. **At the install site**, fire up an internet-tethered phone hotspot
   with the rescue parameters:

   - SSID: `procyon`
   - Password: `horology`
   - Security: WPA2-Personal
   - The phone needs internet so the device can reach Stra2us through
     the hotspot.

3. **Power up the device.** Boot logs:
   - `[procyon] NVS empty; using compiled-in target` (first boot post-
     flash) or `[procyon] loaded target ssid="..." from NVS`
   - WiFi cycler tries the target first; after `WIFI_PHASE_BUDGET_MS`
     (15s) without association, swaps to procyon.

4. **Watch for the visual signal.** When the device pulls fresh creds
   from Stra2us while joined to procyon, an animated **blue chase**
   sweeps the bottom row of the panel for ~10 seconds. That's the
   "you can drop the hotspot now" cue.

   The visual is gated on `(currently_on_procyon AND apply_just_fired)`.
   If the device is already on the target network and pulls a creds
   update, the chase doesn't fire (nothing to drop).

5. **Stop the procyon hotspot.** The device disassociates, the cycler
   returns to the target slot, and within ~15s the device joins the
   target network with the freshly-installed creds.

## Cloud-visible state — heartbeat fields

While the rescue flow is in progress, the heartbeat surfaces it
loudly via `net=<ssid>`:

```
... script=thyme@9d2a15b5 net=procyon bri=(1<4<64 sched) ...
```

After recovery completes:

```
... script=thyme@9d2a15b5 net=YourHomeWifi bri=(1<3<128 sched) ...
```

Tail the device's heartbeat stream and look at `net=` to know which
network the device is currently associated with. This works for
remote diagnostics — the device is online (via procyon), so heartbeats
publish normally; the field shows the rescue state.

## Architecture

### Particle (Photon, Photon 2, Argon)

WiFi credentials live in DCT (Device Configuration Table). DeviceOS
manages the cycle through stored credentials internally.

- **At boot**, `register_procyon_credential_()` adds procyon to DCT
  if not already present. Idempotent — skips the write when SSID is
  already there. Compiled-in `PROCYON_SSID`/`PROCYON_PASSPHRASE`.
- **KV-driven cred install**: `telemetry_cycle()` reads
  `wifi_ssid`/`wifi_password` from Stra2us KV via the
  `Stra2usClient::wifi_ssid()`/`wifi_password()` accessors. FNV-1a
  32-bit hash over `(ssid + NUL + password)` deduplicates re-applies;
  only call `WiFi.setCredentials` when the hash changes.
- **DCT-full handling** (rare): if `setCredentials` returns false
  (DCT slots exhausted after multiple re-primings), nuke via
  `clearCredentials()` and re-install only procyon + the new target.
  Loses any other primary networks the device had accumulated.
  See TODO entry "DCT-full handling — nuke-and-restore" for the
  reasoning.

### ESP32

No DCT — credential management is implemented in firmware over NVS
via the Arduino `Preferences` library.

- **Storage**: two NVS keys in the `critterchron` namespace:
  `wifi_ssid` and `wifi_pw`. Loaded at boot into RAM buffers
  (`g_target_ssid_` / `g_target_pw_`); on first boot or NVS-empty,
  defaults to the compiled-in `WIFI_SSID`/`WIFI_PASSWORD`.
- **Cycler**: 2-slot manual state machine swapping between target
  and procyon. `wifi_cycle_step_()` is called from each main `loop()`
  iteration; no-op when `WiFi.status() == WL_CONNECTED`. When the
  current slot's budget (`WIFI_PHASE_BUDGET_MS`) elapses without
  association, it disconnects and tries the other slot.
- **KV-apply path**: same hash dedup as Particle. On hash change:
  persist new pair to NVS, update RAM buffers, force-restart the
  cycler at the new target via `wifi_cycle_restart_target_()` so the
  device tries the new creds immediately rather than waiting for the
  current slot's budget to elapse.
- **Single-target slot semantics**: ESP32 only ever stores one target
  at a time. No history of past networks; no DCT-full equivalent.
  Re-primings overwrite. That's a deliberate scope cut from the
  Particle 5-10-slot model — most devices stay on one home network
  for life.

### Cross-platform parity matrix

| Feature                              | Particle | ESP32 |
|--------------------------------------|----------|-------|
| Procyon registered at boot           | ✓        | ✓     |
| KV-driven cred install + dedup       | ✓        | ✓     |
| `net=<ssid>` heartbeat field         | ✓        | ✓     |
| Blue-chaser visual on rescue+apply   | ✓        | ✓     |
| DCT-full handling                    | ✓        | n/a   |

Open follow-ups (see TODO entry "Procyon rescue WiFi"): target
visibility scan (`target=visible`/`missing` for remote diagnostics),
encryption of `wifi_password` in transit.

## Wire format — Stra2us KV keys

Stra2us KV keys (under app `critterchron`):

| Key                                       | Value                                    |
|-------------------------------------------|------------------------------------------|
| `critterchron/<device>/wifi_ssid`         | Target network SSID (string)             |
| `critterchron/<device>/wifi_password`     | Target network password (string)         |
| `critterchron/wifi_ssid`                  | Fleet-wide fallback (uncommon)           |
| `critterchron/wifi_password`              | Fleet-wide fallback (uncommon)           |

App-scope fallback exists for symmetry but is rarely useful — wifi
creds are inherently per-device.

Both keys are `ops_only` in the catalog: device-side reads go through
dedicated `Stra2usClient::wifi_ssid()`/`wifi_password()` accessors
populated by `poll_all()`'s string-fallback block, not the cache-based
`get_int`/`get_float` path.

## Failure modes

- **Operator typos a credential.** Device joins procyon → applies bad
  creds → tries them after disassociation → fails → falls back to
  procyon. Loop continues until operator corrects the KV value;
  self-healing once corrected, no operator-side intervention needed.

- **KV pull succeeds but `setCredentials` (Particle) / NVS save (ESP32)
  fails.** Errlog entry into the heartbeat err channel
  (`err=other:wifi_creds setCred fail` or `err=net:wifi_creds NVS save
  failed`); next heartbeat retries. Don't fire blue-chaser signal.

- **Procyon hotspot dropped before apply completes.** Apply is fast
  (~one heartbeat cycle), so this is rare. Worst case device drops
  back to its old credentials and waits for the next time procyon
  comes up. No corruption, no stuck state.

- **DCT/NVS slot exhaustion** (Particle only). Triggers nuke-and-
  restore. See architecture notes above.

## Threat model & limitations

`wifi_password` ships **plaintext** over the Stra2us HTTP layer. Anyone
with passive sniff capability on the procyon network during the install
pull can read it. Accepted v1 tradeoff:

- Physical proximity to the procyon hotspot is already required
  (someone runs that hotspot intentionally).
- Procyon-mode use is rare and operator-supervised.
- Stra2us responses are HMAC-signed (authenticity protected; only
  confidentiality is at risk).

A confidentiality investigation (HMAC-derived stream cipher or
AES-128-CTR) is filed as a separate sub-task in the procyon TODO
entry. Not v1 work.

## Testing

To exercise the full lifecycle on a single device:

1. `stra2us set <device> wifi_ssid <something_visible_but_wrong>`
   (a real SSID near the device, but with the wrong password)
2. Reboot the device. Watch it associate with the wrong creds, fail,
   fall to procyon (assuming you have your phone hotspot up).
3. `stra2us set <device> wifi_password <correct_password>`
4. Within one heartbeat cycle: device pulls, applies, blue chase
   fires for 10s, force-reconnect runs, joins target network.
5. Drop the procyon hotspot — device stays on target.

For the Particle DCT-full path: use `tools/particle_wifi_prime.ino`
to manually pollute DCT with synthetic SSIDs until 4 of 5 slots are
filled (Photon) or 9 of 10 (Photon 2/Argon), then trigger a real
KV-driven apply. Expect log: `wifi_creds: setCredentials failed; nuke+
restore (procyon + target)`.

## Reference

- TODO entry: "Procyon rescue WiFi + KV-driven cred install"
- Catalog: `critterchron.s2s.yaml` — `wifi_ssid`, `wifi_password`
- Source: `hal/particle/src/critterchron_particle.cpp`
  (`register_procyon_credential_`, `hash_wifi_creds_`),
  `hal/esp32/src/critterchron_esp32.ino`
  (`wifi_cycle_step_`, `wifi_cycle_restart_target_`,
  `load_target_creds_from_nvs_`, `save_target_creds_to_nvs_`)
- Tools: `tools/particle_wifi_prime.ino` (manual nuke-and-prime)
