#ifndef STRATUS_MESSAGE_H
#define STRATUS_MESSAGE_H


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
            // String body = String::format("%s\nguid: %s\n%s: %s\n",
            //                secret.c_str(), _guid.c_str(), _key.c_str(), _value.c_str());
            char body[1024];
            char url[1024];
            snprintf(body, sizeof(body), "%s\nguid: %s\n%s: %s\n",
                     secret.c_str(), _guid.c_str(), _key.c_str(), _value.c_str());
            #ifdef STRATUS_DEBUGGING
                Serial.printf("body: >>\n%s<<\n", body);
            #endif
            char * md5 = md5sum(body);
            // String url = String::format("action=%s&key=%s&value=%s&guid=%s&scope=%s&ttl=%d&reset=%d&limit=%d&signature=%s",
            //                       _action.c_str(), _key.c_str(), _value.c_str(), _guid.c_str(), _scope.c_str(), 
            //                       _ttl, _reset ? 1 : 0, _limit, md5.c_str());
            // url.replace(" ", "%20");
            snprintf(url, sizeof(url), "action=%s&key=%s&value=%s&guid=%s&scope=%s&ttl=%d&reset=%d&limit=%d&signature=%s",
                                      _action.c_str(), _key.c_str(), _value.c_str(), _guid.c_str(), _scope.c_str(), 
                                      _ttl, _reset ? 1 : 0, _limit, md5);
            #ifdef STRATUS_DEBUGGING
                Serial.printf("toURL: >>%s<<\n", url);
            #endif
            return url;
        } // String toURL()
}; // StratusMessage


#endif
