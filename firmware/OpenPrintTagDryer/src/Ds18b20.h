// DS18B20 on a one-wire bus, as an alternative to the I2C temperature sensor.
// Same job as the LM75B: report the load cell's own temperature so the scale can
// subtract its drift. Either sensor will do, and ScaleManager takes whichever it
// finds, so this exists because a DS18B20 is what most people already have in a
// drawer.
//
// Two things make it more than the register read an I2C part would be.
//
// The conversion takes up to 750 ms at 12-bit resolution. That is far too long
// to block a device that is also weighing, serving a web portal and talking to
// MQTT, so a reading is split in two: start the conversion, come back for the
// answer. ScaleManager drives that.
//
// And the bus is timed in microseconds by a general purpose processor that is
// also running a Wi-Fi stack, so a bit slot can be stretched by an interrupt at
// the wrong moment and return a plausible-looking wrong number. Every reading is
// therefore checked against the scratchpad's own CRC and thrown away if it fails
// -- a missing temperature costs drift compensation, which the firmware handles,
// while a silently wrong one corrupts the weights the whole device exists to
// report.

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace ds18b20 {

// Maxim's CRC-8, x^8 + x^5 + x^4 + 1 with the bits reflected, which is the
// polynomial as 0x8C. Validated against the datasheet's power-on scratchpad,
// 50 05 4B 46 7F FF 0C 10, whose CRC is 0x1C.
uint8_t crc8(const uint8_t *data, size_t length);

// Decodes the nine scratchpad bytes into degrees. Fails rather than guessing on
// any of the three ways this can be wrong; see the implementation for which.
bool celsius(const uint8_t scratchpad[9], float &out);

// The worst case conversion time, at the 12-bit resolution the part leaves the
// factory with. Lower resolutions are quicker but this is the only number worth
// waiting out, and it costs nothing here: the wait is not a blocking one.
constexpr uint32_t kConversionMs = 750;

}  // namespace ds18b20

class Ds18b20 {
public:
    void begin(int pin);

    // True if a device answers the bus reset and hands over a scratchpad that
    // passes its CRC. Both halves matter: a shorted or floating line can produce
    // a convincing presence pulse on its own.
    bool detect();

    // Each returns false if nobody answers the bus reset.
    bool startConversion();
    bool readConversion(float &celsius);

private:
    bool reset();
    void writeBit(bool value);
    bool readBit();
    void writeByte(uint8_t value);
    uint8_t readByte();
    bool readScratchpad(uint8_t scratchpad[9]);

    int pin_ = -1;
};
