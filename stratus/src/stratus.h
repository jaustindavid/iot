#ifndef STRATUS_H
#define STRATUS_H

#include <HttpClient.h>
#include <md5.h>

/* 
 * A Stratus is a shitty cloud.
 *
 * austindavid.com/stratus
 * 
 * // TODO: handle 0 and broken config data
 * // TODO: update our own URL
 * // TODO: handle other data types -- int (int32_t), hex (uint32_t), bool, String, double
 * // TODO: get() returns bool, modifies a variable inline
 */

#undef DEBUGGING
#undef STRATUS_DEBUGGING

class Stratus {
    private:
        // String _configURL;
        HttpClient _http;
        http_request_t _request;
        String _body, _secret;
        uint16_t _refreshInterval;
        time_t _lastUpdate;
        
        // returns the substring from data between "<key>: " and "\n"
        // or an empty string
        String _get(const String key, const String data);

        // returns an md5 hash of data
        String _md5(String data);
        
        // verifies the MD5 checksum appended to a given String:
        // MD5: <ASCII MD5SUM>
        bool _verifyMD5(String data);

        // TODO: better error-handling        
        bool _toInt(String data, int32_t &result);
        bool _toHex(String data, uint32_t &result);


    public: 
        Stratus(String configURL, String secret);
        
        // update data from the configured URL
        // return true if successful
        bool update();
        
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
        
        bool test();
}; // Stratus


#endif