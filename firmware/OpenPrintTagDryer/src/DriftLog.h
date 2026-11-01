// Thermal drift characterisation: log weight against temperature for hours,
// then fit a line through it.
//
// The hot dryer stands on the load cell, and a load cell's zero moves with its
// own temperature. That is the largest error in this device by a wide margin,
// and the only way to correct it is to measure it: leave a known load on the
// platform, run a full drying cycle, and watch what the reading does as the cell
// warms from room temperature to wherever it settles.
//
// This class does that on the device, rather than requiring a laptop to sit
// there logging serial output overnight. It samples into a fixed ring, fits a
// least-squares line, and reports the slope in the same units the firmware's
// compensation uses -- so a run ends with a number that can be applied from the
// web portal in one click.
//
// Samples record the *uncompensated* weight on purpose. Fitting against the
// compensated figure would measure the residual of whatever coefficient is
// already set, which is a different question and a good way to talk yourself
// into a wrong answer.

#pragma once

#include <Arduino.h>

#include <functional>

#include "../config.h"
#include "ScaleManager.h"

class DriftLog {
public:
    struct Sample {
        uint32_t seconds = 0;         // since the run started
        long rawCounts = 0;
        float uncompensatedGrams = 0.0f;
        float temperatureC = 0.0f;    // NaN when no sensor answered
    };

    struct Fit {
        bool valid = false;
        float gramsPerC = 0.0f;
        float referenceC = 0.0f;      // the coldest end of the run
        float rSquared = 0.0f;
        float temperatureSpanC = 0.0f;
        float weightSpanGrams = 0.0f;
        size_t samples = 0;
        bool spanTooNarrow = false;   // fit is extrapolation, not measurement
    };

    // `intervalSeconds` is clamped to something that cannot fill the ring in
    // less than a few minutes or take a week to fill it.
    void start(uint32_t nowMs, uint32_t intervalSeconds);
    void stop();
    void clear();

    // Records a sample when one is due. Returns true when it stored one.
    bool loop(uint32_t nowMs, const ScaleManager::Status &scale);

    bool running() const { return running_; }
    bool full() const { return count_ >= DRIFT_LOG_CAPACITY; }
    size_t count() const { return count_; }
    size_t capacity() const { return DRIFT_LOG_CAPACITY; }
    uint32_t intervalSeconds() const { return intervalSeconds_; }
    const Sample &at(size_t index) const;

    Fit fit() const;

    // CSV, delivered in chunks so a six-hour log never has to exist as one
    // string in RAM at the same time as the socket buffer it is copied into.
    void writeCsv(const std::function<void(const String &)> &emit) const;

    // One line for the self-test page and the log.
    String summary() const;

private:
    Sample samples_[DRIFT_LOG_CAPACITY];
    size_t count_ = 0;
    bool running_ = false;
    uint32_t startedMs_ = 0;
    uint32_t nextSampleMs_ = 0;
    uint32_t intervalSeconds_ = DRIFT_LOG_DEFAULT_INTERVAL_S;
};
