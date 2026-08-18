#pragma once

#include "resilientdds/telemetry.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace resilientdds {

struct AnomalyEvent {
    AnomalyKind kind;
    std::string source_id;
    std::uint64_t sequence{0};
    std::uint64_t missing_samples{0};
    std::int64_t age_ms{0};
    std::string detail;
};

struct DetectorConfig {
    std::int64_t max_age_ms{250};
    std::int64_t max_future_skew_ms{50};
    std::uint32_t expected_schema_version{kCurrentSchemaVersion};
};

class AnomalyDetector {
public:
    explicit AnomalyDetector(DetectorConfig config = {});

    std::vector<AnomalyEvent> evaluate(const TelemetrySample& sample, std::int64_t now_ns);
    void reset_source(const std::string& source_id);

private:
    struct SourceState {
        bool initialized{false};
        std::uint64_t last_sequence{0};
    };

    DetectorConfig config_;
    std::unordered_map<std::string, SourceState> state_;
};

} // namespace resilientdds
