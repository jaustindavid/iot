#include <HttpClient.h>
#include <md5.h>


#include "stratus.h"
// #define DEBUGGING
// #define STRATUS_DEBUGGING

/*
 * public methods
 */

Stratus::Stratus(String configURL = "http://stratus-iot.s3.amazonaws.com/stratus.txt", 
                    String secret = "stratus key") {
    _lastUpdate = 0;
    _refreshInterval = 3600;
    _body = "";
    setConfigURL(configURL);
    setSecret(secret);
} // Stratus()
        
        
bool Stratus::update() {
    http_header_t headers[] = {
        //  { "Content-Type", "application/json" },
        //  { "Accept" , "application/json" },
        { "User-Agent", "dumpsterfire/0.1" },
        { "Accept" , "*/*"},
        { NULL, NULL } // NOTE: Always terminate headers will NULL
    };

    http_response_t response;
            
    // GET request
    _http.get(_request, response, headers);
            
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Application>\tResponse status: %d\n", response.status);
        Serial.printf("Application>\tHTTP Response Body: %s\n", response.body.c_str());
    #endif
            
    if (response.status == 200) {
        if (_verifyMD5(response.body)) {
            #ifdef STRATUS_DEBUGGING
                Serial.println("data is valid; storing");
            #endif
            _lastUpdate = Time.now();
            _body = response.body;
            Particle.publish("Stratus updated successfully", PRIVATE);
            return true;
        } else {
            #ifdef STRATUS_DEBUGGING
                Serial.println("could not validate data :(");
            #endif
            Particle.publish("Stratus failed to update", "MD5 failure", PRIVATE);
            return false;
        }
    } else {
        Particle.publish("Stratus failed to update", String::format("HTTP response %d", response.status), PRIVATE);
        return false;                
    }
} // bool update()
        
        
// returns true if it's not time to update, or if the update suceeds
// returns false if the update fails
bool Stratus::maybeUpdate(const time_t interval) {
    time_t myInterval;
    if (interval == 0) {
        myInterval = _refreshInterval;
    } else {
        myInterval = interval;
    }
    if (age() > myInterval) {
        return update();
    }
    return true;
} // bool maybeUpdate(time_t)
        

bool Stratus::maybeUpdate() {
    // get("REFRESH INTERVAL", _refreshInterval);
    return maybeUpdate(_refreshInterval);
} // maybeUpdate()


String Stratus::get(const String key, String defaultValue = "") {
    String value = _get(key, _body);
    if (value.length() > 0) {
        return value;
    }
    return defaultValue;
} // String get(key)


// looks for / returns an int32_t specifically
// TODO: correctly handle zero, broken keys (which will look like zero)
int32_t Stratus::getInt(const String key, const int32_t defaultValue) {
    String value = _get(key, _body);
            
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Working with key:%s, value:%s\n", key.c_str(), value.c_str());
    #endif
    if (value.equals("")) {
        #ifdef STRATUS_DEBUGGING
            Serial.println("no value; returning default");
        #endif
        return defaultValue;
    }
            
    int32_t ret = 0;
    bool success = _toInt(value, ret);
    #ifdef STRATUS_DEBUGGING
        Serial.printf("converted to ret=%d (%s)\n", ret, success ? "success" : "fail");
    #endif
    if (success) {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Fetched int '%s'= %d\n", key.c_str(), ret);
        #endif
        return ret;
    } else { 
        return defaultValue;
    }
} // int32_t get(key, defaultValue)


float Stratus::getFloat(const String key, const float defaultValue) {
    String value = _get(key, _body);
    Serial.printf("getting double for %s: %s\n", key.c_str(), value.c_str());
    float result = value.toFloat();
    if (result != 0) {
        return result;
    } else {
        return defaultValue;
    }
}

        
// looks for / returns a uint32_t specifically -- 0xC0FFEE
// TODO: correctly handle zero
uint32_t Stratus::getHex(const String key, const uint32_t defaultValue) {
    String value = _get(key, _body);
            
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Working with key:%s, value:%s\n", key.c_str(), value.c_str());
    #endif
    if (value.startsWith("0x")) {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Hex? '%s'\n", value.c_str());
        #endif
        uint32_t result = strtoul(value.c_str(), NULL, 16);
        if (result) {
            return result;
        } 
    }
    return defaultValue;
} // uint32_t getHex(key, defaultValue)


// this is called in a constructor so must be DEAD SIMPLE
// string ops only
// it parses the configURL for later (repeated) use
void Stratus::setConfigURL(const String configURL) {
    // ignore https (not supported)
    if (configURL.startsWith("https://")) {
        return; 
    }
    
    int start = 0;

    // _configURL = configURL;
    // skip over http 
    if (configURL.startsWith("http://")) {
        start = String("http://").length();
    } 
            
    // find the port, if it exists
    int colon = configURL.indexOf(':', start);
    if (colon >= 0) {
        _request.port = configURL.substring(colon).toInt();
    } 
            
    // grab the hostname -- first part before a : or /
    int slash = configURL.indexOf('/', start);
    if (colon != -1) {
        _request.hostname = configURL.substring(start, colon);
    } else {
        _request.hostname = configURL.substring(start, slash);
    }
            
    // path is everything after that slash
    _request.path = configURL.substring(slash);
} // setConfigURL(String)


// a helper function to iteratively load a GUID-specific config URL
// until it gets to a leaf
void Stratus::chaseConfigURL() {
    String newConfigURL = get(getGUID() + " config url");
    String oldConfigURL = "";
    while (not newConfigURL.equals("") and not newConfigURL.equals(oldConfigURL)) {
        setConfigURL(newConfigURL);
        if (update()) {
            oldConfigURL = newConfigURL;
            newConfigURL = get(getGUID() + " config url");
        } else {
            return;
        }
    }
} // chaseConfigURL()

        
void Stratus::setSecret(const String secret) {
    _secret = secret;
} // setSecret(secret)
        
        
// return a GUID
String Stratus::getGUID() {
    return System.deviceID();
} // String getGUID()
        

// returns the age of stratus data in seconds.
time_t Stratus::age() {
    return Time.now() - _lastUpdate;
} // time_t age()

/*
 * private methods
 */

// returns the substring from data between "<key>: " and "\n"
// or an empty string
String Stratus::_get(const String key, const String data) {
    #ifdef STRATUS_DEBUGGING
        Serial.printf("get()ing key=%s", key.c_str());
    #endif
            
    int start = data.indexOf(key + ": ");
    if (start != -1) {
        int end = data.indexOf("\n", start);
        String ret = data.substring(start + key.length() + 2, end);
        #ifdef STRATUS_DEBUGGING
            Serial.printf("; ret='%s'\n", ret.c_str());
        #endif
        return ret;
    }
            
    #ifdef STRATUS_DEBUGGING
        Serial.println("... not found.");
    #endif
    return String("");
} // String _get(key)


String Stratus::_md5(String line) {
    unsigned char result[16];

    MD5_CTX hash;
    MD5_Init(&hash);
    MD5_Update(&hash, line, line.length());
    MD5_Final(result, &hash);

    char buf[33];
    for (int i=0; i<16; i++)
        sprintf(buf+i*2, "%02x", result[i]);
    buf[32]=0;

    return String(buf);
} // String _md5(data)

        
// verifies the MD5 checksum appended to a given String:
// MD5: <ASCII MD5SUM>
bool Stratus::_verifyMD5(String data) {
    String targetMD5 = _get("MD5", data);
    String md5Sum;
            
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Comparing target MD5 = %s\n", targetMD5.c_str());
        Serial.printf("JFYI, data is this long: %d\n", data.length());
    #endif
            
    int md5Posn = data.indexOf("MD5:");
    if (md5Posn != -1) {
        data.remove(md5Posn);
    } 
    md5Sum = _md5(_secret + "\n" + data); // data);

    #ifdef STRATUS_DEBUGGING
        Serial.printf("Hashing: >%s\n%s<\n", _secret.c_str(), data.c_str());
        Serial.printf("Computed MD5: %s\n", md5Sum.c_str());
    #endif
            
    if (targetMD5 == "") {
        return false;
    }
    return md5Sum == targetMD5;
} // verifyMD5()


// TODO: better error-handling        
bool Stratus::_toInt(String data, int32_t &result) {
    #ifdef STRATUS_DEBUGGING
        Serial.printf("converting %s\n", data.c_str());
    #endif
            
    if (data.equals("0")) {
        result = 0;
        return true;
    } else if (data.startsWith("-")) {
        data = data.substring(1);
        result = -1 * data.toInt();
        return true;
    } else if (data.toInt() != 0) {
        result = data.toInt();
        return true;
    } else if (data.startsWith("0x")) {
        Serial.printf("Hex? '%s'\n", data.c_str());
        result = strtoul(data.c_str(), NULL, 16);
        return true;
    }
            
    return false;
} // bool toInt(String, &int32_t)


// sort of a hacky unit test
bool Stratus::test() {
    setConfigURL("http://stratus-iot.s3.amazonaws.com/stratus-test.txt");
    setSecret("stratus test secret");
    if (update()) {
        Serial.println("successfully loaded test data");
    } else {
        Serial.println("failed to load test data");
        return false;
    }
    Serial.printf("test string: '%s'\n", get("test string", "default").c_str());
    Serial.printf("test hex 1: %#08x\n", getHex("test hex 1", 0x00));
    Serial.printf("test hex 2: %#08x\n", getHex("test hex 2", 0x00));
    Serial.printf("test hex 255: %#08x\n", getHex("test hex 255", 0x00));
    Serial.printf("test float 1: %8.5f\n", getFloat("test float 1", -5.1));
    Serial.printf("test float 2: %8.5f\n", getFloat("test float 2", -5.2));
    return true;
} // bool test()