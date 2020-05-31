#ifndef STRATUS_ESP32_H
#define STRATUS_ESP32_H

#include <HTTPClient.h>
#include <rom/md5_hash.h>


char * md5sum(const char * data) {
    struct MD5Context myContext;
    char *hash, *ret;
    char char_map[17] = "0123456789abcdef";

    hash = (char *)malloc(sizeof(unsigned char) * 16);
    
    // memset((void *)myContext,0x00,sizeof(myContext));
    memset(hash,0x00,sizeof(*hash));
    
    MD5Init(&myContext);
    MD5Update(&myContext, (unsigned char *)data, strlen(data));
    MD5Final((unsigned char *)hash, &myContext);

    ret = (char *)malloc(sizeof(char)*33);
    for (int i = 0; i < 16; i++) {
      ret[i*2] = char_map[0x0F & hash[i] >> 4];
      ret[i*2+1] = char_map[0x0F & hash[i]];
    }
    ret[32] = '\0';
    return ret;
} // unsigned String md5sum(data)


char * httpGet(const char * url) {
  HTTPClient _http;
  char *payload = NULL;
  #ifdef STRATUS_DEBUGGING
    Serial.printf("[HTTP] begin; URL=%s\n", url);
  #endif
  _http.begin(url);
  int httpCode = _http.GET();

  // httpCode will be negative on error
  if(httpCode > 0) {
    // HTTP header has been send and Server response header has been handled
    #ifdef STRATUS_DEBUGGING
      Serial.printf("[HTTP] GET... code: %d\n", httpCode);
    #endif
    
    if(httpCode == HTTP_CODE_OK) {
      payload = (char *)malloc(sizeof(char)*1024);
      strncpy(payload, _http.getString().c_str(), 1024);
      #ifdef STRATUS_DEBUGGING
        Serial.printf("HTTP payload: >>%s<<\n", payload);
      #endif
    }
  } else {
      Serial.printf("[HTTP] GET... failed, error: %s\n", _http.errorToString(httpCode).c_str());
  }

  _http.end();
  return payload;
} //char * httpGet(cost char *)


// return a GUID
char * getGUID() {
  char *ret = (char *)malloc(32*sizeof(char));
  snprintf(ret, sizeof(*ret), "%llx", ESP.getEfuseMac());
  return ret;
  // return String(((uint32_t)ESP.getEfuseMac()), HEX).c_str();
} // String getGUID()


#endif
