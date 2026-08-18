#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace resilientdds {

enum class Reliability { best_effort, reliable };
enum class Durability { volatile_data, transient_local };
enum class Liveliness { automatic, manual_by_topic };

struct QosProfile {
    std::string name;
    Reliability reliability{Reliability::reliable};
    Durability durability{Durability::volatile_data};
    Liveliness liveliness{Liveliness::automatic};
    std::uint32_t deadline_ms{0};
    std::uint32_t liveliness_lease_ms{0};
    std::uint32_t history_depth{1};
};

QosProfile critical_control();
QosProfile periodic_telemetry();
QosProfile high_rate_sensor();
std::vector<std::string> validate(const QosProfile& profile);

} // namespace resilientdds
