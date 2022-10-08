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


void tweetHandler(const char *event, const char *data) {
    ATOMIC_BLOCK() {
        intensity += strlen(data);
        lastEvent = Time.now();
    }
    String s = String(intensity, DEC);
    Particle.publish("intensify", s);
    // Particle.publish("last_event", String(lastEvent));
    // Particle.publish("last_event", strangify(lastEvent));
} // tweetHandler(event, data)


void fuelHandler(const char *event, const char *data) {
    ATOMIC_BLOCK() {
        intensity += String(data).toInt();
        lastEvent = Time.now();
    }
    String s = String(intensity, DEC);
    Particle.publish("fueling", s);
} // tweetHandler(event, data)


int intensifyHandler(String data) {
    int addum = data.toInt();
    
    if (addum < 0) {
        return -1;
    } else if (addum > 0) {
        intensity += addum;
        lastEvent = Time.now();
        return addum;
    } else {
        intensity += 5;
        lastEvent = Time.now();
        return 0;
    }
} // intensifyHandler(data)


static int32_t secondsPerChar = 3600/CHARS_PER_HOUR;
static int32_t oldestRelevant = OLDEST_RELEVANT;
static int32_t peakIntensity = 500;

void setupTweeter() {
    String fuelStream = stratus.get("fuel stream", "dumpsterfire");
    Serial.printf("Fuel stream: %s\n", fuelStream.c_str());
    //Particle.subscribe("djtTweeted", tweetHandler);
    Particle.subscribe(fuelStream, fuelHandler);
    Particle.variable("intensity", intensity);
    // Particle.variable("lastEvent", lastEvent);
    Particle.function("intensify", intensifyHandler);
    secondsPerChar = 3600/stratus.getInt("chars per hour", CHARS_PER_HOUR);
    oldestRelevant = stratus.getInt("oldest relevant", OLDEST_RELEVANT);
    peakIntensity = stratus.getInt("peak intensity", peakIntensity);    
} // setupTweeter()


// returns a normalized intensity 0..100
// intensity is clipped at 500 (values >= 500 are normalized to max)
int checkIntensity() {
    secondsPerChar = 3600/stratus.getInt("chars per hour", CHARS_PER_HOUR);
    peakIntensity = stratus.getInt("peak intensity", peakIntensity);

    EVERY_N_SECONDS(secondsPerChar) {
        if (intensity > 0) {
            --intensity;
        }
    }
    
    #ifdef DISPLAY_ENABLED
        EVERY_N_SECONDS(1) {
            showStats(lastEvent, intensity);
        }
    #endif
    
    // TODO: configurable intensity
    int level = map(min(intensity, peakIntensity), 0, peakIntensity, 0, 100);
    return level;
} // checkIntensity()


// returns a normalized recency 0..100
int checkRecency() {
    if (lastEvent == 0) {
        return 0;
    }
    
    oldestRelevant = stratus.getInt("oldest relevant", OLDEST_RELEVANT);
    
    int elapsedSeconds = Time.now() - lastEvent;
    int recency = map(min(oldestRelevant, elapsedSeconds), oldestRelevant, 0, 0, 100);
    #ifdef DEBUGGING
        EVERY_N_SECONDS(5) {
            Serial.printf("recency: %d\n", recency);
        }
    #endif
    return recency;
} // makeRecency()

#endif