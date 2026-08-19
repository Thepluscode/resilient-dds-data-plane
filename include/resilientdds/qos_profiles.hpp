#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace resilientdds {

enum class Reliability { best_effort, reliable };
enum class Durability { volatile_data, transient_local };
enum class Liveliness { automatic, manual_by_topic };
// KEEP_LAST bounds the writer's history by overwriting the oldest sample, so
// write() always succeeds and loss is silent. KEEP_ALL retains every sample
// until it is acknowledged, so a reader that stops draining eventually makes
// write() block and then fail. The choice decides whether the PRODUCER learns
// about backpressure.
enum class History { keep_last, keep_all };

struct QosProfile {
    std::string name;
    Reliability reliability{Reliability::reliable};
    Durability durability{Durability::volatile_data};
    Liveliness liveliness{Liveliness::automatic};
    std::uint32_t deadline_ms{0};
    std::uint32_t liveliness_lease_ms{0};
    std::uint32_t history_depth{1};
    History history{History::keep_last};
    // Hard ceiling on samples the writer will retain. 0 means unlimited, which
    // with KEEP_ALL is how a slow reader turns into unbounded writer memory.
    std::uint32_t max_samples{0};
    // How long a RELIABLE write() may block once history is full before giving
    // up. 0 means fail immediately rather than wait.
    std::uint32_t max_blocking_ms{100};
    // Maximum distinct keyed instances. 0 leaves the Fast DDS default, which is
    // TEN -- publish an eleventh key and it is silently never delivered, with
    // no error and no anomaly. Any topic with more keys than that must set this.
    std::uint32_t max_instances{0};
};

QosProfile critical_control();
QosProfile periodic_telemetry();
QosProfile high_rate_sensor();
std::vector<std::string> validate(const QosProfile& profile);

} // namespace resilientdds
