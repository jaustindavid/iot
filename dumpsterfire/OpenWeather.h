#ifndef OPENWEATHER_H
#define OPENWEATHER_H

#define DEBUGGING_OW
// #undef DEBUGGING

#define DEBUG_PRINTF Serial.printf


void setupWeather() {
    // not sure there's much to do here
    
} // setupWeather()


void updateWeather() {
    // once per hour, pull new weather
    // TODO: configurable
    EVERY_N_SECONDS (stratus.getInt("weather interval", 30)) {
        String url = stratus.get(stratus.getGUID() + " owurl");
        String weatherdata = stratus.fetch(url);
        if (weatherdata.length() > 0) {
            tokenize(weatherdata, "\"dt\":", ",", token, 64);
            while (strlen(token) > 0) {
                DEBUG_PRINTF("dt: %s\n", token);
                tokenize(NULL, "\"feels_like\":", ",", token, 64);
                DEBUG_PRINTF("feels like: %s\n", token);
                tokenize(NULL, "\"dt\":", ",", token, 64);
            }
        }
    }

}

#endif