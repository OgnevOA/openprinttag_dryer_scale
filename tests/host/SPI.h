// Host stand-in for the Arduino SPI API.
//
// Reads always return 0xFF, which is exactly what a missing or unpowered
// PN5180 looks like. That makes this stub useful for one specific thing:
// proving the reader gives up instead of hanging when nothing answers.

#pragma once

#include <stdint.h>

class SPISettings {
public:
    SPISettings() = default;
    SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
        : clock_(clock), bitOrder_(bitOrder), dataMode_(dataMode) {}

private:
    uint32_t clock_ = 1000000;
    uint8_t bitOrder_ = 1;
    uint8_t dataMode_ = 0;
};

class SPIClass {
public:
    void begin(int sck = -1, int miso = -1, int mosi = -1, int ss = -1);
    void end();
    void beginTransaction(SPISettings settings);
    void endTransaction();
    uint8_t transfer(uint8_t value);
};

extern SPIClass SPI;
