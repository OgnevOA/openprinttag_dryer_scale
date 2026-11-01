// Host stand-in for the Arduino TwoWire API, wired to a simulated
// LM75/TMP102 temperature sensor.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

class TwoWire {
public:
    void begin(int sda = -1, int scl = -1);
    void end();
    void beginTransmission(uint8_t address);
    size_t write(uint8_t value);
    uint8_t endTransmission(bool stop = true);
    uint8_t requestFrom(uint8_t address, uint8_t quantity);
    int read();
    int available();

private:
    uint8_t address_ = 0;
    uint8_t lastRegister_ = 0;
    size_t written_ = 0;
    std::vector<uint8_t> readBuffer_;
    size_t readPosition_ = 0;
};

extern TwoWire Wire;
