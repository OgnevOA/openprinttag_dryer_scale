// Home Assistant integration over MQTT discovery.
//
// One retained discovery message per entity, then a single JSON state document
// that every entity reads with a value_template. That keeps the traffic to one
// publish per update instead of thirty, and means a reconnecting broker only has
// to replay one payload to bring the whole device back.
//
// A key missing from the state document renders as "unknown" in Home Assistant,
// which is exactly right for, say, remaining length on a tag that carries no
// length: better an honest gap than a fabricated number.
//
// The JSON is assembled with snprintf rather than a JSON library. The shape is
// fixed and small, and the dependency is not worth the flash on a board that
// also has to hold Wi-Fi, a web server and two OTA slots.

#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

#include <functional>

#include "../config.h"
#include "NfcManager.h"
#include "ScaleManager.h"
#include "SpoolState.h"
#include "WriteBackPolicy.h"

class HaMqtt {
public:
    struct Config {
        char host[64] = {0};
        uint16_t port = 1883;
        char user[32] = {0};
        char password[64] = {0};
        char baseTopic[32] = MQTT_BASE_TOPIC;
        char discoveryPrefix[24] = MQTT_DISCOVERY_PREFIX;
    };

    // Everything the device can be told to do from Home Assistant. Each returns
    // a message that is published back as the last command result, so a button
    // press that failed says why instead of silently doing nothing.
    struct Callbacks {
        std::function<String()> onTare;
        std::function<String()> onCalibrate;
        std::function<String(float)> onCalibrationMass;
        std::function<String()> onWriteNow;
        std::function<String(bool)> onAutoWriteBack;
        std::function<String()> onWipeAux;
        std::function<String(const String &)> onStorageLocation;
        std::function<String()> onRefresh;
        std::function<String(bool)> onPollEnabled;
    };

    // `client` is the network transport; `deviceId` must be stable across
    // reboots because Home Assistant keys entity identity off it.
    void begin(Client &client, const Config &config, const char *deviceId,
               const char *deviceName, const Callbacks &callbacks);

    void setConfig(const Config &config);
    const Config &config() const { return config_; }

    // Reconnects when needed and services the client. Returns true while
    // connected.
    bool loop(uint32_t nowMs);

    bool connected() const { return client_ != nullptr && client_->connected(); }
    const char *lastError() const { return lastError_; }
    uint32_t publishCount() const { return publishCount_; }

    // What goes into the state document. Gathered by the sketch, which is the
    // only thing that sees all of the pieces.
    struct StateInput {
        const NfcManager::Status *nfc = nullptr;
        const opt::OptTag *tag = nullptr;
        const ScaleManager::Status *scale = nullptr;
        const SpoolState::State *spool = nullptr;
        const WriteBackPolicy *policy = nullptr;
        const char *writeStatus = "";
        const char *lastCommand = "";
        const char *storageLocation = "";
        float calibrationMass = 0.0f;
        uint32_t nowMs = 0;
    };

    // Publishes the state document. The sketch decides when: on any change, and
    // otherwise on a heartbeat.
    void publishState(const StateInput &input);

    // True when a command arrived and its result is waiting to be published.
    // Clears on read.
    bool takePublishRequest();

    // The result of the most recent command, for the state document and the
    // web portal.
    const char *lastCommandResult() const { return lastCommand_; }

    // Re-sends every discovery message, for when Home Assistant was restarted
    // with its MQTT integration cleared.
    void republishDiscovery();

    // Builds the state document. Static and free of any network dependency so
    // the desktop tests can check exactly what Home Assistant will receive.
    static size_t buildStateJson(const StateInput &input, char *out,
                                 size_t outCap);

    void topicFor(const char *suffix, char *out, size_t outCap) const;

private:
    struct Entity {
        const char *component;   // sensor, binary_sensor, button, ...
        const char *objectId;
        const char *name;
        const char *jsonKey;     // nullptr for command-only entities
        const char *unit;
        const char *deviceClass;
        const char *stateClass;
        const char *icon;
        const char *commandSuffix;  // nullptr for read-only entities
        const char *extra;          // component-specific JSON fragment
        bool diagnostic;
    };

    static const Entity kEntities[];
    static const size_t kEntityCount;

    void publishDiscovery(const Entity &entity);
    void subscribeCommands();
    void handleMessage(const char *topic, const uint8_t *payload, size_t len);
    bool reconnect(uint32_t nowMs);
    void appendDeviceBlock(String &json) const;

    Client *client_ = nullptr;
    PubSubClient *mqtt_ = nullptr;
    PubSubClient owned_;
    Config config_;
    Callbacks callbacks_;

    char deviceId_[24] = {0};
    char deviceName_[48] = {0};
    char availabilityTopic_[96] = {0};
    char stateTopic_[96] = {0};
    char commandPrefix_[96] = {0};
    char lastError_[64] = {0};
    char lastCommand_[96] = {0};

    bool publishRequested_ = false;
    bool discoverySent_ = false;
    uint32_t nextConnectAttemptMs_ = 0;
    uint32_t publishCount_ = 0;
};
