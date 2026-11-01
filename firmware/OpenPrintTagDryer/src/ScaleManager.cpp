#include "ScaleManager.h"

#include <Preferences.h>
#include <Wire.h>

namespace {

constexpr char kNamespace[] = "optscale";
constexpr uint32_t kTemperatureIntervalMs = 5000;

// The HX711 powers down if SCK stays high for more than 60 us, so the 25-pulse
// burst runs with interrupts off. It takes about 50 us.
constexpr uint32_t kPulseUs = 1;

// No data at all for this long means the converter is not there.
constexpr uint32_t kOfflineTimeoutMs = 2000;

long median(const long *values, uint8_t count) {
    long sorted[SCALE_SAMPLES_PER_READING];
    for (uint8_t i = 0; i < count; ++i) {
        sorted[i] = values[i];
    }
    for (uint8_t i = 1; i < count; ++i) {
        const long key = sorted[i];
        int8_t j = static_cast<int8_t>(i) - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }
    return sorted[count / 2];
}

}  // namespace

bool ScaleManager::begin() {
    pinMode(PIN_HX711_SCK, OUTPUT);
    pinMode(PIN_HX711_DOUT, INPUT);
    digitalWrite(PIN_HX711_SCK, LOW);

    loadCalibration();

    // I2C first, then the one-wire bus on the same pin. Probing in this order
    // rather than the other way round is deliberate: an I2C sensor cannot be
    // mistaken for a DS18B20, because it never sees a valid bus reset and so
    // never answers with a presence pulse, whereas scanning I2C addresses on a
    // line with a DS18B20 on it is equally harmless.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    for (uint8_t address = TEMP_SENSOR_FIRST_ADDRESS;
         address <= TEMP_SENSOR_LAST_ADDRESS; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            temperatureAddress_ = address;
            temperatureSensor_ = TempSensor::I2c;
            break;
        }
    }

    if (temperatureSensor_ == TempSensor::None) {
        // The I2C peripheral has to let go of the pin before it can be driven
        // by hand.
        Wire.end();
        oneWire_.begin(PIN_ONEWIRE);
        if (oneWire_.detect()) {
            temperatureSensor_ = TempSensor::OneWire;
        }
    }

    // The HX711 signals a ready conversion by pulling DOUT low, which takes up
    // to 400 ms after power-up.
    const uint32_t deadline = millis() + 500;
    while (!sampleReady()) {
        if (static_cast<int32_t>(millis() - deadline) >= 0) {
            status_.online = false;
            return false;
        }
        delay(10);
    }

    long counts = 0;
    status_.online = readSample(counts);
    return status_.online;
}

bool ScaleManager::sampleReady() const {
    return digitalRead(PIN_HX711_DOUT) == LOW;
}

bool ScaleManager::readSample(long &counts) {
    if (!sampleReady()) {
        return false;
    }

    uint32_t value = 0;
    noInterrupts();
    for (uint8_t bit = 0; bit < 24; ++bit) {
        digitalWrite(PIN_HX711_SCK, HIGH);
        delayMicroseconds(kPulseUs);
        value = (value << 1) | (digitalRead(PIN_HX711_DOUT) == HIGH ? 1u : 0u);
        digitalWrite(PIN_HX711_SCK, LOW);
        delayMicroseconds(kPulseUs);
    }
    // A 25th pulse selects channel A at gain 128 for the next conversion.
    digitalWrite(PIN_HX711_SCK, HIGH);
    delayMicroseconds(kPulseUs);
    digitalWrite(PIN_HX711_SCK, LOW);
    delayMicroseconds(kPulseUs);
    interrupts();

    // Sign extend the 24-bit two's complement result.
    if (value & 0x800000u) {
        value |= 0xFF000000u;
    }
    counts = static_cast<long>(static_cast<int32_t>(value));

    // All ones or all zeros means the data line is stuck, not that the load is
    // extreme.
    if (counts == 0 || value == 0xFFFFFFFFu) {
        ++status_.sampleErrorCount;
        return false;
    }
    return true;
}

void ScaleManager::discard(uint8_t samples) {
    discardRemaining_ = samples;
    windowCount_ = 0;
}

bool ScaleManager::loop(uint32_t nowMs) {
    if (temperatureSensor_ != TempSensor::None &&
        static_cast<int32_t>(nowMs - nextTemperatureMs_) >= 0) {
        float celsius = 0.0f;
        switch (readTemperature(celsius)) {
            case TempRead::Ok:
                status_.temperatureC = celsius;
                status_.temperatureValid = true;
                nextTemperatureMs_ = nowMs + kTemperatureIntervalMs;
                break;
            case TempRead::Failed:
                status_.temperatureValid = false;
                nextTemperatureMs_ = nowMs + kTemperatureIntervalMs;
                break;
            case TempRead::Pending:
                // A conversion is now running. Come back for it as soon as it is
                // done rather than at the next poll, so the temperature paired
                // with a weight is never a whole interval old.
                nextTemperatureMs_ = nowMs + ds18b20::kConversionMs + 20;
                break;
        }
    }

    if (!sampleReady()) {
        if (status_.online && lastSampleAttemptMs_ != 0 &&
            static_cast<int32_t>(nowMs - lastSampleAttemptMs_) >
                static_cast<int32_t>(kOfflineTimeoutMs)) {
            status_.online = false;
        }
        return false;
    }

    long counts = 0;
    if (!readSample(counts)) {
        return false;
    }
    lastSampleAttemptMs_ = nowMs;
    status_.online = true;

    if (discardRemaining_ > 0) {
        --discardRemaining_;
        return false;
    }

    window_[windowCount_++] = counts;
    if (windowCount_ < SCALE_SAMPLES_PER_READING) {
        return false;
    }
    windowCount_ = 0;

    applyMeasurement(median(window_, SCALE_SAMPLES_PER_READING));
    status_.lastMeasurementMs = nowMs;
    ++status_.measurementCount;
    return true;
}

void ScaleManager::applyMeasurement(long medianCounts) {
    status_.rawCounts = medianCounts;

    const float span = status_.countsPerGram != 0.0f ? status_.countsPerGram
                                                     : 1.0f;
    status_.uncompensatedGrams =
        (static_cast<float>(medianCounts) - status_.zeroCounts) / span;

    status_.driftCorrectionGrams = 0.0f;
    if (status_.temperatureValid && status_.driftPerC != 0.0f) {
        status_.driftCorrectionGrams =
            status_.driftPerC * (status_.temperatureC - status_.referenceC);
    }
    status_.grossGrams =
        status_.uncompensatedGrams - status_.driftCorrectionGrams;
    status_.haveReading = true;

    // Stability: the last few measurements all inside a narrow band. This is
    // what gates every write to a tag.
    if (recentCount_ < SCALE_STABLE_READINGS) {
        recent_[recentCount_++] = status_.grossGrams;
    } else {
        for (uint8_t i = 1; i < SCALE_STABLE_READINGS; ++i) {
            recent_[i - 1] = recent_[i];
        }
        recent_[SCALE_STABLE_READINGS - 1] = status_.grossGrams;
    }

    if (recentCount_ < SCALE_STABLE_READINGS) {
        status_.stable = false;
        return;
    }
    float lowest = recent_[0];
    float highest = recent_[0];
    for (uint8_t i = 1; i < SCALE_STABLE_READINGS; ++i) {
        if (recent_[i] < lowest) lowest = recent_[i];
        if (recent_[i] > highest) highest = recent_[i];
    }
    status_.stable = (highest - lowest) <= SCALE_STABLE_BAND_G;
}

bool ScaleManager::waitForStable(uint32_t timeoutMs) {
    const uint32_t deadline = millis() + timeoutMs;
    while (static_cast<int32_t>(millis() - deadline) < 0) {
        loop(millis());
        if (status_.stable) {
            return true;
        }
        delay(5);
    }
    return false;
}

bool ScaleManager::zeroPlatform(char *message, size_t messageLen) {
    if (!waitForStable(8000)) {
        snprintf(message, messageLen,
                 "reading never settled; check the load cell wiring");
        return false;
    }
    status_.zeroCounts = static_cast<float>(status_.rawCounts);
    status_.dryerTareGrams = 0.0f;
    saveCalibration();
    applyMeasurement(status_.rawCounts);
    snprintf(message, messageLen, "platform zeroed at %ld counts",
             status_.rawCounts);
    return true;
}

bool ScaleManager::calibrateSpan(float knownMassGrams, char *message,
                                 size_t messageLen) {
    if (knownMassGrams < 50.0f) {
        snprintf(message, messageLen,
                 "use a calibration mass of at least 50 g");
        return false;
    }
    if (!waitForStable(8000)) {
        snprintf(message, messageLen, "reading never settled");
        return false;
    }

    const float delta = static_cast<float>(status_.rawCounts) -
                        status_.zeroCounts;
    if (fabsf(delta) < 1000.0f) {
        snprintf(message, messageLen,
                 "only %.0f counts of change: is the mass on the platform, and "
                 "was the platform zeroed first?",
                 static_cast<double>(delta));
        return false;
    }

    status_.countsPerGram = delta / knownMassGrams;
    status_.calibrated = true;
    saveCalibration();
    applyMeasurement(status_.rawCounts);
    snprintf(message, messageLen, "%.2f counts per gram from a %.0f g mass",
             static_cast<double>(status_.countsPerGram),
             static_cast<double>(knownMassGrams));
    return true;
}

bool ScaleManager::tareDryer(char *message, size_t messageLen) {
    if (!status_.calibrated) {
        snprintf(message, messageLen,
                 "calibrate the span with a known mass first");
        return false;
    }
    if (!waitForStable(8000)) {
        snprintf(message, messageLen, "reading never settled");
        return false;
    }
    status_.dryerTareGrams = status_.grossGrams;
    saveCalibration();
    snprintf(message, messageLen, "dryer tare stored as %.1f g",
             static_cast<double>(status_.dryerTareGrams));
    return true;
}

bool ScaleManager::setDryerTare(float grams, char *message,
                                size_t messageLen) {
    if (grams < 100.0f || grams > 20000.0f) {
        snprintf(message, messageLen, "%.1f g is not a plausible dryer weight",
                 static_cast<double>(grams));
        return false;
    }
    status_.dryerTareGrams = grams;
    saveCalibration();
    snprintf(message, messageLen, "dryer tare set to %.1f g",
             static_cast<double>(grams));
    return true;
}

void ScaleManager::setDriftPerC(float gramsPerC) {
    status_.driftPerC = gramsPerC;
    saveCalibration();
}

void ScaleManager::setReferenceTemperature(float celsius) {
    status_.referenceC = celsius;
    saveCalibration();
}

void ScaleManager::resetCalibration() {
    status_.zeroCounts = 0.0f;
    status_.countsPerGram = SCALE_DEFAULT_COUNTS_PER_GRAM;
    status_.dryerTareGrams = 0.0f;
    status_.driftPerC = SCALE_TEMP_DRIFT_G_PER_C;
    status_.referenceC = SCALE_TEMP_REFERENCE_C;
    status_.calibrated = false;
    saveCalibration();
}

void ScaleManager::loadCalibration() {
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) {
        return;
    }
    status_.zeroCounts = preferences.getFloat("zero", status_.zeroCounts);
    status_.countsPerGram =
        preferences.getFloat("cpg", status_.countsPerGram);
    status_.dryerTareGrams =
        preferences.getFloat("tare", status_.dryerTareGrams);
    status_.driftPerC = preferences.getFloat("driftk", status_.driftPerC);
    status_.referenceC = preferences.getFloat("tref", status_.referenceC);
    status_.calibrated = preferences.getBool("cal", false);
    preferences.end();
}

void ScaleManager::saveCalibration() {
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        return;
    }
    preferences.putFloat("zero", status_.zeroCounts);
    preferences.putFloat("cpg", status_.countsPerGram);
    preferences.putFloat("tare", status_.dryerTareGrams);
    preferences.putFloat("driftk", status_.driftPerC);
    preferences.putFloat("tref", status_.referenceC);
    preferences.putBool("cal", status_.calibrated);
    preferences.end();
}

ScaleManager::TempRead ScaleManager::readTemperature(float &celsius) {
    switch (temperatureSensor_) {
        case TempSensor::I2c:
            return readI2cTemperature(celsius) ? TempRead::Ok : TempRead::Failed;

        case TempSensor::OneWire:
            // Two visits per reading: ask, then collect. Conversion is cleared
            // before the read either way, so a sensor that stops answering
            // starts a fresh conversion next time rather than waiting forever
            // for one that was never accepted.
            if (!conversionRunning_) {
                conversionRunning_ = oneWire_.startConversion();
                return conversionRunning_ ? TempRead::Pending : TempRead::Failed;
            }
            conversionRunning_ = false;
            return oneWire_.readConversion(celsius) ? TempRead::Ok
                                                   : TempRead::Failed;

        case TempSensor::None:
            break;
    }
    return TempRead::Failed;
}

bool ScaleManager::readI2cTemperature(float &celsius) {
    if (temperatureAddress_ == 0) {
        return false;
    }

    // LM75 and TMP102 both expose the temperature as two big-endian bytes in
    // register 0, left aligned, 0.0625 C per LSB.
    Wire.beginTransmission(temperatureAddress_);
    Wire.write(static_cast<uint8_t>(0x00));
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(temperatureAddress_, static_cast<uint8_t>(2)) != 2) {
        return false;
    }
    const uint8_t high = static_cast<uint8_t>(Wire.read());
    const uint8_t low = static_cast<uint8_t>(Wire.read());
    const int16_t raw = static_cast<int16_t>((high << 8) | low);
    celsius = static_cast<float>(raw >> 4) * 0.0625f;

    return celsius > -40.0f && celsius < 125.0f;
}

void ScaleManager::selfTest(String &report) {
    report += F("HX711 scale\n");
    report += F("  converter: ");
    report += status_.online ? F("responding\n") : F("NOT RESPONDING\n");
    report += F("  raw counts: ");
    report += status_.rawCounts;
    report += '\n';
    report += F("  span: ");
    report += String(status_.countsPerGram, 3);
    report += F(" counts/g");
    report += status_.calibrated ? F(" (calibrated)\n") : F(" (DEFAULT)\n");
    report += F("  zero: ");
    report += String(status_.zeroCounts, 0);
    report += F(" counts\n");
    report += F("  dryer tare: ");
    report += String(status_.dryerTareGrams, 1);
    report += F(" g\n");
    report += F("  gross now: ");
    report += String(status_.grossGrams, 1);
    report += F(" g, ");
    report += status_.stable ? F("stable\n") : F("unsettled\n");
    report += F("  temperature sensor: ");
    if (temperatureSensor_ == TempSensor::None) {
        report += F("none found, on I2C or one-wire\n");
    } else {
        if (temperatureSensor_ == TempSensor::I2c) {
            report += F("I2C 0x");
            report += String(temperatureAddress_, 16);
        } else {
            report += F("DS18B20 on one-wire");
        }
        report += F(", ");
        report += status_.temperatureValid ? String(status_.temperatureC, 2)
                                          : String("no reading");
        report += F(" C\n");
        report += F("  drift coefficient: ");
        report += String(status_.driftPerC, 3);
        report += F(" g/C around ");
        report += String(status_.referenceC, 1);
        report += F(" C\n");
    }
}
