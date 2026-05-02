// particle_wifi_prime.ino — paste-and-flash WiFi credential nuke-and-repave.
//
// USAGE:
//   1. Edit NEW_SSID / NEW_PASSWORD below.
//   2. Open https://build.particle.io, paste this whole file in.
//   3. Pick the target device (currently on a network you DO want to
//      replace, since this clears all saved credentials), flash via
//      cloud OTA. The device runs this code, wipes DCT, then writes
//      ONLY the new credential pair.
//   4. Re-flash the real critterchron firmware (still on the OLD
//      network — DCT survives the flash; the new entry is in DCT
//      regardless of the wipe).
//   5. Move the device to the new network. On boot DeviceOS finds
//      the freshly-written credential and connects.
//
// WHAT CHANGED FROM THE EARLIER ADDITIVE BEHAVIOR:
//   The previous version of this script *added* a credential pair
//   alongside any existing entries. That's safer for "roam between
//   known networks" but doesn't help when you need to evict a stale
//   entry (corrupted password on a known SSID, a now-dead network
//   crowding a slot, or a manually-set procyon entry that the
//   firmware-managed registration won't overwrite). This version
//   nukes and repaves: clearCredentials() then a single
//   setCredentials() for the pair you specify below. If you also
//   want to keep the procyon rescue credential after this wipe,
//   either flash the real critterchron firmware afterward (it
//   re-registers procyon at boot) or duplicate the setCredentials
//   call below for procyon/horology.
//
// SAFETY / RISK WINDOW:
//   The clear and the new-set happen back-to-back inside one setup()
//   pass — no reboot in between. The current WiFi association
//   survives until the device tries to reconnect, so cloud OTA
//   reaches the device fine for the next flash. Risk is small:
//   if the device reboots/disconnects between this flash landing
//   and your next flash AND the new credential is wrong, the
//   device is stranded and needs USB recovery. Have the cable
//   accessible the first time you run this on a given device.

// ====================== EDIT THESE TWO ======================
#define NEW_SSID     "YOUR_NEW_SSID_HERE"
#define NEW_PASSWORD "YOUR_NEW_PASSWORD_HERE"
// ============================================================

// WPA2 covers virtually all home/office networks. Override here if
// you're priming for an unusual AP:
//   - WPA2 (default): WPA2-PSK / WPA2-Personal — typical home WiFi
//   - WPA:            legacy WPA-PSK
//   - WEP:            very old open-pre-shared-key
//   - UNSEC:          open / no password (also: pass NULL for password)
#define NEW_SECURITY WPA2

static void dump_creds(const char* label) {
    WiFiAccessPoint aps[10];
    int n = WiFi.getCredentials(aps, sizeof(aps) / sizeof(aps[0]));
    Serial.printlnf("---- %s (%d slot(s)) ----", label, n);
    for (int i = 0; i < n; ++i) {
        Serial.printlnf("  [%d] ssid=\"%s\" security=%lu",
                        i, aps[i].ssid, (unsigned long)aps[i].security);
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);  // give USB serial monitor time to attach

    Serial.println();
    Serial.println("---- particle_wifi_prime (nuke-and-repave) ----");
    Serial.printlnf("device: %s", System.deviceID().c_str());
    Serial.printlnf("uptime: %lus", System.uptime());
    Serial.printlnf("currently connected to: %s (rssi=%d)",
                    WiFi.SSID(), (int)WiFi.RSSI());

    dump_creds("before");

    Serial.println("clearing all saved credentials...");
    WiFi.clearCredentials();

    Serial.printlnf("writing new SSID: %s", NEW_SSID);
    bool ok = WiFi.setCredentials(NEW_SSID, NEW_PASSWORD, NEW_SECURITY);
    Serial.printlnf("setCredentials -> %s", ok ? "OK" : "FAILED");

    dump_creds("after");
    Serial.println("---- done; flash real firmware next ----");
}

void loop() {
    // Heartbeat blink so you know the device is alive and the script
    // didn't panic. D7 is the on-board LED on Photon / Photon 2 / Argon.
    pinMode(D7, OUTPUT);
    digitalWrite(D7, HIGH);
    delay(500);
    digitalWrite(D7, LOW);
    delay(500);
}
