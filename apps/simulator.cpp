#include "resilientdds/anomaly_detector.hpp"
#include "resilientdds/audit_sink.hpp"
#include "resilientdds/health_monitor.hpp"
#include "resilientdds/metrics.hpp"

#include <cstdint>
#include <iostream>

using namespace resilientdds;

int main() {
    AnomalyDetector detector({250, 50, kCurrentSchemaVersion});
    HealthMonitor health(250, 1000);
    MetricsRegistry metrics;
    AuditSink audit("resilientdds-audit.jsonl");

    constexpr std::int64_t step_ns = 100'000'000; // 100 ms
    std::int64_t now_ns = 1'000'000'000;

    for (std::uint64_t seq = 1; seq <= 20; ++seq) {
        now_ns += step_ns;
        TelemetrySample sample{"radar-01", seq, now_ns - 10'000'000, now_ns, 42.0, 27.8, 0, 1};

        if (seq == 6) sample.sequence = 8;            // simulate lost samples
        if (seq == 9) sample.sequence = 8;            // duplicate/out-of-order condition
        if (seq == 12) sample.source_timestamp_ns = now_ns - 900'000'000; // stale
        if (seq == 15) sample.schema_version = 2;      // schema drift

        const auto events = detector.evaluate(sample, now_ns);
        health.on_sample(sample.source_id, now_ns);
        metrics.increment("rdtf_samples_received_total");

        for (const auto& event : events) {
            metrics.increment(std::string("rdtf_anomaly_") + to_string(event.kind) + "_total");
            audit.write(event, now_ns);
            std::cout << "ANOMALY source=" << event.source_id
                      << " seq=" << event.sequence
                      << " kind=" << to_string(event.kind)
                      << " detail=\"" << event.detail << "\"\n";
        }
    }

    health.on_deadline_missed("radar-01");
    metrics.increment("rdtf_deadline_missed_total");

    const auto snap = health.snapshot("radar-01", now_ns + 300'000'000);
    metrics.gauge("rdtf_source_health_state", snap.state == HealthState::healthy ? 2.0 : snap.state == HealthState::degraded ? 1.0 : 0.0);

    std::cout << "\nHEALTH source=radar-01 state=" << to_string(snap.state)
              << " reason=\"" << to_string(snap.reason) << "\"\n\n";
    std::cout << "# Prometheus-style metrics\n" << metrics.render_prometheus();
    return 0;
}
