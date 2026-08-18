// Telemetry subscriber. DDS delivers bytes; this process decides whether the
// resulting state is safe to use, and writes the evidence either way.
#include "resilientdds/dds_transport.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <atomic>
#include <thread>

using namespace resilientdds;

namespace {
QosProfile pick(const std::string& name) {
    if (name == "critical_control") return critical_control();
    if (name == "high_rate_sensor") return high_rate_sensor();
    return periodic_telemetry();
}
} // namespace

int main(int argc, char** argv) {
    int domain = 0;
    std::string topic = "SystemTelemetry";
    std::string profile_name = "periodic_telemetry";
    std::string source_id = "radar-01";
    std::string audit_path = "resilientdds-audit.jsonl";
    std::string metrics_path;
    int duration_s = 10;
    int start_delay_ms = 0;
    int poll_ms = 20;
    int process_delay_us = 0;
    bool allow_shm = false;
    std::int64_t max_age_ms = 250;

    for (int i = 1; i < argc - 1; ++i) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--domain") domain = std::atoi(v);
        else if (k == "--topic") topic = v;
        else if (k == "--profile") profile_name = v;
        else if (k == "--source-id") source_id = v;
        else if (k == "--audit-out") audit_path = v;
        else if (k == "--metrics-out") metrics_path = v;
        else if (k == "--duration-s") duration_s = std::atoi(v);
        else if (k == "--start-delay-ms") start_delay_ms = std::atoi(v);
        else if (k == "--max-age-ms") max_age_ms = std::atoll(v);
        else if (k == "--poll-ms") poll_ms = std::atoi(v);
        else if (k == "--process-delay-us") process_delay_us = std::atoi(v);
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--allow-shm") allow_shm = true;
    }

    // A late joiner is the whole point of transient-local durability; make it
    // reproducible instead of a race.
    if (start_delay_ms > 0) {
        std::cout << "LATE_JOIN delay_ms=" << start_delay_ms << "\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(start_delay_ms));
    }

    AnomalyDetector detector({max_age_ms, 50, kCurrentSchemaVersion});
    HealthMonitor health(HealthPolicy{250, 1000, 2, 3});
    MetricsRegistry metrics;
    AuditSink audit(audit_path);
    TelemetrySubscriber subscriber(detector, health, metrics, audit.good() ? &audit : nullptr);

    const QosProfile profile = pick(profile_name);
    subscriber.set_source(source_id);
    subscriber.set_processing_delay_us(process_delay_us);
    if (!subscriber.start(domain, topic, profile,
                          allow_shm ? TransportMode::defaults : TransportMode::udp_only)) {
        std::cerr << "subscriber failed to start\n";
        return 1;
    }
    std::cout << "SUBSCRIBER up domain=" << domain << " topic=" << topic
              << " profile=" << profile.name << "\n" << std::flush;

    // Health must move when data STOPS, not only when it arrives. A monitor
    // driven purely by callbacks cannot report silence, so poll it and emit a
    // timeline the harness can turn into recovery timings.
    // The harness marks faults on its own wall clock, but HEALTH_AT is relative
    // to this point -- which is AFTER process start and DDS init. Publish the
    // anchor so the two frames can be reconciled exactly instead of assumed
    // equal, which silently understated recovery time by the init duration.
    const auto t_start = now_ns();
    std::cout << "EPOCH_MS=" << t_start / 1'000'000 << "\n" << std::flush;
    std::atomic<bool> stop{false};
    std::thread poller([&] {
        HealthState last = HealthState::lost;
        HealthReason last_reason = HealthReason::no_data;
        bool first = true;
        while (!stop.load()) {
            const auto snap = health.tick(source_id, now_ns());
            if (first || snap.state != last || snap.reason != last_reason) {
                std::cout << "HEALTH_AT t_ms=" << (now_ns() - t_start) / 1'000'000
                          << " state=" << to_string(snap.state)
                          << " reason=" << to_string(snap.reason)
                          << " age_ms=" << snap.age_ms
                          << " matched=" << (snap.writer_matched ? 1 : 0) << "\n"
                          << std::flush;
                last = snap.state;
                last_reason = snap.reason;
                first = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(duration_s));
    stop.store(true);
    poller.join();

    const auto snap = health.snapshot(source_id, now_ns());
    metrics.gauge("rdtf_source_health_state",
                  snap.state == HealthState::healthy   ? 2.0
                  : snap.state == HealthState::degraded ? 1.0
                                                        : 0.0);
    std::cout << "HEALTH source=" << source_id << " state=" << to_string(snap.state)
              << " reason=\"" << to_string(snap.reason) << "\"\n";

    const auto lat = subscriber.latency();
    if (lat.count > 0) {
        metrics.gauge("rdtf_e2e_latency_min_us", static_cast<double>(lat.min_us));
        metrics.gauge("rdtf_e2e_latency_mean_us", lat.mean_us);
        metrics.gauge("rdtf_e2e_latency_p50_us", static_cast<double>(lat.p50_us));
        metrics.gauge("rdtf_e2e_latency_p95_us", static_cast<double>(lat.p95_us));
        metrics.gauge("rdtf_e2e_latency_p99_us", static_cast<double>(lat.p99_us));
        metrics.gauge("rdtf_e2e_latency_p999_us", static_cast<double>(lat.p999_us));
        metrics.gauge("rdtf_e2e_latency_stddev_us", lat.stddev_us);
        metrics.gauge("rdtf_e2e_latency_max_us", static_cast<double>(lat.max_us));
        std::cout << "LATENCY n=" << lat.count << " min_us=" << lat.min_us
                  << " p50_us=" << lat.p50_us << " p95_us=" << lat.p95_us
                  << " p99_us=" << lat.p99_us << " p999_us=" << lat.p999_us
                  << " max_us=" << lat.max_us << " mean_us=" << lat.mean_us
                  << " stddev_us=" << lat.stddev_us << "\n";
    }

    const std::string rendered = metrics.render_prometheus();
    std::cout << rendered << std::flush;
    if (!metrics_path.empty()) {
        std::ofstream out(metrics_path);
        out << rendered;
    }
    return 0;
}
