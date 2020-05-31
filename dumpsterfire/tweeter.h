#ifndef TWEETER_H
#define TWEETER_H

#define DEBUGGING
// #undef DEBUGGING

// takes an hour to burn off this many characters of tweeting
#define CHARS_PER_HOUR 240
// for how long should the display stay >0 (length) ?
#define OLDEST_RELEVANT (60*60)  // seconds

int32_t intensity = 0;
time_t lastEvent = 0;

#ifdef PHOTON
void tweetHandler(const char *event, const char *data) {
    ATOMIC_BLOCK() {
        intensity += strlen(data);
        lastEvent = TIME_NOW;
    }
    String s = String(intensity, DEC);
    Particle.publish("intensify", s);
    // Particle.publish("last_event", String(lastEvent));
    // Particle.publish("last_event", strangify(lastEvent));
} // tweetHandler(event, data)


int intensifyHandler(String data) {
    int addum = data.toInt();
    Serial.printf("'%s' -> %d\n", data.c_str(), addum);
    if (addum < 0) {
        return -1;
    } else if (addum > 0) {
        intensity += addum;
        lastEvent = TIME_NOW;
        return addum;
    } else {
        intensity += 5;
        lastEvent = TIME_NOW;
        return 0;
    }
} // intensifyHandler(data)

#endif

// int fuel = 0;
void callback(const char *key, const char *value) {
    intensity += strtol(value, NULL, 10);
    lastEvent = TIME_NOW;
    Serial.printf("%lu: Intensity is now %d\n", TIME_NOW, intensity);
}

static int32_t secondsPerChar = 3600/CHARS_PER_HOUR;
static time_t oldestRelevant = OLDEST_RELEVANT;
static int32_t peakIntensity = 500;

void setupTweeter() {
  #ifdef PHOTON
    // TODO: make the strings configurable
    Particle.subscribe("djtTweeted", tweetHandler);
    Particle.variable("intensity", intensity);
    Particle.variable("lastEvent", lastEvent);
    Particle.function("intensify", intensifyHandler);
  #else
    stratus.subscribe(callback, "fuel", "dumpsterfire", 0, true);
  #endif
    secondsPerChar = 3600/stratus.getInt("chars per hour", CHARS_PER_HOUR);
    oldestRelevant = stratus.getInt("oldest relevant", OLDEST_RELEVANT);
    peakIntensity = stratus.getInt("peak intensity", peakIntensity); 
} // setupTweeter()


// returns a normalized intensity 0..100
// intensity is clipped at peakIntensity (pre-normalization)
uint8_t checkIntensity() {
    EVERY_N_SECONDS(REFRESH_INTERVAL) {
        // TODO: check for divide-by-zero
        secondsPerChar = 3600/stratus.getInt("chars per hour", CHARS_PER_HOUR);
        peakIntensity = stratus.getInt("peak intensity", peakIntensity);
    }
    
    EVERY_N_SECONDS(secondsPerChar) {
        if (intensity > 0) {
            --intensity;
            Serial.printf("%lu: Intensity: %d\n", TIME_NOW, intensity);
        }
    }
    
    #ifdef DISPLAY_ENABLED
        EVERY_N_SECONDS(1) {
            showStats(lastEvent, intensity);
        }
    #endif
    
    uint8_t level = map(min(intensity, peakIntensity), 0, peakIntensity, 0, 100);
    return level;
} // checkIntensity()


// returns a normalized recency 0..100
uint8_t checkRecency() {
    if (lastEvent == 0) {
        return 0;
    }
    
    EVERY_N_SECONDS(REFRESH_INTERVAL) {
        oldestRelevant = stratus.getInt("oldest relevant", OLDEST_RELEVANT);
    }
    
    time_t elapsedSeconds = TIME_NOW - lastEvent;
    uint8_t recency = map(min((time_t)oldestRelevant, elapsedSeconds), oldestRelevant, 0, 0, 100);
    return recency;
} // checkRecency()

#endif
