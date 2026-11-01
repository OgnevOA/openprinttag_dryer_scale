#include "DriftLog.h"

#include <math.h>

namespace {

constexpr uint32_t kMinIntervalSeconds = 5;
constexpr uint32_t kMaxIntervalSeconds = 600;

bool haveTemperature(const DriftLog::Sample &sample) {
    return !isnan(sample.temperatureC);
}

}  // namespace

void DriftLog::start(uint32_t nowMs, uint32_t intervalSeconds) {
    if (intervalSeconds < kMinIntervalSeconds) {
        intervalSeconds = kMinIntervalSeconds;
    } else if (intervalSeconds > kMaxIntervalSeconds) {
        intervalSeconds = kMaxIntervalSeconds;
    }
    intervalSeconds_ = intervalSeconds;
    count_ = 0;
    running_ = true;
    startedMs_ = nowMs;
    // Sample immediately, so the first row is the cold start rather than a
    // point half an interval into the warm-up.
    nextSampleMs_ = nowMs;
}

void DriftLog::stop() { running_ = false; }

void DriftLog::clear() {
    count_ = 0;
    running_ = false;
}

bool DriftLog::loop(uint32_t nowMs, const ScaleManager::Status &scale) {
    if (!running_ || !scale.haveReading) {
        return false;
    }
    if (static_cast<int32_t>(nowMs - nextSampleMs_) < 0) {
        return false;
    }

    Sample &sample = samples_[count_];
    sample.seconds = (nowMs - startedMs_) / 1000UL;
    sample.rawCounts = scale.rawCounts;
    sample.uncompensatedGrams = scale.uncompensatedGrams;
    sample.temperatureC = scale.temperatureValid ? scale.temperatureC : NAN;
    ++count_;

    nextSampleMs_ = nowMs + intervalSeconds_ * 1000UL;
    if (full()) {
        // Stopping beats wrapping: the interesting part of a drift run is the
        // cold start, and a ring that overwrites it would throw away the one
        // end of the line that is hardest to reproduce.
        running_ = false;
    }
    return true;
}

const DriftLog::Sample &DriftLog::at(size_t index) const {
    static const Sample empty;
    if (index >= count_) {
        return empty;
    }
    return samples_[index];
}

DriftLog::Fit DriftLog::fit() const {
    Fit result;

    double sumT = 0.0;
    double sumG = 0.0;
    double sumTT = 0.0;
    double sumTG = 0.0;
    float minT = 0.0f;
    float maxT = 0.0f;
    float minG = 0.0f;
    float maxG = 0.0f;
    size_t used = 0;

    for (size_t i = 0; i < count_; ++i) {
        const Sample &sample = samples_[i];
        if (!haveTemperature(sample)) {
            continue;
        }
        const double t = sample.temperatureC;
        const double g = sample.uncompensatedGrams;
        if (used == 0) {
            minT = maxT = sample.temperatureC;
            minG = maxG = sample.uncompensatedGrams;
        } else {
            minT = min(minT, sample.temperatureC);
            maxT = max(maxT, sample.temperatureC);
            minG = min(minG, sample.uncompensatedGrams);
            maxG = max(maxG, sample.uncompensatedGrams);
        }
        sumT += t;
        sumG += g;
        sumTT += t * t;
        sumTG += t * g;
        ++used;
    }

    result.samples = used;
    if (used < 8) {
        return result;
    }

    const double n = static_cast<double>(used);
    const double denominator = n * sumTT - sumT * sumT;
    if (fabs(denominator) < 1e-9) {
        // Every sample at the same temperature: nothing to fit.
        return result;
    }

    const double slope = (n * sumTG - sumT * sumG) / denominator;
    const double intercept = (sumG - slope * sumT) / n;

    // How much of the weight's variation the line accounts for. A low value
    // here means the reading moved for some reason other than temperature,
    // which is worth knowing before the number is applied.
    double residual = 0.0;
    double total = 0.0;
    const double meanG = sumG / n;
    for (size_t i = 0; i < count_; ++i) {
        const Sample &sample = samples_[i];
        if (!haveTemperature(sample)) {
            continue;
        }
        const double predicted = intercept + slope * sample.temperatureC;
        const double error = sample.uncompensatedGrams - predicted;
        residual += error * error;
        const double spread = sample.uncompensatedGrams - meanG;
        total += spread * spread;
    }

    result.valid = true;
    result.gramsPerC = static_cast<float>(slope);
    result.referenceC = minT;
    result.rSquared = total > 0.0
                          ? static_cast<float>(1.0 - residual / total)
                          : 1.0f;
    result.temperatureSpanC = maxT - minT;
    result.weightSpanGrams = maxG - minG;
    result.spanTooNarrow = result.temperatureSpanC < DRIFT_FIT_MIN_SPAN_C;
    return result;
}

void DriftLog::writeCsv(const std::function<void(const String &)> &emit) const {
    String chunk;
    chunk.reserve(1024);
    chunk += F("seconds,temperature_c,raw_counts,uncompensated_g\n");

    for (size_t i = 0; i < count_; ++i) {
        const Sample &sample = samples_[i];
        chunk += sample.seconds;
        chunk += ',';
        if (haveTemperature(sample)) {
            chunk += String(sample.temperatureC, 2);
        }
        chunk += ',';
        chunk += sample.rawCounts;
        chunk += ',';
        chunk += String(sample.uncompensatedGrams, 2);
        chunk += '\n';

        if (chunk.length() > 900) {
            emit(chunk);
            chunk = "";
        }
    }
    if (chunk.length() > 0) {
        emit(chunk);
    }
}

String DriftLog::summary() const {
    String out;
    if (count_ == 0) {
        return String(F("no samples logged"));
    }

    out += count_;
    out += F(" samples every ");
    out += intervalSeconds_;
    out += F(" s, ");
    out += samples_[count_ - 1].seconds / 60UL;
    out += F(" minutes");
    if (running_) {
        out += F(", running");
    } else if (full()) {
        out += F(", stopped because the log is full");
    } else {
        out += F(", stopped");
    }

    const Fit result = fit();
    if (!result.valid) {
        out += F(" -- not enough of a temperature range to fit yet");
        return out;
    }

    out += F(" -- drift ");
    out += String(result.gramsPerC, 3);
    out += F(" g/C over ");
    out += String(result.temperatureSpanC, 1);
    out += F(" C, R^2 ");
    out += String(result.rSquared, 3);
    if (result.spanTooNarrow) {
        out += F(" (too narrow a range to trust)");
    }
    return out;
}
