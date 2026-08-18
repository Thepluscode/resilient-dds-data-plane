#include "resilientdds/health_monitor.hpp"

namespace resilientdds {

namespace { constexpr std::int64_t kNsPerMs = 1'000'000; }

HealthMonitor::HealthMonitor(HealthPolicy policy) : policy_(policy) {}

HealthMonitor::HealthMonitor(std::int64_t degraded_after_ms, std::int64_t lost_after_ms)
    : policy_{degraded_after_ms, lost_after_ms, 2, 3} {}

void HealthMonitor::on_sample(const std::string& source_id, std::int64_t now_ns,
                              std::int64_t age_ms) {
    auto& state = states_[source_id];
    state.last_sample_ns = now_ns;
    state.consecutive_misses = 0;

    if (age_ms > policy_.degraded_after_ms) {
        // Arrived, but not usable. This is the post-partition retransmission
        // burst: it must not count toward recovery.
        state.consecutive_fresh = 0;
        state.seen = true;
        if (state.state == HealthState::healthy) state.state = HealthState::degraded;
        if (state.state != HealthState::lost) state.reason = HealthReason::stale_data;
        return;
    }

    if (state.consecutive_fresh < policy_.recover_after_samples) ++state.consecutive_fresh;

    if (!state.seen) {
        // First contact is healthy immediately; hysteresis governs re-entry to
        // healthy, not entry.
        state.seen = true;
        state.state = HealthState::healthy;
        state.reason = HealthReason::none;
        return;
    }
    if (state.state == HealthState::healthy) {
        state.reason = HealthReason::none;
        return;
    }
    if (state.consecutive_fresh >= policy_.recover_after_samples) {
        state.state = HealthState::healthy;
        state.reason = HealthReason::none;
    } else {
        state.reason = HealthReason::recovering;
    }
}

void HealthMonitor::on_deadline_missed(const std::string& source_id) {
    auto& state = states_[source_id];
    ++state.deadline_misses;
    ++state.consecutive_misses;
    state.consecutive_fresh = 0;
    if (state.consecutive_misses >= policy_.degrade_after_misses &&
        state.state == HealthState::healthy) {
        state.state = HealthState::degraded;
        state.reason = HealthReason::deadline_missed;
    }
}

void HealthMonitor::on_liveliness_lost(const std::string& source_id) {
    auto& state = states_[source_id];
    ++state.liveliness_losses;
    state.consecutive_fresh = 0;
    // No hysteresis downward: an expired lease is a fact, not a fluctuation.
    state.state = HealthState::lost;
    state.reason = HealthReason::liveliness_lost;
}

void HealthMonitor::on_incompatible_qos(const std::string& source_id) {
    auto& state = states_[source_id];
    ++state.incompatible_qos_events;
    state.state = HealthState::degraded;
    state.reason = HealthReason::qos_incompatible;
}

void HealthMonitor::on_writer_matched(const std::string& source_id, bool matched) {
    auto& state = states_[source_id];
    state.writer_matched = matched;
    if (!matched) {
        // The graceful counterpart of liveliness loss. A writer that exits
        // cleanly never expires its lease, so without this the orderly
        // shutdown is invisible to the health model.
        state.consecutive_fresh = 0;
        state.state = HealthState::lost;
        state.reason = HealthReason::publisher_unmatched;
    }
}

HealthSnapshot HealthMonitor::decide(const SourceState& state, std::int64_t now_ns) const {
    HealthSnapshot out;
    out.state = state.state;
    out.reason = state.reason;
    out.last_sample_ns = state.last_sample_ns;
    out.deadline_misses = state.deadline_misses;
    out.liveliness_losses = state.liveliness_losses;
    out.incompatible_qos_events = state.incompatible_qos_events;
    out.writer_matched = state.writer_matched;

    if (!state.seen || state.last_sample_ns == 0) {
        out.state = HealthState::lost;
        out.reason = HealthReason::no_data;
        return out;
    }

    const auto age_ns = now_ns - state.last_sample_ns;
    out.age_ms = age_ns / kNsPerMs;

    // Ageing only ever makes things worse, so it overrides upward-only state.
    if (age_ns >= policy_.lost_after_ms * kNsPerMs) {
        out.state = HealthState::lost;
        if (out.reason != HealthReason::liveliness_lost &&
            out.reason != HealthReason::publisher_unmatched) {
            out.reason = HealthReason::no_data;
        }
    } else if (age_ns >= policy_.degraded_after_ms * kNsPerMs &&
               out.state == HealthState::healthy) {
        out.state = HealthState::degraded;
        out.reason = HealthReason::stale_data;
    }
    return out;
}

HealthSnapshot HealthMonitor::tick(const std::string& source_id, std::int64_t now_ns) {
    auto& state = states_[source_id];
    const auto decided = decide(state, now_ns);
    if (decided.state != state.state) state.consecutive_fresh = 0;
    state.state = decided.state;
    state.reason = decided.reason;
    return decided;
}

HealthSnapshot HealthMonitor::snapshot(const std::string& source_id, std::int64_t now_ns) const {
    const auto it = states_.find(source_id);
    if (it == states_.end()) return {};
    return decide(it->second, now_ns);
}

} // namespace resilientdds
