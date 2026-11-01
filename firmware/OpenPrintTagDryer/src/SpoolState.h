// Merges what the tag says about a spool with what the scale sees, and works
// out the numbers Home Assistant and the write-back policy care about.
//
// The tag carries per-spool measured reference weights, so the only unknown in
// the chain is the dryer's own weight:
//
//   remaining = gross - dryerTare - emptyContainerWeight
//   consumed  = actualNettoFullWeight - remaining
//
// The tag's own consumed_weight is kept separately rather than trusted: a large
// disagreement with the scale usually means the dryer was moved or the cell has
// drifted, and that is worth surfacing instead of quietly overwriting.

#pragma once

#include <Arduino.h>

#include "ScaleManager.h"
#include "openprinttag/opt_tag.h"

class SpoolState {
public:
    struct State {
        bool haveTag = false;
        bool haveWeight = false;
        bool stableWeight = false;             // settled enough to write a tag
        bool referenceWeightsPresent = false;  // enough tag data to do the sums

        float grossGrams = 0.0f;          // everything on the platform
        float spoolGrossGrams = 0.0f;     // gross minus the dryer
        float referenceFullGrams = 0.0f;  // filament on a full spool
        float emptyContainerGrams = 0.0f;

        float remainingGrams = 0.0f;
        float consumedGrams = 0.0f;
        float remainingPercent = 0.0f;
        float remainingLengthMm = 0.0f;
        bool remainingLengthValid = false;

        bool clampedEmpty = false;   // scale says less than nothing is left
        bool clampedFull = false;    // scale says more than a full spool

        bool tagConsumedValid = false;
        float tagConsumedGrams = 0.0f;
        float disagreementGrams = 0.0f;
        bool disagrees = false;

        // Cross-check available while a spool is still sealed: with nothing
        // consumed, the dryer's weight is implied by the tag's own numbers.
        bool impliedTareValid = false;
        float impliedDryerTareGrams = 0.0f;
        float impliedTareErrorGrams = 0.0f;
    };

    void update(const opt::OptTag &tag, bool tagValid,
                const ScaleManager::Status &scale);

    const State &state() const { return state_; }

    // Grams the tag's auxiliary region should be told about, rounded to whole
    // grams because that is the resolution the scale can defend.
    float consumedForWriteBack() const;

    // What to show when a field is missing: the spec's fallbacks (nominal for
    // actual, 1.75 mm diameter) are applied here so callers do not each
    // reimplement them.
    static float referenceFullWeight(const opt::OptMainData &main);
    static float fullLength(const opt::OptMainData &main);
    static float filamentDiameter(const opt::OptMainData &main);

private:
    State state_;
};
