// Host stand-in for the ESP32 core's Preferences (NVS) API.
//
// Backed by a process-wide map, so destroying a Preferences object and creating
// another one sees the same data: that is what makes it possible to test that
// calibration survives a reboot.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

class Preferences {
public:
    bool begin(const char *name, bool readOnly = false);
    void end();
    bool clear();
    bool remove(const char *key);
    bool isKey(const char *key);

    size_t putFloat(const char *key, float value);
    float getFloat(const char *key, float fallback = 0.0f);
    size_t putBool(const char *key, bool value);
    bool getBool(const char *key, bool fallback = false);
    size_t putUInt(const char *key, uint32_t value);
    uint32_t getUInt(const char *key, uint32_t fallback = 0);
    size_t putString(const char *key, const char *value);
    size_t getString(const char *key, char *out, size_t maxLength);

private:
    std::string namespace_;
    bool readOnly_ = false;
    bool open_ = false;
};
