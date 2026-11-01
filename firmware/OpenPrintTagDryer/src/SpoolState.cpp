#include "SpoolState.h"

#include <math.h>

float SpoolState::referenceFullWeight(const opt::OptMainData &main) {
    if (main.hasActualNettoFullWeight && main.actualNettoFullWeight > 0.0f) {
        return main.actualNettoFullWeight;
    }
    // The specification names nominal_netto_full_weight as the fallback.
    if (main.hasNominalNettoFullWeight && main.nominalNettoFullWeight > 0.0f) {
        return main.nominalNettoFullWeight;
    }
    return 0.0f;
}

float SpoolState::fullLength(const opt::OptMainData &main) {
    if (main.hasActualFullLength && main.actualFullLength > 0.0f) {
        return main.actualFullLength;
    }
    if (main.hasNominalFullLength && main.nominalFullLength > 0.0f) {
        return main.nominalFullLength;
    }
    return 0.0f;
}

float SpoolState::filamentDiameter(const opt::OptMainData &main) {
    if (main.hasFilamentDiameter && main.filamentDiameter > 0.0f) {
        return main.filamentDiameter;
    }
    return 1.75f;  // the specification's assumed value
}

void SpoolState::update(const opt::OptTag &tag, bool tagValid,
                        const ScaleManager::Status &scale) {
    state_ = State{};
    state_.haveTag = tagValid;
    state_.haveWeight = scale.haveReading && scale.online;
    state_.stableWeight = state_.haveWeight && scale.stable;
    state_.grossGrams = scale.grossGrams;
    state_.spoolGrossGrams = scale.grossGrams - scale.dryerTareGrams;

    if (!tagValid) {
        return;
    }

    state_.referenceFullGrams = referenceFullWeight(tag.main);
    state_.emptyContainerGrams =
        tag.main.hasEmptyContainerWeight ? tag.main.emptyContainerWeight : 0.0f;
    state_.referenceWeightsPresent =
        state_.referenceFullGrams > 0.0f && tag.main.hasEmptyContainerWeight;

    state_.tagConsumedValid = tag.aux.hasConsumedWeight;
    state_.tagConsumedGrams = tag.aux.consumedWeight;

    if (!state_.haveWeight || !state_.referenceWeightsPresent) {
        // Without a usable scale reading the tag's own consumed_weight is the
        // best estimate available, so report that instead of nothing.
        if (state_.tagConsumedValid && state_.referenceFullGrams > 0.0f) {
            state_.consumedGrams = state_.tagConsumedGrams;
            state_.remainingGrams =
                state_.referenceFullGrams - state_.tagConsumedGrams;
            if (state_.remainingGrams < 0.0f) {
                state_.remainingGrams = 0.0f;
            }
            state_.remainingPercent =
                100.0f * state_.remainingGrams / state_.referenceFullGrams;
        }
        return;
    }

    float remaining = state_.spoolGrossGrams - state_.emptyContainerGrams;
    if (remaining < 0.0f) {
        remaining = 0.0f;
        state_.clampedEmpty = true;
    }
    if (remaining > state_.referenceFullGrams) {
        remaining = state_.referenceFullGrams;
        state_.clampedFull = true;
    }

    state_.remainingGrams = remaining;
    state_.consumedGrams = state_.referenceFullGrams - remaining;
    state_.remainingPercent =
        100.0f * remaining / state_.referenceFullGrams;

    const float length = fullLength(tag.main);
    if (length > 0.0f) {
        state_.remainingLengthMm =
            length * remaining / state_.referenceFullGrams;
        state_.remainingLengthValid = true;
    } else if (tag.main.hasDensity && tag.main.density > 0.0f) {
        // Fall back to geometry: grams to cm3 to mm of filament.
        const float diameterCm = filamentDiameter(tag.main) / 10.0f;
        const float radiusCm = diameterCm / 2.0f;
        const float areaCm2 = static_cast<float>(M_PI) * radiusCm * radiusCm;
        if (areaCm2 > 0.0f) {
            const float volumeCm3 = remaining / tag.main.density;
            state_.remainingLengthMm = (volumeCm3 / areaCm2) * 10.0f;
            state_.remainingLengthValid = true;
        }
    }

    if (state_.tagConsumedValid) {
        state_.disagreementGrams =
            fabsf(state_.consumedGrams - state_.tagConsumedGrams);
        state_.disagrees =
            state_.disagreementGrams > WRITEBACK_DISAGREE_WARN_G;
    }

    // A spool nobody has printed from yet is a free calibration reference: the
    // dryer must weigh whatever is left over once the tag's own numbers are
    // subtracted from the gross weight.
    const bool sealed = !state_.tagConsumedValid ||
                        state_.tagConsumedGrams < 1.0f;
    if (sealed) {
        state_.impliedDryerTareGrams =
            state_.grossGrams -
            (state_.referenceFullGrams + state_.emptyContainerGrams);
        state_.impliedTareValid = true;
        state_.impliedTareErrorGrams =
            state_.impliedDryerTareGrams - scale.dryerTareGrams;
    }
}

float SpoolState::consumedForWriteBack() const {
    return roundf(state_.consumedGrams);
}
