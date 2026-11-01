// Copy this file to secrets.h and fill it in. secrets.h is git-ignored.
//
// Everything here is optional: leaving a value empty makes the device fall back
// to its Wi-Fi setup portal, and all of it can be changed at runtime from the
// web UI (runtime values stored in Preferences take precedence).

#pragma once

#define SECRET_WIFI_SSID      ""
#define SECRET_WIFI_PASSWORD  ""

#define SECRET_MQTT_HOST      ""     // e.g. "192.168.1.10"
#define SECRET_MQTT_PORT      1883
#define SECRET_MQTT_USER      ""
#define SECRET_MQTT_PASSWORD  ""

// Shared OpenPrintTag workgroup, max 8 characters. Devices that share a
// workgroup trust each other's auxiliary region data. Leave empty to have the
// device generate one from its MAC address on first boot.
#define SECRET_WORKGROUP      ""

// Spoolman, if you run it. Plain http and no trailing slash; the default port
// is 7912, e.g. "http://192.168.1.10:7912". Leaving this empty disables the
// integration entirely, and it can be set from the Settings page instead.
#define SECRET_SPOOLMAN_URL   ""

// Password for over-the-air updates.
#define SECRET_OTA_PASSWORD   "printtag"
