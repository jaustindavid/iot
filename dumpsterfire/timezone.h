#ifndef TIMEZONE_H
#define TIMEZONE_H

#define TZOFFSET_ADDY 8
int32_t tzOffset = -4; // EDT; a reasonable default

int tzOffsetHandler(String data) {
    int32_t newOffset = 0;
    
    if (data == "0") {
        newOffset = 0;
    } else if (data.startsWith("-")) {
        data = data.substring(1);
        newOffset = -1 * data.toInt();
    } else if (data.toInt() != 0) {
        newOffset = data.toInt();
    } else {
        return -1;
    }

    tzOffset = newOffset;
    EEPROM.put(TZOFFSET_ADDY, tzOffset);
    Time.zone(tzOffset);
    return tzOffset;
} // offsetTZhandler(data)


void setupTZOffset() {
    Particle.function("GMT_offset", tzOffsetHandler);
    Particle.variable("TZ_offset", tzOffset);
    EEPROM.get(TZOFFSET_ADDY, tzOffset);
    
    tzOffset = stratus.getInt("tz offset", tzOffset);
    Time.zone(tzOffset);
} // setupTZOffset();

#endif