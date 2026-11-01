// Runtime settings, persisted in NVS.
//
// Every value has three possible sources, in increasing order of precedence:
// the defaults in config.h, the compile-time values in secrets.h, and whatever
// was last saved from the web portal. That ordering is what lets the same
// firmware image be flashed on a second device and configured through its own
// setup portal without recompiling.

#pragma once

#include <Arduino.h>

#include "../config.h"
#include "openprinttag/opt_tag.h"

class Settings {
public:
    char wifiSsid[33] = {0};
    char wifiPassword[65] = {0};

    char mqttHost[64] = {0};
    uint16_t mqttPort = 1883;
    char mqttUser[32] = {0};
    char mqttPassword[64] = {0};

    char deviceName[32] = {0};
    char workgroup[opt::kWorkgroupSize] = {0};
    char storageLocation[opt::kStorageLocationSize] = {0};

    // Empty disables the integration. Plain http only, and Spoolman's default
    // port is 7912: "http://10.0.0.5:7912".
    char spoolmanUrl[80] = {0};
    bool spoolmanPush = true;

    float calibrationMassGrams = 1000.0f;
    bool autoWriteBack = true;
    float writeMinDeltaGrams = WRITEBACK_MIN_DELTA_G;
    uint32_t writeMinIntervalMinutes = WRITEBACK_MIN_INTERVAL_MS / 60000UL;

    // Loads from NVS, filling anything unset from secrets.h. `fallbackId` is
    // used to invent a workgroup and device name on a first boot, so two of
    // these on one network do not collide.
    void load(const char *fallbackId);
    void save() const;

    // Clears NVS, for a device changing hands or a botched configuration.
    void factoryReset();

    bool haveWifi() const { return wifiSsid[0] != '\0'; }
    bool haveMqtt() const { return mqttHost[0] != '\0'; }
};
