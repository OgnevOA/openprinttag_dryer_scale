// Host tests for the firmware logic that does not need a radio: the HX711
// driver against a simulated converter, the calibration and weight maths, the
// spool state derivation, the material presets, and the reader's behaviour when
// nothing answers on the SPI bus.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

// The fake Spoolman lives behind the HTTP client stub, so the tests reach it
// the same way the firmware reaches the real one.
#include <HTTPClient.h>

#include "../firmware/OpenPrintTagDryer/src/App.h"
#include "../firmware/OpenPrintTagDryer/src/DriftLog.h"
#include "../firmware/OpenPrintTagDryer/src/Ds18b20.h"
#include "../firmware/OpenPrintTagDryer/src/HaMqtt.h"
#include "../firmware/OpenPrintTagDryer/src/NfcManager.h"
#include "../firmware/OpenPrintTagDryer/src/PrusamentPresets.h"
#include "../firmware/OpenPrintTagDryer/src/ScaleManager.h"
#include "../firmware/OpenPrintTagDryer/src/SpoolState.h"
#include "../firmware/OpenPrintTagDryer/src/SpoolmanClient.h"
#include "../firmware/OpenPrintTagDryer/src/WebPortal.h"
#include "../firmware/OpenPrintTagDryer/src/WriteBackPolicy.h"
#include "../firmware/OpenPrintTagDryer/src/openprinttag/opt_enums.h"

namespace {

int gChecks = 0;
int gFailures = 0;
const char *gCurrentTest = "";

void fail(const std::string &what, const std::string &detail) {
    ++gFailures;
    printf("  FAIL %s: %s\n        %s\n", gCurrentTest, what.c_str(),
           detail.c_str());
}

void check(bool condition, const std::string &what,
           const std::string &detail = "") {
    ++gChecks;
    if (!condition) {
        fail(what, detail);
    }
}

void checkEqualInt(long long actual, long long expected,
                   const std::string &what) {
    ++gChecks;
    if (actual != expected) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "expected %lld, got %lld", expected,
                 actual);
        fail(what, buffer);
    }
}

void checkNear(double actual, double expected, double tolerance,
               const std::string &what) {
    ++gChecks;
    if (fabs(actual - expected) > tolerance) {
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "expected %g +/- %g, got %g", expected,
                 tolerance, actual);
        fail(what, buffer);
    }
}

// Feeds the scale a steady value until it produces measurements.
void feedScale(ScaleManager &scale, long counts, int measurements) {
    hostio::setHx711Value(counts, true);
    int produced = 0;
    for (int guard = 0; guard < 20000 && produced < measurements; ++guard) {
        hostio::hx711().ready = true;  // a fresh conversion is always waiting
        if (scale.loop(hostio::now())) {
            ++produced;
        }
        hostio::advance(1);
    }
    check(produced == measurements, "scale produced the requested measurements",
          std::to_string(produced) + " of " + std::to_string(measurements));
}

void startScale(ScaleManager &scale) {
    hostio::attachHx711(PIN_HX711_DOUT, PIN_HX711_SCK);
    hostio::setHx711Value(100000, true);
    check(scale.begin(), "scale begins with a responding converter");
}

// ---------------------------------------------------------------------------

void testHx711Driver() {
    gCurrentTest = "hx711 driver";
    hostio::reset();

    ScaleManager scale;
    startScale(scale);

    // The driver must reproduce the converter's 24-bit two's complement output
    // exactly, including the sign, and clock a 25th pulse to keep channel A at
    // gain 128.
    struct Case {
        long value;
        const char *label;
    };
    const Case cases[] = {
        {123456, "positive mid-range"},
        {0x7FFFFF, "largest positive"},
        {-123456, "negative mid-range"},
        {-0x800000, "largest negative"},
    };

    for (const Case &testCase : cases) {
        hostio::setHx711Value(testCase.value, true);
        const uint32_t before = hostio::hx711().conversions;
        // 15 samples make one measurement; all of them read the same value.
        feedScale(scale, testCase.value, 1);
        checkEqualInt(scale.status().rawCounts, testCase.value, testCase.label);
        check(hostio::hx711().conversions > before,
              "the 25th pulse completed the conversion");
    }

    // A data line stuck high yields no samples rather than a wild reading.
    hostio::hx711().stuckHigh = true;
    const uint32_t measurementsBefore = scale.status().measurementCount;
    for (int i = 0; i < 200; ++i) {
        scale.loop(hostio::now());
        hostio::advance(10);
    }
    checkEqualInt(scale.status().measurementCount, measurementsBefore,
                  "no measurements from a stuck data line");
    hostio::hx711().stuckHigh = false;

    // A stuck-low line looks ready forever and clocks out nothing but zeros,
    // which the driver rejects as a fault rather than reporting as zero load.
    hostio::hx711().stuckLow = true;
    for (int i = 0; i < 50; ++i) {
        scale.loop(hostio::now());
        hostio::advance(10);
    }
    check(scale.status().sampleErrorCount > 0,
          "stuck-low data line counted as a sample error");
    hostio::hx711().stuckLow = false;
}

void testMedianFiltering() {
    gCurrentTest = "median filtering";
    hostio::reset();

    ScaleManager scale;
    startScale(scale);

    // A single wild sample in the middle of a window must not move the result.
    int samples = 0;
    bool gotMeasurement = false;
    for (int guard = 0; guard < 20000 && !gotMeasurement; ++guard) {
        hostio::hx711().ready = true;
        hostio::hx711().value = (samples == 7) ? 900000 : 100000;
        if (scale.loop(hostio::now())) {
            gotMeasurement = true;
        }
        ++samples;
        hostio::advance(1);
    }
    check(gotMeasurement, "measurement produced");
    checkEqualInt(scale.status().rawCounts, 100000,
                  "median rejected the outlier");
}

void testCalibrationAndPersistence() {
    gCurrentTest = "calibration";
    hostio::reset();

    char message[128] = {0};

    {
        ScaleManager scale;
        startScale(scale);

        // Empty platform.
        feedScale(scale, 100000, 6);
        check(scale.zeroPlatform(message, sizeof(message)), "zero the platform",
              message);
        checkNear(scale.status().zeroCounts, 100000, 1, "zero counts stored");

        // Span: 1000 g reads 21000 counts higher, so 21 counts per gram.
        feedScale(scale, 121000, 6);
        check(scale.calibrateSpan(1000.0f, message, sizeof(message)),
              "calibrate the span", message);
        checkNear(scale.status().countsPerGram, 21.0, 0.01,
                  "counts per gram from a known mass");
        checkNear(scale.gross(), 1000.0, 0.5, "known mass reads back");

        // Refusals: an implausible mass, and a mass that is not on the platform.
        check(!scale.calibrateSpan(10.0f, message, sizeof(message)),
              "tiny calibration mass refused");
        feedScale(scale, 100000, 6);
        check(!scale.calibrateSpan(1000.0f, message, sizeof(message)),
              "calibration with nothing on the platform refused");

        // Empty dryer: 1.14 kg, which becomes the tare.
        feedScale(scale, 100000 + static_cast<long>(1140 * 21), 6);
        check(scale.tareDryer(message, sizeof(message)), "tare the dryer",
              message);
        checkNear(scale.status().dryerTareGrams, 1140.0, 1.0,
                  "dryer tare stored");

        // A full spool on top: 1044 g filament plus a 201 g spool.
        feedScale(scale, 100000 + static_cast<long>((1140 + 1245) * 21), 6);
        checkNear(scale.gross(), 2385.0, 1.5, "gross weight");
        checkNear(scale.spoolGrams(), 1245.0, 1.5, "spool weight after tare");
    }

    // A new instance, as after a reboot, must find its calibration in NVS.
    {
        ScaleManager scale;
        hostio::setHx711Value(100000, true);
        check(scale.begin(), "scale begins after reboot");
        checkNear(scale.status().countsPerGram, 21.0, 0.01, "span restored");
        checkNear(scale.status().zeroCounts, 100000, 1, "zero restored");
        checkNear(scale.status().dryerTareGrams, 1140.0, 1.0, "tare restored");
        check(scale.status().calibrated, "calibrated flag restored");

        check(scale.setDryerTare(1150.0f, message, sizeof(message)),
              "tare can be set directly");
        checkNear(scale.status().dryerTareGrams, 1150.0, 0.01, "tare updated");
        check(!scale.setDryerTare(5.0f, message, sizeof(message)),
              "implausible tare refused");
    }
}

void testTemperatureCompensation() {
    gCurrentTest = "temperature compensation";
    hostio::reset();

    hostio::lm75().attached = true;
    hostio::lm75().address = 0x48;
    hostio::lm75().celsius = 22.0f;

    ScaleManager scale;
    startScale(scale);
    feedScale(scale, 100000, 6);
    char message[128] = {0};
    scale.zeroPlatform(message, sizeof(message));
    feedScale(scale, 121000, 6);
    scale.calibrateSpan(1000.0f, message, sizeof(message));

    check(scale.status().temperatureValid, "temperature sensor found");
    checkNear(scale.status().temperatureC, 22.0, 0.1, "temperature read");

    // Half a gram per degree: at 50 C that is 14 g of apparent extra weight,
    // which compensation should remove. The sensor is only polled every few
    // seconds, so the clock has to move for the new temperature to be seen.
    scale.setDriftPerC(0.5f);
    hostio::lm75().celsius = 50.0f;
    hostio::advance(6000);
    feedScale(scale, 121000, 8);
    checkNear(scale.status().temperatureC, 50.0, 0.1, "warm temperature read");
    checkNear(scale.status().driftCorrectionGrams, 14.0, 0.1,
              "drift correction");
    checkNear(scale.status().uncompensatedGrams, 1000.0, 0.5,
              "uncompensated reading");
    checkNear(scale.gross(), 986.0, 0.5, "compensated reading");

    // Losing the sensor must not apply a stale correction.
    hostio::lm75().failReads = true;
    hostio::advance(6000);
    feedScale(scale, 121000, 8);
    check(!scale.status().temperatureValid, "sensor loss noticed");
    checkNear(scale.status().driftCorrectionGrams, 0.0, 0.01,
              "no correction without a temperature");
}

void testStability() {
    gCurrentTest = "stability detection";
    hostio::reset();

    ScaleManager scale;
    startScale(scale);
    char message[128] = {0};
    feedScale(scale, 100000, 6);
    scale.zeroPlatform(message, sizeof(message));
    feedScale(scale, 121000, 6);
    scale.calibrateSpan(1000.0f, message, sizeof(message));

    feedScale(scale, 121000, 6);
    check(scale.stable(), "steady load reads as stable");

    // Alternating by 100 counts is about 5 g, well outside the band.
    bool sawUnstable = false;
    for (int i = 0; i < 8; ++i) {
        feedScale(scale, (i % 2 == 0) ? 121000 : 121400, 1);
        if (!scale.stable()) {
            sawUnstable = true;
        }
    }
    check(sawUnstable, "a moving load reads as unstable");

    feedScale(scale, 121000, 6);
    check(scale.stable(), "settles again once the load stops moving");
}

opt::OptTag makeTag(float fullWeight, float spoolWeight, float lengthMm,
                    bool haveConsumed, float consumed) {
    opt::OptTag tag;
    tag.main.hasActualNettoFullWeight = true;
    tag.main.actualNettoFullWeight = fullWeight;
    tag.main.hasEmptyContainerWeight = true;
    tag.main.emptyContainerWeight = spoolWeight;
    if (lengthMm > 0.0f) {
        tag.main.hasActualFullLength = true;
        tag.main.actualFullLength = lengthMm;
    }
    tag.main.hasDensity = true;
    tag.main.density = 1.24f;
    tag.main.hasFilamentDiameter = true;
    tag.main.filamentDiameter = 1.75f;
    tag.regions.hasAux = true;
    tag.aux.hasConsumedWeight = haveConsumed;
    tag.aux.consumedWeight = consumed;
    return tag;
}

ScaleManager::Status makeScale(float gross, float tare) {
    ScaleManager::Status status;
    status.online = true;
    status.haveReading = true;
    status.stable = true;
    status.grossGrams = gross;
    status.dryerTareGrams = tare;
    status.calibrated = true;
    return status;
}

void testSpoolState() {
    gCurrentTest = "spool state";

    const float tare = 1140.0f;
    const opt::OptTag tag = makeTag(1044.0f, 201.0f, 343000.0f, true, 0.0f);

    // A full spool: gross is dryer + spool + all the filament.
    {
        SpoolState state;
        state.update(tag, true, makeScale(tare + 201.0f + 1044.0f, tare));
        const SpoolState::State &s = state.state();
        check(s.referenceWeightsPresent, "reference weights present");
        checkNear(s.remainingGrams, 1044.0, 0.1, "remaining on a full spool");
        checkNear(s.consumedGrams, 0.0, 0.1, "consumed on a full spool");
        checkNear(s.remainingPercent, 100.0, 0.1, "percent remaining");
        check(s.remainingLengthValid, "length available");
        checkNear(s.remainingLengthMm, 343000.0, 1.0, "full length");
        check(!s.clampedEmpty && !s.clampedFull, "no clamping needed");
        check(s.impliedTareValid, "sealed spool implies a tare");
        checkNear(s.impliedDryerTareGrams, tare, 0.1, "implied tare matches");
        checkNear(s.impliedTareErrorGrams, 0.0, 0.1, "implied tare error zero");
    }

    // Half used.
    {
        SpoolState state;
        state.update(tag, true, makeScale(tare + 201.0f + 522.0f, tare));
        const SpoolState::State &s = state.state();
        checkNear(s.remainingGrams, 522.0, 0.1, "remaining at half");
        checkNear(s.consumedGrams, 522.0, 0.1, "consumed at half");
        checkNear(s.remainingPercent, 50.0, 0.1, "percent at half");
        checkNear(s.remainingLengthMm, 171500.0, 10.0, "length at half");
        checkNear(state.consumedForWriteBack(), 522.0, 0.01,
                  "write-back value rounded to grams");
    }

    // Below empty and above full both clamp, because the alternative is writing
    // nonsense to the tag.
    {
        SpoolState state;
        state.update(tag, true, makeScale(tare + 150.0f, tare));
        check(state.state().clampedEmpty, "clamped at empty");
        checkNear(state.state().remainingGrams, 0.0, 0.01, "remaining is zero");
        checkNear(state.state().consumedGrams, 1044.0, 0.01,
                  "consumed is the whole spool");

        state.update(tag, true, makeScale(tare + 201.0f + 1200.0f, tare));
        check(state.state().clampedFull, "clamped at full");
        checkNear(state.state().remainingGrams, 1044.0, 0.01,
                  "remaining is a full spool");
    }

    // Length falls back to density and diameter when the tag has no length.
    {
        const opt::OptTag noLength = makeTag(1000.0f, 201.0f, 0.0f, false, 0.0f);
        SpoolState state;
        state.update(noLength, true, makeScale(tare + 201.0f + 1000.0f, tare));
        check(state.state().remainingLengthValid,
              "length derived from density");
        // 1000 g at 1.24 g/cm3 through a 1.75 mm filament is about 335 m.
        checkNear(state.state().remainingLengthMm, 335300.0, 2000.0,
                  "derived length is plausible");
    }

    // The tag's own consumed_weight is reported alongside, and a large
    // disagreement is flagged rather than silently overwritten.
    {
        const opt::OptTag used = makeTag(1044.0f, 201.0f, 343000.0f, true,
                                         500.0f);
        SpoolState state;
        state.update(used, true, makeScale(tare + 201.0f + 544.0f, tare));
        const SpoolState::State &s = state.state();
        checkNear(s.consumedGrams, 500.0, 0.5, "scale agrees with the tag");
        check(!s.disagrees, "no disagreement flagged");
        check(!s.impliedTareValid, "a used spool cannot imply the tare");

        state.update(used, true, makeScale(tare + 201.0f + 844.0f, tare));
        check(state.state().disagrees, "300 g disagreement flagged");
        checkNear(state.state().disagreementGrams, 300.0, 1.0,
                  "disagreement size");
    }

    // Without a scale reading the tag's own value is still reported.
    {
        const opt::OptTag used = makeTag(1044.0f, 201.0f, 343000.0f, true,
                                         600.0f);
        ScaleManager::Status offline;
        offline.online = false;
        SpoolState state;
        state.update(used, true, offline);
        checkNear(state.state().remainingGrams, 444.0, 0.1,
                  "remaining from the tag alone");
        check(!state.state().haveWeight, "weight marked unavailable");
    }

    // No tag: nothing is asserted about the spool.
    {
        SpoolState state;
        state.update(opt::OptTag{}, false, makeScale(tare, tare));
        check(!state.state().haveTag, "no tag");
        check(!state.state().referenceWeightsPresent, "no reference weights");
    }
}

void testWriteBackPolicy() {
    gCurrentTest = "write-back policy";

    const float tare = 1140.0f;
    // A spool the tag believes is untouched, but which the scale says is 100 g
    // down: exactly the case a first write should correct.
    const opt::OptTag tag = makeTag(1044.0f, 201.0f, 343000.0f, true, 0.0f);
    SpoolState spool;
    spool.update(tag, true, makeScale(tare + 201.0f + 944.0f, tare));

    WriteBackPolicy policy;
    policy.setWorkgroup("dryer1");

    uint32_t now = 1000;
    check(policy.evaluate(now, tag, true, false, spool.state(),
                          spool.consumedForWriteBack()) ==
              WriteBackPolicy::Decision::Write,
          "a real disagreement is written immediately");

    // Each guard in turn.
    check(policy.evaluate(now, tag, false, false, spool.state(), 100.0f) ==
              WriteBackPolicy::Decision::NoTag,
          "no tag, no write");
    check(policy.evaluate(now, tag, true, true, spool.state(), 100.0f) ==
              WriteBackPolicy::Decision::TagWriteProtected,
          "a write protected tag is left alone");

    policy.setEnabled(false);
    check(policy.evaluate(now, tag, true, false, spool.state(), 100.0f) ==
              WriteBackPolicy::Decision::Disabled,
          "the switch is respected");
    policy.setEnabled(true);

    {
        SpoolState::State moving = spool.state();
        moving.stableWeight = false;
        check(policy.evaluate(now, tag, true, false, moving, 100.0f) ==
                  WriteBackPolicy::Decision::Unstable,
              "an unsettled weight is never written");

        SpoolState::State noReference = spool.state();
        noReference.referenceWeightsPresent = false;
        check(policy.evaluate(now, tag, true, false, noReference, 100.0f) ==
                  WriteBackPolicy::Decision::NoReferenceWeights,
              "nothing to write without reference weights");
    }

    // Somebody else's workgroup: their aux data stays theirs.
    {
        opt::OptTag foreign = tag;
        snprintf(foreign.aux.workgroup, sizeof(foreign.aux.workgroup), "shelf2");
        check(policy.evaluate(now, foreign, true, false, spool.state(),
                              100.0f) ==
                  WriteBackPolicy::Decision::WorkgroupMismatch,
              "a foreign workgroup blocks the write");

        opt::OptTag ours = tag;
        snprintf(ours.aux.workgroup, sizeof(ours.aux.workgroup), "dryer1");
        check(policy.evaluate(now, ours, true, false, spool.state(), 100.0f) ==
                  WriteBackPolicy::Decision::Write,
              "our own workgroup does not");
    }

    // After a write: the threshold and then the interval take over.
    policy.noteWrite(now, 100.0f);
    check(policy.evaluate(now, tag, true, false, spool.state(), 100.0f) ==
              WriteBackPolicy::Decision::AlreadyMatches,
          "the same value is not rewritten");
    check(policy.evaluate(now, tag, true, false, spool.state(), 102.0f) ==
              WriteBackPolicy::Decision::BelowThreshold,
          "2 g of change is below the 5 g threshold");
    check(policy.evaluate(now, tag, true, false, spool.state(), 130.0f) ==
              WriteBackPolicy::Decision::TooSoon,
          "30 g of change still waits out the interval");

    checkNear(policy.millisUntilAllowed(now), WRITEBACK_MIN_INTERVAL_MS, 1,
              "time remaining reported");

    now += WRITEBACK_MIN_INTERVAL_MS;
    checkEqualInt(policy.millisUntilAllowed(now), 0, "interval elapsed");
    check(policy.evaluate(now, tag, true, false, spool.state(), 130.0f) ==
              WriteBackPolicy::Decision::Write,
          "written once the interval has elapsed");
    check(policy.evaluate(now, tag, true, false, spool.state(), 101.0f) ==
              WriteBackPolicy::Decision::BelowThreshold,
          "the threshold still applies after the interval");

    // A swapped spool starts over, comparing against the new tag's own value.
    policy.reset();
    check(!policy.haveWritten(), "reset forgets the last write");
    check(policy.evaluate(now, tag, true, false, spool.state(), 100.0f) ==
              WriteBackPolicy::Decision::Write,
          "a new tag is compared against its own stored value");

    // Every reason has text, because these end up in front of a user.
    const WriteBackPolicy::Decision decisions[] = {
        WriteBackPolicy::Decision::Write,
        WriteBackPolicy::Decision::Disabled,
        WriteBackPolicy::Decision::NoTag,
        WriteBackPolicy::Decision::TagWriteProtected,
        WriteBackPolicy::Decision::NoReferenceWeights,
        WriteBackPolicy::Decision::Unstable,
        WriteBackPolicy::Decision::BelowThreshold,
        WriteBackPolicy::Decision::TooSoon,
        WriteBackPolicy::Decision::WorkgroupMismatch,
        WriteBackPolicy::Decision::AlreadyMatches,
    };
    for (WriteBackPolicy::Decision decision : decisions) {
        const char *text = WriteBackPolicy::decisionText(decision);
        check(text != nullptr && strcmp(text, "unknown") != 0,
              "every decision explains itself");
    }
}

void testPresets() {
    gCurrentTest = "prusament presets";

    size_t count = 0;
    const presets::MaterialPreset *table = presets::all(count);
    check(count >= 8, "preset table populated",
          std::to_string(count) + " entries");

    const presets::MaterialPreset *petg = presets::byId("petg");
    check(petg != nullptr, "petg preset found");
    if (petg != nullptr) {
        checkNear(petg->density, 1.27, 0.001, "petg density");
        checkEqualInt(petg->materialType, 1, "petg material type enum");
        checkEqualInt(petg->dryingC, 55, "petg drying temperature");
        checkEqualInt(petg->dryingMinutes, 360, "petg drying time");
        check(!presets::needsHotterDryer(*petg), "petg fits this dryer");
    }

    const presets::MaterialPreset *asa = presets::byId("asa");
    check(asa != nullptr, "asa preset found");
    if (asa != nullptr) {
        checkEqualInt(asa->dryingC, 80, "asa drying temperature");
        check(presets::needsHotterDryer(*asa),
              "asa needs more than this dryer can do");
    }

    check(presets::byId("nonexistent") == nullptr, "unknown id rejected");

    // Every entry must map to a material type the specification defines and
    // stay inside the tag's field limits.
    for (size_t i = 0; i < count; ++i) {
        check(opt::materialTypeName(table[i].materialType) != nullptr,
              "material type is a specification value", table[i].id);
        check(strlen(table[i].abbreviation) <= 7,
              "abbreviation fits the tag field", table[i].id);
        check(table[i].minPrintC < table[i].maxPrintC,
              "print temperature range ordered", table[i].id);
        check(table[i].minBedC <= table[i].maxBedC,
              "bed temperature range ordered", table[i].id);
        for (uint8_t t = 0; t < table[i].tagCount; ++t) {
            check(opt::materialTagName(table[i].tags[t]) != nullptr,
                  "material tag is a specification value", table[i].id);
        }
    }

    // Colour parsing, both forms plus rejection of nonsense.
    uint8_t rgba[4] = {0, 0, 0, 0};
    check(presets::parseColor("#3d3e3d", rgba), "6 digit hex parses");
    checkEqualInt(rgba[0], 0x3D, "red");
    checkEqualInt(rgba[3], 0xFF, "alpha defaults to opaque");
    check(presets::parseColor("24292aff", rgba), "8 digit hex without hash");
    checkEqualInt(rgba[2], 0x2A, "blue");
    check(!presets::parseColor("#12345", rgba), "wrong length rejected");
    check(!presets::parseColor("#3d3e3g", rgba), "non-hex rejected");

    // Applying a preset fills the tag's main region the way Prusa's own tags
    // are laid out.
    opt::OptMainData main;
    presets::parseColor("#24292a", rgba);
    presets::applyToMain(*petg, "Jet Black", rgba, main);
    check(strcmp(main.materialName, "PETG Jet Black") == 0,
          "material name composed from type and colour", main.materialName);
    check(strcmp(main.brandName, "Prusament") == 0, "brand name");
    checkEqualInt(main.materialClass, 0, "material class is FFF");
    checkNear(main.density, 1.27, 0.001, "density applied");
    checkNear(main.filamentDiameter, 1.75, 0.001, "diameter applied");
    checkEqualInt(main.dryingTemperature, 55, "drying temperature applied");
    checkEqualInt(main.minBedTemperature, 70, "bed temperature applied");
    checkEqualInt(main.writeProtection, 0, "our own tags stay unlocked");
    checkNear(main.nominalNettoFullWeight, 1000.0, 0.1, "nominal weight");
    checkEqualInt(main.primaryColorLen, 4, "colour stored with alpha");

    // The Galaxy preset against a spool Prusa provisioned itself: Prusament PLA
    // Galaxy Black, read out of a factory tag with the Prusa app. material_uuid
    // is derived by UUIDv5 over brand_uuid plus material_name, so these two
    // strings are not decoration -- get either wrong and a spool this device
    // provisions is a different material to every other tool that reads it.
    {
        const presets::MaterialPreset *galaxy = presets::byId("pla_galaxy");
        check(galaxy != nullptr, "galaxy preset exists");
        opt::OptMainData galaxyMain;
        uint8_t galaxyRgba[4] = {0, 0, 0, 0};
        const presets::ColorPreset *black = presets::colorByName("Galaxy Black");
        check(black != nullptr, "Galaxy Black colour preset exists");
        memcpy(galaxyRgba, black->rgba, 4);
        presets::applyToMain(*galaxy, black->name, galaxyRgba, galaxyMain);
        check(strcmp(galaxyMain.materialName, "PLA Prusa Galaxy Black") == 0,
              "material name matches the factory tag exactly",
              galaxyMain.materialName);
        check(strcmp(galaxyMain.brandName, "Prusament") == 0,
              "brand name matches the factory tag exactly");
        checkEqualInt(galaxyMain.primaryColor[0], 0x3D, "Galaxy Black red");
        // Temperatures as the factory tag carries them, not as a data sheet
        // rounds them.
        checkEqualInt(galaxyMain.minPrintTemperature, 205, "nozzle minimum");
        checkEqualInt(galaxyMain.maxPrintTemperature, 225, "nozzle maximum");
        checkEqualInt(galaxyMain.preheatTemperature, 170, "preheat");
        checkEqualInt(galaxyMain.minBedTemperature, 40, "bed minimum");
        checkEqualInt(galaxyMain.maxBedTemperature, 60, "bed maximum");
        checkNear(galaxyMain.density, 1.24, 0.001, "PLA density");
    }

    // A preset plus per-spool numbers has to encode into a real tag image.
    opt::OptBuildRequest request;
    request.main = main;
    request.main.hasActualNettoFullWeight = true;
    request.main.actualNettoFullWeight = 1012.0f;
    request.main.hasEmptyContainerWeight = true;
    request.main.emptyContainerWeight = 201.0f;
    request.main.hasActualFullLength = true;
    request.main.actualFullLength = 331000.0f;
    snprintf(request.main.brandSpecificInstanceId,
             sizeof(request.main.brandSpecificInstanceId), "7ab2acb509");
    snprintf(request.workgroup, sizeof(request.workgroup), "dryer1");
    request.tagCapacity = opt::kSlix2Bytes;

    uint8_t image[opt::kMaxTagImage];
    size_t imageLen = 0;
    check(opt::optBuildImage(request, image, sizeof(image), imageLen,
                             nullptr) == opt::OptStatus::Ok,
          "preset-built tag image encodes");

    opt::OptTag decoded;
    check(opt::optParse(image, imageLen, decoded) == opt::OptStatus::Ok,
          "preset-built tag image decodes");
    check(strcmp(decoded.main.materialName, "PETG Jet Black") == 0,
          "material name survives the tag round trip");
    checkNear(decoded.main.actualNettoFullWeight, 1012.0, 0.5,
              "spool weight survives the tag round trip");
    checkEqualInt(decoded.main.dryingTemperature, 55,
                  "drying temperature survives the tag round trip");
}

// ---------------------------------------------------------------------------
// A JSON reader just good enough for what the firmware emits: one object of
// string and number values. It exists to prove the output is well formed even
// when a tag carries quotes or control characters in its material name.
// ---------------------------------------------------------------------------
class JsonObject {
public:
    bool parse(const std::string &text) {
        keys_.clear();
        values_.clear();
        size_t i = 0;
        skipSpace(text, i);
        if (i >= text.size() || text[i] != '{') return false;
        ++i;
        skipSpace(text, i);
        if (i < text.size() && text[i] == '}') return i + 1 == text.size();

        while (i < text.size()) {
            std::string key;
            if (!parseString(text, i, key)) return false;
            skipSpace(text, i);
            if (i >= text.size() || text[i] != ':') return false;
            ++i;
            skipSpace(text, i);
            std::string value;
            if (!parseValue(text, i, value)) return false;
            for (const std::string &existing : keys_) {
                if (existing == key) return false;  // duplicate key
            }
            keys_.push_back(key);
            values_.push_back(value);
            skipSpace(text, i);
            if (i < text.size() && text[i] == ',') {
                ++i;
                skipSpace(text, i);
                continue;
            }
            if (i < text.size() && text[i] == '}') {
                ++i;
                skipSpace(text, i);
                return i == text.size();
            }
            return false;
        }
        return false;
    }

    bool has(const std::string &key) const { return index(key) >= 0; }

    std::string get(const std::string &key) const {
        const int at = index(key);
        return at < 0 ? std::string() : values_[static_cast<size_t>(at)];
    }

    double number(const std::string &key) const {
        return strtod(get(key).c_str(), nullptr);
    }

    size_t size() const { return keys_.size(); }

private:
    static void skipSpace(const std::string &text, size_t &i) {
        while (i < text.size() && isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
    }

    static bool parseString(const std::string &text, size_t &i,
                            std::string &out) {
        if (i >= text.size() || text[i] != '"') return false;
        ++i;
        out.clear();
        while (i < text.size()) {
            const char c = text[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= text.size()) return false;
            const char escape = text[i++];
            switch (escape) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > text.size()) return false;
                for (int digit = 0; digit < 4; ++digit) {
                    if (!isxdigit(static_cast<unsigned char>(text[i + digit]))) {
                        return false;
                    }
                }
                out += '?';  // the value does not matter here, only the syntax
                i += 4;
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    static bool parseValue(const std::string &text, size_t &i,
                           std::string &out) {
        if (i < text.size() && text[i] == '"') {
            return parseString(text, i, out);
        }
        const size_t start = i;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
        bool digits = false;
        while (i < text.size() &&
               (isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.' ||
                text[i] == 'e' || text[i] == 'E' || text[i] == '-' ||
                text[i] == '+')) {
            if (isdigit(static_cast<unsigned char>(text[i]))) digits = true;
            ++i;
        }
        if (!digits) {
            // true, false and null are legal JSON but nothing here emits them.
            return false;
        }
        out = text.substr(start, i - start);
        return true;
    }

    int index(const std::string &key) const {
        for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) return static_cast<int>(i);
        }
        return -1;
    }

    std::vector<std::string> keys_;
    std::vector<std::string> values_;
};

void testMqttStateDocument() {
    gCurrentTest = "mqtt state document";

    const float tare = 1140.0f;
    opt::OptTag tag = makeTag(1044.0f, 201.0f, 343000.0f, true, 500.0f);
    snprintf(tag.main.brandName, sizeof(tag.main.brandName), "Prusament");
    snprintf(tag.main.materialName, sizeof(tag.main.materialName),
             "PETG Jet Black");
    snprintf(tag.main.brandSpecificInstanceId,
             sizeof(tag.main.brandSpecificInstanceId), "7ab2acb509");
    tag.main.hasMaterialType = true;
    tag.main.materialType = 1;  // petg
    tag.main.hasPrimaryColor = true;
    tag.main.primaryColor[0] = 0x24;
    tag.main.primaryColor[1] = 0x29;
    tag.main.primaryColor[2] = 0x2A;
    tag.main.hasMinPrintTemperature = true;
    tag.main.minPrintTemperature = 220;
    tag.main.hasMaxPrintTemperature = true;
    tag.main.maxPrintTemperature = 240;
    tag.main.hasDryingTemperature = true;
    tag.main.dryingTemperature = 55;
    tag.main.hasDryingTime = true;
    tag.main.dryingTime = 360;

    SpoolState spool;
    ScaleManager::Status scale = makeScale(tare + 201.0f + 544.0f, tare);
    scale.temperatureValid = true;
    scale.temperatureC = 41.5f;
    spool.update(tag, true, scale);

    NfcManager::Status nfc;
    nfc.readerOnline = true;
    nfc.tagPresent = true;
    nfc.tagDecoded = true;
    nfc.blocksWritten = 3;
    nfc.lastReadMs = 10000;
    nfc.lastWriteMs = 5000;
    snprintf(nfc.uid, sizeof(nfc.uid), "E00401509A2B3C4D");

    WriteBackPolicy policy;

    HaMqtt::StateInput input;
    input.nfc = &nfc;
    input.tag = &tag;
    input.scale = &scale;
    input.spool = &spool.state();
    input.policy = &policy;
    input.writeStatus = "waiting for the weight to settle";
    input.lastCommand = "dryer tare stored as 1140.0 g";
    input.storageLocation = "shelf-a";
    input.calibrationMass = 1000.0f;
    input.nowMs = 70000;

    char json[1024];
    const size_t len = HaMqtt::buildStateJson(input, json, sizeof(json));
    check(len > 0 && len < sizeof(json), "state document fits the buffer",
          std::to_string(len) + " B");

    JsonObject parsed;
    check(parsed.parse(json), "state document is valid JSON", json);

    checkNear(parsed.number("gross_g"), 1885.0, 0.05, "gross weight");
    checkNear(parsed.number("remaining_g"), 544.0, 0.05, "remaining weight");
    checkNear(parsed.number("remaining_pct"), 52.1, 0.1, "remaining percent");
    checkNear(parsed.number("remaining_m"), 178.7, 0.5,
              "remaining length in metres");
    checkNear(parsed.number("consumed_g"), 500.0, 0.05, "consumed weight");
    checkNear(parsed.number("tag_consumed_g"), 500.0, 0.05,
              "consumed weight from the tag");
    checkNear(parsed.number("reference_g"), 1044.0, 0.05, "reference weight");
    checkNear(parsed.number("empty_g"), 201.0, 0.05, "empty spool weight");
    checkNear(parsed.number("cell_c"), 41.5, 0.01, "cell temperature");
    checkNear(parsed.number("drying_c"), 55, 0.01, "drying temperature");
    checkNear(parsed.number("drying_min"), 360, 0.01, "drying time");
    checkNear(parsed.number("last_read_s"), 60, 0.01, "seconds since the read");
    checkNear(parsed.number("last_write_s"), 65, 0.01,
              "seconds since the write");
    checkNear(parsed.number("blocks_written"), 3, 0.01, "blocks written");
    checkNear(parsed.number("calibration_mass"), 1000, 0.01,
              "calibration mass");

    check(parsed.get("brand") == "Prusament", "brand");
    check(parsed.get("material_name") == "PETG Jet Black", "material name");
    check(parsed.get("material_type") == "PETG",
          "material type resolved through the specification enum",
          parsed.get("material_type"));
    check(parsed.get("spool_id") == "7ab2acb509", "spool id");
    check(parsed.get("color") == "#24292A", "colour as hex");
    check(parsed.get("tag_uid") == "E00401509A2B3C4D", "tag uid");
    check(parsed.get("tag_present") == "ON", "tag present");
    check(parsed.get("stable") == "ON", "weight stable");
    check(parsed.get("reader_online") == "ON", "reader online");
    check(parsed.get("scale_online") == "ON", "scale online");
    check(parsed.get("disagrees") == "OFF", "no disagreement");
    check(parsed.get("auto_writeback") == "ON", "write-back switch state");
    check(parsed.get("error") == "none", "no error");
    check(parsed.get("storage_location") == "shelf-a", "storage location");
    check(parsed.get("print_temp").find("220-240") == 0,
          "print temperature range", parsed.get("print_temp"));

    // Missing measurements are left out rather than published as zero, so Home
    // Assistant shows "unknown" instead of a plausible lie.
    {
        NfcManager::Status empty;
        ScaleManager::Status noScale;
        SpoolState noSpool;
        noSpool.update(opt::OptTag{}, false, noScale);

        HaMqtt::StateInput bare;
        bare.nfc = &empty;
        bare.scale = &noScale;
        bare.spool = &noSpool.state();
        bare.policy = &policy;
        bare.nowMs = 1000;

        char bareJson[1024];
        HaMqtt::buildStateJson(bare, bareJson, sizeof(bareJson));
        JsonObject bareParsed;
        check(bareParsed.parse(bareJson), "empty state document is valid JSON",
              bareJson);
        check(!bareParsed.has("gross_g"), "no weight without a reading");
        check(!bareParsed.has("remaining_g"), "no remaining without a tag");
        check(!bareParsed.has("brand"), "no brand without a tag");
        check(!bareParsed.has("last_read_s"), "no read age before a read");
        check(bareParsed.get("tag_present") == "OFF", "tag absent");
        check(bareParsed.get("reader_online") == "OFF", "reader offline");
    }

    // A tag whose text fields contain JSON metacharacters must not be able to
    // break the document. Tag contents are untrusted input.
    {
        opt::OptTag hostile = tag;
        snprintf(hostile.main.materialName, sizeof(hostile.main.materialName),
                 "PLA \"quoted\" \\ back\tslash");
        snprintf(hostile.main.brandName, sizeof(hostile.main.brandName),
                 "a\"b");
        SpoolState hostileSpool;
        hostileSpool.update(hostile, true, scale);

        HaMqtt::StateInput input2 = input;
        input2.tag = &hostile;
        input2.spool = &hostileSpool.state();

        char hostileJson[1024];
        HaMqtt::buildStateJson(input2, hostileJson, sizeof(hostileJson));
        JsonObject hostileParsed;
        check(hostileParsed.parse(hostileJson),
              "quotes and control characters stay inside the string",
              hostileJson);
        check(hostileParsed.get("material_name") ==
                  "PLA \"quoted\" \\ back\tslash",
              "escaped text round-trips",
              hostileParsed.get("material_name"));
    }
}

void testMqttDiscoveryAndCommands() {
    gCurrentTest = "mqtt discovery and commands";
    hostio::reset();
    hostio::resetBroker();

    HostClient client;
    HaMqtt::Config config;
    snprintf(config.host, sizeof(config.host), "mqtt.local");

    // Every command records that it ran, so the dispatch can be checked.
    std::string log;
    HaMqtt::Callbacks callbacks;
    callbacks.onTare = [&log]() { log += "tare;"; return String("tared"); };
    callbacks.onCalibrate = [&log]() {
        log += "calibrate;";
        return String("calibrated");
    };
    callbacks.onCalibrationMass = [&log](float grams) {
        log += "mass=" + std::to_string(static_cast<int>(grams)) + ";";
        return String("mass set");
    };
    callbacks.onWriteNow = [&log]() { log += "write;"; return String("written"); };
    callbacks.onAutoWriteBack = [&log](bool on) {
        log += on ? "auto=on;" : "auto=off;";
        return String("switched");
    };
    callbacks.onPollEnabled = [&log](bool on) {
        log += on ? "poll=on;" : "poll=off;";
        return String("polling changed");
    };
    callbacks.onWipeAux = [&log]() { log += "wipe;"; return String("wiped"); };
    callbacks.onStorageLocation = [&log](const String &where) {
        log += "where=" + std::string(where.c_str()) + ";";
        return String("location set");
    };
    callbacks.onRefresh = [&log]() { log += "refresh;"; return String("re-read"); };

    HaMqtt mqtt;
    mqtt.begin(client, config, "dryer_a1b2c3", "Filament dryer", callbacks);
    check(mqtt.loop(0), "connects to the broker");
    check(hostio::broker().clientId == "dryer_a1b2c3", "client id");

    // Availability, including the will so a power cut shows as unavailable.
    check(hostio::broker().willTopic ==
              "openprinttag_dryer/dryer_a1b2c3/status",
          "will topic", hostio::broker().willTopic);
    check(hostio::broker().willPayload == "offline", "will payload");
    const hostio::MqttMessage *availability =
        hostio::broker().find("openprinttag_dryer/dryer_a1b2c3/status");
    check(availability != nullptr && availability->payload == "online",
          "published as online");
    check(availability != nullptr && availability->retained,
          "availability is retained");

    // Discovery: one retained config per entity, all with unique topics and
    // unique ids, all small enough for the client's buffer.
    const size_t discoveryCount =
        hostio::broker().countWithPrefix("homeassistant/");
    check(discoveryCount >= 30, "a discovery message per entity",
          std::to_string(discoveryCount));

    std::vector<std::string> topics;
    std::vector<std::string> uniqueIds;
    bool allRetained = true;
    bool allHaveDevice = true;
    bool allHaveAvailability = true;
    size_t largest = 0;
    for (const hostio::MqttMessage &message : hostio::broker().published) {
        if (message.topic.rfind("homeassistant/", 0) != 0) {
            continue;
        }
        for (const std::string &seen : topics) {
            check(seen != message.topic, "discovery topics are unique",
                  message.topic);
        }
        topics.push_back(message.topic);
        if (!message.retained) allRetained = false;
        if (message.payload.find("\"dev\":{") == std::string::npos) {
            allHaveDevice = false;
        }
        if (message.payload.find("\"avty_t\":") == std::string::npos) {
            allHaveAvailability = false;
        }
        if (message.payload.size() > largest) largest = message.payload.size();

        const size_t at = message.payload.find("\"uniq_id\":\"");
        if (at != std::string::npos) {
            const size_t start = at + 11;
            const size_t end = message.payload.find('"', start);
            const std::string id = message.payload.substr(start, end - start);
            for (const std::string &seen : uniqueIds) {
                check(seen != id, "unique ids are unique", id);
            }
            uniqueIds.push_back(id);
        }

        JsonObject ignored;
        (void)ignored;  // discovery payloads nest, so only the shape is checked
        check(message.payload.front() == '{' && message.payload.back() == '}',
              "discovery payload is an object");
    }
    check(allRetained, "discovery messages are retained");
    check(allHaveDevice, "every entity carries the device block");
    check(allHaveAvailability, "every entity carries the availability topic");
    check(uniqueIds.size() == topics.size(), "every entity has a unique id");
    check(largest + 64 < hostio::broker().bufferSize,
          "largest discovery payload fits the client buffer with room to spare",
          std::to_string(largest) + " B of " +
              std::to_string(hostio::broker().bufferSize));

    // The state topic and value templates have to agree with the document the
    // firmware actually builds, or every entity reads as unknown.
    const hostio::MqttMessage *remaining = hostio::broker().find(
        "homeassistant/sensor/dryer_a1b2c3/remaining_weight/config");
    check(remaining != nullptr, "remaining weight was announced");
    if (remaining != nullptr) {
        check(remaining->payload.find(
                  "\"stat_t\":\"openprinttag_dryer/dryer_a1b2c3/state\"") !=
                  std::string::npos,
              "state topic");
        check(remaining->payload.find("value_json.remaining_g") !=
                  std::string::npos,
              "value template");
        check(remaining->payload.find("\"dev_cla\":\"weight\"") !=
                  std::string::npos,
              "device class");
    }

    // Commands: subscribed, then dispatched by topic suffix.
    check(hostio::broker().subscribed("openprinttag_dryer/dryer_a1b2c3/cmd/tare"),
          "subscribed to the tare command");
    check(hostio::broker().subscribed(
              "openprinttag_dryer/dryer_a1b2c3/cmd/storage_location"),
          "subscribed to the storage location command");

    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/tare", "PRESS");
    check(log == "tare;", "tare dispatched", log);
    check(mqtt.takePublishRequest(), "a command asks for a state publish");
    check(!mqtt.takePublishRequest(), "the request clears when taken");
    check(std::string(mqtt.lastCommandResult()) == "tared",
          "the command result is kept", mqtt.lastCommandResult());

    log.clear();
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/calibration_mass",
                            "500");
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/calibrate",
                            "PRESS");
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/auto_writeback",
                            "OFF");
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/auto_writeback",
                            "ON");
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/poll_enabled",
                            "OFF");
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/write_now",
                            "PRESS");
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/wipe_aux",
                            "PRESS");
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/refresh",
                            "PRESS");
    hostio::deliverToClient(
        "openprinttag_dryer/dryer_a1b2c3/cmd/storage_location", "shelf-b");
    check(log ==
              "mass=500;calibrate;auto=off;auto=on;poll=off;write;wipe;refresh;"
              "where=shelf-b;",
          "every command reaches its handler", log);

    // An unknown command is ignored rather than mistaken for another one.
    log.clear();
    hostio::deliverToClient("openprinttag_dryer/dryer_a1b2c3/cmd/nonsense",
                            "PRESS");
    check(log.empty(), "unknown commands are ignored", log);

    // A broker that will not accept the connection must not wedge the device.
    hostio::resetBroker();
    hostio::broker().acceptConnections = false;
    HaMqtt offline;
    offline.begin(client, config, "dryer_a1b2c3", "Filament dryer", callbacks);
    check(!offline.loop(0), "a refused connection is reported");
    check(offline.lastError()[0] != '\0', "and explained",
          offline.lastError());
    check(!offline.loop(1000), "retries are rate limited, not hammered");
}

// Brings an App up with a calibrated scale and a 1.14 kg dryer on it, which is
// the state every one of these tests wants to start from.
void startApp(App &app, float extraGrams) {
    hostio::reset();
    hostio::attachHx711(PIN_HX711_DOUT, PIN_HX711_SCK);
    hostio::setHx711Value(100000, true);

    app.begin("dryer_test");

    char message[128] = {0};
    feedScale(app.scale(), 100000, 6);
    app.scale().zeroPlatform(message, sizeof(message));
    feedScale(app.scale(), 121000, 6);
    app.scale().calibrateSpan(1000.0f, message, sizeof(message));
    feedScale(app.scale(), 100000 + static_cast<long>(1140 * 21), 6);
    app.scale().tareDryer(message, sizeof(message));

    const long counts = 100000 + static_cast<long>((1140.0f + extraGrams) * 21);
    feedScale(app.scale(), counts, 6);
}

void testProvisioningInput() {
    gCurrentTest = "provisioning input";

    App app;
    // A full Prusament PETG spool: 1044 g of filament on a 201 g spool.
    startApp(app, 1245.0f);

    App::ProvisionInput input;
    snprintf(input.materialId, sizeof(input.materialId), "petg");
    snprintf(input.colorName, sizeof(input.colorName), "Jet Black");
    snprintf(input.spoolId, sizeof(input.spoolId), "7ab2acb509");
    input.filamentGrams = 1044.0f;
    input.spoolGrams = 201.0f;
    input.lengthMetres = 343.0f;

    // The cross-check is the guard against a typo in the entered numbers.
    App::WeightCheck weights = app.checkEnteredWeights(1044.0f, 201.0f);
    check(weights.performed, "the scale can check the entered weights",
          weights.note);
    check(weights.agrees, "correct numbers agree with the scale", weights.note);
    checkNear(weights.measuredGrams, 1245.0, 2.0, "measured spool weight");

    weights = app.checkEnteredWeights(1440.0f, 201.0f);  // transposed digits
    check(weights.performed, "a transposed weight is still checked");
    check(!weights.agrees, "a transposed weight is caught", weights.note);
    checkNear(weights.differenceGrams, -396.0, 2.0, "size of the disagreement");
    check(std::string(weights.note).find("Check the spool ID") !=
              std::string::npos,
          "the message says what to do", weights.note);

    // The built image has to be a tag a phone would read as this spool.
    opt::OptBuildRequest request;
    String error;
    check(app.buildRequest(input, request, error), "request builds",
           error.c_str());

    uint8_t image[opt::kMaxTagImage];
    size_t imageLen = 0;
    opt::OptRegions regions;
    check(opt::optBuildImage(request, image, sizeof(image), imageLen,
                              &regions) == opt::OptStatus::Ok,
           "image encodes");
    check(imageLen <= opt::kSlix2Bytes, "image fits a SLIX2",
           std::to_string(imageLen) + " B");

    opt::OptTag decoded;
    check(opt::optParse(image, imageLen, decoded) == opt::OptStatus::Ok,
           "image decodes");
    check(strcmp(decoded.main.materialName, "PETG Jet Black") == 0,
           "material name", decoded.main.materialName);
    check(strcmp(decoded.main.brandName, "Prusament") == 0, "brand name");
    check(strcmp(decoded.main.brandSpecificInstanceId, "7ab2acb509") == 0,
           "spool id", decoded.main.brandSpecificInstanceId);
    checkNear(decoded.main.actualNettoFullWeight, 1044.0, 0.5,
              "filament weight");
    checkNear(decoded.main.emptyContainerWeight, 201.0, 0.5, "spool weight");
    checkNear(decoded.main.actualFullLength, 343000.0, 1.0, "length in mm");
    checkNear(decoded.main.density, 1.27, 0.005, "density from the preset");
    checkEqualInt(decoded.main.dryingTemperature, 55, "drying temperature");
    checkEqualInt(decoded.main.writeProtection, 0, "tag is left unlocked");
    check(decoded.regions.hasAux, "an aux region was reserved");
    check(decoded.regions.auxSize >= AUX_REGION_SIZE_BYTES,
           "aux region is at least the configured size",
           std::to_string(decoded.regions.auxSize));
    check(decoded.aux.hasConsumedWeight && decoded.aux.consumedWeight == 0.0f,
           "aux region starts at zero consumed");
    check(decoded.aux.workgroup[0] != '\0', "aux region carries a workgroup",
           decoded.aux.workgroup);
    check(!decoded.hasUriRecord, "no URI record unless asked for");

    // The optional URI record, which competes for the tag's memory.
    {
        App::ProvisionInput withUri = input;
        withUri.includeUri = true;
        opt::OptBuildRequest uriRequest;
        check(app.buildRequest(withUri, uriRequest, error), "URI request builds",
               error.c_str());
        size_t uriLen = 0;
        check(opt::optBuildImage(uriRequest, image, sizeof(image), uriLen,
                                  nullptr) == opt::OptStatus::Ok,
               "image with a URI record encodes");
        opt::OptTag uriDecoded;
        check(opt::optParse(image, uriLen, uriDecoded) == opt::OptStatus::Ok,
               "image with a URI record decodes");
        check(uriDecoded.hasUriRecord, "the URI record is there");
        check(strcmp(uriDecoded.main.materialName, "PETG Jet Black") == 0,
               "and the spool data survives beside it");
    }

    // Rejections, all before anything reaches a tag.
    {
        App::ProvisionInput bad = input;
        snprintf(bad.materialId, sizeof(bad.materialId), "unobtainium");
        opt::OptBuildRequest ignored;
        check(!app.buildRequest(bad, ignored, error), "unknown material");
        check(std::string(error.c_str()).find("unobtainium") !=
                   std::string::npos,
               "and it says which one", error.c_str());

        bad = input;
        bad.filamentGrams = 0.0f;
        check(!app.buildRequest(bad, ignored, error), "no filament weight");

        bad = input;
        bad.spoolGrams = 5.0f;
        check(!app.buildRequest(bad, ignored, error), "absurd spool weight");

        bad = input;
        snprintf(bad.colorHex, sizeof(bad.colorHex), "#nothex");
        check(!app.buildRequest(bad, ignored, error), "bad colour");
    }

    // A colour given as hex overrides the named list.
    {
        App::ProvisionInput custom = input;
        snprintf(custom.colorName, sizeof(custom.colorName), "Chartreuse");
        snprintf(custom.colorHex, sizeof(custom.colorHex), "#7fff00");
        opt::OptBuildRequest customRequest;
        check(app.buildRequest(custom, customRequest, error),
               "custom colour builds", error.c_str());
        checkEqualInt(customRequest.main.primaryColor[0], 0x7F, "red");
        checkEqualInt(customRequest.main.primaryColor[1], 0xFF, "green");
        checkEqualInt(customRequest.main.primaryColor[2], 0x00, "blue");
        check(strcmp(customRequest.main.materialName, "PETG Chartreuse") == 0,
               "name uses the free-form colour",
               customRequest.main.materialName);
    }

    // The same colour without a hex has nowhere to get one from, and grey would
    // be a confidently wrong answer.
    {
        App::ProvisionInput nameOnly = input;
        snprintf(nameOnly.colorName, sizeof(nameOnly.colorName), "Chartreuse");
        nameOnly.colorHex[0] = '\0';
        opt::OptBuildRequest ignored;
        check(!app.buildRequest(nameOnly, ignored, error),
               "a colour outside the presets needs a hex");
        check(std::string(error.c_str()).find("Chartreuse") !=
                   std::string::npos,
               "and the refusal names the colour", error.c_str());

        // A preset colour still needs no hex.
        App::ProvisionInput preset = input;
        snprintf(preset.colorName, sizeof(preset.colorName), "Galaxy Black");
        preset.colorHex[0] = '\0';
        opt::OptBuildRequest presetRequest;
        check(app.buildRequest(preset, presetRequest, error),
               "a named preset colour still builds", error.c_str());
        checkEqualInt(presetRequest.main.primaryColor[0], 0x3D,
                      "and brings its own hex");
    }

    // Woodfill: a wood-filled PLA, which is how a spool with no Prusa tag to
    // copy from gets provisioned. The colour is typed in with its hex because
    // Prusa publishes none for it.
    {
        App::ProvisionInput wood = input;
        snprintf(wood.materialId, sizeof(wood.materialId), "woodfill");
        snprintf(wood.colorName, sizeof(wood.colorName), "Linden Light");
        snprintf(wood.colorHex, sizeof(wood.colorHex), "#c8a87c");
        opt::OptBuildRequest woodRequest;
        check(app.buildRequest(wood, woodRequest, error), "woodfill builds",
               error.c_str());
        // Prusa's product is "Prusament Woodfill Linden Light", and
        // material_uuid is derived from brand plus this string, so it has to
        // match rather than merely read well.
        check(strcmp(woodRequest.main.materialName, "Woodfill Linden Light") == 0,
               "material name matches Prusa's own product name",
               woodRequest.main.materialName);
        checkEqualInt(woodRequest.main.materialType, 0,
                      "wood-filled PLA is still PLA to the specification");
        check(strcmp(woodRequest.main.materialAbbreviation, "PLA-W") == 0,
               "abbreviation says which PLA",
               woodRequest.main.materialAbbreviation);
    }

    // The bounds on a hand-entered tare. Both digit slips a person actually
    // makes, and the figures this project's own spools use.
    check(!App::emptySpoolWeightPlausible(0.0f), "an empty box is not a tare");
    check(!App::emptySpoolWeightPlausible(18.0f), "18 g is not a spool");
    check(!App::emptySpoolWeightPlausible(1860.0f),
          "nor is 1860 g, the digit slip for 186");
    check(App::emptySpoolWeightPlausible(186.0f), "a bare Prusament spool");
    check(App::emptySpoolWeightPlausible(217.0f), "and one with the adapter");

    // Whatever the figure, a tag has to be in the field: the reader's absence is
    // more useful to report than an opinion about the number.
    {
        const String noTag = app.setEmptySpoolWeight(217.0f);
        check(std::string(noTag.c_str()).find("no readable OpenPrintTag") !=
                   std::string::npos,
               "correcting the tare needs a tag to correct", noTag.c_str());
    }

    // Provisioning without a reader fails at the radio, not before: the point
    // is that the refusal is explained rather than silent.
    const String result = app.provision(input);
    check(std::string(result.c_str()).find("failed") != std::string::npos ||
               std::string(result.c_str()).find("reader") != std::string::npos,
           "provisioning without a reader is reported", result.c_str());
}

void testAppActions() {
    gCurrentTest = "app actions";

    App app;
    startApp(app, 1245.0f);

    // Calibration mass bounds.
    check(std::string(app.setCalibrationMass(1000.0f).c_str())
               .find("1000") != std::string::npos,
           "a sensible calibration mass is accepted");
    check(std::string(app.setCalibrationMass(5.0f).c_str())
               .find("between") != std::string::npos,
           "a silly calibration mass is refused");
    checkNear(app.settings().calibrationMassGrams, 1000.0, 0.01,
              "the refused value did not stick");

    // Write-back switch and storage location persist.
    app.setAutoWriteBack(false);
    check(!app.policy().enabled(), "write-back switched off");
    check(!app.settings().autoWriteBack, "and remembered");
    app.setAutoWriteBack(true);
    check(app.policy().enabled(), "and back on");

    app.setStorageLocation(String("shelf-b"));
    check(strcmp(app.settings().storageLocation, "shelf-b") == 0,
           "storage location set");
    app.setStorageLocation(String(""));
    check(app.settings().storageLocation[0] == '\0',
           "and can be cleared");

    // Drift coefficient bounds.
    app.setDriftCoefficient(0.4f);
    checkNear(app.scale().status().driftPerC, 0.4, 0.001, "drift set");
    check(std::string(app.setDriftCoefficient(500.0f).c_str())
               .find("believable") != std::string::npos,
           "an absurd drift coefficient is refused");
    checkNear(app.scale().status().driftPerC, 0.4, 0.001, "and ignored");

    // Actions that need a tag say so instead of failing obscurely.
    check(std::string(app.writeNow().c_str()).find("no readable tag") !=
               std::string::npos,
           "write with no tag", app.writeNow().c_str());
    check(std::string(app.wipeAux().c_str()).find("no readable tag") !=
               std::string::npos,
           "wipe with no tag");
    check(std::string(app.adoptImpliedTare().c_str()).find("sealed spool") !=
               std::string::npos,
           "adopting the implied tare needs a sealed spool");

    // The self-test has to work with nothing attached: that is exactly when
    // somebody reads it.
    const String report = app.selfTest();
    check(report.length() > 100, "self-test produces a report",
           std::to_string(report.length()) + " B");
    const std::string text(report.c_str());
    check(text.find("PN5180") != std::string::npos, "covers the reader");
    check(text.find("HX711") != std::string::npos, "covers the scale");
    check(text.find("workgroup") != std::string::npos, "covers the workgroup");

    // Settings survive a restart.
    app.settings().writeMinDeltaGrams = 12.5f;
    app.settings().writeMinIntervalMinutes = 30;
    snprintf(app.settings().workgroup, sizeof(app.settings().workgroup),
             "shelf1");
    app.settings().save();

    Settings reloaded;
    reloaded.load("dryer_test");
    checkNear(reloaded.writeMinDeltaGrams, 12.5, 0.01, "threshold persisted");
    checkEqualInt(reloaded.writeMinIntervalMinutes, 30, "interval persisted");
    check(strcmp(reloaded.workgroup, "shelf1") == 0, "workgroup persisted");

    // A device that has never been configured still gets a workgroup, because
    // writing aux data without one would trample anybody else's.
    hostio::clearPreferences();
    Settings fresh;
    fresh.load("dryer_a1b2c3");
    check(fresh.workgroup[0] != '\0', "a workgroup is invented on first boot",
           fresh.workgroup);
    check(strlen(fresh.workgroup) <= 8, "and fits the tag field",
           fresh.workgroup);
    check(fresh.deviceName[0] != '\0', "as is a device name", fresh.deviceName);
}

// Runs a synthetic drying cycle through the log: the cell warms one degree per
// sample and the reading climbs by a known amount per degree, so the fit has a
// right answer to be judged against.
void feedDriftRun(DriftLog &log, float startC, float stepC, float gramsPerC,
                  int samples, bool haveTemperature = true) {
    ScaleManager::Status status;
    status.haveReading = true;
    status.temperatureValid = haveTemperature;

    uint32_t nowMs = 0;
    log.start(nowMs, 60);
    for (int i = 0; i < samples && log.running(); ++i) {
        const float celsius = startC + stepC * static_cast<float>(i);
        status.temperatureC = celsius;
        status.uncompensatedGrams = 1000.0f + gramsPerC * (celsius - startC);
        status.rawCounts = 400000 + i;
        log.loop(nowMs, status);
        nowMs += 60000;
    }
}

void testDriftLog() {
    gCurrentTest = "drift log";

    // Idle.
    {
        DriftLog log;
        check(!log.running(), "starts idle");
        checkEqualInt(static_cast<long long>(log.count()), 0,
                      "with nothing logged");
        check(std::string(log.summary().c_str()).find("no samples") !=
                  std::string::npos,
              "and says so rather than showing an empty fit", log.summary().c_str());
        check(!log.fit().valid, "no fit from no data");
    }

    // A clean run recovers the coefficient it was given.
    {
        DriftLog log;
        feedDriftRun(log, 25.0f, 1.0f, 0.2f, 31);
        checkEqualInt(static_cast<long long>(log.count()), 31,
                      "every interval produced a sample");

        const DriftLog::Fit fit = log.fit();
        check(fit.valid, "the run is fittable");
        checkNear(fit.gramsPerC, 0.2, 0.001, "the known drift is recovered");
        checkNear(fit.referenceC, 25.0, 0.01,
                  "referenced to the cold end of the run");
        checkNear(fit.rSquared, 1.0, 0.001, "a straight line fits perfectly");
        checkNear(fit.temperatureSpanC, 30.0, 0.01, "span reported");
        checkNear(fit.weightSpanGrams, 6.0, 0.01, "as is how far the weight went");
        check(!fit.spanTooNarrow, "30 C is a wide enough range to trust");

        const std::string text(log.summary().c_str());
        check(text.find("0.200") != std::string::npos,
              "the summary quotes the coefficient", text);
    }

    // The interval is honoured rather than sampling on every loop.
    {
        DriftLog log;
        ScaleManager::Status status;
        status.haveReading = true;
        status.temperatureValid = true;
        status.temperatureC = 30.0f;
        status.uncompensatedGrams = 1000.0f;

        log.start(0, 60);
        check(log.loop(0, status), "the first sample is taken immediately");
        check(!log.loop(1000, status), "and not again a second later");
        check(!log.loop(59000, status), "nor just short of the interval");
        check(log.loop(60000, status), "but yes on the interval");
        checkEqualInt(static_cast<long long>(log.count()), 2,
                      "two samples in total");

        // A stopped log ignores everything.
        log.stop();
        check(!log.loop(120000, status), "a stopped log records nothing");
        checkEqualInt(static_cast<long long>(log.count()), 2,
                      "and keeps what it had");
    }

    // Silly intervals are clamped rather than accepted.
    {
        DriftLog log;
        log.start(0, 1);
        check(log.intervalSeconds() >= 5, "an absurdly short interval is raised",
              std::to_string(log.intervalSeconds()));
        log.start(0, 100000);
        check(log.intervalSeconds() <= 600, "and a long one lowered",
              std::to_string(log.intervalSeconds()));
    }

    // A scale with no reading yet contributes nothing: logging zeroes would
    // put a fake point at the cold end, which is the end that matters.
    {
        DriftLog log;
        ScaleManager::Status status;
        status.haveReading = false;
        log.start(0, 60);
        check(!log.loop(0, status), "no reading, no sample");
        checkEqualInt(static_cast<long long>(log.count()), 0, "log still empty");
    }

    // Too narrow a temperature range is flagged: the slope over half a degree
    // of noise would be applied over forty.
    {
        DriftLog log;
        feedDriftRun(log, 25.0f, 0.05f, 0.2f, 20);
        const DriftLog::Fit fit = log.fit();
        check(fit.valid, "a narrow run still fits");
        check(fit.spanTooNarrow, "but is marked as extrapolation");
        check(std::string(log.summary().c_str()).find("too narrow") !=
                  std::string::npos,
              "and the summary says so", log.summary().c_str());
    }

    // Without a temperature sensor there is nothing to fit, and saying that is
    // more use than returning a slope of zero.
    {
        DriftLog log;
        feedDriftRun(log, 25.0f, 1.0f, 0.2f, 20, /*haveTemperature=*/false);
        checkEqualInt(static_cast<long long>(log.count()), 20,
                      "samples are still logged without a sensor");
        check(!log.fit().valid, "but no fit is claimed");
        checkEqualInt(static_cast<long long>(log.fit().samples), 0,
                      "no sample counted towards a fit");
    }

    // A run where the weight wanders for other reasons has a poor R^2, which
    // is the signal not to apply it.
    {
        DriftLog log;
        ScaleManager::Status status;
        status.haveReading = true;
        status.temperatureValid = true;

        uint32_t nowMs = 0;
        log.start(nowMs, 60);
        for (int i = 0; i < 40; ++i) {
            status.temperatureC = 25.0f + static_cast<float>(i) * 0.5f;
            // Sawtooth: nothing to do with temperature.
            status.uncompensatedGrams = 1000.0f + ((i % 4) * 10.0f);
            log.loop(nowMs, status);
            nowMs += 60000;
        }
        const DriftLog::Fit fit = log.fit();
        check(fit.valid, "it is still a fit");
        check(fit.rSquared < 0.5f, "with an R^2 that says not to trust it",
              std::to_string(fit.rSquared));
    }

    // CSV: one header plus one line per sample, and chunked without losing or
    // splitting a row.
    {
        DriftLog log;
        feedDriftRun(log, 25.0f, 0.1f, 0.2f, 200);

        std::string csv;
        int chunks = 0;
        log.writeCsv([&csv, &chunks](const String &chunk) {
            csv += chunk.c_str();
            ++chunks;
        });
        check(chunks > 1, "a long log is streamed in chunks",
              std::to_string(chunks));
        check(csv.find("seconds,temperature_c") == 0, "starts with a header",
              csv.substr(0, 40));

        size_t lines = 0;
        for (char c : csv) {
            if (c == '\n') {
                ++lines;
            }
        }
        checkEqualInt(static_cast<long long>(lines), 201,
                      "a line per sample plus the header");
        check(csv.back() == '\n', "and ends cleanly");
        check(csv.find("400000") != std::string::npos,
              "raw counts are included, so a fit can be redone off-device");
    }

    // Filling up stops the run rather than overwriting the cold start.
    {
        DriftLog log;
        feedDriftRun(log, 20.0f, 0.05f, 0.1f,
                     static_cast<int>(DRIFT_LOG_CAPACITY) + 50);
        check(log.full(), "the log filled");
        check(!log.running(), "and stopped itself");
        checkEqualInt(static_cast<long long>(log.count()), DRIFT_LOG_CAPACITY,
                      "at capacity");
        checkNear(log.at(0).temperatureC, 20.0, 0.01,
                  "the first sample is still the cold start");
        check(std::string(log.summary().c_str()).find("full") !=
                  std::string::npos,
              "and the summary explains why it stopped", log.summary().c_str());

        log.clear();
        checkEqualInt(static_cast<long long>(log.count()), 0, "clear empties it");
        check(!log.running(), "and leaves it stopped");
    }
}

void testWebPortal() {
    gCurrentTest = "web portal";

    App app;
    startApp(app, 1245.0f);

    bool restarted = false;
    bool wifiReconnected = false;
    bool mqttReconnected = false;

    WebPortal portal;
    WebPortal::Hooks hooks;
    hooks.networkSummary = []() { return String("192.168.1.50, -58 dBm"); };
    hooks.restart = [&restarted]() { restarted = true; };
    hooks.reconnectWifi = [&wifiReconnected]() { wifiReconnected = true; };
    hooks.reconnectMqtt = [&mqttReconnected]() { mqttReconnected = true; };
    hooks.stateJson = []() { return String("{\"probe\":1}"); };
    portal.begin(app, hooks, "dryer_test");

    // Every page renders, is complete HTML, and does not leak an unescaped
    // template.
    const char *pages[] = {"/", "/provision", "/calibrate", "/settings", "/tag",
                           "/selftest"};
    for (const char *path : pages) {
        const WebServer::Response response =
            portal.server().dispatch(path, HTTP_GET);
        check(response.handled, std::string("route exists: ") + path);
        checkEqualInt(response.code, 200, path);
        check(response.body.find("<!doctype html>") == 0,
               std::string("complete document: ") + path);
        check(response.body.find("</html>") != std::string::npos,
               std::string("document is closed: ") + path);
        check(response.body.find("192.168.1.50") != std::string::npos,
               std::string("footer shows the network: ") + path);
    }

    // An uncalibrated device says so on the status page rather than showing
    // confident nonsense.
    {
        App raw;
        hostio::reset();
        hostio::attachHx711(PIN_HX711_DOUT, PIN_HX711_SCK);
        hostio::setHx711Value(100000, true);
        raw.begin("dryer_raw");
        WebPortal rawPortal;
        rawPortal.begin(raw, hooks, "dryer_raw");
        const WebServer::Response response =
            rawPortal.server().dispatch("/", HTTP_GET);
        check(response.body.find("never been calibrated") != std::string::npos,
               "an uncalibrated scale is called out");
        startApp(app, 1245.0f);  // restore the shared fixture's clock and NVS
    }

    // The provisioning form offers every preset and every colour.
    {
        const WebServer::Response response =
            portal.server().dispatch("/provision", HTTP_GET);
        size_t materialCount = 0;
        const presets::MaterialPreset *materials = presets::all(materialCount);
        bool allPresent = true;
        for (size_t i = 0; i < materialCount; ++i) {
            if (response.body.find(std::string("value=\"") + materials[i].id +
                                   "\"") == std::string::npos) {
                allPresent = false;
            }
        }
        check(allPresent, "every material preset is offered");
        check(response.body.find("Galaxy Black") != std::string::npos,
               "colours are offered");
        check(response.body.find("80 \xC2\xB0" "C to dry") !=
                   std::string::npos,
               "materials the dryer cannot handle are flagged");
    }

    // Submitting the form with numbers that disagree with the scale must be
    // refused before any tag is touched, and the refusal must be visible.
    {
        const WebServer::Response post = portal.server().dispatch(
            "/provision", HTTP_POST,
            {{"material", "petg"},
             {"color", "Jet Black"},
             {"spool_id", "7ab2acb509"},
             {"filament", "1440"},
             {"spool", "201"},
             {"length", "343"}});
        checkEqualInt(post.code, 303, "form posts redirect");
        check(post.headers.count("Location") == 1 &&
                   post.headers.at("Location") == "/provision",
               "back to the form");

        const WebServer::Response page =
            portal.server().dispatch("/provision", HTTP_GET);
        check(page.body.find("refused") != std::string::npos,
               "the refusal is shown on the page");
        check(page.body.find("Check the spool ID") != std::string::npos,
               "with the reason");

        // The flash message is shown once, not on every later page load.
        const WebServer::Response again =
            portal.server().dispatch("/provision", HTTP_GET);
        check(again.body.find("refused") == std::string::npos,
               "and only once");
    }

    // "Custom" with no hex value would produce a tag named "PETG Custom".
    {
        portal.server().dispatch("/provision", HTTP_POST,
                                 {{"material", "petg"},
                                  {"color", "Custom"},
                                  {"filament", "1044"},
                                  {"spool", "201"}});
        const WebServer::Response page =
            portal.server().dispatch("/provision", HTTP_GET);
        check(page.body.find("pick a real colour") != std::string::npos,
               "the placeholder colour is rejected");
    }

    // Settings round-trip, including the rule that an empty password field
    // leaves the stored password alone.
    {
        snprintf(app.settings().wifiPassword, sizeof(app.settings().wifiPassword),
                 "originalsecret");
        portal.server().dispatch("/settings", HTTP_POST,
                                 {{"ssid", "workshop"},
                                  {"wifi_pass", ""},
                                  {"host", "192.168.1.10"},
                                  {"port", "1883"},
                                  {"user", "ha"},
                                  {"pass", ""},
                                  {"workgroup", "dryer1"},
                                  {"storage", "shelf-a"},
                                  {"delta", "8"},
                                  {"interval", "20"},
                                  {"name", "Filament dryer"}});
        check(strcmp(app.settings().wifiSsid, "workshop") == 0, "ssid saved");
        check(strcmp(app.settings().wifiPassword, "originalsecret") == 0,
               "an empty password field does not wipe the password");
        check(strcmp(app.settings().mqttHost, "192.168.1.10") == 0,
               "broker saved");
        checkEqualInt(app.settings().mqttPort, 1883, "port saved");
        check(strcmp(app.settings().storageLocation, "shelf-a") == 0,
               "storage location saved");
        checkNear(app.settings().writeMinDeltaGrams, 8.0, 0.01,
                  "threshold saved");
        checkEqualInt(app.settings().writeMinIntervalMinutes, 20,
                      "interval saved");
        check(wifiReconnected, "changing the network triggers a reconnect");
        check(mqttReconnected, "changing the broker triggers a reconnect");

        // A host typed without a scheme is the likeliest thing to be entered,
        // and a URL with no scheme would be refused by the transport later with
        // nothing to point at the cause.
        portal.server().dispatch("/settings", HTTP_POST,
                                 {{"ssid", "workshop"},
                                  {"spoolman", " 192.168.1.10:7912 "},
                                  {"spoolman_push", "1"}});
        check(String(app.settings().spoolmanUrl) == "http://192.168.1.10:7912",
               "a bare host becomes a URL", app.settings().spoolmanUrl);
        check(app.settings().spoolmanPush, "automatic pushes on");

        // An unchecked box is absent from the post, which is the only way a
        // checkbox can mean "off".
        portal.server().dispatch("/settings", HTTP_POST,
                                 {{"ssid", "workshop"},
                                  {"spoolman", "http://192.168.1.10:7912"}});
        check(!app.settings().spoolmanPush, "and off again");

        // Out-of-range values are ignored rather than stored.
        portal.server().dispatch("/settings", HTTP_POST,
                                 {{"ssid", "workshop"},
                                  {"port", "70000"},
                                  {"delta", "9000"},
                                  {"interval", "99999"}});
        checkEqualInt(app.settings().mqttPort, 1883, "impossible port ignored");
        checkNear(app.settings().writeMinDeltaGrams, 8.0, 0.01,
                  "impossible threshold ignored");
        checkEqualInt(app.settings().writeMinIntervalMinutes, 20,
                      "impossible interval ignored");
    }

    // Actions.
    {
        const WebServer::Response response = portal.server().dispatch(
            "/action", HTTP_POST, {{"do", "auto_off"}});
        checkEqualInt(response.code, 303, "action redirects");
        check(!app.policy().enabled(), "write-back switched off from the web");

        portal.server().dispatch("/action", HTTP_POST, {{"do", "auto_on"}});
        check(app.policy().enabled(), "and back on");

        portal.server().dispatch("/action", HTTP_POST, {{"do", "poll_off"}});
        check(!app.nfc().pollEnabled(), "polling switched off");
        portal.server().dispatch("/action", HTTP_POST, {{"do", "poll_on"}});
        check(app.nfc().pollEnabled(), "and back on");

        portal.server().dispatch("/action", HTTP_POST, {{"do", "restart"}});
        check(restarted, "restart reaches the sketch");

        // Adding an auxiliary region rewrites a whole tag, so with no reader
        // and no tag it has to refuse rather than proceed on assumptions.
        portal.server().dispatch("/action", HTTP_POST, {{"do", "add_aux"}});
        const WebServer::Response tagPage =
            portal.server().dispatch("/tag", HTTP_GET);
        check(tagPage.body.find("no readable OpenPrintTag") != std::string::npos,
               "adding an aux region refuses without a tag", tagPage.body);

        // Correcting the empty spool weight rewrites the main region, so it has
        // the same duty to refuse with nothing in the field.
        {
            portal.server().dispatch("/action", HTTP_POST,
                                     {{"do", "empty_weight"}, {"empty", "217"}});
            const WebServer::Response page =
                portal.server().dispatch("/tag", HTTP_GET);
            check(page.body.find("no readable OpenPrintTag") != std::string::npos,
                   "rewriting the empty weight refuses without a tag",
                   page.body);

            // An empty box is a different mistake from an implausible number,
            // and saying "0 g is not plausible" would describe the wrong one.
            portal.server().dispatch("/action", HTTP_POST,
                                     {{"do", "empty_weight"}, {"empty", ""}});
            const WebServer::Response blank =
                portal.server().dispatch("/tag", HTTP_GET);
            check(blank.body.find("enter the empty spool weight") !=
                       std::string::npos,
                   "and asks for a figure when the box is empty", blank.body);
        }

        // Spoolman actions with no tag in the field: each has to refuse with
        // something a person can act on, and the Tag page is where the answer
        // belongs since that is where the buttons are.
        {
            portal.server().dispatch("/action", HTTP_POST,
                                     {{"do", "spoolman_link"},
                                      {"spool", "42"}});
            const WebServer::Response page =
                portal.server().dispatch("/tag", HTTP_GET);
            check(page.body.find("no tag in the field") != std::string::npos,
                   "linking refuses without a tag", page.body);

            portal.server().dispatch("/action", HTTP_POST,
                                     {{"do", "spoolman_unlink"}});
            const WebServer::Response unlinked =
                portal.server().dispatch("/tag", HTTP_GET);
            check(unlinked.body.find("not linked") != std::string::npos,
                   "unlinking says there is nothing to unlink", unlinked.body);

            check(unlinked.body.find("192.168.1.10:7912") != std::string::npos,
                   "and the card shows which Spoolman is configured");
        }

        portal.server().dispatch("/action", HTTP_POST, {{"do", "nonsense"}});
        const WebServer::Response page = portal.server().dispatch("/", HTTP_GET);
        check(page.body.find("unknown action") != std::string::npos,
               "an unknown action is reported rather than ignored");
    }

    // The state endpoint and the unknown-route redirect.
    {
        const WebServer::Response json =
            portal.server().dispatch("/api/state", HTTP_GET);
        checkEqualInt(json.code, 200, "state endpoint responds");
        check(json.contentType == "application/json", "as JSON",
               json.contentType);
        check(json.body == "{\"probe\":1}", "with the sketch's document",
               json.body);

        const WebServer::Response missing =
            portal.server().dispatch("/nope", HTTP_GET);
        checkEqualInt(missing.code, 303, "unknown paths redirect home");
    }

    // Tag text is escaped: a material name with a script tag in it must not
    // come back as markup.
    {
        const WebServer::Response before =
            portal.server().dispatch("/tag", HTTP_GET);
        check(before.body.find("No tag in the field") != std::string::npos,
               "the tag page copes with no tag");
    }

    // The raw image download. With no tag read there is nothing to serve, and
    // serving a truncated or empty file would be worse than refusing: it would
    // land in tests/fixtures/ looking like evidence.
    {
        const WebServer::Response missing =
            portal.server().dispatch("/tag.bin", HTTP_GET);
        checkEqualInt(missing.code, 404, "no tag, no download");
        check(missing.body.find("no tag") != std::string::npos,
               "and it says why", missing.body);
    }

    // The read test runs from the page and reports the failure honestly when
    // there is no reader, which is the case somebody will hit first.
    {
        const WebServer::Response response = portal.server().dispatch(
            "/selftest", HTTP_POST, {{"attempts", "5"}});
        checkEqualInt(response.code, 200, "the read test answers in place");
        check(response.body.find("Read test") != std::string::npos,
               "on the self-test page");
        const bool explained =
            response.body.find("reader") != std::string::npos ||
            response.body.find("0 of 5") != std::string::npos;
        check(explained, "and says what happened");
    }

    // Drift log controls.
    {
        portal.server().dispatch("/action", HTTP_POST,
                                 {{"do", "drift_start"}, {"interval", "45"}});
        check(app.driftLog().running(), "the drift log starts from the web");
        checkEqualInt(static_cast<long long>(app.driftLog().intervalSeconds()),
                      45, "with the interval asked for");

        const WebServer::Response page =
            portal.server().dispatch("/selftest", HTTP_GET);
        check(page.body.find("running") != std::string::npos,
               "and the page shows it running");

        portal.server().dispatch("/action", HTTP_POST, {{"do", "drift_stop"}});
        check(!app.driftLog().running(), "and stops again");

        // Applying a fit that does not exist must be refused, not silently
        // installed as a zero coefficient.
        const float before = app.scale().status().driftPerC;
        portal.server().dispatch("/action", HTTP_POST, {{"do", "drift_apply"}});
        checkNear(app.scale().status().driftPerC, before, 0.0001,
                  "an unfittable log does not change the coefficient");
        const WebServer::Response after =
            portal.server().dispatch("/selftest", HTTP_GET);
        check(after.body.find("not enough") != std::string::npos ||
                  after.body.find("no samples") != std::string::npos,
               "and the refusal is explained");

        portal.server().dispatch("/action", HTTP_POST, {{"do", "drift_clear"}});
        checkEqualInt(static_cast<long long>(app.driftLog().count()), 0,
                      "and the log can be cleared");
    }

    // The CSV download, including the header a browser needs to save it.
    {
        const WebServer::Response csv =
            portal.server().dispatch("/drift.csv", HTTP_GET);
        checkEqualInt(csv.code, 200, "the CSV route responds");
        check(csv.contentType.find("text/csv") != std::string::npos,
               "as CSV", csv.contentType);
        check(csv.body.find("seconds,temperature_c") == 0,
               "with the header row", csv.body.substr(0, 40));
        const auto disposition = csv.headers.find("Content-Disposition");
        check(disposition != csv.headers.end() &&
                  disposition->second.find("drift.csv") != std::string::npos,
               "and a filename to save it under");
    }
}

void testReaderWithNothingConnected() {
    gCurrentTest = "reader with nothing connected";
    hostio::reset();

    // BUSY high forever is what an absent or unpowered PN5180 looks like. The
    // upstream driver spins on it; ours must give up and say so.
    hostio::setPin(PIN_PN5180_BUSY, HIGH);

    NfcManager nfc;
    const uint32_t startedAt = hostio::now();
    check(!nfc.begin(), "begin fails without a reader");
    check(!nfc.status().readerOnline, "reader marked offline");
    check(nfc.status().lastError[0] != '\0', "an error message was recorded",
          nfc.status().lastError);
    check(hostio::now() - startedAt < 60000,
          "begin gave up rather than hanging",
          std::to_string(hostio::now() - startedAt) + " ms");

    // Polling must stay cheap and bounded once the reader is known to be dead.
    for (int i = 0; i < 50; ++i) {
        nfc.loop(hostio::now());
        hostio::advance(1000);
    }
    check(!nfc.status().tagPresent, "no tag reported without a reader");

    // Writes are refused with a message rather than attempted.
    char message[128] = {0};
    opt::OptAuxUpdate update;
    update.setConsumedWeight = true;
    update.consumedWeight = 100.0f;
    check(!nfc.updateAux(update, message, sizeof(message)),
          "aux update refused without a tag");
    check(message[0] != '\0', "refusal explained", message);
}

// Spoolman. The transport is faked, so what is under test is the part that
// matters: which requests get made, what weight goes in them, and what a
// device does with an answer it did not expect.
void testSpoolmanClient() {
    gCurrentTest = "spoolman client";

    hostio::reset();
    hostio::resetHttp();

    SpoolmanClient client;
    client.begin("http://spoolman.local:7912/", true, "Dryer");
    check(client.configured(), "a URL configures the client");
    check(String(client.baseUrl()) == "http://spoolman.local:7912",
          "a trailing slash is dropped", client.baseUrl());

    // A tag Spoolman knows.
    {
        hostio::resetHttp();
        hostio::http().responses.push_back(
            {200,
             "{\"uid\":\"E00401509A2B3C4D\",\"reader_id\":\"dryer\","
             "\"format\":\"openprinttag\",\"matched_spool_id\":42,"
             "\"matched_filament_id\":null}"});

        String detail;
        check(client.scan("E0:04:01:50:9A:2B:3C:4D", detail), "a scan succeeds",
              detail.c_str());
        checkEqualInt(client.status().linkedSpoolId, 42, "the spool is found");

        const hostio::HttpRequest *request =
            hostio::http().find("POST", "/api/v1/tag/scan");
        check(request != nullptr, "the scan endpoint is the one called");
        if (request != nullptr) {
            // Spoolman would normalise it anyway, but sending the canonical
            // form means the UID this device links under is the same one a
            // Spoolman upgraded to the tag table later looks for.
            check(request->body.find("\"uid\":\"E00401509A2B3C4D\"") !=
                       std::string::npos,
                   "the UID is sent in Spoolman's canonical form",
                   request->body);
            check(request->body.find("\"format\":\"openprinttag\"") !=
                       std::string::npos,
                   "labelled with the tag format", request->body);
            check(request->contentType == "application/json", "as JSON",
                   request->contentType);
        }
    }

    // A measurement, and the weight in it.
    {
        hostio::resetHttp();
        hostio::http().responses.push_back(
            {200, "{\"id\":42,\"used_weight\":137.0,\"remaining_weight\":907}"});

        String detail;
        check(client.measure(1108.4f, detail), "a measurement is accepted",
              detail.c_str());
        const hostio::HttpRequest *request =
            hostio::http().find("PUT", "/api/v1/spool/42/measure");
        check(request != nullptr, "sent to the right spool");
        if (request != nullptr) {
            check(request->body.find("1108.4") != std::string::npos,
                   "with the measured gross weight", request->body);
        }
        check(detail.indexOf("137") >= 0,
               "and Spoolman's own conclusion is reported back",
               detail.c_str());
        checkEqualInt(client.status().measureCount, 1, "the push is counted");
    }

    // A tag Spoolman has never seen is not an error: it is the state every tag
    // starts in, and saying so is how the portal knows to offer the link form.
    {
        hostio::resetHttp();
        hostio::http().responses.push_back(
            {200, "{\"uid\":\"AABB\",\"matched_spool_id\":null,"
                  "\"matched_filament_id\":null}"});
        String detail;
        check(client.scan("AA:BB", detail), "an unknown tag is a clean answer",
              detail.c_str());
        checkEqualInt(client.status().linkedSpoolId, 0, "with no spool");
        check(detail.indexOf("does not know") >= 0, "and is described as such",
               detail.c_str());
        checkEqualInt(client.status().errorCount, 0, "and is not an error");
    }

    // Nothing can be measured until a spool is known, or the weight would go to
    // whichever spool happened to be linked last.
    {
        hostio::resetHttp();
        String detail;
        check(!client.measure(1108.0f, detail),
              "no measurement without a linked spool");
        checkEqualInt(static_cast<int>(hostio::http().requests.size()), 0,
                      "and no request is made at all");
    }

    // Linking, which is the one manual step.
    {
        hostio::resetHttp();
        hostio::http().responses.push_back({200, "{\"id\":7}"});
        String detail;
        check(client.link(7, "E0:04:01:50:9A:2B:3C:4D", detail),
              "linking succeeds", detail.c_str());
        checkEqualInt(client.status().linkedSpoolId, 7,
                      "and is remembered without a second lookup");
        const hostio::HttpRequest *request =
            hostio::http().find("PUT", "/api/v1/spool/7/tag");
        check(request != nullptr, "on the spool's tag endpoint");
    }

    // A UID another spool already holds: Spoolman refuses, and the message it
    // gives is the only thing that explains why.
    {
        hostio::resetHttp();
        hostio::http().responses.push_back(
            {409, "{\"message\":\"Tag E00401 is already linked to spool 3.\"}"});
        String detail;
        check(!client.link(9, "E0:04:01", detail), "a conflict is a failure");
        check(detail.indexOf("already linked") >= 0,
               "and Spoolman's explanation is passed through", detail.c_str());
        check(String(client.status().lastError).indexOf("409") >= 0,
               "the status carries the code", client.status().lastError);
    }

    // A server that is not there. This is the common case in a house where the
    // Spoolman box gets rebooted, and it must not look like a tag problem.
    {
        hostio::resetHttp();
        hostio::http().transportError = HTTPC_ERROR_CONNECTION_REFUSED;
        String detail;
        check(!client.scan("E0:04", detail), "an unreachable server fails");
        check(detail.indexOf("cannot reach") >= 0, "and says so",
               detail.c_str());
        check(!client.status().reachable, "the status records it");
        hostio::http().transportError = 0;
    }

    // An answer in a shape this firmware does not expect must not be read as a
    // spool number. A missing key is the realistic version of that: a Spoolman
    // too old to have the tag endpoints answers 404 with something else.
    {
        hostio::resetHttp();
        hostio::http().responses.push_back({200, "{\"detail\":\"Not Found\"}"});
        String detail;
        check(!client.scan("E0:04", detail),
              "an answer without matched_spool_id is refused");
        checkEqualInt(client.status().linkedSpoolId, 0, "and links nothing");
    }

    // A released Spoolman has no tag endpoints at all, so the client has to
    // notice and use the nfc_uid field instead. This is the path that will
    // actually run in a house running the stable Docker image.
    {
        hostio::resetHttp();
        SpoolmanClient old;
        old.begin("http://spoolman.local:7912", true, "Dryer");

        hostio::http().responses.push_back({404, "{\"detail\":\"Not Found\"}"});
        hostio::http().responses.push_back(
            {200,
             "[{\"id\":42,\"registered\":\"2026-09-01T10:00:00\","
             "\"filament\":{\"id\":3,\"name\":\"PLA\"},"
             "\"used_weight\":137.0,"
             "\"extra\":{\"nfc_uid\":\"\\\"E00401509A2B3C4D\\\"\"}}]"});

        String detail;
        check(old.scan("E0:04:01:50:9A:2B:3C:4D", detail),
              "the fallback finds the spool", detail.c_str());
        checkEqualInt(old.status().linkedSpoolId, 42,
                      "and it is the spool's own id, not the filament's");
        check(old.status().api == SpoolmanClient::Api::ExtraField,
              "the older API is remembered");

        const hostio::HttpRequest *query =
            hostio::http().find("GET", "extra.nfc_uid=E00401509A2B3C4D");
        check(query != nullptr,
               "the UID is normalised into the query, as Spoolman stores it");

        // And the next scan does not retry the endpoint that is not there.
        hostio::http().requests.clear();
        hostio::http().responses.clear();
        hostio::http().responses.push_back({200, "[]"});
        check(old.scan("E0:04:01:50:9A:2B:3C:4D", detail),
              "a second scan works", detail.c_str());
        checkEqualInt(
            static_cast<int>(hostio::http().count("POST", "/api/v1/tag/scan")),
            0, "without asking for the tag endpoint again");
        checkEqualInt(old.status().linkedSpoolId, 0,
                      "an empty list means no spool");
    }

    // The trap in the fallback: Spoolman ignores a filter on an extra field it
    // has never heard of and answers with the whole inventory. Taking the first
    // spool of that would put this dryer's weight on a stranger's spool.
    {
        hostio::resetHttp();
        SpoolmanClient old;
        old.begin("http://spoolman.local:7912", true, "Dryer");
        hostio::http().responses.push_back({404, "{}"});
        hostio::http().responses.push_back(
            {200,
             "[{\"id\":1,\"extra\":{}},{\"id\":2,\"extra\":{}},"
             "{\"id\":3,\"extra\":{}}]"});

        String detail;
        check(!old.scan("E0:04:01:50:9A:2B:3C:4D", detail),
              "a filter that was ignored is not treated as a match");
        checkEqualInt(old.status().linkedSpoolId, 0, "nothing is linked");
        check(detail.indexOf("nfc_uid") >= 0,
               "and the fix is named in the message", detail.c_str());
    }

    // The same UID on two spools is somebody's data-entry mistake, and picking
    // one would hide it.
    {
        hostio::resetHttp();
        SpoolmanClient old;
        old.begin("http://spoolman.local:7912", true, "Dryer");
        hostio::http().responses.push_back({404, "{}"});
        hostio::http().responses.push_back(
            {200,
             "[{\"id\":1,\"extra\":{\"nfc_uid\":\"\\\"AABB\\\"\"}},"
             "{\"id\":2,\"extra\":{\"nfc_uid\":\"\\\"AABB\\\"\"}}]"});

        String detail;
        check(!old.scan("AA:BB", detail), "a duplicated UID is refused");
        check(detail.indexOf("2 spools") >= 0, "and counted", detail.c_str());
    }

    // Linking against a released Spoolman: the field has to be created before
    // filtering on it does anything, and the value has to be JSON inside JSON.
    {
        hostio::resetHttp();
        SpoolmanClient old;
        old.begin("http://spoolman.local:7912", true, "Dryer");
        hostio::http().responses.push_back({404, "{}"});   // no tag endpoint
        hostio::http().responses.push_back({200, "{}"});    // field registered
        hostio::http().responses.push_back({200, "{\"id\":42}"});  // patched

        String detail;
        check(old.link(42, "E0:04:01:50:9A:2B:3C:4D", detail),
              "linking falls back too", detail.c_str());

        check(hostio::http().find("POST", "/api/v1/field/spool/nfc_uid") !=
                   nullptr,
               "the extra field is registered first");
        const hostio::HttpRequest *patch =
            hostio::http().find("PATCH", "/api/v1/spool/42");
        check(patch != nullptr, "and the spool is patched");
        if (patch != nullptr) {
            check(patch->body.find("\\\"E00401509A2B3C4D\\\"") !=
                       std::string::npos,
                   "with the UID as a JSON string inside the value",
                   patch->body);
        }
    }

    // https is honest about not being supported rather than failing obscurely
    // inside the transport.
    {
        hostio::resetHttp();
        SpoolmanClient secure;
        secure.begin("https://spoolman.example.com", true, "Dryer");
        String detail;
        check(!secure.scan("E0:04", detail), "https is refused");
        check(detail.indexOf("http://") >= 0, "with the reason", detail.c_str());
        checkEqualInt(static_cast<int>(hostio::http().requests.size()), 0,
                      "before anything is sent");
    }
}

// The JSON reader, on its own. It is a substitute for a parser, so the ways it
// could quietly be wrong are worth pinning down.
void testSpoolmanJson() {
    gCurrentTest = "spoolman json";

    long value = 0;
    bool wasNull = false;

    check(spoolman_json::number("{\"matched_spool_id\": 42}",
                                "matched_spool_id", value, wasNull),
          "a number is found");
    checkEqualInt(static_cast<int>(value), 42, "and read");
    check(!wasNull, "and is not null");

    check(spoolman_json::number("{\"matched_spool_id\":null}",
                                "matched_spool_id", value, wasNull),
          "an explicit null is found");
    check(wasNull, "and reported as null rather than as zero");

    value = 0;
    check(!spoolman_json::number("{\"other\":1}", "matched_spool_id", value,
                                 wasNull),
          "an absent key is absent");

    // The trap: a key that is a suffix or prefix of another. "id" must not be
    // satisfied by "matched_spool_id", or every scan would return a filament's
    // id as a spool's.
    check(spoolman_json::number("{\"matched_spool_id\":42,\"id\":7}", "id",
                                value, wasNull),
          "a short key is found");
    checkEqualInt(static_cast<int>(value), 7, "and is the right one");

    char text[32] = {0};
    check(spoolman_json::text("{\"uid\":\"E00401509A2B3C4D\"}", "uid", text,
                              sizeof(text)),
          "a string is found");
    check(String(text) == "E00401509A2B3C4D", "and read whole", text);

    // A value longer than the buffer is truncated rather than overrunning it.
    char small[8] = {0};
    check(spoolman_json::text("{\"uid\":\"E00401509A2B3C4D\"}", "uid", small,
                              sizeof(small)),
          "an over-long string still parses");
    checkEqualInt(static_cast<int>(strlen(small)), 7, "truncated to the buffer");

    check(spoolman_json::message("{\"message\":\"Spool not found.\"}") ==
              "Spool not found.",
          "an error message is extracted");
    check(spoolman_json::message("{\"id\":4}").length() == 0,
          "and nothing is invented when there is none");

    // Depth matters in a list response: a spool's `id` is the outer one, and the
    // filament nested inside it has an `id` too.
    const String list =
        "[{\"id\":42,\"filament\":{\"id\":3},\"used_weight\":7}]";
    check(spoolman_json::numberAtDepth(list, "id", 2, value, wasNull),
          "an id is found inside a list");
    checkEqualInt(static_cast<int>(value), 42, "and it is the spool's");
    check(spoolman_json::numberAtDepth(list, "id", 3, value, wasNull),
          "the nested one is reachable too");
    checkEqualInt(static_cast<int>(value), 3, "and is the filament's");

    // A key that only appears inside a string is not a key.
    check(!spoolman_json::number("{\"comment\":\"the \\\"id\\\": is 4\"}", "id",
                                 value, wasNull),
          "text that looks like a key is not read as one");

    checkEqualInt(spoolman_json::arrayLength("[]"), 0, "an empty list");
    checkEqualInt(spoolman_json::arrayLength("[{\"id\":1}]"), 1, "one spool");
    checkEqualInt(
        spoolman_json::arrayLength("[{\"id\":1,\"f\":{\"x\":1}},{\"id\":2}]"), 2,
        "two spools, not counting what is nested in them");

    char uid[40] = {0};
    spoolmanNormalizeUid("e0:04:01:50:9a:2b:3c:4d", uid, sizeof(uid));
    check(String(uid) == "E00401509A2B3C4D",
          "a UID is normalised the way Spoolman stores it", uid);
    spoolmanNormalizeUid("E0-04 01_50", uid, sizeof(uid));
    check(String(uid) == "E0040150", "whatever the separators are", uid);
}

// The guard that stands between a stale dryer tare and somebody's inventory.
void testSpoolmanMeasurementGuard() {
    gCurrentTest = "spoolman guard";

    const float tare = 1140.0f;
    const opt::OptTag tag = makeTag(1044.0f, 201.0f, 343000.0f, true, 0.0f);
    SpoolState spool;

    float grams = 0.0f;
    String reason;

    // The good case: a half-used spool in a tared dryer.
    spool.update(tag, true, makeScale(tare + 201.0f + 500.0f, tare));
    check(App::measurementUsable(makeScale(tare + 201.0f + 500.0f, tare),
                                 spool.state(), grams, reason),
          "a plausible weight is sent", reason.c_str());
    checkNear(grams, 701.0, 0.5,
              "and it is the spool's gross weight, not the dryer's");

    // Never calibrated: the grams are invented, so nothing may be sent.
    {
        ScaleManager::Status scale = makeScale(tare + 701.0f, tare);
        scale.calibrated = false;
        spool.update(tag, true, scale);
        check(!App::measurementUsable(scale, spool.state(), grams, reason),
              "an uncalibrated scale sends nothing");
        check(reason.indexOf("calibrated") >= 0, "and says why", reason.c_str());
    }

    // The dangerous one: the dryer was never tared, so its own 1140 g is still
    // in the reading. Spoolman would take that as a spool heavier than full and
    // redefine what full means for it.
    {
        ScaleManager::Status scale = makeScale(tare + 701.0f, 0.0f);
        spool.update(tag, true, scale);
        check(!App::measurementUsable(scale, spool.state(), grams, reason),
              "an untared dryer sends nothing");
        check(reason.indexOf("tared") >= 0, "and says why", reason.c_str());
    }

    // A tare that has drifted far enough to make the spool look overfull is
    // caught by the tag's own reference weights.
    {
        ScaleManager::Status scale = makeScale(tare + 1600.0f, tare);
        spool.update(tag, true, scale);
        check(!App::measurementUsable(scale, spool.state(), grams, reason),
              "a weight above a full spool sends nothing");
        check(reason.indexOf("more than a full spool") >= 0,
               "and explains the arithmetic", reason.c_str());
    }

    // Just over full by a gram or two is measurement error, not a drifted tare,
    // and refusing it would mean a nearly-full spool never reported at all.
    {
        ScaleManager::Status scale = makeScale(tare + 1245.0f + 8.0f, tare);
        spool.update(tag, true, scale);
        check(App::measurementUsable(scale, spool.state(), grams, reason),
              "a few grams over full is still sent", reason.c_str());
    }

    // Nothing on the platform: sending zero would tell Spoolman the spool is
    // empty, which is a real number with real consequences.
    {
        ScaleManager::Status scale = makeScale(tare, tare);
        spool.update(tag, true, scale);
        check(!App::measurementUsable(scale, spool.state(), grams, reason),
              "an empty platform sends nothing");
    }
}

}  // namespace

// The DS18B20's arithmetic, which is the half of that driver a host can check.
// The bus timing is not testable here and is not the part that goes wrong
// quietly: a mistimed bus fails to answer at all, whereas a CRC checked the
// wrong way accepts corrupted readings and feeds them to drift compensation.
void testDs18b20() {
    gCurrentTest = "ds18b20";

    // The datasheet's power-on scratchpad, with the CRC it publishes. This is
    // the one independent check available -- everything below is built with
    // crc8() once this has established that crc8() is right.
    const uint8_t defaults[9] = {0x50, 0x05, 0x4B, 0x46, 0x7F,
                                 0xFF, 0x0C, 0x10, 0x1C};
    checkEqualInt(ds18b20::crc8(defaults, 8), 0x1C,
                  "crc8 matches the datasheet's published scratchpad");

    // Builds a scratchpad around a raw reading and a resolution, CRC included.
    auto pad = [](int16_t raw, uint8_t config, uint8_t out[9]) {
        out[0] = static_cast<uint8_t>(raw & 0xFF);
        out[1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
        out[2] = 0x4B;
        out[3] = 0x46;
        out[4] = config;
        out[5] = 0xFF;
        out[6] = 0x0C;
        out[7] = 0x10;
        out[8] = ds18b20::crc8(out, 8);
    };
    constexpr uint8_t k12Bit = 0x7F;

    uint8_t scratchpad[9] = {0};
    float celsius = 0.0f;

    // The datasheet's temperature/data table, at both signs. Negatives cannot
    // happen on a load cell in a dryer, but sign extension is the kind of thing
    // that is free to get right and irritating to find later.
    pad(0x0191, k12Bit, scratchpad);
    check(ds18b20::celsius(scratchpad, celsius), "a normal reading decodes");
    checkNear(celsius, 25.0625, 0.0001, "and matches the datasheet");

    pad(static_cast<int16_t>(0xFE6F), k12Bit, scratchpad);
    check(ds18b20::celsius(scratchpad, celsius), "a negative reading decodes");
    checkNear(celsius, -25.0625, 0.0001, "sign extension is right");

    // Rejections. Each is a reading that looks like a number but is not one.
    pad(0x0191, k12Bit, scratchpad);
    scratchpad[8] ^= 0xFF;
    check(!ds18b20::celsius(scratchpad, celsius), "a bad CRC is refused");

    pad(0x0191, k12Bit, scratchpad);
    scratchpad[0] ^= 0x10;  // a flipped data bit, CRC left as it was
    check(!ds18b20::celsius(scratchpad, celsius),
          "and so is a reading corrupted under an intact CRC");

    for (uint8_t i = 0; i < 9; ++i) {
        scratchpad[i] = 0xFF;
    }
    check(!ds18b20::celsius(scratchpad, celsius),
          "an undriven bus is not a temperature");

    pad(0x0550, k12Bit, scratchpad);
    check(!ds18b20::celsius(scratchpad, celsius),
          "85 C exactly means it never converted");

    pad(0x7FF0, k12Bit, scratchpad);
    check(!ds18b20::celsius(scratchpad, celsius),
          "and a reading past the part's own range is refused");

    // A part someone reconfigured to 9 bits leaves the low three bits undefined
    // rather than zero, so they have to be masked off instead of read as 0.4 C.
    pad(0x0197, 0x1F, scratchpad);
    check(ds18b20::celsius(scratchpad, celsius), "a 9-bit reading decodes");
    checkNear(celsius, 25.0, 0.0001, "with the undefined bits discarded");

    pad(0x0197, k12Bit, scratchpad);
    check(ds18b20::celsius(scratchpad, celsius), "the same bits at 12-bit");
    checkNear(celsius, 25.4375, 0.0001, "keep their resolution");
}

int main() {
    printf("Firmware logic tests\n");

    testHx711Driver();
    testMedianFiltering();
    testCalibrationAndPersistence();
    testTemperatureCompensation();
    testDs18b20();
    testStability();
    testSpoolState();
    testWriteBackPolicy();
    testPresets();
    testMqttStateDocument();
    testMqttDiscoveryAndCommands();
    testProvisioningInput();
    testAppActions();
    testDriftLog();
    testWebPortal();
    testSpoolmanClient();
    testSpoolmanJson();
    testSpoolmanMeasurementGuard();
    testReaderWithNothingConnected();

    printf("\n%d checks, %d failure%s\n", gChecks, gFailures,
           gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 1;
}
