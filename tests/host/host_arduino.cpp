// Implementation of the host Arduino stubs, including the HX711 and LM75
// simulators that let the drivers be exercised without hardware.

#include "Arduino.h"

#include <map>

#include "Preferences.h"
#include "PubSubClient.h"
#include "SPI.h"
#include "Wire.h"

namespace hostio {
namespace {

uint32_t gNow = 0;
int gPinLevels[kPinCount];
int gPinModes[kPinCount];
Hx711Simulator gHx711;
Lm75Simulator gLm75;
bool gSerialVerbose = false;

}  // namespace

void reset() {
    gNow = 0;
    for (int i = 0; i < kPinCount; ++i) {
        gPinLevels[i] = LOW;
        gPinModes[i] = INPUT;
    }
    gHx711 = Hx711Simulator{};
    gLm75 = Lm75Simulator{};
    clearPreferences();
}

uint32_t now() { return gNow; }
void advance(uint32_t milliseconds) { gNow += milliseconds; }

void setPin(int pin, int level) {
    if (pin >= 0 && pin < kPinCount) {
        gPinLevels[pin] = level;
    }
}

int pinLevel(int pin) {
    return (pin >= 0 && pin < kPinCount) ? gPinLevels[pin] : LOW;
}

Hx711Simulator &hx711() { return gHx711; }
Lm75Simulator &lm75() { return gLm75; }

void attachHx711(int doutPin, int sckPin) {
    gHx711 = Hx711Simulator{};
    gHx711.attached = true;
    gHx711.doutPin = doutPin;
    gHx711.sckPin = sckPin;
}

void setHx711Value(long counts, bool ready) {
    gHx711.value = counts;
    gHx711.ready = ready;
    gHx711.pulses = 0;
}

void setSerialVerbose(bool verbose) { gSerialVerbose = verbose; }

bool serialVerbose() { return gSerialVerbose; }

}  // namespace hostio

void pinMode(int pin, int mode) {
    if (pin >= 0 && pin < hostio::kPinCount) {
        hostio::setPin(pin, hostio::pinLevel(pin));
    }
    (void)mode;
}

void digitalWrite(int pin, int level) {
    hostio::Hx711Simulator &converter = hostio::hx711();

    // A rising edge on SCK clocks the next bit out of the converter.
    if (converter.attached && pin == converter.sckPin) {
        const bool rising = hostio::pinLevel(pin) == LOW && level == HIGH;
        if (rising && converter.ready) {
            if (converter.pulses < 24) {
                converter.currentBit = static_cast<int>(
                    (converter.value >> (23 - converter.pulses)) & 1);
                ++converter.pulses;
            } else {
                // Pulse 25 and up select the gain for the next conversion; the
                // data line then goes high until that conversion finishes.
                ++converter.pulses;
                if (converter.pulses >= 25) {
                    converter.ready = false;
                    converter.pulses = 0;
                    ++converter.conversions;
                }
            }
        }
    }

    hostio::setPin(pin, level);
}

int digitalRead(int pin) {
    hostio::Hx711Simulator &converter = hostio::hx711();
    if (converter.attached && pin == converter.doutPin) {
        if (converter.stuckHigh) return HIGH;
        if (converter.stuckLow) return LOW;
        if (!converter.ready) {
            return HIGH;  // no conversion available
        }
        if (converter.pulses == 0) {
            // Data ready is signalled by pulling the line low; the first bit
            // only appears after the first clock pulse.
            return LOW;
        }
        return converter.currentBit;
    }
    return hostio::pinLevel(pin);
}

void HostSerial::emit(const String &text) {
    if (hostio::serialVerbose()) {
        fputs(text.c_str(), stdout);
    }
}

HostSerial Serial;

// ---------------------------------------------------------------------------
// Preferences: a std::map standing in for NVS.
// ---------------------------------------------------------------------------
namespace {

std::map<std::string, std::string> gStore;

std::string storeKey(const std::string &space, const char *key) {
    return space + "/" + (key ? key : "");
}

}  // namespace

namespace hostio {
void clearPreferences() { gStore.clear(); }
}  // namespace hostio

bool Preferences::begin(const char *name, bool readOnly) {
    namespace_ = name ? name : "";
    readOnly_ = readOnly;
    open_ = true;
    return true;
}

void Preferences::end() { open_ = false; }

bool Preferences::clear() {
    for (auto iterator = gStore.begin(); iterator != gStore.end();) {
        if (iterator->first.rfind(namespace_ + "/", 0) == 0) {
            iterator = gStore.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return true;
}

bool Preferences::remove(const char *key) {
    return gStore.erase(storeKey(namespace_, key)) > 0;
}

bool Preferences::isKey(const char *key) {
    return gStore.count(storeKey(namespace_, key)) > 0;
}

size_t Preferences::putFloat(const char *key, float value) {
    gStore[storeKey(namespace_, key)] = std::to_string(value);
    return sizeof(float);
}

float Preferences::getFloat(const char *key, float fallback) {
    const auto found = gStore.find(storeKey(namespace_, key));
    return found == gStore.end() ? fallback : strtof(found->second.c_str(), nullptr);
}

size_t Preferences::putBool(const char *key, bool value) {
    gStore[storeKey(namespace_, key)] = value ? "1" : "0";
    return 1;
}

bool Preferences::getBool(const char *key, bool fallback) {
    const auto found = gStore.find(storeKey(namespace_, key));
    return found == gStore.end() ? fallback : found->second == "1";
}

size_t Preferences::putUInt(const char *key, uint32_t value) {
    gStore[storeKey(namespace_, key)] = std::to_string(value);
    return sizeof(uint32_t);
}

uint32_t Preferences::getUInt(const char *key, uint32_t fallback) {
    const auto found = gStore.find(storeKey(namespace_, key));
    return found == gStore.end()
               ? fallback
               : static_cast<uint32_t>(strtoul(found->second.c_str(), nullptr, 10));
}

size_t Preferences::putString(const char *key, const char *value) {
    gStore[storeKey(namespace_, key)] = value ? value : "";
    return strlen(value ? value : "");
}

size_t Preferences::getString(const char *key, char *out, size_t maxLength) {
    const auto found = gStore.find(storeKey(namespace_, key));
    if (found == gStore.end() || maxLength == 0) {
        if (maxLength > 0) out[0] = '\0';
        return 0;
    }
    const size_t copied = found->second.copy(out, maxLength - 1);
    out[copied] = '\0';
    return copied;
}

// ---------------------------------------------------------------------------
// I2C: enough of a TwoWire to talk to the temperature sensor simulator.
// ---------------------------------------------------------------------------
void TwoWire::begin(int sda, int scl) {
    (void)sda;
    (void)scl;
}

void TwoWire::end() {}

void TwoWire::beginTransmission(uint8_t address) {
    address_ = address;
    written_ = 0;
}

size_t TwoWire::write(uint8_t value) {
    lastRegister_ = value;
    ++written_;
    return 1;
}

uint8_t TwoWire::endTransmission(bool) {
    hostio::Lm75Simulator &sensor = hostio::lm75();
    if (!sensor.attached || sensor.address != address_) {
        return 2;  // address NACK
    }
    return 0;
}

uint8_t TwoWire::requestFrom(uint8_t address, uint8_t quantity) {
    hostio::Lm75Simulator &sensor = hostio::lm75();
    readBuffer_.clear();
    readPosition_ = 0;
    if (!sensor.attached || sensor.address != address || sensor.failReads) {
        return 0;
    }

    // LM75/TMP102 register 0: 12-bit left aligned, 0.0625 C per LSB.
    const int16_t raw = static_cast<int16_t>(
        static_cast<int>(sensor.celsius / 0.0625f) << 4);
    readBuffer_.push_back(static_cast<uint8_t>(raw >> 8));
    readBuffer_.push_back(static_cast<uint8_t>(raw & 0xFF));
    while (readBuffer_.size() > quantity) {
        readBuffer_.pop_back();
    }
    return static_cast<uint8_t>(readBuffer_.size());
}

int TwoWire::read() {
    if (readPosition_ >= readBuffer_.size()) {
        return -1;
    }
    return readBuffer_[readPosition_++];
}

int TwoWire::available() {
    return static_cast<int>(readBuffer_.size() - readPosition_);
}

TwoWire Wire;

// ---------------------------------------------------------------------------
// SPI: accepts everything, returns 0xFF, which is what an absent PN5180 looks
// like. Exercising the reader against this is how the timeout paths get tested.
// ---------------------------------------------------------------------------
void SPIClass::begin(int, int, int, int) {}
void SPIClass::end() {}
void SPIClass::beginTransaction(SPISettings) {}
void SPIClass::endTransaction() {}
uint8_t SPIClass::transfer(uint8_t) { return 0xFF; }

SPIClass SPI;

// ---------------------------------------------------------------------------
// Network client: never used to move bytes, only to satisfy PubSubClient.
// ---------------------------------------------------------------------------
int Client::connect(const char *, uint16_t) { return 1; }
size_t Client::write(const uint8_t *, size_t size) { return size; }
int Client::available() { return 0; }
int Client::read() { return -1; }
void Client::stop() {}
uint8_t Client::connected() { return 1; }

// ---------------------------------------------------------------------------
// MQTT: a fake broker that records everything.
// ---------------------------------------------------------------------------
namespace hostio {
namespace {

FakeBroker gBroker;
PubSubClient *gLastClient = nullptr;

}  // namespace

FakeBroker &broker() { return gBroker; }

void resetBroker() {
    gBroker = FakeBroker{};
    gLastClient = nullptr;
}

void deliverToClient(const std::string &topic, const std::string &payload) {
    if (gLastClient != nullptr) {
        gLastClient->deliver(topic.c_str(),
                             reinterpret_cast<const uint8_t *>(payload.data()),
                             static_cast<unsigned int>(payload.size()));
    }
}

bool FakeBroker::subscribed(const std::string &topic) const {
    for (const std::string &candidate : subscriptions) {
        if (candidate == topic) {
            return true;
        }
    }
    return false;
}

const MqttMessage *FakeBroker::find(const std::string &topic) const {
    // Last write wins, which is what a retained topic looks like to a
    // subscriber that connects afterwards.
    const MqttMessage *found = nullptr;
    for (const MqttMessage &message : published) {
        if (message.topic == topic) {
            found = &message;
        }
    }
    return found;
}

size_t FakeBroker::countWithPrefix(const std::string &prefix) const {
    size_t count = 0;
    for (const MqttMessage &message : published) {
        if (message.topic.rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

}  // namespace hostio

void PubSubClient::setClient(Client &client) { client_ = &client; }

void PubSubClient::setServer(const char *host, uint16_t port) {
    host_ = host ? host : "";
    port_ = port;
}

void PubSubClient::setCallback(MQTT_CALLBACK_SIGNATURE callback) {
    callback_ = callback;
    hostio::gLastClient = this;
}

bool PubSubClient::setBufferSize(uint16_t size) {
    hostio::broker().bufferSize = size;
    return true;
}

bool PubSubClient::connect(const char *clientId, const char *user,
                           const char *password, const char *willTopic,
                           uint8_t, bool, const char *willMessage) {
    hostio::FakeBroker &state = hostio::broker();
    if (!state.acceptConnections || host_.empty()) {
        state.connected = false;
        state.state = -2;  // MQTT_CONNECT_FAILED
        return false;
    }
    state.connected = true;
    state.state = 0;
    state.clientId = clientId ? clientId : "";
    state.user = user ? user : "";
    state.password = password ? password : "";
    state.willTopic = willTopic ? willTopic : "";
    state.willPayload = willMessage ? willMessage : "";
    hostio::gLastClient = this;
    return true;
}

bool PubSubClient::connected() { return hostio::broker().connected; }
int PubSubClient::state() { return hostio::broker().state; }

void PubSubClient::disconnect() {
    hostio::broker().connected = false;
    hostio::broker().state = -1;
}

bool PubSubClient::publish(const char *topic, const char *payload,
                           bool retained) {
    return publish(topic, reinterpret_cast<const uint8_t *>(payload),
                   static_cast<unsigned int>(payload ? strlen(payload) : 0),
                   retained);
}

bool PubSubClient::publish(const char *topic, const uint8_t *payload,
                           unsigned int length, bool retained) {
    hostio::FakeBroker &state = hostio::broker();
    if (!state.connected) {
        return false;
    }
    // The real client refuses anything that will not fit its buffer, header
    // included. Getting that wrong is how discovery silently stops working.
    const size_t needed = 5 + 2 + strlen(topic) + length;
    if (needed > state.bufferSize) {
        return false;
    }
    if (needed > state.largestPublish) {
        state.largestPublish = needed;
    }
    hostio::MqttMessage message;
    message.topic = topic;
    message.payload.assign(reinterpret_cast<const char *>(payload), length);
    message.retained = retained;
    state.published.push_back(message);
    return true;
}

bool PubSubClient::subscribe(const char *topic) {
    hostio::FakeBroker &state = hostio::broker();
    if (!state.connected) {
        return false;
    }
    state.subscriptions.push_back(topic);
    return true;
}

bool PubSubClient::loop() { return hostio::broker().connected; }

void PubSubClient::deliver(const char *topic, const uint8_t *payload,
                           unsigned int length) {
    if (callback_ == nullptr) {
        return;
    }
    std::string topicCopy(topic);
    std::vector<uint8_t> payloadCopy(payload, payload + length);
    callback_(&topicCopy[0], payloadCopy.data(), length);
}
