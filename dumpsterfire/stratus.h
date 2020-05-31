#ifndef STRATUS_H
#define STRATUS_H

#ifdef ESP32
  #define TIME_NOW (millis()/1000)
  #include "stratus_esp32.h"
#endif

#define CACHE_ENTRIES 10 // about 1kB
#include "cache.h"
#include "stratus_message.h"

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







class Stratus {
    private:
        String _configURL;
        time_t _age;
        time_t _lastUpdate;
        String _body, _secret;
        char _guid[32];
        
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
        // String _md5(const String data);
        
        // verifies the MD5 checksum appended to a given String:
        // MD5: <ASCII MD5SUM>
        bool _verifyMD5(const String data);

        // TODO: better error-handling        
        bool _toInt(const String data, int32_t &result);
        bool _toHex(const String data, uint32_t &result);
        // String _httpGet(const String);

      
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
        // bool maybeUpdate();
        
        String get(const String key, const String defaultValue);
        
        // looks for / returns an int32_t (or uint / hex, float) specifically
        // TODO: correctly handle zero, broken keys (which will look like zero)
        int32_t getInt(const String key, const int32_t defaulValue);
        uint32_t getHex(const String key, const uint32_t defaultValue);
        float getFloat(const String key, const float defaultValue);

        // this is called in a constructor so must be DEAD SIMPLE
        // string ops only
        void setConfigURL(const String configURL);
        // void chaseConfigURL();
        void setSecret(const String secret);
        
        // return a GUID
        // String getGUID();
        
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

    char *guid;
    _lastUpdate = 0;
    _body = "";
    setConfigURL(configURL);
    setSecret(secret);
    guid = getGUID();
    strncpy(_guid, guid, sizeof(_guid));
    free(guid);
} // Stratus()
        

bool Stratus::update() {
  bool ret = false;
  char * payload = httpGet(_configURL.c_str());
  if (payload == NULL) {
    Serial.printf("failed to retreive a payload\n");
    return false;
  }
  #ifdef STRATUS_DEBUGGING
    Serial.printf("Updating with >>%s<<\n", payload);
  #endif
        
  if (_verifyMD5(payload)) {
    #ifdef STRATUS_DEBUGGING
      Serial.println("data is valid; storing");
    #endif
    _age = TIME_NOW;
    // strncpy(_body, payload, sizeof(_body));
    _body = String(payload);
    free(payload);
    ret = true;
  } else {
    #ifdef STRATUS_DEBUGGING
      Serial.println("could not validate data :(");
    #endif
    free(payload);
  }

  updateSubscriptions();
  
  return ret;
} // bool update()


// returns true if it's not time to update, or if the update succeeds
// returns false if the update fails
//
// potential problem: if an update is not successful (age() doesn't get small)
// this may fire *every* time.  For somtehing like a 404, this could be upgly
//
// TODO: rate limit
bool Stratus::maybeUpdate(const time_t interval) {
    if (age() > interval) {
        return update();
    }
    return true;
} // bool maybeUpdate(time_t)


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
} // bool _toInt(String, &int32_t)


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
} // int32_t getInt(key, defaultValue)


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
    
    #ifdef PHOTON
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


void Stratus::setSecret(const String secret) {
    _secret = secret;
} // setSecret(secret)
        

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
    
    #ifdef STRATUS_DEBUGGING
        Serial.printf("JFYI, data is pruned to this long: %d\n", data.length());
        Serial.printf("data: >>%s<<\n", data.c_str());
    #endif

    char md5data[2048];
    snprintf(md5data, sizeof(md5data), "%s\n%s", _secret.c_str(), data.c_str());
    #ifdef STRATUS_DEBUGGING
      Serial.printf("hashing >>%s<<\n", md5data);
    #endif
    md5Sum = md5sum(md5data);

    #ifdef STRATUS_DEBUGGING
        Serial.printf("Computed MD5: %s\n", md5Sum.c_str());
    #endif
            
    if (targetMD5 == "") {
        return false;
    }
    return md5Sum == targetMD5;
} // _verifyMD5()





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
    String data = httpGet(iotmqURL.c_str());
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
} // updateSubscriptions()


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
