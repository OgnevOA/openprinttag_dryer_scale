// Host stand-in for WiFiClient. The fake HTTP client does the transport, so
// this only has to exist and accept a timeout.

#pragma once

#include <stdint.h>

class WiFiClient {
public:
    void setTimeout(uint32_t seconds) { (void)seconds; }
};
