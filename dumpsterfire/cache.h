#ifndef CACHE_H
#define CACHE_H


/*
 * The Cache seems to take about 1k per 10 entries.
 * Wildly variable ofc
 *
 * In non-naiive testing the Cache gives 10x improvement
 * in Stratus: 54ms (1000 cached lookups) vs. 455 ms pre-cache
 * 
    private:
        Cache<String> _strings;
        Cache<int32_t> _ints;
        Cache<uint32_t> _hexes;
        Cache<float> _floats;


            _strings.init();
            _ints.init();
            _hexes.init();
            _floats.init();
            
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

 */
#ifndef CACHE_ENTRIES
#define CACHE_ENTRIES 10
#endif

template <class T>
class Cache {
    private:
        String _keys[CACHE_ENTRIES];
        T _data[CACHE_ENTRIES];
        
    public:
        Cache(){
            init();
        }
        void init();
        T get(const String key);
        void insert(const String key, const T value);
        void del(const String key);
};


template <class T>
void Cache<T>::init() {
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        _keys[i] = "";    
    }
}


template <class T>
T Cache<T>::get(String key) {
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (_keys[i].equals(key)) {
            // Serial.printf("Found %s at i=%d: ", key.c_str(), i);
            // Serial.println(_data[i]);
            return _data[i];
        }
    }
    return (T)0;
}


template <class T>
void Cache<T>::insert(String key, T value) {
    int i = 0;
    while (not _keys[i].equals("") and
           not _keys[i].equals(key)) {
        i ++;
    }
    // Serial.printf("insert: i=%d\n", i);
    if (i < CACHE_ENTRIES) {
        _keys[i] = key;
        _data[i] = value;
        // Serial.print("data[i]=");
        // Serial.println(value);
    }
}


template <class T>
void Cache<T>::del(String key) {
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (_keys[i].equals(key)) {
            _keys[i] = "";
        }
    }
  
}

#endif
