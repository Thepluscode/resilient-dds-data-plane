#include "resilientdds/qos_profiles.hpp"

namespace resilientdds {

QosProfile critical_control() {
    return {"critical_control", Reliability::reliable, Durability::transient_local,
            Liveliness::manual_by_topic, 100, 500, 8};
}

QosProfile periodic_telemetry() {
    return {"periodic_telemetry", Reliability::reliable, Durability::transient_local,
            Liveliness::automatic, 250, 1000, 32};
}

QosProfile high_rate_sensor() {
    return {"high_rate_sensor", Reliability::best_effort, Durability::volatile_data,
            Liveliness::automatic, 50, 250, 4};
}

std::vector<std::string> validate(const QosProfile& profile) {
    std::vector<std::string> errors;
    if (profile.history_depth == 0) errors.emplace_back("history_depth must be > 0");
    if (profile.liveliness == Liveliness::manual_by_topic && profile.liveliness_lease_ms == 0) {
        errors.emplace_back("manual liveliness requires a non-zero lease");
    }
    if (profile.durability == Durability::transient_local && profile.reliability != Reliability::reliable) {
        errors.emplace_back("transient-local replay should use reliable delivery for deterministic late-joiner recovery");
    }
    return errors;
}

} // namespace resilientdds
