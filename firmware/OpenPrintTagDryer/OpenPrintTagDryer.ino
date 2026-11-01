// OpenPrintTag dryer reader/writer with scale.
//
// A XIAO ESP32-C6 on a Creality Space Pi filament dryer: a PN5180 at the lid
// reads and writes the spool's OpenPrintTag, an HX711 under the dryer weighs the
// whole thing, and Home Assistant sees the result over MQTT discovery.
//
// Everything that decides anything lives in src/App.*, which has no network code
// in it and is unit tested on a desktop. What is left here is the part that can
// only run on the board: Wi-Fi, mDNS, OTA, and handing out slices of time.
//
// See docs/build.md for the board package version and IDE settings this expects.

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "src/App.h"
#include "src/HaMqtt.h"
#include "src/WebPortal.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

namespace {

// How long to wait for the configured network before falling back to the setup
// access point. The device is useful without Wi-Fi -- it still reads, weighs and
// writes tags -- so this stays short.
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiRetryIntervalMs = 60000;
constexpr uint32_t kLedUpdateIntervalMs = 250;

App gApp;
HaMqtt gMqtt;
WebPortal gPortal;
WiFiClient gNetwork;

char gDeviceId[24];
bool gAccessPointMode = false;
uint32_t gNextWifiAttemptMs = 0;
uint32_t gNextPublishMs = 0;
uint32_t gNextLedMs = 0;
uint32_t gLastLedColor = 0xFFFFFFFF;

// The efuse MAC is stable across reflashes, which matters because Home Assistant
// keys entity identity off this.
void makeDeviceId() {
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(gDeviceId, sizeof(gDeviceId), "dryer_%02x%02x%02x",
             static_cast<unsigned>((mac >> 24) & 0xFF),
             static_cast<unsigned>((mac >> 16) & 0xFF),
             static_cast<unsigned>((mac >> 8) & 0xFF));
}

String networkSummary() {
    if (gAccessPointMode) {
        return String("setup AP ") + AP_FALLBACK_SSID + " at " +
               WiFi.softAPIP().toString();
    }
    if (WiFi.status() != WL_CONNECTED) {
        return String("Wi-Fi disconnected");
    }
    String summary = WiFi.localIP().toString();
    summary += ", ";
    summary += WiFi.RSSI();
    summary += " dBm";
    summary += gMqtt.connected() ? ", broker connected" : ", broker offline";
    return summary;
}

void startAccessPoint() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASSWORD);
    gAccessPointMode = true;
    Serial.print(F("Setup access point "));
    Serial.print(AP_FALLBACK_SSID);
    Serial.print(F(" at "));
    Serial.println(WiFi.softAPIP());
}

void connectWifi() {
    const Settings &settings = gApp.settings();
    gNextWifiAttemptMs = millis() + kWifiRetryIntervalMs;

    if (!settings.haveWifi()) {
        startAccessPoint();
        return;
    }

    Serial.print(F("Joining "));
    Serial.println(settings.wifiSsid);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.begin(settings.wifiSsid, settings.wifiPassword);

    const uint32_t deadline = millis() + kWifiConnectTimeoutMs;
    while (WiFi.status() != WL_CONNECTED &&
           static_cast<int32_t>(millis() - deadline) < 0) {
        // The scale keeps sampling while we wait: 15 s is a long time to stop
        // weighing, and the load cell settles better if it is never idle.
        gApp.loop(millis());
        delay(50);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("Wi-Fi did not come up; starting the setup AP"));
        startAccessPoint();
        return;
    }

    gAccessPointMode = false;
    Serial.print(F("Wi-Fi up at "));
    Serial.println(WiFi.localIP());

    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
    }
}

void startOta() {
    ArduinoOTA.setHostname(MDNS_HOSTNAME);
    ArduinoOTA.setPassword(SECRET_OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        // Writing flash with the RF field up and a 100 mA field driver on the
        // same 5 V rail is not worth finding out about the hard way.
        gApp.nfc().setPollEnabled(false);
        Serial.println(F("OTA update starting"));
    });
    ArduinoOTA.onEnd([]() { Serial.println(F("OTA update done")); });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.print(F("OTA failed, code "));
        Serial.println(error);
        gApp.nfc().setPollEnabled(true);
    });
    ArduinoOTA.begin();
}

HaMqtt::Config mqttConfigFromSettings() {
    const Settings &settings = gApp.settings();
    HaMqtt::Config config;
    snprintf(config.host, sizeof(config.host), "%s", settings.mqttHost);
    config.port = settings.mqttPort;
    snprintf(config.user, sizeof(config.user), "%s", settings.mqttUser);
    snprintf(config.password, sizeof(config.password), "%s",
             settings.mqttPassword);
    return config;
}

HaMqtt::StateInput stateInput() {
    HaMqtt::StateInput input;
    input.nfc = &gApp.nfc().status();
    input.tag = &gApp.tag();
    input.scale = &gApp.scale().status();
    input.spool = &gApp.spool();
    input.policy = &gApp.policy();
    input.writeStatus = gApp.writeStatus();
    input.lastCommand = gMqtt.lastCommandResult();
    input.storageLocation = gApp.settings().storageLocation;
    input.calibrationMass = gApp.settings().calibrationMassGrams;
    input.nowMs = millis();
    return input;
}

void startMqtt() {
    HaMqtt::Callbacks callbacks;
    callbacks.onTare = []() { return gApp.tareDryer(); };
    callbacks.onCalibrate = []() { return gApp.calibrate(); };
    callbacks.onCalibrationMass = [](float grams) {
        return gApp.setCalibrationMass(grams);
    };
    callbacks.onWriteNow = []() { return gApp.writeNow(); };
    callbacks.onAutoWriteBack = [](bool on) {
        return gApp.setAutoWriteBack(on);
    };
    callbacks.onPollEnabled = [](bool on) { return gApp.setPollEnabled(on); };
    callbacks.onWipeAux = []() { return gApp.wipeAux(); };
    callbacks.onStorageLocation = [](const String &where) {
        return gApp.setStorageLocation(where);
    };
    callbacks.onRefresh = []() { return gApp.refreshTag(); };

    gMqtt.begin(gNetwork, mqttConfigFromSettings(), gDeviceId,
                gApp.settings().deviceName, callbacks);
}

void startPortal() {
    WebPortal::Hooks hooks;
    hooks.networkSummary = []() { return networkSummary(); };
    hooks.reconnectWifi = []() {
        WiFi.disconnect(true);
        gNextWifiAttemptMs = millis();  // the main loop picks it up
    };
    hooks.reconnectMqtt = []() {
        gMqtt.setConfig(mqttConfigFromSettings());
    };
    hooks.restart = []() {
        delay(250);  // let the response reach the browser first
        ESP.restart();
    };
    hooks.stateJson = []() {
        char json[1024];
        HaMqtt::buildStateJson(stateInput(), json, sizeof(json));
        return String(json);
    };
    gPortal.begin(gApp, hooks, gDeviceId);
}

// A single WS2812 says what state the device is in from across the room. The
// ESP32 core drives it directly, so this costs no library.
void updateLed(uint32_t nowMs) {
    if (static_cast<int32_t>(nowMs - gNextLedMs) < 0) {
        return;
    }
    gNextLedMs = nowMs + kLedUpdateIntervalMs;

    const NfcManager::Status &nfc = gApp.nfc().status();
    const ScaleManager::Status &scale = gApp.scale().status();

    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    if (!nfc.readerOnline || !scale.online) {
        r = 40;  // hardware missing: the one state that needs attention
    } else if (!nfc.tagDecoded) {
        r = 30;
        g = 18;  // amber: working, but no spool to report on
    } else if (!scale.stable) {
        b = 40;  // weighing
    } else {
        g = 30;  // spool known, weight settled
    }
    if (gAccessPointMode) {
        b = 40;  // waiting to be configured
    }

    const uint32_t color = (static_cast<uint32_t>(r) << 16) |
                           (static_cast<uint32_t>(g) << 8) | b;
    if (color != gLastLedColor) {
        gLastLedColor = color;
        rgbLedWrite(PIN_STATUS_LED, r, g, b);
    }
}

// Bring-up console, for the stage where there is no network yet and the board is
// on a bench with a USB cable.
void serviceSerialConsole() {
    if (Serial.available() <= 0) {
        return;
    }
    const int key = Serial.read();
    switch (key) {
    case 's':
        Serial.println(gApp.selfTest());
        break;
    case 't':
        Serial.println(gApp.tareDryer());
        break;
    case 'c':
        Serial.println(gApp.calibrate());
        break;
    case 'r':
        Serial.println(gApp.refreshTag());
        break;
    case 'w':
        Serial.println(gApp.writeNow());
        break;
    case 'n':
        Serial.println(networkSummary());
        break;
    case 'i':
        Serial.print(F("device id "));
        Serial.print(gDeviceId);
        Serial.print(F(", firmware "));
        Serial.println(FW_VERSION);
        break;
    case '?':
        Serial.println(F("s self-test, t tare, c calibrate, r re-read, "
                         "w write now, n network, i identity"));
        break;
    default:
        break;
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.print(F("\nOpenPrintTag dryer "));
    Serial.println(FW_VERSION);

    makeDeviceId();
    gApp.begin(gDeviceId);
    Serial.print(F("Device id "));
    Serial.print(gDeviceId);
    Serial.print(F(", workgroup "));
    Serial.println(gApp.settings().workgroup);

    connectWifi();
    startOta();
    startMqtt();
    startPortal();

    Serial.println(F("Ready. Press ? for the serial console."));
}

void loop() {
    const uint32_t now = millis();

    gApp.loop(now);
    gPortal.loop();
    ArduinoOTA.handle();
    serviceSerialConsole();
    updateLed(now);

    if (!gAccessPointMode && WiFi.status() != WL_CONNECTED &&
        static_cast<int32_t>(now - gNextWifiAttemptMs) >= 0) {
        connectWifi();
    }

    if (WiFi.status() == WL_CONNECTED && gApp.settings().haveMqtt()) {
        gMqtt.loop(now);

        // Publish on any change, on a command's result, and on a heartbeat so
        // Home Assistant's availability never depends on nothing happening.
        const bool changed = gApp.takeStateChanged();
        const bool commanded = gMqtt.takePublishRequest();
        if (changed || commanded ||
            static_cast<int32_t>(now - gNextPublishMs) >= 0) {
            gNextPublishMs = now + MQTT_PUBLISH_INTERVAL_MS;
            gMqtt.publishState(stateInput());
        }
    }
}
