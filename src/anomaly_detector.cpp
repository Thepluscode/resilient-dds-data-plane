#include "resilientdds/anomaly_detector.hpp"

#include <sstream>

namespace resilientdds {

namespace {
constexpr std::int64_t kNsPerMs = 1'000'000;
}

AnomalyDetector::AnomalyDetector(DetectorConfig config) : config_(config) {}

std::vector<AnomalyEvent> AnomalyDetector::evaluate(const TelemetrySample& sample, std::int64_t now_ns) {
    std::vector<AnomalyEvent> events;

    if (sample.schema_version != config_.expected_schema_version) {
        std::ostringstream detail;
        detail << "expected schema " << config_.expected_schema_version
               << " but received " << sample.schema_version;
        events.push_back({AnomalyKind::schema_mismatch, sample.source_id, sample.sequence, 0, 0, detail.str()});
    }

    const auto age_ns = now_ns - sample.source_timestamp_ns;
    const auto age_ms = age_ns / kNsPerMs;
    if (age_ms > config_.max_age_ms) {
        events.push_back({AnomalyKind::stale, sample.source_id, sample.sequence, 0, age_ms, "sample exceeded freshness budget"});
    } else if (age_ms < -config_.max_future_skew_ms) {
        events.push_back({AnomalyKind::future_timestamp, sample.source_id, sample.sequence, 0, age_ms,
                          "source clock is ahead of accepted skew"});
    }

    auto& state = state_[sample.source_id];
    if (!state.initialized) {
        state.initialized = true;
        state.last_sequence = sample.sequence;
        return events;
    }

    if (sample.sequence == state.last_sequence) {
        events.push_back({AnomalyKind::duplicate, sample.source_id, sample.sequence, 0, age_ms,
                          "same sequence observed twice"});
        return events;
    }

    if (sample.sequence < state.last_sequence) {
        events.push_back({AnomalyKind::out_of_order, sample.source_id, sample.sequence, 0, age_ms,
                          "sequence regressed"});
        return events;
    }

    if (sample.sequence > state.last_sequence + 1) {
        const auto missing = sample.sequence - state.last_sequence - 1;
        events.push_back({AnomalyKind::sequence_gap, sample.source_id, sample.sequence, missing, age_ms,
                          "one or more samples were not observed"});
    }

    state.last_sequence = sample.sequence;
    return events;
}

void AnomalyDetector::reset_source(const std::string& source_id) {
    state_.erase(source_id);
}

} // namespace resilientdds
