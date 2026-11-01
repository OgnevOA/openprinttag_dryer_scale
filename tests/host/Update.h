// Host stand-in for the ESP32 core's Update (OTA flash writer).

#pragma once

#include <stddef.h>
#include <stdint.h>

#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF

class UpdateClass {
public:
    bool begin(size_t size = UPDATE_SIZE_UNKNOWN);
    size_t write(uint8_t *data, size_t length);
    bool end(bool evenIfRemaining = false);
    void abort();
    bool hasError();
    const char *errorString();
};

extern UpdateClass Update;
