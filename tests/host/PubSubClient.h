// Host stand-in for PubSubClient, backed by a fake broker the tests can read.
//
// It records what was published and subscribed, enforces the same buffer size
// limit as the real client, and can deliver an inbound message to the callback.
// That is enough to check the two things that actually matter about the MQTT
// layer: that Home Assistant receives well-formed discovery and state
// documents, and that a command from Home Assistant reaches the right code.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "Client.h"

namespace hostio {

struct MqttMessage {
    std::string topic;
    std::string payload;
    bool retained = false;
};

struct FakeBroker {
    bool acceptConnections = true;
    bool connected = false;
    int state = -1;  // MQTT_DISCONNECTED

    std::string clientId;
    std::string user;
    std::string password;
    std::string willTopic;
    std::string willPayload;

    std::vector<MqttMessage> published;
    std::vector<std::string> subscriptions;

    // Publishes larger than this fail, exactly as they do on the device.
    size_t bufferSize = 256;

    // Largest publish the client accepted, for reporting how close the
    // discovery payloads run to the limit.
    size_t largestPublish = 0;

    bool subscribed(const std::string &topic) const;
    const MqttMessage *find(const std::string &topic) const;
    size_t countWithPrefix(const std::string &prefix) const;
};

FakeBroker &broker();
void resetBroker();

// Hands a message to the client's callback, as a broker would.
void deliverToClient(const std::string &topic, const std::string &payload);

}  // namespace hostio

using MQTT_CALLBACK_SIGNATURE = void (*)(char *, uint8_t *, unsigned int);

class PubSubClient {
public:
    void setClient(Client &client);
    void setServer(const char *host, uint16_t port);
    void setCallback(MQTT_CALLBACK_SIGNATURE callback);
    bool setBufferSize(uint16_t size);

    bool connect(const char *clientId, const char *user, const char *password,
                 const char *willTopic, uint8_t willQos, bool willRetain,
                 const char *willMessage);
    bool connected();
    int state();
    void disconnect();

    bool publish(const char *topic, const char *payload, bool retained = false);
    bool publish(const char *topic, const uint8_t *payload, unsigned int length,
                 bool retained = false);
    bool subscribe(const char *topic);
    bool loop();

    void deliver(const char *topic, const uint8_t *payload, unsigned int length);

private:
    Client *client_ = nullptr;
    std::string host_;
    uint16_t port_ = 1883;
    MQTT_CALLBACK_SIGNATURE callback_ = nullptr;
};
