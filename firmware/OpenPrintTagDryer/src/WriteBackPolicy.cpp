#include "WriteBackPolicy.h"

#include <math.h>
#include <string.h>

const char *WriteBackPolicy::decisionText(Decision decision) {
    switch (decision) {
    case Decision::Write: return "ready to write";
    case Decision::Disabled: return "automatic write-back is off";
    case Decision::NoTag: return "no tag decoded";
    case Decision::TagWriteProtected: return "tag is write protected";
    case Decision::NoReferenceWeights:
        return "tag has no reference weights to measure against";
    case Decision::Unstable: return "waiting for the weight to settle";
    case Decision::BelowThreshold: return "change is below the threshold";
    case Decision::TooSoon: return "minimum interval has not elapsed";
    case Decision::WorkgroupMismatch: return "tag belongs to another workgroup";
    case Decision::AlreadyMatches: return "tag already holds this value";
    }
    return "unknown";
}

void WriteBackPolicy::setWorkgroup(const char *workgroup) {
    snprintf(workgroup_, sizeof(workgroup_), "%s",
             workgroup == nullptr ? "" : workgroup);
}

WriteBackPolicy::Decision WriteBackPolicy::evaluate(
    uint32_t nowMs, const opt::OptTag &tag, bool tagDecoded,
    bool tagWriteProtected, const SpoolState::State &spool,
    float consumedForWrite) const {
    if (!enabled_) {
        return Decision::Disabled;
    }
    if (!tagDecoded) {
        return Decision::NoTag;
    }
    if (tagWriteProtected) {
        return Decision::TagWriteProtected;
    }
    if (!spool.referenceWeightsPresent || !spool.haveWeight) {
        return Decision::NoReferenceWeights;
    }
    if (!spool.stableWeight) {
        return Decision::Unstable;
    }

    // A tag with somebody else's workgroup is left alone. The aux region is
    // shared space and overwriting it is how two devices fight over one spool.
    if (tag.aux.workgroup[0] != '\0' && workgroup_[0] != '\0' &&
        strcmp(tag.aux.workgroup, workgroup_) != 0) {
        return Decision::WorkgroupMismatch;
    }

    // What to compare against: the last value this device wrote, or failing
    // that whatever the tag itself says. Without either, any value is news.
    float reference = 0.0f;
    bool haveReference = false;
    if (haveWritten_) {
        reference = lastWrittenGrams_;
        haveReference = true;
    } else if (spool.tagConsumedValid) {
        reference = spool.tagConsumedGrams;
        haveReference = true;
    }

    if (haveReference) {
        const float delta = fabsf(consumedForWrite - reference);
        if (delta < 0.5f) {
            return Decision::AlreadyMatches;
        }
        if (delta < minDeltaGrams_) {
            return Decision::BelowThreshold;
        }
    }

    // The interval only applies to writes this device has actually made; a
    // freshly seen tag that disagrees is written at once.
    if (haveWritten_ && millisUntilAllowed(nowMs) > 0) {
        return Decision::TooSoon;
    }

    return Decision::Write;
}

void WriteBackPolicy::noteWrite(uint32_t nowMs, float consumedGrams) {
    haveWritten_ = true;
    lastWrittenGrams_ = consumedGrams;
    lastWriteMs_ = nowMs;
}

void WriteBackPolicy::reset() {
    haveWritten_ = false;
    lastWrittenGrams_ = 0.0f;
    lastWriteMs_ = 0;
}

uint32_t WriteBackPolicy::millisUntilAllowed(uint32_t nowMs) const {
    if (!haveWritten_) {
        return 0;
    }
    const uint32_t elapsed = nowMs - lastWriteMs_;
    if (elapsed >= minIntervalMs_) {
        return 0;
    }
    return minIntervalMs_ - elapsed;
}
