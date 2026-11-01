// Host stand-in for the Arduino Client interface. Only enough of it exists for
// PubSubClient to hold a reference; nothing is ever actually transmitted.

#pragma once

#include <stddef.h>
#include <stdint.h>

class Client {
public:
    virtual ~Client() = default;
    virtual int connect(const char *host, uint16_t port);
    virtual size_t write(const uint8_t *buffer, size_t size);
    virtual int available();
    virtual int read();
    virtual void stop();
    virtual uint8_t connected();
};

// A client that always connects, for tests that only care about what the MQTT
// layer says rather than how it gets there.
class HostClient : public Client {};
