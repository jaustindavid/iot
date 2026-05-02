// particle_wifi_remove_procyon.ino — paste-and-flash credential remover.
//
// USAGE:
//   1. Open https://build.particle.io, paste this whole file in.
//   2. Pick the target device, flash via cloud OTA. The device runs
//      this code, removes any saved credential matching SSID
//      "procyon", and dumps the credential list before/after.
//   3. Re-flash the real critterchron firmware. (The new firmware
//      will register procyon at boot anyway — this script is for
//      clearing OLD manual entries before testing the firmware
//      path, or for any other one-shot procyon-removal need.)
//
// SAFETY:
//   Removes ONLY the entry whose SSID is "procyon". Other saved
//   credentials are untouched. No connect attempt, no disturbance to
//   the currently-connected network. The matrix is dark, the engine
//   is not running.

#define TARGET_SSID "procyon"

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
    Serial.println("---- particle_wifi_remove_procyon ----");
    Serial.printlnf("device: %s", System.deviceID().c_str());

    dump_creds("before");

    bool ok = WiFi.removeCredentials(TARGET_SSID);
    Serial.printlnf("removeCredentials(\"%s\") -> %s",
                    TARGET_SSID, ok ? "OK" : "FAIL/absent");

    dump_creds("after");
    Serial.println("---- done; flash real firmware next ----");
}

void loop() {
    pinMode(D7, OUTPUT);
    digitalWrite(D7, HIGH);
    delay(500);
    digitalWrite(D7, LOW);
    delay(500);
}
