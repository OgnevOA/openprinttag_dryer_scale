#include "App.h"

#include <math.h>

namespace {

// How far the entered spool weights may differ from what the scale sees before
// provisioning is refused. The load cell is good to a gram or two and the
// dryer tare to a few more, so 20 g comfortably clears real measurement error
// while still catching a transposed digit.
constexpr float kProvisionToleranceGrams = 20.0f;

uint32_t hashUid(const char *uid) {
    uint32_t hash = 2166136261u;  // FNV-1a, plenty for "did the tag change"
    for (const char *p = uid; *p != '\0'; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

void App::begin(const char *deviceId) {
    snprintf(deviceId_, sizeof(deviceId_), "%s", deviceId);
    settings_.load(deviceId_);

    policy_.setEnabled(settings_.autoWriteBack);
    policy_.setWorkgroup(settings_.workgroup);
    policy_.setMinDelta(settings_.writeMinDeltaGrams);
    policy_.setMinInterval(settings_.writeMinIntervalMinutes * 60UL * 1000UL);

    if (!scale_.begin()) {
        Serial.println(F("HX711 not responding at startup"));
    }
    if (!nfc_.begin()) {
        Serial.print(F("PN5180 not ready: "));
        Serial.println(nfc_.status().lastError);
    }
    spoolman_.begin(settings_.spoolmanUrl, settings_.spoolmanPush,
                    settings_.deviceName);
    recompute();
}

void App::loop(uint32_t nowMs) {
    // The reader's field driver pulls about 100 mA and sits next to a 20-bit
    // bridge amplifier, so weighing and polling take turns rather than
    // overlapping: samples taken during a poll are thrown away.
    const bool tagChanged = nfc_.loop(nowMs);
    if (nfc_.rfActive()) {
        scale_.discard(2);
    }

    const bool newMeasurement = scale_.loop(nowMs);

    if (tagChanged) {
        const uint32_t hash = hashUid(nfc_.status().uid);
        if (hash != lastTagUidHash_) {
            // A different spool: the previous write history says nothing about
            // this one, and neither does the Spoolman spool it resolved to.
            policy_.reset();
            lastTagUidHash_ = hash;
            spoolman_.forgetTag();
            spoolmanLookupPending_ = nfc_.status().tagPresent;
            spoolmanNextAttemptMs_ = nowMs;
            resetSpoolmanPushState();
        }
        stateChanged_ = true;
    }
    if (newMeasurement) {
        stateChanged_ = true;
    }

    if (tagChanged || newMeasurement) {
        recompute();
    }
    if (driftLog_.loop(nowMs, scale_.status())) {
        stateChanged_ = true;
    }
    serviceWriteBack(nowMs);
    serviceSpoolman(nowMs);
}

void App::recompute() {
    spool_.update(nfc_.tag(), nfc_.status().tagDecoded, scale_.status());
}

void App::setWriteStatus(const char *text) {
    if (strncmp(writeStatus_, text, sizeof(writeStatus_)) != 0) {
        snprintf(writeStatus_, sizeof(writeStatus_), "%s", text);
        stateChanged_ = true;
    }
}

void App::serviceWriteBack(uint32_t nowMs) {
    const float consumed = spool_.consumedForWriteBack();
    const WriteBackPolicy::Decision decision =
        policy_.evaluate(nowMs, nfc_.tag(), nfc_.status().tagDecoded,
                         nfc_.status().writeProtected, spool_.state(), consumed);
    if (decision != WriteBackPolicy::Decision::Write) {
        setWriteStatus(WriteBackPolicy::decisionText(decision));
        return;
    }

    opt::OptAuxUpdate update;
    update.setConsumedWeight = true;
    update.consumedWeight = consumed;
    if (settings_.workgroup[0] != '\0') {
        update.setWorkgroup = true;
        snprintf(update.workgroup, sizeof(update.workgroup), "%s",
                 settings_.workgroup);
    }
    if (settings_.storageLocation[0] != '\0') {
        update.setStorageLocation = true;
        snprintf(update.storageLocation, sizeof(update.storageLocation), "%s",
                 settings_.storageLocation);
    }

    char message[96] = {0};
    if (nfc_.updateAux(update, message, sizeof(message))) {
        policy_.noteWrite(nowMs, consumed);
        recompute();
        char status[96];
        snprintf(status, sizeof(status), "wrote %.0f g consumed: %s",
                 static_cast<double>(consumed), message);
        setWriteStatus(status);
    } else {
        setWriteStatus(message[0] != '\0' ? message : "write failed");
    }
    stateChanged_ = true;
}

void App::resetSpoolmanPushState() {
    spoolmanHavePushed_ = false;
    spoolmanRetryWanted_ = false;
    spoolmanAttempted_ = false;
    spoolmanLastPushGrams_ = 0.0f;
}

void App::setSpoolmanStatus(const char *text) {
    if (strncmp(spoolmanStatus_, text, sizeof(spoolmanStatus_)) != 0) {
        snprintf(spoolmanStatus_, sizeof(spoolmanStatus_), "%s", text);
        stateChanged_ = true;
    }
}

bool App::spoolmanWeightUsable(float &grossGrams, String &reason) const {
    return measurementUsable(scale_.status(), spool_.state(), grossGrams,
                             reason);
}

bool App::measurementUsable(const ScaleManager::Status &scale,
                            const SpoolState::State &state, float &grossGrams,
                            String &reason) {
    if (!scale.calibrated) {
        reason = F("the scale has never been calibrated");
        return false;
    }
    if (!(scale.dryerTareGrams > 0.0f)) {
        reason = F("the dryer has not been tared, so the reading still includes it");
        return false;
    }
    if (!state.haveWeight) {
        reason = F("no weight has been measured yet");
        return false;
    }

    grossGrams = state.spoolGrossGrams;
    if (!(grossGrams > 0.0f)) {
        reason = F("the platform reads empty");
        return false;
    }

    // The hazard worth guarding: Spoolman takes a measurement heavier than a
    // full spool as a correction to what full means, and rewrites the spool's
    // initial weight. A tag that knows its own reference weights can rule that
    // out before anything is sent, and one that does not gets no benefit of the
    // doubt either -- a reading that large is far more likely to be a dryer
    // whose tare has gone stale.
    if (state.referenceWeightsPresent) {
        const float full = state.referenceFullGrams + state.emptyContainerGrams;
        if (grossGrams > full + kProvisionToleranceGrams) {
            reason = String(F("the platform reads ")) + String(grossGrams, 0) +
                     F(" g, more than a full spool's ") + String(full, 0) +
                     F(" g: re-tare the dryer before sending this to Spoolman");
            return false;
        }
    }
    return true;
}

void App::serviceSpoolman(uint32_t nowMs) {
    if (!spoolman_.enabled()) {
        return;
    }
    const NfcManager::Status &nfc = nfc_.status();
    if (!nfc.tagPresent) {
        return;
    }

    // One lookup per tag, retried on the same schedule as a push so a Spoolman
    // that is briefly down does not cost a tag its link for the whole session.
    if (spoolmanLookupPending_ && static_cast<int32_t>(nowMs - spoolmanNextAttemptMs_) >= 0) {
        String detail;
        const bool ok = spoolman_.scan(nfc.uid, detail);
        spoolmanNextAttemptMs_ = nowMs + SPOOLMAN_MIN_INTERVAL_MS;
        if (ok) {
            spoolmanLookupPending_ = false;
        }
        setSpoolmanStatus(detail.c_str());
        stateChanged_ = true;
        return;  // the scan took its turn of the loop; weigh before pushing
    }

    if (spoolman_.status().linkedSpoolId <= 0) {
        return;
    }

    const SpoolState::State &state = spool_.state();
    if (!state.stableWeight) {
        return;
    }

    float grossGrams = 0.0f;
    String reason;
    if (!spoolmanWeightUsable(grossGrams, reason)) {
        setSpoolmanStatus(reason.c_str());
        return;
    }

    // Is there anything new to say? A first measurement always is, a retry is
    // because the last one did not arrive, and otherwise the weight has to have
    // actually moved.
    const bool worthSending =
        !spoolmanHavePushed_ || spoolmanRetryWanted_ ||
        fabsf(grossGrams - spoolmanLastPushGrams_) >= SPOOLMAN_MIN_DELTA_G;
    if (!worthSending) {
        return;
    }
    if (spoolmanAttempted_ &&
        static_cast<int32_t>(nowMs - spoolmanLastAttemptMs_) <
            static_cast<int32_t>(SPOOLMAN_MIN_INTERVAL_MS)) {
        return;
    }

    spoolmanAttempted_ = true;
    spoolmanLastAttemptMs_ = nowMs;

    String detail;
    if (spoolman_.measure(grossGrams, detail)) {
        spoolmanHavePushed_ = true;
        spoolmanRetryWanted_ = false;
        spoolmanLastPushGrams_ = grossGrams;
    } else {
        spoolmanRetryWanted_ = true;
    }
    setSpoolmanStatus(detail.c_str());
    stateChanged_ = true;
}

String App::linkSpoolman(int spoolId) {
    if (!spoolman_.configured()) {
        return String(F("no Spoolman URL is set on the Settings page"));
    }
    const NfcManager::Status &nfc = nfc_.status();
    if (!nfc.tagPresent) {
        return String(F("no tag in the field to link"));
    }

    String detail;
    const bool ok = spoolman_.link(spoolId, nfc.uid, detail);
    if (ok) {
        spoolmanLookupPending_ = false;
        resetSpoolmanPushState();
    }
    setSpoolmanStatus(detail.c_str());
    stateChanged_ = true;
    return detail;
}

String App::unlinkSpoolman() {
    const int spoolId = spoolman_.status().linkedSpoolId;
    if (spoolId <= 0) {
        return String(F("this tag is not linked to a Spoolman spool"));
    }
    String detail;
    spoolman_.unlink(spoolId, nfc_.status().uid, detail);
    spoolmanLookupPending_ = false;
    setSpoolmanStatus(detail.c_str());
    stateChanged_ = true;
    return detail;
}

String App::pushSpoolman() {
    if (!spoolman_.configured()) {
        return String(F("no Spoolman URL is set on the Settings page"));
    }
    // With automatic updates off no lookup has happened, so the button does it
    // rather than telling the user to turn a setting on to press a button.
    if (spoolman_.status().linkedSpoolId <= 0) {
        String lookup;
        if (!spoolman_.scan(nfc_.status().uid, lookup)) {
            setSpoolmanStatus(lookup.c_str());
            stateChanged_ = true;
            return lookup;
        }
        if (spoolman_.status().linkedSpoolId <= 0) {
            setSpoolmanStatus(lookup.c_str());
            stateChanged_ = true;
            return lookup;
        }
        spoolmanLookupPending_ = false;
    }

    float grossGrams = 0.0f;
    String reason;
    if (!spoolmanWeightUsable(grossGrams, reason)) {
        setSpoolmanStatus(reason.c_str());
        return reason;
    }

    // Deliberately not gated on stability or the interval: the button means
    // "now", and the weight checks above are the ones that protect Spoolman's
    // data. Whether the reading has settled is visible on the same page.
    String detail;
    const bool ok = spoolman_.measure(grossGrams, detail);
    spoolmanAttempted_ = true;
    spoolmanLastAttemptMs_ = millis();
    spoolmanHavePushed_ = spoolmanHavePushed_ || ok;
    spoolmanRetryWanted_ = !ok;
    if (ok) {
        spoolmanLastPushGrams_ = grossGrams;
    }
    setSpoolmanStatus(detail.c_str());
    stateChanged_ = true;
    return detail;
}

void App::applySpoolmanSettings() {
    spoolman_.configure(settings_.spoolmanUrl, settings_.spoolmanPush,
                        settings_.deviceName);
    spoolmanLookupPending_ = nfc_.status().tagPresent;
    spoolmanNextAttemptMs_ = millis();
    resetSpoolmanPushState();
    spoolmanStatus_[0] = '\0';
    stateChanged_ = true;
}

bool App::takeStateChanged() {
    const bool changed = stateChanged_;
    stateChanged_ = false;
    return changed;
}

String App::tareDryer() {
    char message[128] = {0};
    const bool ok = scale_.tareDryer(message, sizeof(message));
    recompute();
    stateChanged_ = true;
    return String(ok ? "" : "could not tare: ") + message;
}

String App::calibrate() {
    char message[128] = {0};
    const bool ok =
        scale_.calibrateSpan(settings_.calibrationMassGrams, message,
                             sizeof(message));
    recompute();
    stateChanged_ = true;
    return String(ok ? "" : "could not calibrate: ") + message;
}

String App::setCalibrationMass(float grams) {
    if (grams < 50.0f || grams > 5000.0f) {
        return String("calibration mass must be between 50 and 5000 g");
    }
    settings_.calibrationMassGrams = grams;
    settings_.save();
    stateChanged_ = true;
    return String("calibration mass set to ") + String(grams, 0) + " g";
}

String App::writeNow() {
    if (!nfc_.status().tagDecoded) {
        return String("no readable tag present");
    }
    if (!spool_.state().referenceWeightsPresent) {
        return String("tag has no reference weights, so there is nothing to "
                      "compute consumption from");
    }
    if (!scale_.status().haveReading) {
        return String("no weight reading yet");
    }

    const float consumed = spool_.consumedForWriteBack();
    opt::OptAuxUpdate update;
    update.setConsumedWeight = true;
    update.consumedWeight = consumed;
    if (settings_.workgroup[0] != '\0') {
        update.setWorkgroup = true;
        snprintf(update.workgroup, sizeof(update.workgroup), "%s",
                 settings_.workgroup);
    }
    if (settings_.storageLocation[0] != '\0') {
        update.setStorageLocation = true;
        snprintf(update.storageLocation, sizeof(update.storageLocation), "%s",
                 settings_.storageLocation);
    }

    char message[96] = {0};
    const bool ok = nfc_.updateAux(update, message, sizeof(message));
    if (ok) {
        policy_.noteWrite(millis(), consumed);
        recompute();
    }
    stateChanged_ = true;
    return String(consumed, 0) + " g consumed: " + message;
}

String App::wipeAux() {
    if (!nfc_.status().tagDecoded) {
        return String("no readable tag present");
    }

    // Everything the firmware does not recognise goes, which is the point: this
    // is for a tag carrying another workgroup's data that cannot be trusted.
    opt::OptAuxUpdate update;
    update.wipeUnknown = true;
    update.setConsumedWeight = true;
    update.consumedWeight = 0.0f;
    update.setWorkgroup = true;
    snprintf(update.workgroup, sizeof(update.workgroup), "%s",
             settings_.workgroup);
    update.setStorageLocation = true;
    update.storageLocation[0] = '\0';

    char message[96] = {0};
    const bool ok = nfc_.updateAux(update, message, sizeof(message));
    if (ok) {
        policy_.reset();
        recompute();
    }
    stateChanged_ = true;
    return String(ok ? "auxiliary region reset: " : "wipe failed: ") + message;
}

String App::refreshTag() {
    const bool ok = nfc_.refresh();
    recompute();
    stateChanged_ = true;
    if (ok) {
        return String("re-read ") + nfc_.status().uid;
    }
    return String("re-read failed: ") + nfc_.status().lastError;
}

String App::setAutoWriteBack(bool enabled) {
    settings_.autoWriteBack = enabled;
    settings_.save();
    policy_.setEnabled(enabled);
    stateChanged_ = true;
    return String(enabled ? "automatic write-back on"
                          : "automatic write-back off");
}

String App::setPollEnabled(bool enabled) {
    nfc_.setPollEnabled(enabled);
    stateChanged_ = true;
    return String(enabled ? "tag polling on" : "tag polling off");
}

String App::setStorageLocation(const String &location) {
    snprintf(settings_.storageLocation, sizeof(settings_.storageLocation), "%s",
             location.c_str());
    settings_.save();
    stateChanged_ = true;
    if (settings_.storageLocation[0] == '\0') {
        return String("storage location cleared");
    }
    return String("storage location set to ") + settings_.storageLocation;
}

String App::setDriftCoefficient(float gramsPerC) {
    if (gramsPerC < -20.0f || gramsPerC > 20.0f) {
        return String("drift coefficient outside a believable range");
    }
    scale_.setDriftPerC(gramsPerC);
    stateChanged_ = true;
    return String("drift coefficient set to ") + String(gramsPerC, 3) + " g/C";
}

String App::adoptImpliedTare() {
    const SpoolState::State &state = spool_.state();
    if (!state.impliedTareValid) {
        return String("needs a sealed spool: the tag must say nothing has been "
                      "consumed yet");
    }
    char message[128] = {0};
    const bool ok = scale_.setDryerTare(state.impliedDryerTareGrams, message,
                                        sizeof(message));
    recompute();
    stateChanged_ = true;
    return String(ok ? "" : "could not set the tare: ") + message;
}

App::WeightCheck App::checkEnteredWeights(float filamentGrams,
                                          float spoolGrams) const {
    WeightCheck check;
    check.expectedGrams = filamentGrams + spoolGrams;

    const ScaleManager::Status &status = scale_.status();
    if (!status.online || !status.haveReading || !status.calibrated) {
        snprintf(check.note, sizeof(check.note),
                 "scale not available, so the entered weights were not checked");
        return check;
    }
    if (status.dryerTareGrams <= 0.0f) {
        snprintf(check.note, sizeof(check.note),
                 "dryer has not been tared, so the entered weights were not "
                 "checked");
        return check;
    }
    if (!status.stable) {
        snprintf(check.note, sizeof(check.note),
                 "weight is still moving, so the entered weights were not "
                 "checked");
        return check;
    }

    check.performed = true;
    check.measuredGrams = status.grossGrams - status.dryerTareGrams;
    check.differenceGrams = check.measuredGrams - check.expectedGrams;
    check.agrees = fabsf(check.differenceGrams) <= kProvisionToleranceGrams;
    if (check.agrees) {
        snprintf(check.note, sizeof(check.note),
                 "scale agrees: %.0f g entered, %.0f g measured",
                 static_cast<double>(check.expectedGrams),
                 static_cast<double>(check.measuredGrams));
    } else {
        snprintf(check.note, sizeof(check.note),
                 "scale disagrees by %.0f g: %.0f g entered, %.0f g measured. "
                 "Check the spool ID and the numbers from its page.",
                 static_cast<double>(check.differenceGrams),
                 static_cast<double>(check.expectedGrams),
                 static_cast<double>(check.measuredGrams));
    }
    return check;
}

bool App::buildRequest(const ProvisionInput &input,
                       opt::OptBuildRequest &request, String &error) const {
    const presets::MaterialPreset *preset = presets::byId(input.materialId);
    if (preset == nullptr) {
        error = String("unknown material \"") + input.materialId + "\"";
        return false;
    }
    if (input.filamentGrams < 100.0f || input.filamentGrams > 10000.0f) {
        error = String("filament weight must be between 100 and 10000 g");
        return false;
    }
    if (input.spoolGrams < 20.0f || input.spoolGrams > 2000.0f) {
        error = String("spool weight must be between 20 and 2000 g");
        return false;
    }

    uint8_t rgba[4] = {0x80, 0x80, 0x80, 0xFF};
    if (input.colorHex[0] != '\0') {
        if (!presets::parseColor(input.colorHex, rgba)) {
            error = String("colour \"") + input.colorHex +
                    "\" is not a hex value like #24292a";
            return false;
        }
    } else if (input.colorName[0] != '\0') {
        const presets::ColorPreset *known = presets::colorByName(input.colorName);
        if (known != nullptr) {
            memcpy(rgba, known->rgba, sizeof(rgba));
        } else {
            // A colour typed in by hand carries no hex with it, and defaulting
            // to grey would write a confidently wrong colour rather than
            // refusing. Prusa publishes no hex for every colour it sells, so
            // sampling it off the spool is the only source for some.
            error = String("colour \"") + input.colorName +
                    "\" is not one of the presets, so it needs a hex value "
                    "like #24292a as well";
            return false;
        }
    }

    request = opt::OptBuildRequest{};
    presets::applyToMain(*preset, input.colorName, rgba, request.main);

    request.main.hasActualNettoFullWeight = true;
    request.main.actualNettoFullWeight = input.filamentGrams;
    request.main.hasEmptyContainerWeight = true;
    request.main.emptyContainerWeight = input.spoolGrams;
    if (input.lengthMetres > 0.0f) {
        request.main.hasActualFullLength = true;
        request.main.actualFullLength = input.lengthMetres * 1000.0f;
    }
    if (input.spoolId[0] != '\0') {
        snprintf(request.main.brandSpecificInstanceId,
                 sizeof(request.main.brandSpecificInstanceId), "%s",
                 input.spoolId);
    }

    snprintf(request.workgroup, sizeof(request.workgroup), "%s",
             settings_.workgroup);
    request.writeConsumedWeight = true;
    request.consumedWeight = 0.0f;
    request.minAuxSize = AUX_REGION_SIZE_BYTES;
    request.tagCapacity = opt::kSlix2Bytes;

    static char uri[96];
    if (input.includeUri && input.spoolId[0] != '\0') {
        snprintf(uri, sizeof(uri), "https://prusament.com/spool/?spoolId=%s",
                 input.spoolId);
        request.uri = uri;
    }
    return true;
}

String App::provision(const ProvisionInput &input) {
    opt::OptBuildRequest request;
    String error;
    if (!buildRequest(input, request, error)) {
        return error;
    }

    const WeightCheck check =
        checkEnteredWeights(input.filamentGrams, input.spoolGrams);
    if (check.performed && !check.agrees && !input.ignoreWeightCheck) {
        return String("refused: ") + check.note;
    }

    char message[128] = {0};
    const bool ok = nfc_.provision(request, message, sizeof(message));
    policy_.reset();
    recompute();
    stateChanged_ = true;

    String result = String(ok ? "" : "provisioning failed: ") + message;
    if (check.performed && !check.agrees) {
        result += String(" (weight check overridden: ") + check.note + ")";
    } else if (check.note[0] != '\0') {
        result += String(" (") + check.note + ")";
    }
    return result;
}

String App::addAuxRegion() {
    const NfcManager::Status &nfc = nfc_.status();
    if (!nfc.tagPresent || !nfc.tagDecoded) {
        return String(F("no readable OpenPrintTag in the field"));
    }
    const opt::OptTag &tag = nfc_.tag();
    if (tag.regions.hasAux) {
        return String(F("this tag already has an auxiliary region, so there is "
                        "nothing to do"));
    }

    // The region has to be copied out first: provisioning re-reads the tag into
    // the same buffer it would be read from, and the copy has to survive that.
    const uint8_t *span = nullptr;
    size_t spanLen = 0;
    if (!opt::optMainRegionSpan(nfc_.image(), nfc_.imageLength(), tag, span,
                                spanLen)) {
        return String(F("cannot locate the main region to copy"));
    }
    static uint8_t mainCopy[opt::kMaxTagImage];
    if (spanLen > sizeof(mainCopy)) {
        return String(F("main region is larger than this firmware can copy"));
    }
    memcpy(mainCopy, span, spanLen);

    opt::OptBuildRequest request;
    request.mainVerbatim = mainCopy;
    request.mainVerbatimLen = spanLen;
    request.minAuxSize = AUX_REGION_SIZE_BYTES;
    request.tagCapacity = opt::kSlix2Bytes;
    request.writeConsumedWeight = true;
    request.consumedWeight = 0.0f;
    snprintf(request.workgroup, sizeof(request.workgroup), "%s",
             settings_.workgroup);

    const uint8_t unrecognised = tag.main.unknownFieldCount;

    char message[128] = {0};
    const bool ok = nfc_.provision(request, message, sizeof(message));
    policy_.reset();
    recompute();
    stateChanged_ = true;

    if (!ok) {
        return String(F("could not add an auxiliary region: ")) + message;
    }

    String result = String(F("auxiliary region added, ")) + message;
    result += String(F(" (main region copied byte for byte, including "));
    result += String(unrecognised);
    result += F(" field(s) this firmware does not model)");
    return result;
}

String App::setEmptySpoolWeight(float grams) {
    const NfcManager::Status &nfc = nfc_.status();
    if (!nfc.tagPresent || !nfc.tagDecoded) {
        return String(F("no readable OpenPrintTag in the field"));
    }
    if (nfc.writeProtected) {
        return String(F("this tag is write protected"));
    }
    if (!emptySpoolWeightPlausible(grams)) {
        return String(F("an empty spool weight of ")) + String(grams, 0) +
               F(" g is not plausible; expected something between 20 and "
                 "1000 g");
    }

    const opt::OptTag &tag = nfc_.tag();

    // This rewrites the main region from the decoded fields rather than copying
    // it, because one of those fields is changing. Anything the decoder could
    // not represent, the encoder cannot put back, so a tag carrying such a field
    // has to be left alone: a silently shortened main region is worse than an
    // uncorrected tare.
    if (tag.main.unknownFieldCount > 0) {
        return String(F("this tag carries ")) + String(tag.main.unknownFieldCount) +
               F(" main field(s) this firmware does not model, and rewriting "
                 "the region would drop them");
    }
    if (tag.hasUriRecord && tag.uri[0] == '\0') {
        return String(F("this tag has a URI record this firmware cannot "
                        "reproduce, so rewriting it would remove the link"));
    }

    // The tare is everything left on the platform once the filament is gone, so
    // it cannot weigh more than what is on the platform now. This catches the
    // digit-slip cases -- 1860 for 186 -- while the scale is in a position to
    // know.
    const SpoolState::State &state = spool_.state();
    const ScaleManager::Status &scale = scale_.status();
    if (scale.calibrated && scale.stable && state.haveTag &&
        grams >= state.spoolGrossGrams) {
        return String(F("that is heavier than the ")) +
               String(state.spoolGrossGrams, 0) +
               F(" g of spool and filament on the platform");
    }

    opt::OptBuildRequest request;
    request.main = tag.main;
    request.main.hasEmptyContainerWeight = true;
    request.main.emptyContainerWeight = grams;
    request.minAuxSize = AUX_REGION_SIZE_BYTES;
    request.tagCapacity = opt::kSlix2Bytes;

    // Both of these have to outlive the call, and provisioning re-reads the tag
    // into the buffer they would otherwise point into.
    static char uriCopy[opt::kUriSize];
    if (tag.hasUriRecord) {
        snprintf(uriCopy, sizeof(uriCopy), "%s", tag.uri);
        request.uri = uriCopy;
    }

    // Carry the auxiliary region across. The consumed weight is deliberately the
    // tag's existing figure and not a corrected one: it was computed from the old
    // tare and is now 31-odd grams out, but recomputing it here would mean
    // trusting whatever the scale happens to read during a write. Resetting the
    // policy below lets the ordinary write-back put the right number on at the
    // next settled reading, which is the path that already knows how.
    request.writeConsumedWeight = tag.aux.hasConsumedWeight;
    request.consumedWeight = tag.aux.consumedWeight;
    snprintf(request.workgroup, sizeof(request.workgroup), "%s",
             tag.aux.workgroup[0] != '\0' ? tag.aux.workgroup
                                          : settings_.workgroup);
    snprintf(request.storageLocation, sizeof(request.storageLocation), "%s",
             tag.aux.storageLocation);

    const float was = tag.main.hasEmptyContainerWeight
                          ? tag.main.emptyContainerWeight
                          : 0.0f;

    char message[128] = {0};
    const bool ok = nfc_.provision(request, message, sizeof(message));
    policy_.reset();
    // Spoolman's own copy of this number is separate and still stale, so let the
    // next settled reading go out rather than waiting for the delta to build.
    resetSpoolmanPushState();
    recompute();
    stateChanged_ = true;

    if (!ok) {
        return String(F("could not rewrite the tag: ")) + message;
    }

    String result = String(F("empty spool weight is now "));
    result += String(grams, 0);
    result += F(" g, was ");
    result += String(was, 0);
    result += F(" g. ");
    result += message;
    result += F(". Set the same figure on the spool in Spoolman, and the "
               "consumed weight on the tag corrects itself at the next "
               "settled reading.");
    return result;
}

String App::selfTest() {
    String report;
    report.reserve(1024);
    report += F("OpenPrintTag dryer self-test, firmware ");
    report += FW_VERSION;
    report += F("\n\n");

    nfc_.selfTest(report);
    report += '\n';
    scale_.selfTest(report);
    report += '\n';

    report += F("Derived state\n");
    const SpoolState::State &state = spool_.state();
    if (!state.haveTag) {
        report += F("  no tag decoded\n");
    } else if (!state.referenceWeightsPresent) {
        report += F("  tag carries no reference weights\n");
    } else {
        report += F("  remaining: ");
        report += String(state.remainingGrams, 1);
        report += F(" g of ");
        report += String(state.referenceFullGrams, 1);
        report += F(" g\n");
        report += F("  consumed: ");
        report += String(state.consumedGrams, 1);
        report += F(" g (tag says ");
        report += state.tagConsumedValid ? String(state.tagConsumedGrams, 1)
                                         : String("nothing");
        report += F(")\n");
        if (state.impliedTareValid) {
            report += F("  sealed-spool cross-check: dryer implied at ");
            report += String(state.impliedDryerTareGrams, 1);
            report += F(" g, stored tare is off by ");
            report += String(state.impliedTareErrorGrams, 1);
            report += F(" g\n");
        }
        if (state.disagrees) {
            report += F("  WARNING: scale and tag disagree by ");
            report += String(state.disagreementGrams, 1);
            report += F(" g\n");
        }
    }

    report += F("  write-back: ");
    report += policy_.enabled() ? F("on, ") : F("off, ");
    report += writeStatus_;
    report += '\n';
    report += F("  workgroup: ");
    report += settings_.workgroup;
    report += '\n';

    report += F("\nDrift log\n  ");
    report += driftLog_.summary();
    report += '\n';
    return report;
}

String App::readTest(uint16_t attempts) {
    String report;
    report.reserve(512);
    // Polling in the middle of the test would insert its own inventories and
    // muddle both the count and the timing.
    const bool wasPolling = nfc_.pollEnabled();
    nfc_.setPollEnabled(false);
    nfc_.readTest(attempts, report);
    nfc_.setPollEnabled(wasPolling);
    scale_.discard(4);
    stateChanged_ = true;
    return report;
}

String App::startDriftLog(uint32_t intervalSeconds) {
    if (!scale_.status().calibrated) {
        return String(F("calibrate the span first: a drift run in raw counts "
                        "cannot be turned into grams per degree afterwards"));
    }
    driftLog_.start(millis(), intervalSeconds);
    stateChanged_ = true;

    String result = String(F("drift log started, sampling every ")) +
                    driftLog_.intervalSeconds() + F(" s for up to ") +
                    (driftLog_.capacity() * driftLog_.intervalSeconds() / 3600) +
                    F(" hours");
    if (!scale_.status().temperatureValid) {
        result += F(" -- but no temperature sensor is answering, so the log "
                    "cannot be fitted");
    }
    return result;
}

String App::stopDriftLog() {
    driftLog_.stop();
    stateChanged_ = true;
    return String(F("drift log stopped: ")) + driftLog_.summary();
}

String App::clearDriftLog() {
    driftLog_.clear();
    stateChanged_ = true;
    return String(F("drift log cleared"));
}

String App::applyDriftFit() {
    const DriftLog::Fit fit = driftLog_.fit();
    if (!fit.valid) {
        return String(F("nothing to apply: the log needs at least 8 samples "
                        "with a temperature reading"));
    }
    if (fit.spanTooNarrow) {
        return String(F("refusing to apply a fit over only ")) +
               String(fit.temperatureSpanC, 1) +
               F(" C: run a full drying cycle so the cell actually warms up");
    }

    // A line through noise fits anything. Below this the reading moved for some
    // reason other than temperature, and applying the slope would bake that
    // reason into every future weight.
    if (fit.rSquared < 0.5f) {
        return String(F("refusing to apply a fit with R^2 ")) +
               String(fit.rSquared, 2) +
               F(": the weight moved for some reason other than temperature, "
                 "so check that nothing touched the platform during the run");
    }

    scale_.setDriftPerC(fit.gramsPerC);
    scale_.setReferenceTemperature(fit.referenceC);
    recompute();
    stateChanged_ = true;
    return String(F("drift coefficient set to ")) + String(fit.gramsPerC, 3) +
           F(" g/C, referenced to ") + String(fit.referenceC, 1) + F(" C");
}
