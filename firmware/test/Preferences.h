#pragma once
#include <string>
#include <map>
#include <stddef.h>

class Preferences {
public:
    std::map<std::string, double> doubles;
    std::map<std::string, bool> bools;

    bool begin(const char* name, bool readOnly) { return true; }
    void end() {}

    void putDouble(const char* key, double val) { doubles[key] = val; }
    double getDouble(const char* key, double defaultVal) {
        if (doubles.find(key) != doubles.end()) return doubles[key];
        return defaultVal;
    }

    void putBool(const char* key, bool val) { bools[key] = val; }
    bool getBool(const char* key, bool defaultVal) {
        if (bools.find(key) != bools.end()) return bools[key];
        return defaultVal;
    }

    void putBytes(const char* key, const void* val, size_t len) {}
    size_t getBytes(const char* key, void* buf, size_t maxLen) { return 0; }
    bool isKey(const char* key) { return false; }
};
