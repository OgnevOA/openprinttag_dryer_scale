// Compile-time configuration for the OpenPrintTag dryer reader/writer.
//
// Arduino IDE offers no per-project build flags, so everything that would
// normally be a -D define lives here. Values that a user may want to change
// after flashing are only defaults: the runtime copies in Preferences win.

#pragma once

// Not FIRMWARE_VERSION: the vendored PN5180 header uses that name for an
// EEPROM address, and both headers end up in the same translation unit.
#define FW_VERSION       "0.2.0"
#define DEVICE_MODEL     "OpenPrintTag Dryer Scale"
#define DEVICE_MANUFACTURER "openprinttag-dryer"
// Home Assistant builds every entity id out of the device name, so this being
// short and plain is what stands between the dashboard and a page full of
// sensor.openprinttag_dryer_scale_recommended_drying_temperature. Renaming the
// device in Home Assistant later renames the entities with it.
#define DEVICE_DEFAULT_NAME "Filament dryer"

// ---------------------------------------------------------------------------
// Pin map: Seeed Studio XIAO ESP32-C6
//
// Uses all 11 header pins. D4/D5 are the I2C pair, D8/D9/D10 the SPI trio.
// GPIO4/5/8/9/15 (strapping pins) are not on the XIAO header, so nothing here
// can interfere with boot mode selection.
// ---------------------------------------------------------------------------
#define PIN_PN5180_SCK   D8   // GPIO19
#define PIN_PN5180_MISO  D9   // GPIO20
#define PIN_PN5180_MOSI  D10  // GPIO18
#define PIN_PN5180_NSS   D3   // GPIO21
#define PIN_PN5180_BUSY  D6   // GPIO16
#define PIN_PN5180_RST   D7   // GPIO17

#define PIN_HX711_DOUT   D0   // GPIO0
#define PIN_HX711_SCK    D1   // GPIO1

#define PIN_STATUS_LED   D2   // GPIO2, WS2812 data

#define PIN_I2C_SDA      D4   // GPIO22, optional temperature sensor
#define PIN_I2C_SCL      D5   // GPIO23

// SPI clock for the PN5180. The reader sits at the dryer lid on a ribbon
// cable, so the bus is derated well below the PN5180's 7 MHz maximum.
#define PN5180_SPI_HZ    3500000

// ---------------------------------------------------------------------------
// Scale
// ---------------------------------------------------------------------------
// Raw counts per gram. Meaningless until calibrated; overwritten by the value
// stored in Preferences after a calibration run.
#define SCALE_DEFAULT_COUNTS_PER_GRAM 21.0f

// Samples per reported measurement. The HX711 runs at 10 SPS, so 15 samples
// is roughly 1.5 s of averaging.
#define SCALE_SAMPLES_PER_READING     15

// A reading is "stable" once this many consecutive measurements stay inside
// the band below.
#define SCALE_STABLE_READINGS         4
#define SCALE_STABLE_BAND_G           1.5f

// Weight of the printed OpenPrintTag adapter that holds the tag on the spool.
// It is bonded to the spool and never comes off, so it is part of the spool's
// tare, not the dryer's: every weight the spool's page quotes is that much
// lighter than what the platform reads. Only used to prefill the provisioning
// form's spool weight, which is the one place the total has to be stated.
// Reweigh it if you reprint the adapter at a different infill.
#define OPT_ADAPTER_GRAMS             31.0f

// Load cell zero drift in grams per degree C. The dryer runs at up to 70 C and
// sits on the cell, so this is the dominant error term; the characterisation
// run in docs/validation.md produces the number. Positive means the reading
// climbs as the cell warms.
#define SCALE_TEMP_DRIFT_G_PER_C      0.0f
#define SCALE_TEMP_REFERENCE_C        22.0f

// I2C address range probed for an LM75/TMP102 compatible temperature sensor.
#define TEMP_SENSOR_FIRST_ADDRESS     0x48
#define TEMP_SENSOR_LAST_ADDRESS      0x4B

// A DS18B20 is the other sensor the firmware accepts, and it shares the I2C data
// pin. Nothing is given up by sharing: the two are alternatives, only one is
// ever fitted, and every other pin on the header is taken. The firmware probes
// I2C first and falls back to the one-wire bus here, so no build-time choice is
// needed -- but note that the DS18B20 needs a 4.7k pull-up to 3.3 V on this pin.
// The processor's internal pull-up is around 45k, which is far too weak to pull
// the bus back up inside a bit slot.
#define PIN_ONEWIRE                   PIN_I2C_SDA

// The drift characterisation log: 16 bytes per sample, so 720 samples is about
// 11 kB of RAM and, at the default interval, six hours of a drying cycle.
#define DRIFT_LOG_CAPACITY            720
#define DRIFT_LOG_DEFAULT_INTERVAL_S  30
// A fit over a narrower temperature range than this is extrapolation, not
// measurement, and is reported as such.
#define DRIFT_FIT_MIN_SPAN_C          5.0f

// ---------------------------------------------------------------------------
// Tag write-back policy
// ---------------------------------------------------------------------------
// Only write when the measured consumption moved by at least this much.
#define WRITEBACK_MIN_DELTA_G         5.0f
// And never more often than this, regardless of the delta.
#define WRITEBACK_MIN_INTERVAL_MS     (10UL * 60UL * 1000UL)
// Warn when scale-derived and tag-stored consumption disagree by more than this.
#define WRITEBACK_DISAGREE_WARN_G     25.0f

// Aux region allocation for tags we initialise ourselves. The spec requires at
// least 16 B and recommends 32 B; must stay a multiple of the 4 B block size.
#define AUX_REGION_SIZE_BYTES         32

// ---------------------------------------------------------------------------
// Spoolman
// ---------------------------------------------------------------------------
// Off unless a URL is configured. Every call blocks the main loop, so the
// timeout is short: a Spoolman that has gone away must not stall the scale.
#define SPOOLMAN_TIMEOUT_MS           3000
// A measurement is only worth sending when it has moved, and Spoolman is a
// database rather than a tag with a write endurance, so this is about noise
// rather than wear.
#define SPOOLMAN_MIN_DELTA_G          5.0f
#define SPOOLMAN_MIN_INTERVAL_MS      (5UL * 60UL * 1000UL)

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define NFC_POLL_INTERVAL_MS          3000UL
#define NFC_ABSENT_CONFIRM_COUNT      3     // polls without an answer before "removed"
#define MQTT_PUBLISH_INTERVAL_MS      15000UL
#define MQTT_RECONNECT_INTERVAL_MS    5000UL

// ---------------------------------------------------------------------------
// Networking defaults
// ---------------------------------------------------------------------------
#define AP_FALLBACK_SSID              "openprinttag-dryer"
#define AP_FALLBACK_PASSWORD          "printtag"
#define MDNS_HOSTNAME                 "openprinttag-dryer"
#define MQTT_DISCOVERY_PREFIX         "homeassistant"
#define MQTT_BASE_TOPIC               "openprinttag_dryer"
