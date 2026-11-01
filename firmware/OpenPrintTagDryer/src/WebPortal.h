// Local web UI: status, calibration, tag provisioning, raw tag dump, settings
// and a firmware upload.
//
// This is the interface that does the things Home Assistant should not: entering
// a new spool's measured weights, calibrating the load cell against a known
// mass, and looking at the raw bytes on a tag when something disagrees.
//
// Pages are assembled as strings rather than served from a filesystem, so there
// is no second artefact to keep in sync with the firmware and nothing to upload
// separately.

#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

#include "App.h"

class WebPortal {
public:
    // Things only the sketch can do, because they need the network stack.
    struct Hooks {
        std::function<String()> networkSummary;
        std::function<void()> reconnectWifi;
        std::function<void()> reconnectMqtt;
        std::function<void()> restart;
        std::function<String()> stateJson;
    };

    void begin(App &app, const Hooks &hooks, const char *deviceId);
    void loop();

    // Host tests reach the routes through this.
    WebServer &server() { return server_; }

private:
    void handleRoot();
    void handleProvision();
    void handleProvisionPost();
    void handleCalibrate();
    void handleCalibratePost();
    void handleSettings();
    void handleSettingsPost();
    void renderFlash(String &out);
    void renderSpoolmanCard(String &out);
    void handleTagDump();
    void handleTagBin();
    void handleSelfTest();
    void handleReadTest();
    void handleDriftCsv();
    void handleAction();
    void handleStateJson();
    void handleUpdatePage();
    void handleUpdateDone();
    void handleUpdateUpload();
    void handleNotFound();

    void pageHead(String &out, const char *title, const char *activePath) const;
    void pageFoot(String &out) const;
    // Shared by the self-test page and the read test that posts back to it.
    void renderSelfTest(const String &readTestReport);
    void redirectWith(const char *path, const String &message);
    void sendPage(const String &body);

    // Carried across the redirect that follows every form post, so the result of
    // an action is shown on the page it came from.
    String flash_;

    App *app_ = nullptr;
    Hooks hooks_;
    WebServer server_{80};
    char deviceId_[24] = {0};
    bool started_ = false;
};
