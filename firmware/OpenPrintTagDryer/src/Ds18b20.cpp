#include "Ds18b20.h"

namespace {

// Bus commands. Only three are needed: there is one device on this bus, so it
// can be addressed by skipping the ROM match entirely rather than searching for
// and then matching a 64-bit address.
constexpr uint8_t kSkipRom = 0xCC;
constexpr uint8_t kConvertT = 0x44;
constexpr uint8_t kReadScratchpad = 0xBE;

// The value the part reports before its first conversion, 85.0 C exactly.
constexpr int16_t kPowerOnRaw = 0x0550;

}  // namespace

namespace ds18b20 {

uint8_t crc8(const uint8_t *data, size_t length) {
    uint8_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? static_cast<uint8_t>((crc >> 1) ^ 0x8C)
                            : static_cast<uint8_t>(crc >> 1);
        }
    }
    return crc;
}

bool celsius(const uint8_t scratchpad[9], float &out) {
    // An undriven bus reads as all ones, which is the shape of a disconnected
    // sensor rather than a cold one. The CRC below would almost certainly catch
    // it too, but saying so here makes the intent legible.
    bool allOnes = true;
    for (uint8_t i = 0; i < 9; ++i) {
        if (scratchpad[i] != 0xFF) {
            allOnes = false;
            break;
        }
    }
    if (allOnes) {
        return false;
    }

    if (crc8(scratchpad, 8) != scratchpad[8]) {
        return false;
    }

    int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(scratchpad[1]) << 8) |
                                       scratchpad[0]);

    // At less than 12-bit resolution the unused low bits are undefined rather
    // than zero, so a part someone has reconfigured would otherwise read up to
    // 0.4 C off. The factory default is 12-bit and this firmware never changes
    // it, but clones have been known to arrive set differently.
    switch ((scratchpad[4] >> 5) & 0x03) {
        case 0:  // 9-bit, 0.5 C
            raw &= ~0x0007;
            break;
        case 1:  // 10-bit, 0.25 C
            raw &= ~0x0003;
            break;
        case 2:  // 11-bit, 0.125 C
            raw &= ~0x0001;
            break;
        default:  // 12-bit, 0.0625 C
            break;
    }

    // Read before it ever converted. On a load cell in a dryer that cannot reach
    // 70 C of air, a genuine 85.0 C is not a reading worth preserving anyway.
    if (raw == kPowerOnRaw) {
        return false;
    }

    out = static_cast<float>(raw) / 16.0f;
    return out >= -55.0f && out <= 125.0f;
}

}  // namespace ds18b20

void Ds18b20::begin(int pin) {
    pin_ = pin;
    // Released, so the external pull-up holds the bus idle high. The internal
    // pull-up is far too weak to run this bus on its own; see the header.
    pinMode(pin_, INPUT_PULLUP);
}

// Interrupts are held off only across the windows where being late changes the
// answer. A stretched low pulse is harmless -- the device only asks for a
// minimum -- but a late sample reads the bus after the device has let go of it.
bool Ds18b20::reset() {
    if (pin_ < 0) {
        return false;
    }

    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
    delayMicroseconds(480);

    noInterrupts();
    pinMode(pin_, INPUT_PULLUP);
    delayMicroseconds(70);
    const bool present = digitalRead(pin_) == LOW;
    interrupts();

    delayMicroseconds(410);
    return present;
}

void Ds18b20::writeBit(bool value) {
    noInterrupts();
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
    delayMicroseconds(value ? 6 : 60);
    pinMode(pin_, INPUT_PULLUP);
    delayMicroseconds(value ? 64 : 10);
    interrupts();
}

bool Ds18b20::readBit() {
    noInterrupts();
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
    delayMicroseconds(6);
    pinMode(pin_, INPUT_PULLUP);
    delayMicroseconds(9);
    const bool value = digitalRead(pin_) == HIGH;
    interrupts();

    delayMicroseconds(55);
    return value;
}

void Ds18b20::writeByte(uint8_t value) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
        writeBit((value & (1u << bit)) != 0);
    }
}

uint8_t Ds18b20::readByte() {
    uint8_t value = 0;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (readBit()) {
            value |= static_cast<uint8_t>(1u << bit);
        }
    }
    return value;
}

bool Ds18b20::readScratchpad(uint8_t scratchpad[9]) {
    if (!reset()) {
        return false;
    }
    writeByte(kSkipRom);
    writeByte(kReadScratchpad);
    for (uint8_t i = 0; i < 9; ++i) {
        scratchpad[i] = readByte();
    }
    return true;
}

bool Ds18b20::detect() {
    uint8_t scratchpad[9] = {0};
    if (!readScratchpad(scratchpad)) {
        return false;
    }
    // The scratchpad has to be intact, but not necessarily hold a temperature:
    // a part that has never converted reads 85 C, which is exactly the state a
    // freshly powered sensor is in when this runs.
    return ds18b20::crc8(scratchpad, 8) == scratchpad[8];
}

bool Ds18b20::startConversion() {
    if (!reset()) {
        return false;
    }
    writeByte(kSkipRom);
    writeByte(kConvertT);
    return true;
}

bool Ds18b20::readConversion(float &celsius) {
    uint8_t scratchpad[9] = {0};
    if (!readScratchpad(scratchpad)) {
        return false;
    }
    return ds18b20::celsius(scratchpad, celsius);
}
