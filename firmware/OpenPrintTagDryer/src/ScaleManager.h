// Weighing: an HX711 bridge ADC under the whole dryer.
//
// Three stored constants turn raw counts into grams of filament:
//
//   zeroCounts     raw reading with nothing at all on the platform
//   countsPerGram  span, from a known calibration mass
//   dryerTare      weight of the empty dryer, lid closed
//
// gross  = (counts - zeroCounts) / countsPerGram - temperature drift
// spool  = gross - dryerTare
//
// The HX711 driver is in-project rather than a library dependency: the protocol
// is 25 clock pulses, and owning it means owning the timing, which is the part
// that actually matters next to a Wi-Fi radio.

#pragma once

#include <Arduino.h>

#include "../config.h"
#include "Ds18b20.h"

class ScaleManager {
public:
    struct Status {
        bool online = false;          // HX711 is answering
        bool stable = false;          // reading settled within the band
        bool haveReading = false;     // at least one full measurement taken

        long rawCounts = 0;
        float grossGrams = 0.0f;      // everything on the platform
        float uncompensatedGrams = 0.0f;
        float driftCorrectionGrams = 0.0f;

        bool temperatureValid = false;
        float temperatureC = 0.0f;

        float zeroCounts = 0.0f;
        float countsPerGram = SCALE_DEFAULT_COUNTS_PER_GRAM;
        float dryerTareGrams = 0.0f;
        float driftPerC = SCALE_TEMP_DRIFT_G_PER_C;
        float referenceC = SCALE_TEMP_REFERENCE_C;
        bool calibrated = false;      // span has been set by a real calibration

        uint32_t lastMeasurementMs = 0;
        uint32_t measurementCount = 0;
        uint32_t sampleErrorCount = 0;
    };

    bool begin();

    // Samples when the converter has data ready. Returns true when a new
    // measurement (a full median window) became available.
    bool loop(uint32_t nowMs);

    // Throws away the next `samples` readings. Called after NFC activity: the
    // reader's field driver is not something to average into a weight.
    void discard(uint8_t samples);

    const Status &status() const { return status_; }
    float gross() const { return status_.grossGrams; }
    bool stable() const { return status_.stable; }

    // Weight of everything except the dryer itself.
    float spoolGrams() const {
        return status_.grossGrams - status_.dryerTareGrams;
    }

    // Calibration steps, in the order they are performed. Each waits for a
    // stable reading and reports what it did through `message`.
    bool zeroPlatform(char *message, size_t messageLen);
    bool calibrateSpan(float knownMassGrams, char *message, size_t messageLen);
    bool tareDryer(char *message, size_t messageLen);

    // Sets the dryer tare directly, used by the sealed-spool cross-check where
    // the tag's own reference weights imply what the dryer must weigh.
    bool setDryerTare(float grams, char *message, size_t messageLen);

    void setDriftPerC(float gramsPerC);
    void setReferenceTemperature(float celsius);

    // Discards calibration, for when the hardware changed under the firmware.
    void resetCalibration();

    void selfTest(String &report);

private:
    // Which temperature sensor answered at startup, if either.
    enum class TempSensor : uint8_t { None, I2c, OneWire };

    // A DS18B20 reading arrives two visits later than it is asked for, so "no
    // reading" and "not yet" have to be different answers: the second one must
    // not throw away the temperature the drift compensation is still using.
    enum class TempRead : uint8_t { Pending, Ok, Failed };

    bool sampleReady() const;
    bool readSample(long &counts);
    bool waitForStable(uint32_t timeoutMs);
    void applyMeasurement(long median);
    void loadCalibration();
    void saveCalibration();
    TempRead readTemperature(float &celsius);
    bool readI2cTemperature(float &celsius);

    Status status_;
    long window_[SCALE_SAMPLES_PER_READING] = {0};
    uint8_t windowCount_ = 0;
    uint8_t discardRemaining_ = 0;

    float recent_[SCALE_STABLE_READINGS] = {0};
    uint8_t recentCount_ = 0;

    TempSensor temperatureSensor_ = TempSensor::None;
    uint8_t temperatureAddress_ = 0;
    Ds18b20 oneWire_;
    bool conversionRunning_ = false;
    uint32_t nextTemperatureMs_ = 0;
    uint32_t lastSampleAttemptMs_ = 0;
};
