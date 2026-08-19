#include "resilientdds/anomaly_detector.hpp"
#include "resilientdds/health_monitor.hpp"
#include "resilientdds/metrics.hpp"
#include "resilientdds/qos_profiles.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace resilientdds;

namespace {
int failures = 0;

void check(bool condition, const std::string& name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    } else {
        std::cout << "PASS: " << name << '\n';
    }
}

bool has_kind(const std::vector<AnomalyEvent>& events, AnomalyKind kind) {
    for (const auto& event : events) if (event.kind == kind) return true;
    return false;
}
}

int main() {
    constexpr std::int64_t ms = 1'000'000;

    {
        AnomalyDetector detector({250, 50, 1});
        auto first = detector.evaluate({"a", 10, 1000 * ms, 0, 0, 0, 0, 1}, 1010 * ms);
        check(first.empty(), "first valid sample accepted");

        auto gap = detector.evaluate({"a", 13, 1020 * ms, 0, 0, 0, 0, 1}, 1030 * ms);
        check(has_kind(gap, AnomalyKind::sequence_gap), "sequence gap detected");
        check(gap.front().missing_samples == 2, "gap count is exact");

        auto dup = detector.evaluate({"a", 13, 1030 * ms, 0, 0, 0, 0, 1}, 1040 * ms);
        check(has_kind(dup, AnomalyKind::duplicate), "duplicate detected");

        auto old = detector.evaluate({"a", 14, 500 * ms, 0, 0, 0, 0, 1}, 1050 * ms);
        check(has_kind(old, AnomalyKind::stale), "stale sample detected");

        auto schema = detector.evaluate({"a", 15, 1060 * ms, 0, 0, 0, 0, 2}, 1065 * ms);
        check(has_kind(schema, AnomalyKind::schema_mismatch), "schema mismatch detected");
    }

    {
        HealthMonitor health(250, 1000);
        health.on_sample("node", 1000 * ms);
        check(health.snapshot("node", 1100 * ms).state == HealthState::healthy, "fresh source healthy");
        check(health.snapshot("node", 1300 * ms).state == HealthState::degraded, "aging source degraded");
        check(health.snapshot("node", 1300 * ms).reason == HealthReason::stale_data,
              "aging source reports stale_data");
        check(health.snapshot("node", 2100 * ms).state == HealthState::lost, "silent source lost");
        health.on_sample("node", 2200 * ms);
        health.on_liveliness_lost("node");
        check(health.snapshot("node", 2200 * ms).state == HealthState::lost, "liveliness loss is immediate");
        check(health.snapshot("node", 2200 * ms).reason == HealthReason::liveliness_lost,
              "liveliness loss reports its own reason");
    }

    {
        // Hysteresis: one deadline miss must not degrade, and one good sample
        // must not clear a degraded state. Both halves are what stops flapping.
        HealthMonitor health(HealthPolicy{250, 1000, 2, 3});
        health.on_sample("n", 1000 * ms);
        health.on_deadline_missed("n");
        check(health.snapshot("n", 1000 * ms).state == HealthState::healthy,
              "single deadline miss does not degrade");
        health.on_deadline_missed("n");
        check(health.snapshot("n", 1000 * ms).state == HealthState::degraded,
              "second consecutive deadline miss degrades");
        health.on_sample("n", 1010 * ms);
        check(health.snapshot("n", 1010 * ms).state == HealthState::degraded,
              "one fresh sample does not clear degraded");
        health.on_sample("n", 1020 * ms);
        health.on_sample("n", 1030 * ms);
        check(health.snapshot("n", 1030 * ms).state == HealthState::healthy,
              "three fresh samples clear degraded");
    }

    {
        // Regression: a partition's retransmission burst arrives all at once but
        // is already stale. Counting it as recovery evidence declared HEALTHY
        // 27 ms after restore while the detector was still flagging the very
        // same samples stale. Observed 2026-08-18 in scripts/run_netem.sh.
        HealthMonitor health(HealthPolicy{250, 1000, 2, 3});
        health.on_sample("n", 1000 * ms);
        health.on_liveliness_lost("n");
        check(health.snapshot("n", 1000 * ms).state == HealthState::lost, "partition marks lost");
        for (int i = 1; i <= 5; ++i) health.on_sample("n", (1000 + i) * ms, 900);
        check(health.snapshot("n", 1005 * ms).state != HealthState::healthy,
              "stale retransmission burst does not restore health");
        for (int i = 1; i <= 3; ++i) health.on_sample("n", (1010 + i) * ms, 5);
        check(health.snapshot("n", 1013 * ms).state == HealthState::healthy,
              "three genuinely fresh samples do restore health");
    }

    {
        check(validate(critical_control()).empty(), "critical control QoS valid");
        check(validate(periodic_telemetry()).empty(), "telemetry QoS valid");
        check(validate(high_rate_sensor()).empty(), "sensor QoS valid");
        auto broken = critical_control();
        broken.reliability = Reliability::best_effort;
        check(!validate(broken).empty(), "unsafe transient-local/best-effort combination rejected");

        // Milestone 4B. KEEP_ALL retains until acknowledged, so a reader that
        // stops draining grows writer memory with no ceiling. That failure mode
        // is an OOM kill, not a QoS event, so it must be refused up front.
        auto unbounded = periodic_telemetry();
        unbounded.history = History::keep_all;
        unbounded.max_samples = 0;
        check(!validate(unbounded).empty(), "keep-all reliable with unlimited samples rejected");

        auto bounded = unbounded;
        bounded.max_samples = 64;
        check(validate(bounded).empty(), "keep-all reliable with a sample ceiling accepted");

        // Best-effort never retains for acknowledgement, so the ceiling is not
        // required there; without this the rule could be over-broad and nobody
        // would notice.
        auto best_effort_keep_all = high_rate_sensor();
        best_effort_keep_all.history = History::keep_all;
        best_effort_keep_all.max_samples = 0;
        check(validate(best_effort_keep_all).empty(), "keep-all best-effort does not require a ceiling");
    }

    {
        MetricsRegistry metrics;
        metrics.increment("samples_total");
        metrics.increment("samples_total", 2);
        metrics.gauge("health", 1.0);
        const auto text = metrics.render_prometheus();
        check(text.find("samples_total 3") != std::string::npos, "counter aggregation works");
        check(text.find("health 1.000") != std::string::npos, "gauge rendering works");
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
