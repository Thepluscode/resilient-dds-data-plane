#pragma once

#include <cstdint>
#include <string>

namespace resilientdds {

constexpr std::uint32_t kCurrentSchemaVersion = 1;

struct TelemetrySample {
    std::string source_id;
    std::uint64_t sequence{0};
    std::int64_t source_timestamp_ns{0};
    std::int64_t ingest_timestamp_ns{0};
    double temperature_c{0.0};
    double voltage_v{0.0};
    std::uint32_t health_flags{0};
    std::uint32_t schema_version{kCurrentSchemaVersion};
};

enum class AnomalyKind {
    duplicate,
    sequence_gap,
    out_of_order,
    stale,
    future_timestamp,
    schema_mismatch
};

inline const char* to_string(AnomalyKind kind) noexcept {
    switch (kind) {
        case AnomalyKind::duplicate: return "duplicate";
        case AnomalyKind::sequence_gap: return "sequence_gap";
        case AnomalyKind::out_of_order: return "out_of_order";
        case AnomalyKind::stale: return "stale";
        case AnomalyKind::future_timestamp: return "future_timestamp";
        case AnomalyKind::schema_mismatch: return "schema_mismatch";
    }
    return "unknown";
}

} // namespace resilientdds
