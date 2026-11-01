// Decides when the auxiliary region may be written.
//
// The tag is rated for 100k write cycles per block and lives inside a 70 C
// dryer, so "write whenever the number changes" is not an option: a spool that
// sits in the dryer for a month must not accumulate thousands of writes because
// the reading wobbles by a gram.
//
// Four conditions have to hold, and the reason a write was skipped is kept so
// the web portal and Home Assistant can show it:
//
//   1. a tag is present, decoded, and not write protected
//   2. the weight has been stable for a whole stability window
//   3. the value moved by at least WRITEBACK_MIN_DELTA_G since the last write
//   4. at least WRITEBACK_MIN_INTERVAL_MS has passed since the last write
//
// The workgroup is checked separately: a tag belonging to someone else's
// workgroup is never written to silently, because their aux data is theirs.

#pragma once

#include <Arduino.h>

#include "../config.h"
#include "SpoolState.h"
#include "openprinttag/opt_tag.h"

class WriteBackPolicy {
public:
    enum class Decision : uint8_t {
        Write,
        Disabled,          // automatic write-back switched off
        NoTag,
        TagWriteProtected,
        NoReferenceWeights, // nothing to compute consumption from
        Unstable,
        BelowThreshold,
        TooSoon,
        WorkgroupMismatch,
        AlreadyMatches,
    };

    static const char *decisionText(Decision decision);

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    void setWorkgroup(const char *workgroup);
    const char *workgroup() const { return workgroup_; }

    void setMinDelta(float grams) { minDeltaGrams_ = grams; }
    void setMinInterval(uint32_t milliseconds) {
        minIntervalMs_ = milliseconds;
    }

    // Evaluates the policy. `nowMs` is millis(); `tagWorkgroup` is what the tag
    // currently carries, empty when it has none.
    Decision evaluate(uint32_t nowMs, const opt::OptTag &tag, bool tagDecoded,
                      bool tagWriteProtected, const SpoolState::State &spool,
                      float consumedForWrite) const;

    // Records that a write succeeded, which starts both the interval and the
    // delta counting again.
    void noteWrite(uint32_t nowMs, float consumedGrams);

    // Forgets the last write, for when the tag was swapped: the new tag's own
    // stored value is what the next decision should compare against.
    void reset();

    bool haveWritten() const { return haveWritten_; }
    float lastWrittenGrams() const { return lastWrittenGrams_; }
    uint32_t lastWriteMs() const { return lastWriteMs_; }

    // Milliseconds until the interval allows another write, 0 when it already
    // does. Shown in the portal so a skipped write is explainable.
    uint32_t millisUntilAllowed(uint32_t nowMs) const;

private:
    bool enabled_ = true;
    char workgroup_[opt::kWorkgroupSize] = {0};
    float minDeltaGrams_ = WRITEBACK_MIN_DELTA_G;
    uint32_t minIntervalMs_ = WRITEBACK_MIN_INTERVAL_MS;

    bool haveWritten_ = false;
    float lastWrittenGrams_ = 0.0f;
    uint32_t lastWriteMs_ = 0;
};
