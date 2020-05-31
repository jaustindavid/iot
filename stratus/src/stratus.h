#ifndef STRATUS_H
#define STRATUS_H

#ifdef PHOTON
  #include <md5.h>
  #include <HttpClient.h>
  #define TIME_NOW Time.now()
#else
  #define TIME_NOW time()
  #ifdef ESP32
    // #include <MD5.h>
    #include <rom/md5_hash.h>
    #include <esp_wifi.h>
    #include <WiFi.h>
    #include <WiFiClient.h>
    #include <HTTPClient.h>
    #define ESP_getChipId()   ((uint32_t)ESP.getEfuseMac())
  #else
    #include <MD5.h>
    #include <ESP8266WiFi.h>
    #include <ESP8266HTTPClient.h>
    #include <WiFiClient.h>
  #endif
#endif

#define CACHE_ENTRIES 50 // about 1kB
#include "cache.h"

/* 
 * A Stratus is a shitty cloud.
 *
 * austindavid.com/stratus
 * 
 * // TODO: handle 0 and broken config data
 * // TODO: update our own URL
 */


// TODO: lose the Strings

// A safe tokenizer that (a) works, (b) doesn't malloc, and 
// (c) won't overrun a static token buffer.
// if data != NULL, searches for the next separator
//   and copies data into token, max n chars, up to end or \0.
// if data == NULL, resumes search at previous separator.
// returns  if no more separators.
/*
    char token[64];
    char sep[6] = "sep'n";
    char end[2] = "\n";
    tokenize(data, sep, end, token, 64);
    while(strlen(token) > 0) {
        Serial.printf("Token: >%s<\n", token);
        tokenize(NULL, sep, end, token, 64);
    }
 */
#undef DEBUG_TOKENIZE
char * tokenize(char *data, const char *start, const char *end,
             char *token, int n) {
    static char *placeholder = NULL;
    #ifdef DEBUG_TOKENIZE
        Serial.printf("starting tokenize\n");
    #endif
    if (data != NULL) {
        #ifdef DEBUG_TOKENIZE
            Serial.printf("Tokenizing %s by >%s<\n", data ? data : "NULL", start);
        #endif
        placeholder = data;
    }
    placeholder = strstr(placeholder, start);
    if (data != NULL && placeholder == data) {
        Serial.printf("trivial case: first is a token\n");
    }
    if (placeholder) {
        placeholder += strlen(start);
        #ifdef DEBUG_TOKENIZER
            Serial.printf("I think the next starts at >%s<\n", placeholder);
        #endif
        char *next = strstr(placeholder, end);
        if (next == NULL) {
            next = strchr(placeholder, '\0');
        }
        int len = next - placeholder + strlen(end);
        if (len > n) {
            len = n;
        }
        strncpy(token, placeholder, len);
        token[len] = '\0';
    } else {
        #ifdef DEBUG_TOKENIZER
            Serial.printf("all done\n");
        #endif
        token[0] = '\0';
    }
    return placeholder;
} // char * tokenize(char *data, const char *start, const char *end, 
  //                 char *token, int len)



String md5sum(String data) {
    #ifdef PHOTON
        unsigned char result[16];
    
        MD5_CTX hash;
        MD5_Init(&hash);
        MD5_Update(&hash, data, data.length());
        MD5_Final(result, &hash);
    
        char buf[33];
        for (int i=0; i<16; i++)
            sprintf(buf+i*2, "%02x", result[i]);
        buf[32]=0;
    
        return String(buf);
    #else
        MD5Builder md5;
        md5.begin();
        md5.add(data);
        md5.calculate();
        return md5.toString();
    #endif
} // String _md5(data)


class StratusMessage {
    private:
        String _action;
        String _guid;
        String _value;
        int32_t _ttl;
    
    public:
        String _key;
        String _scope;
        int32_t _limit;
        bool _reset;

        StratusMessage() {
            _action = "";
            _key = "";
        } // StratusMessage()
        
        
        StratusMessage(const String action, const String key, const String value="", 
                        const String guid="guid", const String scope="PRIVATE",
                        const int32_t ttl=60, const int32_t limit=0, const bool reset=false) {
            _action = action;
            _key = key;
            _value = value;
            _guid = guid;
            _scope = scope;
            _ttl = ttl;
            _limit = limit;
            _reset = reset;
        } // StratusMessage(action, key, defaults)
        
        
        String toURL(const String secret) {
            String body = String::format("%s\nguid: %s\n%s: %s\n",
                            secret.c_str(), _guid.c_str(), _key.c_str(), _value.c_str());
            #ifdef STRATUS_DEBUGGING
                Serial.printf("body: >>\n%s<<\n", body.c_str());
            #endif
            String md5 = md5sum(body);
            String url = String::format("action=%s&key=%s&value=%s&guid=%s&scope=%s&ttl=%d&reset=%d&limit=%d&signature=%s",
                                  _action.c_str(), _key.c_str(), _value.c_str(), _guid.c_str(), _scope.c_str(), 
                                  _ttl, _reset ? 1 : 0, _limit, md5.c_str());
            url.replace(" ", "%20");
            /* all this caused some sort of String corruption
            Serial.printf("MD5: %s\n", md5.c_str());
            Serial.printf("obtw: scope=>%s<\n", _scope.c_str());
            url.reserve(1024);
            url.concat("action=" + _action + "&key=" + _key);
            if (_value.length() > 0) {
                // url += "&value=" + _value;
                url.concat("&value=" + _value);
            }
            // Serial.printf("checkpoint: scope=>%s<, url=>%s<\n", _scope.c_str(), url.c_str());
            // url += "&guid=" + _guid + "&scope=" + _scope;
            url.concat("&guid=" + _guid + "&scope=" + _scope);
            // Serial.printf("checkpoint2: scope=>%s<, url=>%s<\n", _scope.c_str(), url.c_str());
            if (_ttl) url += "&ttl=" + _ttl;
            // Serial.printf("checkpoint3: scope=>%s<, url=>%s<\n", _scope.c_str(), url.c_str());
            if (_reset) url += "&reset=1";
            // Serial.printf("checkpoint4: scope=>%s<, url=>%s<\n", _scope.c_str(), url.c_str());
            if (_limit) url += "&limit=" + _limit;
            // Serial.printf("checkpoint5: scope=>%s<, url=>%s<\n", _scope.c_str(), url.c_str());
            // url += "&signature=" + md5;
            url.concat("&signature=" + md5);
            Serial.printf("checkpoint6: scope=>%s<, url=>%s<\n", _scope.c_str(), url.c_str());
            */
            #ifdef STRATUS_DEBUGGING
                Serial.printf("toURL: >>%s<<\n", url.c_str());
            #endif
            return url;
        } // String toURL()
}; // StratusMessage


class Stratus {
    private:
        String _configURL;
        time_t _age;
        uint16_t _refreshInterval;
        time_t _lastUpdate;
        String _body, _secret;
        
        #ifdef PHOTON
          HttpClient _http;
        #else
          WiFiClient _client;
          HTTPClient _http;
        #endif

        Cache<String> _strings;
        Cache<int32_t> _ints;
        Cache<uint32_t> _hexes;
        Cache<float> _floats;
        
        StratusMessage _subscriptions[10];
        void (*_callbacks[10])(const char *, const char *);
        
        
        // returns the substring from data between "<key>: " and "\n"
        // or an empty string
        String _get(const String key, const String data);

        // returns an md5 hash of data
        String _md5(const String data);
        
        // verifies the MD5 checksum appended to a given String:
        // MD5: <ASCII MD5SUM>
        bool _verifyMD5(const String data);

        // TODO: better error-handling        
        bool _toInt(const String data, int32_t &result);
        bool _toHex(const String data, uint32_t &result);
        String _httpGet(const String);
      
        /* * * * * *
         * pub/sub
         * * * * * */
        String _send(StratusMessage message);
        String _validate(const String);
        void _handleCallback(int index, char *);


    public: 
        Stratus(const String configURL, const String secret);
        
        // update data from the configured URL
        // return true if successful
        bool update();
        void updateSubscriptions();
        
        bool maybeUpdate(time_t interval);
        bool maybeUpdate();
        
        String get(const String key, const String defaultValue);
        
        // looks for / returns an int32_t (or uint / hex, float) specifically
        // TODO: correctly handle zero, broken keys (which will look like zero)
        int32_t getInt(const String key, const int32_t defaulValue);
        uint32_t getHex(const String key, const uint32_t defaultValue);
        float getFloat(const String key, const float defaultValue);

        // this is called in a constructor so must be DEAD SIMPLE
        // string ops only
        void setConfigURL(const String configURL);
        void chaseConfigURL();
        void setSecret(const String secret);
        
        // return a GUID
        String getGUID();
        
        time_t age();
        
        /* * * * * *
         * pub/sub
         * * * * * */
         
        bool publish(const String, const String, const String, const int32_t);
        bool subscribe(void (*callback)(const char *, const char *), const String, 
                        const String, const int32_t, const bool);
        void unsubscribe();

        bool test();
}; // Stratus


/*
 * public methods
 */

Stratus::Stratus(const String configURL = "http://stratus-iot.s3.amazonaws.com/stratus.txt", 
                    const String secret = "default stratus key") {
    _lastUpdate = 0;
    _refreshInterval = 3600;
    _body = "";
    setConfigURL(configURL);
    setSecret(secret);
} // Stratus()
        

#ifdef PHOTON
    bool Stratus::update() {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("httpGet'ing %s\n", _configURL.c_str());
        #endif
        String body = _httpGet(_configURL);
        #ifdef STRATUS_DEBUGGING
            Serial.printf("update() got body=>>\n%s<<\n", body.c_str());
        #endif

        if (body.length() > 0) {
            if (_verifyMD5(body)) {
                #ifdef STRATUS_DEBUGGING
                    Serial.println("data is valid; storing");
                #endif
                _lastUpdate = Time.now();
                _body = body;
                Particle.publish("Stratus updated successfully", PRIVATE);
                _strings.init();
                _ints.init();
                _hexes.init();
                _floats.init();
                return true;
            } else {
                #ifdef STRATUS_DEBUGGING
                    Serial.println("could not validate data :(");
                #endif
                Particle.publish("Stratus failed to update", "MD5 failure", PRIVATE);
                return false;
            }
        } else {
            Particle.publish("Stratus failed to update", PRIVATE);
            return false;                
        }
        updateSubscriptions();
    } // bool update()

#else
    bool Stratus::update() {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("[HTTP] begin...\n");
        #endif
        if (! _http.begin(_client, _configURL)) { 
            Serial.printf("[HTTP] unable to connect");
            return false;
        }
        #ifdef STRATUS_DEBUGGING
            Serial.print("[HTTP] GET...\n");
        #endif
        // start connection and send HTTP header
        int httpCode = _http.GET();
    
        if (httpCode < 0) { 
            #ifdef STRATUS_DEBUGGING
                Serial.printf("[HTTP] GET... failed, error: %s\n", _http.errorToString(httpCode).c_str());
            #endif
            return false;
        }
    
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            String payload = _http.getString();
            #ifdef STRATUS_DEBUGGING
                Serial.println(payload);
            #endif
            
            if (_verifyMD5(payload)) {
                #ifdef STRATUS_DEBUGGING
                    Serial.println("data is valid; storing");
                #endif
                _age = TIME_NOW;
                _body = payload;
                return true;
            } else {
                #ifdef STRATUS_DEBUGGING
                    Serial.println("could not validate data :(");
                #endif
                return false;
            }
        }
     } // bool update()
#endif


        
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
    String value;
    if ((value = _strings.get(key)) != (String)0) {
        return value;
    }
    
    value = _get(key, _body);
    if (value.length() > 0) {
        _strings.insert(key, value);
        return value;
    }
    return defaultValue;
} // String get(key)


// looks for / returns an int32_t specifically
// TODO: correctly handle zero, broken keys (which will look like zero)
int32_t Stratus::getInt(const String key, const int32_t defaultValue) {
    int32_t ret;
    if ((ret = _ints.get(key)) != 0) {
        return ret;
    }
    
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
            
    ret = 0;
    bool success = _toInt(value, ret);
    #ifdef STRATUS_DEBUGGING
        Serial.printf("converted to ret=%d (%s)\n", ret, success ? "success" : "fail");
    #endif
    if (success) {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Fetched int '%s'= %d\n", key.c_str(), ret);
        #endif
        _ints.insert(key, ret);
        return ret;
    } else { 
        return defaultValue;
    }
} // int32_t get(key, defaultValue)


float Stratus::getFloat(const String key, const float defaultValue) {
    float ret = _floats.get(key);
    if (ret != 0) {
        return ret;
    }

    String value = _get(key, _body);
    #ifdef STRATUS_DEBUGGING
        Serial.printf("getting double for %s: %s\n", key.c_str(), value.c_str());
    #endif
    ret = value.toFloat();
    if (ret != 0) {
        _floats.insert(key, ret);
        return ret;
    } else {
        return defaultValue;
    }
}

        
// looks for / returns a uint32_t specifically -- 0xC0FFEE
// TODO: correctly handle zero
uint32_t Stratus::getHex(const String key, const uint32_t defaultValue) {
    uint32_t ret = _hexes.get(key);
    if (ret != 0) {
        return ret;
    }
    
    String value = _get(key, _body);
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Working with key:%s, value:%s\n", key.c_str(), value.c_str());
    #endif
    if (value.startsWith("0x")) {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Hex? '%s'\n", value.c_str());
        #endif
        ret = strtoul(value.c_str(), NULL, 16);
        if (ret) {
            _hexes.insert(key, ret);
            return ret;
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
    
    #ifdef PHWOTON
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
    #endif
    _configURL = configURL;
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
    #ifdef PHOTON
        return System.deviceID();
    #else
        return String(ESP_getChipId(), HEX);
    #endif
} // String getGUID()
        

// returns the age of stratus data in seconds.
time_t Stratus::age() {
    return TIME_NOW - _lastUpdate;
} // time_t age()


/*
 * private methods
 */


// returns the substring from data between "<key>: " and "\n"
// or an empty string
//
// lower-level function; this is never cached
String Stratus::_get(const String key, const String data) {
    #ifdef STRATUS_DEBUGGING
        Serial.printf("get()ing key=%s", key.c_str());
    #endif
    String ret;

    int start = data.indexOf(key + ": ");
    if (start != -1) {
        int end = data.indexOf("\n", start);
        ret = data.substring(start + key.length() + 2, end);
        #ifdef STRATUS_DEBUGGING
            Serial.printf("; ret='%s'\n", ret.c_str());
        #endif
        _strings.insert(key, ret);
        return ret;
    }
            
    #ifdef STRATUS_DEBUGGING
        Serial.println("... not found.");
    #endif
    return String("");
} // String _get(key)


String Stratus::_httpGet(const String url) {
    #ifdef STRATUS_DEBUGGING
        Serial.printf("_httpGet(%s)\n", url.c_str());
    #endif
    #ifdef PHOTON
        http_header_t headers[] = {
            //  { "Content-Type", "application/json" },
            //  { "Accept" , "application/json" },
            { "User-Agent", "stratus/0.1" },
            { "Accept" , "*/*"},
            { NULL, NULL } // NOTE: Always terminate headers will NULL
        };
        
        http_request_t request;
        http_response_t response;
        response.body.reserve(1024);
        
        int start = 0;
        
        // skip over http 
        if (url.startsWith("http://")) {
            start = String("http://").length();
            // Serial.printf("starting at %d\n", start);
        }
        // find the port, if it exists
        int colon = url.indexOf(':', start);
        if (colon >= 0) {
            request.port = url.substring(colon).toInt();
        } else {
            request.port = 80;
        }
        // grab the hostname -- first part before a : or /
        int slash = url.indexOf('/', start);
        if (colon != -1) {
            request.hostname = url.substring(start, colon);
        } else {
            request.hostname = url.substring(start, slash);
        }
        // path is everything after that slash
        request.path = url.substring(slash);
        // Serial.printf("got: host=%s, path=%s\n", request.hostname.c_str(), request.path.c_str());
    
        // GET request
        _http.get(request, response, headers);
                    
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Application>\tResponse status: %d\n", response.status);
            Serial.printf("Application>\tHTTP Response Body: %s\n", response.body.c_str());
        #endif
                    
        if (response.status == 200) {
            return response.body;
        } else {
            return "";
        }
    #endif
} // String _httpGet(url)


String Stratus::_md5(const String data) {
    #ifdef PHOTON
        unsigned char result[16];
    
        MD5_CTX hash;
        MD5_Init(&hash);
        MD5_Update(&hash, data, data.length());
        MD5_Final(result, &hash);
    
        char buf[33];
        for (int i=0; i<16; i++)
            sprintf(buf+i*2, "%02x", result[i]);
        buf[32]=0;
    
        return String(buf);
    #else
      #ifdef ESP32
        struct MD5Context myContext;
        unsigned char md5sum[16];
        // memset((void)myContext,0x00,sizeof(myContext));
        memset(md5sum,0x00,16);
        MD5Init(&myContext);
        MD5Update(&myContext, (unsigned char *)data.c_str(), data.length());
        MD5Final(md5sum, &myContext);
        return (char *)md5sum;
      #else
        MD5Builder md5;
        md5.begin();
        md5.add(data);
        md5.calculate();
        return md5.toString();
      #endif 
    #endif
} // String _md5(data)

        
// verifies the MD5 checksum appended to a given String:
// MD5: <ASCII MD5SUM>
// WIP: signature: <ASCII/HEX MD5SUM>
bool Stratus::_verifyMD5(const String dataIn) {
    String data = String(dataIn);
    String targetMD5 = _get("signature", data);
    String md5Sum;
            
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Comparing target MD5 = %s\n", targetMD5.c_str());
        Serial.printf("JFYI, data is this long: %d\n", data.length());
    #endif
            
    int md5Posn = data.indexOf("signature: ");
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
bool Stratus::_toInt(const String data, int32_t &result) {
    #ifdef STRATUS_DEBUGGING
        Serial.printf("converting %s\n", data.c_str());
    #endif
            
    if (data.equals("0")) {
        result = 0;
        return true;
    } else if (data.startsWith("-")) {
        // data = data.substring(1);
        result = -1 * data.substring(1).toInt();
        return true;
    } else if (data.toInt() != 0) {
        result = data.toInt();
        return true;
    } else if (data.startsWith("0x")) {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Hex? '%s'\n", data.c_str());
        #endif
        result = strtoul(data.c_str(), NULL, 16);
        return true;
    }
            
    return false;
} // bool toInt(String, &int32_t)


/* * * * * *
 * pub/sub
 * * * * * */
         
bool Stratus::publish(const String key, const String value, 
                    const String scope="PRIVATE", const int32_t ttl = 60) {
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Publishing %s:%s\n", key.c_str(), value.c_str());
    #endif
    StratusMessage message = StratusMessage("publish", key, value, getGUID(), scope, ttl);
    String result = _send(message);
    // print(f'Publish got:>>>>>>>>>\n{result}<<<<<<<<<<<<<')
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Publish got: >>>%s<<<", result.c_str());
    #endif
    return result.length() > 0 and result.startsWith("result: success");
} // bool publish(key, value, scope, ttl)


bool Stratus::subscribe(void (*callback)(const char *, const char *), const String key, 
                        const String scope="PRIVATE", int32_t limit=0, bool reset=false) {

    for (int i = 0; i < 10; i++) {
        if (_subscriptions[i]._key.equals("")) {
            _subscriptions[i] = StratusMessage("subscribe", key, "", getGUID(), scope, 0, limit, reset);
            _callbacks[i] = callback;
            return true;
        }
    }
    return false;
} // bool subscribe(callback, key, scope)


// on success, returns whatever the server replied
// or "" if failed.
String Stratus::_send(StratusMessage message) {
/*
        url = f'{self.path}?' + message.to_url(self.secret)
        if debuggery: print(f'sending >>{url}<<')
        conn = http.client.HTTPConnection(self.host)
        conn.request("GET", url)
        response = conn.getresponse()
        # TODO if r1.status == 200
        data = response.read().decode('utf-8')
        conn.close()
        if debuggery: print(f'got back: >>\n{data}<<')
        return self.validate(data)
*/
    String iotmqURL = get("IOTMQ URL", "");
    if (iotmqURL.length() == 0) {
        Serial.println("can't send: no IOTMQ URL");
        return "";
    }
    iotmqURL += "?" + message.toURL(_secret);
    #ifdef STRATUS_DEBUGGING
        Serial.printf("send URL: >%s<\n", iotmqURL.c_str());
    #endif
    String data = _httpGet(iotmqURL);
    #ifdef STRATUS_DEBUGGING
        Serial.printf("data: >>%s<<", data.c_str());
    #endif
    if (_verifyMD5(data)) {
        // strip out debug info from server
        int debugStart = data.indexOf("<<<<<>>>>>\n");
        if (debugStart > 0) {
            #ifdef STRATUS_DEBUGGING
                Serial.printf("SERVER DEBUG:\n%s", data.substring(debugStart+11).c_str());
            #endif
            return data.substring(0, debugStart);
        }
        return data;
    } else {
        return "";
    }
    // return _validate(data);
} // 

/*
 * Subscription data is returned like:
 *   result: success
 *   test: this is a test message
 *   test: this is a test message
 *   test: this is a test message
 *   test: this is a test message
 */
void Stratus::_handleCallback(int index, char *data) {
    char start[64];
    char end[2] = "\n";
    char value[1024];
    snprintf(start, 64, "%s: ", _subscriptions[index]._key.c_str());
    #ifdef STRATUS_DEBUGGING
        Serial.printf("Handling callback for key %s in >%s<<<<<<<<<<\n", 
                        _subscriptions[index]._key.c_str(), data);
    #endif
    tokenize(data, start, end, value, 1024);
    while (strlen(value)>0) {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Want to callback for key %s value %s\n", 
                            _subscriptions[index]._key.c_str(), value);
        #endif
        _callbacks[index](_subscriptions[index]._key.c_str(), value);
        delay(100);
        tokenize(NULL, start, end, value, 1024);
    }
    #ifdef STRATUS_DEBUGGING
        Serial.printf("I handled 'em all\n");
    #endif
} // _handleCallback(index, data)


void Stratus::updateSubscriptions() {
    char buf[1024];
    for (int i = 0; i < 10; i++) {
        #ifdef STRATUS_DEBUGGING
            Serial.printf("Updating #%d\n", i);
        #endif
        if (! _subscriptions[i]._key.equals("")) {
            String data = _send(_subscriptions[i]);
            strncpy(buf, data.c_str(), 1024);
            _handleCallback(i, buf);
            // restore default limit & reset
            _subscriptions[i]._limit = 0;
            _subscriptions[i]._reset = false;
        }
    }
} // _updateSubscriptions()


void Stratus::unsubscribe() {
    for (int i = 0; i < 10; i++) {
        _subscriptions[i]._key = "";
    }
}



// sort of a hacky unit test
bool Stratus::test() {
    setConfigURL("http://stratus-iot.s3.amazonaws.com/stratus-test.txt");
    setSecret("stratus test secret");
    if (update()) {
        Serial.println("successfully loaded test data");
        Serial.printf("test data: \n%s", _body.c_str());
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
    
    uint32_t start = micros();
    Serial.printf("getInt() x 1000: start @ %d us\n", start);
    for (int i = 0; i < 1000; i++) {
        getInt("test float 2", -20);
    }
    uint32_t end = micros();
    Serial.printf("executed in %d us\n", end - start);
    return true;
} // bool test()

#endif