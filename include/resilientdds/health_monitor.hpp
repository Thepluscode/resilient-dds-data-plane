#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace resilientdds {

enum class HealthState { healthy, degraded, lost };

// State says how bad it is; reason says what to go and look at. "LOST" alone
// tells an operator nothing actionable.
enum class HealthReason {
    none,
    no_data,
    stale_data,
    deadline_missed,
    liveliness_lost,
    publisher_unmatched,
    qos_incompatible,
    recovering
};

inline const char* to_string(HealthState state) noexcept {
    switch (state) {
        case HealthState::healthy: return "healthy";
        case HealthState::degraded: return "degraded";
        case HealthState::lost: return "lost";
    }
    return "unknown";
}

inline const char* to_string(HealthReason reason) noexcept {
    switch (reason) {
        case HealthReason::none: return "none";
        case HealthReason::no_data: return "no_data";
        case HealthReason::stale_data: return "stale_data";
        case HealthReason::deadline_missed: return "deadline_missed";
        case HealthReason::liveliness_lost: return "liveliness_lost";
        case HealthReason::publisher_unmatched: return "publisher_unmatched";
        case HealthReason::qos_incompatible: return "qos_incompatible";
        case HealthReason::recovering: return "recovering";
    }
    return "unknown";
}

struct HealthPolicy {
    std::int64_t degraded_after_ms{250};
    std::int64_t lost_after_ms{1000};
    // Jitter makes a single late sample look like a failure. Degrading on one
    // deadline miss produces alert flapping, so require a run of them.
    std::uint32_t degrade_after_misses{2};
    // Recovery is deliberately slower than failure: dropping back to healthy on
    // the first good sample is the other half of the flap.
    std::uint32_t recover_after_samples{3};
};

struct HealthSnapshot {
    HealthState state{HealthState::lost};
    HealthReason reason{HealthReason::no_data};
    std::int64_t last_sample_ns{0};
    std::int64_t age_ms{0};
    std::uint64_t deadline_misses{0};
    std::uint64_t liveliness_losses{0};
    std::uint64_t incompatible_qos_events{0};
    bool writer_matched{false};
};

class HealthMonitor {
public:
    explicit HealthMonitor(HealthPolicy policy = {});
    HealthMonitor(std::int64_t degraded_after_ms, std::int64_t lost_after_ms);

    // age_ms is the sample's own age against its source timestamp. A burst of
    // retransmitted samples after a partition ARRIVES but is not fresh, and
    // counting it as recovery evidence declares health while the detector is
    // still flagging the same samples stale.
    void on_sample(const std::string& source_id, std::int64_t now_ns, std::int64_t age_ms = 0);
    void on_deadline_missed(const std::string& source_id);
    void on_liveliness_lost(const std::string& source_id);
    void on_incompatible_qos(const std::string& source_id);
    void on_writer_matched(const std::string& source_id, bool matched);

    // Applies age-based transitions and persists them. Call from a poller; a
    // health model that only moves when data arrives can never report silence.
    HealthSnapshot tick(const std::string& source_id, std::int64_t now_ns);
    // Same decision, without persisting.
    HealthSnapshot snapshot(const std::string& source_id, std::int64_t now_ns) const;

private:
    struct SourceState {
        HealthState state{HealthState::lost};
        HealthReason reason{HealthReason::no_data};
        std::int64_t last_sample_ns{0};
        std::uint64_t deadline_misses{0};
        std::uint64_t liveliness_losses{0};
        std::uint64_t incompatible_qos_events{0};
        std::uint32_t consecutive_misses{0};
        std::uint32_t consecutive_fresh{0};
        bool writer_matched{false};
        bool seen{false};
    };

    HealthSnapshot decide(const SourceState& state, std::int64_t now_ns) const;

    HealthPolicy policy_;
    std::unordered_map<std::string, SourceState> states_;
};

} // namespace resilientdds
