#include "HaMqtt.h"

#include <math.h>

#include "openprinttag/opt_enums.h"

namespace {

HaMqtt *gInstance = nullptr;

constexpr uint32_t kReconnectIntervalMs = MQTT_RECONNECT_INTERVAL_MS;
constexpr size_t kStateJsonCapacity = 1024;
constexpr uint16_t kMqttBufferSize = 1200;   // discovery payloads are the big ones

// Appends `"key":value,` only when the value exists. A key left out renders as
// unknown in Home Assistant, which is what a missing measurement is.
void appendNumber(char *out, size_t cap, size_t &len, const char *key,
                  float value, int decimals, bool present) {
    if (!present || isnan(value)) {
        return;
    }
    len += snprintf(out + len, len < cap ? cap - len : 0, "\"%s\":%.*f,", key,
                    decimals, static_cast<double>(value));
}

void appendInt(char *out, size_t cap, size_t &len, const char *key, long value,
               bool present) {
    if (!present) {
        return;
    }
    len += snprintf(out + len, len < cap ? cap - len : 0, "\"%s\":%ld,", key,
                    value);
}

void appendBool(char *out, size_t cap, size_t &len, const char *key,
                bool value) {
    len += snprintf(out + len, len < cap ? cap - len : 0, "\"%s\":\"%s\",", key,
                    value ? "ON" : "OFF");
}

// JSON string escaping, for the handful of fields that come off a tag and could
// contain anything a filament vendor felt like writing.
void appendString(char *out, size_t cap, size_t &len, const char *key,
                  const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return;
    }
    len += snprintf(out + len, len < cap ? cap - len : 0, "\"%s\":\"", key);
    for (const char *p = value; *p != '\0'; ++p) {
        const char c = *p;
        const char *escape = nullptr;
        switch (c) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape != nullptr) {
            len += snprintf(out + len, len < cap ? cap - len : 0, "%s", escape);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            len += snprintf(out + len, len < cap ? cap - len : 0, "\\u%04x",
                            static_cast<unsigned>(c));
        } else {
            len += snprintf(out + len, len < cap ? cap - len : 0, "%c", c);
        }
    }
    len += snprintf(out + len, len < cap ? cap - len : 0, "\",");
}

// Seconds since an event, or absent when it has not happened yet.
void appendAge(char *out, size_t cap, size_t &len, const char *key,
               uint32_t eventMs, uint32_t nowMs) {
    if (eventMs == 0) {
        return;
    }
    appendInt(out, cap, len, key, static_cast<long>((nowMs - eventMs) / 1000),
              true);
}

void appendColorHex(char *out, size_t cap, size_t &len, const char *key,
                    const opt::OptMainData &main) {
    if (!main.hasPrimaryColor) {
        return;
    }
    char hex[10];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", main.primaryColor[0],
             main.primaryColor[1], main.primaryColor[2]);
    appendString(out, cap, len, key, hex);
}

}  // namespace

// ---------------------------------------------------------------------------
// The entity table. Order is the order Home Assistant first sees them, which is
// also the order they appear on the device page.
// ---------------------------------------------------------------------------
const HaMqtt::Entity HaMqtt::kEntities[] = {
    // Spool identity.
    {"sensor", "brand", "Brand", "brand", nullptr, nullptr, nullptr,
     "mdi:factory", nullptr, nullptr, false},
    {"sensor", "material_type", "Material type", "material_type", nullptr,
     nullptr, nullptr, "mdi:shape-outline", nullptr, nullptr, false},
    {"sensor", "material_name", "Material name", "material_name", nullptr,
     nullptr, nullptr, "mdi:tag-text-outline", nullptr, nullptr, false},
    {"sensor", "color", "Colour", "color", nullptr, nullptr, nullptr,
     "mdi:palette", nullptr, nullptr, false},
    {"sensor", "spool_id", "Spool ID", "spool_id", nullptr, nullptr, nullptr,
     "mdi:barcode", nullptr, nullptr, false},
    {"sensor", "tag_uid", "Tag UID", "tag_uid", nullptr, nullptr, nullptr,
     "mdi:nfc-variant", nullptr, nullptr, true},

    // Weights. The two the user actually looks at come first.
    {"sensor", "remaining_weight", "Remaining filament", "remaining_g", "g",
     "weight", "measurement", "mdi:printer-3d-nozzle", nullptr, nullptr, false},
    {"sensor", "remaining_percent", "Remaining", "remaining_pct", "%", nullptr,
     "measurement", "mdi:gauge", nullptr, nullptr, false},
    {"sensor", "remaining_length", "Remaining length", "remaining_m", "m",
     "distance", "measurement", "mdi:tape-measure", nullptr, nullptr, false},
    {"sensor", "consumed_weight", "Consumed filament", "consumed_g", "g",
     "weight", "measurement", "mdi:printer-3d", nullptr, nullptr, false},
    {"sensor", "gross_weight", "Gross weight", "gross_g", "g", "weight",
     "measurement", "mdi:scale", nullptr, nullptr, false},
    {"sensor", "tag_consumed_weight", "Consumed weight on tag", "tag_consumed_g",
     "g", "weight", "measurement", "mdi:tag-check-outline", nullptr, nullptr,
     true},
    {"sensor", "reference_weight", "Reference full weight", "reference_g", "g",
     "weight", nullptr, "mdi:weight-gram", nullptr, nullptr, true},
    {"sensor", "empty_spool_weight", "Empty spool weight", "empty_g", "g",
     "weight", nullptr, "mdi:weight-gram", nullptr, nullptr, true},

    // What the tag recommends. These are what HA automations act on.
    {"sensor", "print_temp", "Print temperature", "print_temp", nullptr, nullptr,
     nullptr, "mdi:thermometer", nullptr, nullptr, false},
    {"sensor", "drying_temp", "Recommended drying temperature", "drying_c",
     "\\u00b0C", "temperature", nullptr, "mdi:air-filter", nullptr, nullptr,
     false},
    {"sensor", "drying_time", "Recommended drying time", "drying_min", "min",
     "duration", nullptr, "mdi:timer-outline", nullptr, nullptr, false},

    // Hardware and housekeeping.
    {"sensor", "cell_temperature", "Load cell temperature", "cell_c",
     "\\u00b0C", "temperature", "measurement", "mdi:thermometer", nullptr,
     nullptr, true},
    {"sensor", "last_read", "Last tag read", "last_read_s", "s", "duration",
     nullptr, "mdi:history", nullptr, nullptr, true},
    {"sensor", "last_write", "Last tag write", "last_write_s", "s", "duration",
     nullptr, "mdi:content-save-outline", nullptr, nullptr, true},
    {"sensor", "write_status", "Write-back status", "write_status", nullptr,
     nullptr, nullptr, "mdi:pencil-outline", nullptr, nullptr, true},
    {"sensor", "error", "Error", "error", nullptr, nullptr, nullptr,
     "mdi:alert-circle-outline", nullptr, nullptr, true},
    {"sensor", "last_command", "Last command result", "last_command", nullptr,
     nullptr, nullptr, "mdi:console", nullptr, nullptr, true},
    {"sensor", "blocks_written", "Tag blocks written", "blocks_written", nullptr,
     nullptr, "total_increasing", "mdi:counter", nullptr, nullptr, true},

    // Binary sensors.
    {"binary_sensor", "tag_present", "Tag present", "tag_present", nullptr,
     nullptr, nullptr, "mdi:nfc", nullptr,
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"", false},
    {"binary_sensor", "weight_stable", "Weight stable", "stable", nullptr,
     nullptr, nullptr, "mdi:scale-balance", nullptr,
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"", true},
    {"binary_sensor", "reader_online", "Reader online", "reader_online", nullptr,
     "connectivity", nullptr, nullptr, nullptr,
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"", true},
    {"binary_sensor", "scale_online", "Scale online", "scale_online", nullptr,
     "connectivity", nullptr, nullptr, nullptr,
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"", true},
    {"binary_sensor", "tag_disagrees", "Tag disagrees with scale", "disagrees",
     nullptr, "problem", nullptr, nullptr, nullptr,
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\"", true},

    // Controls.
    {"button", "tare", "Tare empty dryer", nullptr, nullptr, nullptr, nullptr,
     "mdi:scale-off", "tare", "\"payload_press\":\"PRESS\"", false},
    {"number", "calibration_mass", "Calibration mass", "calibration_mass", "g",
     "weight", nullptr, "mdi:weight", "calibration_mass",
     "\"min\":50,\"max\":5000,\"step\":1,\"mode\":\"box\"", false},
    {"button", "calibrate", "Calibrate with mass", nullptr, nullptr, nullptr,
     nullptr, "mdi:tune", "calibrate", "\"payload_press\":\"PRESS\"", false},
    {"button", "write_tag", "Write tag now", nullptr, nullptr, nullptr, nullptr,
     "mdi:content-save-edit-outline", "write_now",
     "\"payload_press\":\"PRESS\"", false},
    {"switch", "auto_writeback", "Automatic write-back", "auto_writeback",
     nullptr, nullptr, nullptr, "mdi:sync", "auto_writeback",
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"state_on\":\"ON\","
     "\"state_off\":\"OFF\"",
     false},
    {"switch", "polling", "Tag polling", "polling", nullptr, nullptr, nullptr,
     "mdi:radar", "poll_enabled",
     "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"state_on\":\"ON\","
     "\"state_off\":\"OFF\"",
     true},
    {"button", "refresh", "Re-read tag", nullptr, nullptr, nullptr, nullptr,
     "mdi:refresh", "refresh", "\"payload_press\":\"PRESS\"", true},
    {"button", "wipe_aux", "Wipe auxiliary region", nullptr, nullptr, nullptr,
     nullptr, "mdi:eraser", "wipe_aux", "\"payload_press\":\"PRESS\"", true},
    {"text", "storage_location", "Storage location", "storage_location", nullptr,
     nullptr, nullptr, "mdi:map-marker-outline", "storage_location",
     "\"max\":8", false},
};

const size_t HaMqtt::kEntityCount =
    sizeof(HaMqtt::kEntities) / sizeof(HaMqtt::kEntities[0]);

// ---------------------------------------------------------------------------

void HaMqtt::begin(Client &client, const Config &config, const char *deviceId,
                   const char *deviceName, const Callbacks &callbacks) {
    client_ = &client;
    config_ = config;
    callbacks_ = callbacks;
    snprintf(deviceId_, sizeof(deviceId_), "%s", deviceId);
    snprintf(deviceName_, sizeof(deviceName_), "%s", deviceName);

    owned_.setClient(client);
    mqtt_ = &owned_;
    mqtt_->setBufferSize(kMqttBufferSize);
    mqtt_->setServer(config_.host, config_.port);

    snprintf(availabilityTopic_, sizeof(availabilityTopic_), "%s/%s/status",
             config_.baseTopic, deviceId_);
    snprintf(stateTopic_, sizeof(stateTopic_), "%s/%s/state", config_.baseTopic,
             deviceId_);
    snprintf(commandPrefix_, sizeof(commandPrefix_), "%s/%s/cmd",
             config_.baseTopic, deviceId_);

    // PubSubClient's callback is a plain function pointer with nowhere to put a
    // `this`, so it goes through the file-scope pointer above. One device, one
    // broker connection, so there is nothing to disambiguate.
    gInstance = this;
    mqtt_->setCallback([](char *topic, uint8_t *payload, unsigned int length) {
        if (gInstance != nullptr) {
            gInstance->handleMessage(topic, payload, length);
        }
    });
}

void HaMqtt::setConfig(const Config &config) {
    config_ = config;
    if (mqtt_ != nullptr) {
        mqtt_->setServer(config_.host, config_.port);
    }
    snprintf(availabilityTopic_, sizeof(availabilityTopic_), "%s/%s/status",
             config_.baseTopic, deviceId_);
    snprintf(stateTopic_, sizeof(stateTopic_), "%s/%s/state", config_.baseTopic,
             deviceId_);
    snprintf(commandPrefix_, sizeof(commandPrefix_), "%s/%s/cmd",
             config_.baseTopic, deviceId_);
    discoverySent_ = false;
}

void HaMqtt::topicFor(const char *suffix, char *out, size_t outCap) const {
    snprintf(out, outCap, "%s/%s", commandPrefix_, suffix);
}

bool HaMqtt::loop(uint32_t nowMs) {
    if (mqtt_ == nullptr || config_.host[0] == '\0') {
        return false;
    }
    if (!mqtt_->connected()) {
        if (static_cast<int32_t>(nowMs - nextConnectAttemptMs_) < 0) {
            return false;
        }
        nextConnectAttemptMs_ = nowMs + kReconnectIntervalMs;
        return reconnect(nowMs);
    }
    mqtt_->loop();
    return true;
}

bool HaMqtt::reconnect(uint32_t nowMs) {
    (void)nowMs;

    // The will makes the device show as unavailable if it drops off without
    // saying goodbye, which is the normal case for a power cut.
    const bool ok = mqtt_->connect(
        deviceId_, config_.user[0] ? config_.user : nullptr,
        config_.user[0] ? config_.password : nullptr, availabilityTopic_, 0,
        true, "offline");
    if (!ok) {
        snprintf(lastError_, sizeof(lastError_), "broker connect failed (%d)",
                 mqtt_->state());
        return false;
    }

    lastError_[0] = '\0';
    mqtt_->publish(availabilityTopic_, "online", true);
    subscribeCommands();
    if (!discoverySent_) {
        republishDiscovery();
    }
    return true;
}

void HaMqtt::subscribeCommands() {
    char topic[128];
    for (size_t i = 0; i < kEntityCount; ++i) {
        if (kEntities[i].commandSuffix == nullptr) {
            continue;
        }
        topicFor(kEntities[i].commandSuffix, topic, sizeof(topic));
        mqtt_->subscribe(topic);
    }
}

void HaMqtt::appendDeviceBlock(String &json) const {
    json += F("\"dev\":{\"ids\":[\"");
    json += deviceId_;
    json += F("\"],\"name\":\"");
    json += deviceName_;
    json += F("\",\"mf\":\"");
    json += DEVICE_MANUFACTURER;
    json += F("\",\"mdl\":\"");
    json += DEVICE_MODEL;
    json += F("\",\"sw\":\"");
    json += FW_VERSION;
    json += F("\"}");
}

void HaMqtt::publishDiscovery(const Entity &entity) {
    // Abbreviated keys throughout: discovery payloads have to fit the client's
    // buffer, and Home Assistant has understood the short forms for years.
    String json;
    json.reserve(640);
    json += F("{\"name\":\"");
    json += entity.name;
    json += F("\",\"uniq_id\":\"");
    json += deviceId_;
    json += '_';
    json += entity.objectId;
    json += F("\",\"avty_t\":\"");
    json += availabilityTopic_;
    json += F("\"");

    if (entity.jsonKey != nullptr) {
        json += F(",\"stat_t\":\"");
        json += stateTopic_;
        json += F("\",\"val_tpl\":\"{{ value_json.");
        json += entity.jsonKey;
        json += F(" }}\"");
    }
    if (entity.commandSuffix != nullptr) {
        char topic[128];
        topicFor(entity.commandSuffix, topic, sizeof(topic));
        json += F(",\"cmd_t\":\"");
        json += topic;
        json += F("\"");
    }
    if (entity.unit != nullptr) {
        json += F(",\"unit_of_meas\":\"");
        json += entity.unit;
        json += F("\"");
    }
    if (entity.deviceClass != nullptr) {
        json += F(",\"dev_cla\":\"");
        json += entity.deviceClass;
        json += F("\"");
    }
    if (entity.stateClass != nullptr) {
        json += F(",\"stat_cla\":\"");
        json += entity.stateClass;
        json += F("\"");
    }
    if (entity.icon != nullptr) {
        json += F(",\"ic\":\"");
        json += entity.icon;
        json += F("\"");
    }
    if (entity.extra != nullptr) {
        json += ',';
        json += entity.extra;
    }
    if (entity.diagnostic) {
        json += F(",\"ent_cat\":\"diagnostic\"");
    }
    json += ',';
    appendDeviceBlock(json);
    json += '}';

    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
             config_.discoveryPrefix, entity.component, deviceId_,
             entity.objectId);
    if (!mqtt_->publish(topic, json.c_str(), true)) {
        snprintf(lastError_, sizeof(lastError_), "discovery too big for %s",
                 entity.objectId);
    }
}

void HaMqtt::republishDiscovery() {
    if (mqtt_ == nullptr || !mqtt_->connected()) {
        return;
    }
    for (size_t i = 0; i < kEntityCount; ++i) {
        publishDiscovery(kEntities[i]);
    }
    discoverySent_ = true;
}

size_t HaMqtt::buildStateJson(const StateInput &input, char *out,
                              size_t outCap) {
    static const NfcManager::Status kNoNfc;
    static const ScaleManager::Status kNoScale;
    static const SpoolState::State kNoSpool;
    static const opt::OptTag kNoTag;

    const NfcManager::Status &nfc = input.nfc ? *input.nfc : kNoNfc;
    const ScaleManager::Status &scale = input.scale ? *input.scale : kNoScale;
    const SpoolState::State &spool = input.spool ? *input.spool : kNoSpool;
    const opt::OptTag &tag = input.tag ? *input.tag : kNoTag;
    const uint32_t nowMs = input.nowMs;

    size_t len = 0;
    len += snprintf(out, outCap, "{");

    // Weights and lengths first: they change most often and matter most.
    appendNumber(out, outCap, len, "gross_g", scale.grossGrams, 1,
                 scale.haveReading);
    appendNumber(out, outCap, len, "remaining_g", spool.remainingGrams, 1,
                 spool.referenceWeightsPresent);
    appendNumber(out, outCap, len, "remaining_pct", spool.remainingPercent, 1,
                 spool.referenceWeightsPresent);
    appendNumber(out, outCap, len, "remaining_m", spool.remainingLengthMm / 1000.0f,
                 1, spool.remainingLengthValid);
    appendNumber(out, outCap, len, "consumed_g", spool.consumedGrams, 1,
                 spool.referenceWeightsPresent);
    appendNumber(out, outCap, len, "tag_consumed_g", spool.tagConsumedGrams, 1,
                 spool.tagConsumedValid);
    appendNumber(out, outCap, len, "reference_g", spool.referenceFullGrams, 1,
                 spool.referenceFullGrams > 0.0f);
    appendNumber(out, outCap, len, "empty_g", spool.emptyContainerGrams, 1,
                 spool.haveTag && spool.emptyContainerGrams > 0.0f);

    // Spool identity.
    if (spool.haveTag) {
        const opt::OptMainData &main = tag.main;
        appendString(out, outCap, len, "brand", main.brandName);
        appendString(out, outCap, len, "material_name", main.materialName);
        appendString(out, outCap, len, "spool_id", main.brandSpecificInstanceId);
        appendColorHex(out, outCap, len, "color", main);

        if (main.hasMaterialType) {
            const char *name = opt::materialTypeName(main.materialType);
            if (name != nullptr) {
                appendString(out, outCap, len, "material_type", name);
            } else if (main.materialAbbreviation[0] != '\0') {
                appendString(out, outCap, len, "material_type",
                             main.materialAbbreviation);
            }
        } else if (main.materialAbbreviation[0] != '\0') {
            appendString(out, outCap, len, "material_type",
                         main.materialAbbreviation);
        }

        if (main.hasMinPrintTemperature && main.hasMaxPrintTemperature) {
            char range[24];
            snprintf(range, sizeof(range), "%d-%d \xC2\xB0" "C",
                     main.minPrintTemperature, main.maxPrintTemperature);
            appendString(out, outCap, len, "print_temp", range);
        }
        appendInt(out, outCap, len, "drying_c", main.dryingTemperature,
                  main.hasDryingTemperature);
        appendInt(out, outCap, len, "drying_min", main.dryingTime,
                  main.hasDryingTime);
    }

    appendString(out, outCap, len, "tag_uid", nfc.uid);
    appendNumber(out, outCap, len, "cell_c", scale.temperatureC, 2,
                 scale.temperatureValid);
    appendAge(out, outCap, len, "last_read_s", nfc.lastReadMs, nowMs);
    appendAge(out, outCap, len, "last_write_s", nfc.lastWriteMs, nowMs);
    appendInt(out, outCap, len, "blocks_written",
              static_cast<long>(nfc.blocksWritten), true);
    appendString(out, outCap, len, "write_status", input.writeStatus);
    appendString(out, outCap, len, "last_command", input.lastCommand);
    appendString(out, outCap, len, "error",
                 nfc.lastError[0] != '\0' ? nfc.lastError : "none");
    appendString(out, outCap, len, "storage_location", input.storageLocation);
    appendNumber(out, outCap, len, "calibration_mass", input.calibrationMass, 0,
                 true);

    appendBool(out, outCap, len, "tag_present", nfc.tagPresent && nfc.tagDecoded);
    appendBool(out, outCap, len, "stable", scale.stable);
    appendBool(out, outCap, len, "reader_online", nfc.readerOnline);
    appendBool(out, outCap, len, "scale_online", scale.online);
    appendBool(out, outCap, len, "disagrees", spool.disagrees);
    appendBool(out, outCap, len, "auto_writeback",
               input.policy != nullptr && input.policy->enabled());
    appendBool(out, outCap, len, "polling", nfc.pollEnabled);

    // Trim the trailing comma, then close.
    if (len > 1 && len < outCap && out[len - 1] == ',') {
        --len;
    }
    if (len < outCap) {
        out[len++] = '}';
    }
    if (len < outCap) {
        out[len] = '\0';
    } else if (outCap > 0) {
        out[outCap - 1] = '\0';
    }
    return len;
}

void HaMqtt::publishState(const StateInput &input) {
    if (mqtt_ == nullptr || !mqtt_->connected()) {
        return;
    }
    char json[kStateJsonCapacity];
    const size_t len = buildStateJson(input, json, sizeof(json));
    if (mqtt_->publish(stateTopic_, reinterpret_cast<const uint8_t *>(json), len,
                       true)) {
        ++publishCount_;
    } else {
        snprintf(lastError_, sizeof(lastError_), "state publish failed (%u B)",
                 static_cast<unsigned>(len));
    }
}

bool HaMqtt::takePublishRequest() {
    const bool requested = publishRequested_;
    publishRequested_ = false;
    return requested;
}

void HaMqtt::handleMessage(const char *topic, const uint8_t *payload,
                           size_t len) {
    // Payloads are short: a number, ON/OFF, PRESS, or a storage location.
    char value[64];
    const size_t copied = len < sizeof(value) - 1 ? len : sizeof(value) - 1;
    memcpy(value, payload, copied);
    value[copied] = '\0';

    const char *suffix = strrchr(topic, '/');
    if (suffix == nullptr) {
        return;
    }
    ++suffix;

    String result;
    if (strcmp(suffix, "tare") == 0 && callbacks_.onTare) {
        result = callbacks_.onTare();
    } else if (strcmp(suffix, "calibrate") == 0 && callbacks_.onCalibrate) {
        result = callbacks_.onCalibrate();
    } else if (strcmp(suffix, "calibration_mass") == 0 &&
               callbacks_.onCalibrationMass) {
        result = callbacks_.onCalibrationMass(strtof(value, nullptr));
    } else if (strcmp(suffix, "write_now") == 0 && callbacks_.onWriteNow) {
        result = callbacks_.onWriteNow();
    } else if (strcmp(suffix, "auto_writeback") == 0 &&
               callbacks_.onAutoWriteBack) {
        result = callbacks_.onAutoWriteBack(strcmp(value, "ON") == 0);
    } else if (strcmp(suffix, "poll_enabled") == 0 &&
               callbacks_.onPollEnabled) {
        result = callbacks_.onPollEnabled(strcmp(value, "ON") == 0);
    } else if (strcmp(suffix, "wipe_aux") == 0 && callbacks_.onWipeAux) {
        result = callbacks_.onWipeAux();
    } else if (strcmp(suffix, "storage_location") == 0 &&
               callbacks_.onStorageLocation) {
        result = callbacks_.onStorageLocation(String(value));
    } else if (strcmp(suffix, "refresh") == 0 && callbacks_.onRefresh) {
        result = callbacks_.onRefresh();
    } else {
        return;
    }

    snprintf(lastCommand_, sizeof(lastCommand_), "%s", result.c_str());
    // The sketch owns publishing, so the result rides out on the next state
    // document rather than being published from inside the network callback.
    publishRequested_ = true;
}
