#include "Settings.h"

#include <Preferences.h>

// secrets.h is git-ignored, so a fresh clone falls back to the template. That
// keeps the project compiling out of the box, with everything empty and the
// setup portal taking over.
#if __has_include("../secrets.h")
#include "../secrets.h"
#else
#include "../secrets.example.h"
#endif

// A secrets.h written before this setting existed is still a valid one: it is
// git-ignored, so an update never touches it.
#ifndef SECRET_SPOOLMAN_URL
#define SECRET_SPOOLMAN_URL ""
#endif

namespace {

constexpr char kNamespace[] = "optcfg";

// Copies a C string into a fixed buffer, truncating rather than overflowing.
void copyInto(char *destination, size_t capacity, const char *source) {
    snprintf(destination, capacity, "%s", source == nullptr ? "" : source);
}

void loadString(Preferences &preferences, const char *key, char *destination,
                size_t capacity, const char *fallback) {
    if (preferences.isKey(key)) {
        preferences.getString(key, destination, capacity);
        if (destination[0] != '\0') {
            return;
        }
    }
    copyInto(destination, capacity, fallback);
}

}  // namespace

void Settings::load(const char *fallbackId) {
    Preferences preferences;
    const bool opened = preferences.begin(kNamespace, true);

    if (opened) {
        loadString(preferences, "wifi_ssid", wifiSsid, sizeof(wifiSsid),
                   SECRET_WIFI_SSID);
        loadString(preferences, "wifi_pass", wifiPassword, sizeof(wifiPassword),
                   SECRET_WIFI_PASSWORD);
        loadString(preferences, "mqtt_host", mqttHost, sizeof(mqttHost),
                   SECRET_MQTT_HOST);
        mqttPort = static_cast<uint16_t>(
            preferences.getUInt("mqtt_port", SECRET_MQTT_PORT));
        loadString(preferences, "mqtt_user", mqttUser, sizeof(mqttUser),
                   SECRET_MQTT_USER);
        loadString(preferences, "mqtt_pass", mqttPassword, sizeof(mqttPassword),
                   SECRET_MQTT_PASSWORD);
        loadString(preferences, "workgroup", workgroup, sizeof(workgroup),
                   SECRET_WORKGROUP);
        loadString(preferences, "storage", storageLocation,
                   sizeof(storageLocation), "");
        loadString(preferences, "name", deviceName, sizeof(deviceName), "");
        loadString(preferences, "spoolman", spoolmanUrl, sizeof(spoolmanUrl),
                   SECRET_SPOOLMAN_URL);
        spoolmanPush = preferences.getBool("spoolman_push", true);
        calibrationMassGrams = preferences.getFloat("cal_mass", 1000.0f);
        autoWriteBack = preferences.getBool("auto_wb", true);
        writeMinDeltaGrams =
            preferences.getFloat("wb_delta", WRITEBACK_MIN_DELTA_G);
        writeMinIntervalMinutes =
            preferences.getUInt("wb_interval", WRITEBACK_MIN_INTERVAL_MS / 60000UL);
        preferences.end();
    } else {
        copyInto(wifiSsid, sizeof(wifiSsid), SECRET_WIFI_SSID);
        copyInto(wifiPassword, sizeof(wifiPassword), SECRET_WIFI_PASSWORD);
        copyInto(mqttHost, sizeof(mqttHost), SECRET_MQTT_HOST);
        mqttPort = SECRET_MQTT_PORT;
        copyInto(mqttUser, sizeof(mqttUser), SECRET_MQTT_USER);
        copyInto(mqttPassword, sizeof(mqttPassword), SECRET_MQTT_PASSWORD);
        copyInto(workgroup, sizeof(workgroup), SECRET_WORKGROUP);
        copyInto(spoolmanUrl, sizeof(spoolmanUrl), SECRET_SPOOLMAN_URL);
    }

    // A workgroup has to exist: it is what stops this device from overwriting
    // aux data written by somebody else's reader. Eight characters, so the
    // device id's tail is what distinguishes two of these on one network.
    if (workgroup[0] == '\0') {
        const size_t idLength = fallbackId == nullptr ? 0 : strlen(fallbackId);
        const char *tail = (idLength > 6) ? fallbackId + idLength - 6 : fallbackId;
        snprintf(workgroup, sizeof(workgroup), "d%s", tail == nullptr ? "1" : tail);
    }
    if (deviceName[0] == '\0') {
        snprintf(deviceName, sizeof(deviceName), "%s", DEVICE_DEFAULT_NAME);
    }
    if (mqttPort == 0) {
        mqttPort = 1883;
    }
}

void Settings::save() const {
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        return;
    }
    preferences.putString("wifi_ssid", wifiSsid);
    preferences.putString("wifi_pass", wifiPassword);
    preferences.putString("mqtt_host", mqttHost);
    preferences.putUInt("mqtt_port", mqttPort);
    preferences.putString("mqtt_user", mqttUser);
    preferences.putString("mqtt_pass", mqttPassword);
    preferences.putString("workgroup", workgroup);
    preferences.putString("storage", storageLocation);
    preferences.putString("name", deviceName);
    preferences.putString("spoolman", spoolmanUrl);
    preferences.putBool("spoolman_push", spoolmanPush);
    preferences.putFloat("cal_mass", calibrationMassGrams);
    preferences.putBool("auto_wb", autoWriteBack);
    preferences.putFloat("wb_delta", writeMinDeltaGrams);
    preferences.putUInt("wb_interval", writeMinIntervalMinutes);
    preferences.end();
}

void Settings::factoryReset() {
    Preferences preferences;
    if (preferences.begin(kNamespace, false)) {
        preferences.clear();
        preferences.end();
    }
}
