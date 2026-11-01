// The device's behaviour, with no network code in it.
//
// Both the web portal and Home Assistant need the same operations -- tare,
// calibrate, provision a tag, write consumed weight now -- so those live here
// once and return a sentence describing what happened. The sketch reduces to
// wiring: bring up Wi-Fi, hand the App a slice of time, publish state.
//
// Keeping this free of WiFi, WebServer and MQTT includes is what lets the whole
// thing be exercised by tests/ on a desktop, with a simulated load cell and a
// PN5180 that answers nothing.
//
// Spoolman is the one exception, because deciding when a measurement is worth
// sending is the same judgement as deciding when to write a tag and belongs
// beside it. Its HTTP client is stubbed on the host like the reader is, so the
// decisions stay testable even though the transport is not.

#pragma once

#include <Arduino.h>

#include "../config.h"
#include "DriftLog.h"
#include "NfcManager.h"
#include "PrusamentPresets.h"
#include "ScaleManager.h"
#include "Settings.h"
#include "SpoolState.h"
#include "SpoolmanClient.h"
#include "WriteBackPolicy.h"

class App {
public:
    // What the provisioning form collects. The per-spool numbers come off the
    // spool's prusament.com page; everything else comes from the preset.
    struct ProvisionInput {
        char materialId[24] = {0};    // preset id, for example "petg"
        char colorName[32] = {0};
        char colorHex[10] = {0};      // "#rrggbb", optional if colorName is known
        char spoolId[opt::kBrandIdSize] = {0};
        float filamentGrams = 0.0f;   // actual_netto_full_weight
        float spoolGrams = 0.0f;      // empty_container_weight
        float lengthMetres = 0.0f;    // actual_full_length, optional
        bool includeUri = false;      // add a prusament.com URI record
        bool ignoreWeightCheck = false;
    };

    // Result of checking the entered weights against the scale, before
    // anything is written. This is the guard against a typo in the spool ID
    // producing a confidently wrong tag.
    struct WeightCheck {
        bool performed = false;    // false when the scale cannot help
        bool agrees = false;
        float expectedGrams = 0.0f;
        float measuredGrams = 0.0f;
        float differenceGrams = 0.0f;
        char note[144] = {0};
    };

    void begin(const char *deviceId);
    void loop(uint32_t nowMs);

    // Actions. Each returns a sentence for the portal, MQTT and the log.
    String tareDryer();
    String calibrate();
    String setCalibrationMass(float grams);
    String writeNow();
    String wipeAux();
    String refreshTag();
    String setAutoWriteBack(bool enabled);
    String setPollEnabled(bool enabled);
    String setStorageLocation(const String &location);
    String setDriftCoefficient(float gramsPerC);
    String adoptImpliedTare();
    String provision(const ProvisionInput &input);

    // Rewrites the tag in the field with the same main region it already
    // carries, changing only the layout so that an auxiliary region exists.
    // For tags written by a tool that left no room for one, which would
    // otherwise be readable here but never writable.
    String addAuxRegion();

    // Rewrites empty_container_weight and nothing else, for the common case of a
    // tag whose tare is right about the bare spool and wrong about the physical
    // object: a printed adapter bonded to it, a sticker, a repaired hub. The
    // whole main region is re-encoded, so this refuses rather than proceeding
    // whenever that would lose something -- see the implementation.
    String setEmptySpoolWeight(float grams);

    // Whether a hand-entered tare is worth writing to a tag at all. Separate and
    // pure because the tag checks in front of it need a reader, and a bound that
    // rejects the digit slips -- 1860 for 186, 18 for 186 -- is worth aiming
    // numbers at directly.
    static bool emptySpoolWeightPlausible(float grams) {
        return grams >= 20.0f && grams <= 1000.0f;
    }

    String selfTest();

    // Spoolman. `linkSpoolman` claims the tag in the field for a spool number,
    // which is the one-off step that makes everything after it automatic;
    // `pushSpoolman` sends a measurement now, ignoring the interval but not the
    // sanity checks.
    String linkSpoolman(int spoolId);
    String unlinkSpoolman();
    String pushSpoolman();

    // Re-reads the Spoolman settings after the settings page has saved them.
    void applySpoolmanSettings();
    const SpoolmanClient &spoolman() const { return spoolman_; }

    // Whether the weight on the platform is one Spoolman may be told about, and
    // if not, why not. Static and pure because this is the check that protects
    // somebody's inventory from a stale dryer tare, and it is worth being able
    // to aim numbers straight at it.
    //
    // The hazard: Spoolman takes a measurement heavier than a full spool as a
    // correction to what full means and rewrites the spool's initial weight. An
    // uncalibrated scale or an untared dryer would do exactly that.
    static bool measurementUsable(const ScaleManager::Status &scale,
                                  const SpoolState::State &spool,
                                  float &grossGrams, String &reason);

    // Validation tools. The read test measures how reliably the reader sees the
    // tag from wherever the bracket currently is; the drift log records weight
    // against temperature over a drying cycle and fits a coefficient to it.
    String readTest(uint16_t attempts);
    String startDriftLog(uint32_t intervalSeconds);
    String stopDriftLog();
    String clearDriftLog();
    String applyDriftFit();
    const DriftLog &driftLog() const { return driftLog_; }

    // Checks entered weights against the scale without writing anything.
    WeightCheck checkEnteredWeights(float filamentGrams,
                                    float spoolGrams) const;

    // Builds the tag image request for `input`, so the portal can show exactly
    // what will be written. Returns false with a reason when the input is not
    // usable.
    bool buildRequest(const ProvisionInput &input, opt::OptBuildRequest &request,
                      String &error) const;

    const NfcManager &nfc() const { return nfc_; }
    NfcManager &nfc() { return nfc_; }
    const ScaleManager &scale() const { return scale_; }
    ScaleManager &scale() { return scale_; }
    const SpoolState::State &spool() const { return spool_.state(); }
    const WriteBackPolicy &policy() const { return policy_; }
    Settings &settings() { return settings_; }
    const Settings &settings() const { return settings_; }
    const opt::OptTag &tag() const { return nfc_.tag(); }

    // Why the last write-back decision went the way it did, for display.
    const char *writeStatus() const { return writeStatus_; }
    const char *spoolmanStatus() const { return spoolmanStatus_; }

    // True when something changed that Home Assistant should hear about.
    bool takeStateChanged();

    // Set when the device has never been calibrated, so the UI can say so
    // rather than reporting confident nonsense.
    bool needsCalibration() const { return !scale_.status().calibrated; }

private:
    void recompute();
    void serviceWriteBack(uint32_t nowMs);
    void serviceSpoolman(uint32_t nowMs);
    void setWriteStatus(const char *text);
    void setSpoolmanStatus(const char *text);
    bool spoolmanWeightUsable(float &grossGrams, String &reason) const;

    // Forgets what has been sent, so that a different spool is measured at once
    // rather than waiting out an interval that belonged to the last one.
    void resetSpoolmanPushState();

    NfcManager nfc_;
    ScaleManager scale_;
    SpoolState spool_;
    WriteBackPolicy policy_;
    Settings settings_;
    DriftLog driftLog_;
    SpoolmanClient spoolman_;

    char deviceId_[24] = {0};
    char writeStatus_[96] = {0};
    char spoolmanStatus_[120] = {0};
    bool stateChanged_ = true;
    uint32_t lastTagUidHash_ = 0;

    bool spoolmanLookupPending_ = false;
    uint32_t spoolmanNextAttemptMs_ = 0;

    // Attempts are rate-limited; only the ones that worked move the baseline the
    // delta is measured against. `spoolmanRetryWanted_` is what stops a failed
    // push from being suppressed by a weight that is not going to change:
    // a spool being dried rather than printed from sits at the same weight for
    // hours, and the point of a retry is that Spoolman came back, not that the
    // spool moved.
    uint32_t spoolmanLastAttemptMs_ = 0;
    bool spoolmanAttempted_ = false;
    float spoolmanLastPushGrams_ = 0.0f;
    bool spoolmanHavePushed_ = false;
    bool spoolmanRetryWanted_ = false;
};
