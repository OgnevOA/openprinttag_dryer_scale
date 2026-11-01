// Minimal Arduino API for building firmware logic on a desktop.
//
// This exists so the modules that do real work -- the HX711 driver, the weight
// maths, the tag write policy -- can be compiled and unit tested with g++
// instead of only ever being tried on hardware. It simulates the parts of the
// platform those modules touch: digital pins, a clock, NVS, I2C and SPI.
//
// It is not an emulator and makes no attempt to be one. Anything that talks to
// Wi-Fi, MQTT or the web server is excluded from the host build.

#pragma once

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define MSBFIRST 1
#define SPI_MODE0 0

// XIAO ESP32-C6 header pins, matching the real variant's GPIO numbers.
#define D0 0
#define D1 1
#define D2 2
#define D3 21
#define D4 22
#define D5 23
#define D6 16
#define D7 17
#define D8 19
#define D9 20
#define D10 18

using boolean = bool;
using byte = uint8_t;

// ---------------------------------------------------------------------------
// Simulated hardware. Tests drive these directly.
// ---------------------------------------------------------------------------
namespace hostio {

constexpr int kPinCount = 48;

struct Hx711Simulator {
    bool attached = false;
    int doutPin = -1;
    int sckPin = -1;
    bool ready = false;      // conversion available (DOUT pulled low)
    long value = 0;          // 24-bit two's complement result
    bool stuckHigh = false;  // simulates a disconnected data line
    bool stuckLow = false;

    uint8_t pulses = 0;
    int currentBit = 0;
    uint32_t conversions = 0;
};

struct Lm75Simulator {
    bool attached = false;
    uint8_t address = 0x48;
    float celsius = 22.0f;
    bool failReads = false;
};

void reset();

// Virtual clock. millis() advances it so that spin loops always terminate.
uint32_t now();
void advance(uint32_t milliseconds);

void setPin(int pin, int level);
int pinLevel(int pin);

Hx711Simulator &hx711();
Lm75Simulator &lm75();

// Attaches the converter simulator to the pins the firmware uses.
void attachHx711(int doutPin, int sckPin);
void setHx711Value(long counts, bool ready = true);

void setSerialVerbose(bool verbose);

// Persistent storage backing the Preferences stub. Clearing it simulates a
// factory-fresh device; keeping it across objects simulates a reboot.
void clearPreferences();

}  // namespace hostio

// ---------------------------------------------------------------------------
// Core Arduino functions
// ---------------------------------------------------------------------------
inline uint32_t millis() {
    // Every call nudges the clock, which is what keeps a firmware busy-wait
    // from spinning forever on the host.
    hostio::advance(1);
    return hostio::now();
}

inline uint32_t micros() { return hostio::now() * 1000; }

inline void delay(uint32_t milliseconds) { hostio::advance(milliseconds); }

inline void delayMicroseconds(uint32_t microseconds) {
    hostio::advance(microseconds >= 1000 ? microseconds / 1000 : 0);
}

void pinMode(int pin, int mode);
void digitalWrite(int pin, int level);
int digitalRead(int pin);

inline void noInterrupts() {}
inline void interrupts() {}

inline bool isPrintable(char c) { return c >= 0x20 && c < 0x7F; }

// Deliberately functions rather than the core's macros: as macros they rewrite
// std::max inside libstdc++'s own headers and the host build stops compiling.
template <typename T>
T min(T a, T b) {
    return a < b ? a : b;
}
template <typename T>
T max(T a, T b) {
    return a > b ? a : b;
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------
class __FlashStringHelper;
#define F(string_literal) \
    (reinterpret_cast<const __FlashStringHelper *>(string_literal))

class String {
public:
    String() = default;
    String(const char *text) : value_(text ? text : "") {}
    String(const std::string &text) : value_(text) {}
    String(char c) : value_(1, c) {}
    String(int number) : value_(std::to_string(number)) {}
    String(unsigned number) : value_(std::to_string(number)) {}
    String(long number) : value_(std::to_string(number)) {}
    String(unsigned long number) : value_(std::to_string(number)) {}
    String(const __FlashStringHelper *text)
        : value_(reinterpret_cast<const char *>(text)) {}

    // Two-argument forms: decimal places for the floating point types, a number
    // base for the integers. One overload per type, because a single (double,
    // int) plus a single (unsigned, int) leaves calls with a uint8_t ambiguous.
    String(double number, int decimals) { formatReal(number, decimals); }
    String(float number, int decimals) { formatReal(number, decimals); }
    String(unsigned char number, int base) { formatUnsigned(number, base); }
    String(unsigned number, int base) { formatUnsigned(number, base); }
    String(unsigned long number, int base) { formatUnsigned(number, base); }
    String(int number, int base) {
        formatUnsigned(static_cast<unsigned long>(number), base);
    }

    String &operator+=(const String &other) {
        value_ += other.value_;
        return *this;
    }
    String &operator+=(const char *text) {
        if (text) value_ += text;
        return *this;
    }
    String &operator+=(char c) {
        value_ += c;
        return *this;
    }
    String &operator+=(int number) {
        value_ += std::to_string(number);
        return *this;
    }
    String &operator+=(unsigned number) {
        value_ += std::to_string(number);
        return *this;
    }
    String &operator+=(long number) {
        value_ += std::to_string(number);
        return *this;
    }
    String &operator+=(unsigned long number) {
        value_ += std::to_string(number);
        return *this;
    }
    String &operator+=(const __FlashStringHelper *text) {
        value_ += reinterpret_cast<const char *>(text);
        return *this;
    }

    const char *c_str() const { return value_.c_str(); }
    size_t length() const { return value_.length(); }
    bool reserve(size_t bytes) {
        value_.reserve(bytes);
        return true;
    }
    bool isEmpty() const { return value_.empty(); }
    int indexOf(const char *needle) const {
        const size_t position = value_.find(needle);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    int indexOf(const char *needle, unsigned int from) const {
        if (from > value_.size()) {
            return -1;
        }
        const size_t position = value_.find(needle, from);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    int indexOf(const String &needle) const { return indexOf(needle.c_str()); }
    int indexOf(const String &needle, unsigned int from) const {
        return indexOf(needle.c_str(), from);
    }
    bool startsWith(const char *prefix) const {
        const size_t len = prefix ? strlen(prefix) : 0;
        return value_.size() >= len && value_.compare(0, len, prefix, len) == 0;
    }
    bool startsWith(const String &prefix, unsigned int offset) const {
        if (offset + prefix.length() > value_.size()) {
            return false;
        }
        return value_.compare(offset, prefix.length(), prefix.value_) == 0;
    }
    // Out of range reads a NUL, as the real String does.
    char charAt(unsigned int index) const {
        return index < value_.size() ? value_[index] : '\0';
    }
    char operator[](unsigned int index) const { return charAt(index); }
    void trim() {
        const size_t first = value_.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            value_.clear();
            return;
        }
        const size_t last = value_.find_last_not_of(" \t\r\n");
        value_ = value_.substr(first, last - first + 1);
    }
    float toFloat() const { return strtof(value_.c_str(), nullptr); }
    long toInt() const { return atol(value_.c_str()); }
    bool operator==(const char *text) const {
        return value_ == (text ? text : "");
    }
    bool operator!=(const char *text) const { return !(*this == text); }
    bool operator==(const String &other) const { return value_ == other.value_; }
    bool operator!=(const String &other) const { return value_ != other.value_; }

    const std::string &std_str() const { return value_; }

private:
    void formatReal(double number, int decimals) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.*f", decimals, number);
        value_ = buffer;
    }
    void formatUnsigned(unsigned long number, int base) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), base == 16 ? "%lx" : "%lu", number);
        value_ = buffer;
    }

    std::string value_;
};

inline String operator+(const String &left, const String &right) {
    String result = left;
    result += right;
    return result;
}

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------
class HostSerial {
public:
    void begin(unsigned long = 115200) {}
    operator bool() const { return true; }
    void flush() {}

    template <typename T>
    void print(const T &value) {
        emit(String(value));
    }
    void print(const char *text) { emit(String(text)); }
    void print(const __FlashStringHelper *text) { emit(String(text)); }
    void print(const String &text) { emit(text); }
    void print(double value, int decimals) { emit(String(value, decimals)); }

    template <typename T>
    void println(const T &value) {
        emit(String(value));
        emit(String("\n"));
    }
    void println() { emit(String("\n")); }
    void println(const char *text) {
        emit(String(text));
        emit(String("\n"));
    }
    void println(const __FlashStringHelper *text) {
        emit(String(text));
        emit(String("\n"));
    }
    void println(const String &text) {
        emit(text);
        emit(String("\n"));
    }

    void printf(const char *format, ...) {
        char buffer[512];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        emit(String(buffer));
    }

private:
    void emit(const String &text);
};

extern HostSerial Serial;
