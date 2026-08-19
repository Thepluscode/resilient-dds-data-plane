// Telemetry publisher with deliberate fault injection. Every flag here maps to a
// failure the subscriber's trustworthiness layer claims to detect; without them
// the detection code is untested against real RTPS.
#include "resilientdds/dds_transport.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace resilientdds;

namespace {

struct Options {
    int domain{0};
    std::string topic{"SystemTelemetry"};
    std::string profile{"periodic_telemetry"};
    std::string source_id{"radar-01"};
    std::string security_dir;
    std::string security_role{"publisher"};
    std::uint64_t count{50};
    std::uint32_t rate_hz{20};
    std::uint64_t drop_at{0};
    std::uint64_t duplicate_at{0};
    std::uint64_t stale_at{0};
    std::uint64_t schema_drift_at{0};
    std::uint32_t stall_at{0};
    std::uint32_t wait_match_ms{3000};
    bool allow_shm{false};
};

std::uint64_t u64(const char* v) { return std::strtoull(v, nullptr, 10); }

QosProfile pick(const std::string& name) {
    if (name == "critical_control") return critical_control();
    if (name == "high_rate_sensor") return high_rate_sensor();
    return periodic_telemetry();
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc - 1; ++i) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--domain") o.domain = std::atoi(v);
        else if (k == "--topic") o.topic = v;
        else if (k == "--profile") o.profile = v;
        else if (k == "--source-id") o.source_id = v;
        else if (k == "--security-dir") o.security_dir = v;
        else if (k == "--security-role") o.security_role = v;
        else if (k == "--count") o.count = u64(v);
        else if (k == "--rate-hz") o.rate_hz = static_cast<std::uint32_t>(u64(v));
        else if (k == "--drop-at") o.drop_at = u64(v);
        else if (k == "--duplicate-at") o.duplicate_at = u64(v);
        else if (k == "--stale-at") o.stale_at = u64(v);
        else if (k == "--schema-drift-at") o.schema_drift_at = u64(v);
        else if (k == "--stall-at") o.stall_at = static_cast<std::uint32_t>(u64(v));
        else if (k == "--wait-match-ms") o.wait_match_ms = static_cast<std::uint32_t>(u64(v));
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--allow-shm") o.allow_shm = true;
    }
    if (o.rate_hz == 0) o.rate_hz = 1;

    MetricsRegistry metrics;
    TelemetryPublisher publisher(metrics);
    const QosProfile profile = pick(o.profile);
    const auto security = security_from_directory(o.security_dir, o.security_role);

    if (!publisher.start(o.domain, o.topic, profile,
                         o.allow_shm ? TransportMode::defaults : TransportMode::udp_only,
                         security.enabled ? &security : nullptr)) {
        std::cerr << "publisher failed to start\n";
        return 1;
    }
    std::cout << "PUBLISHER up domain=" << o.domain << " topic=" << o.topic
              << " profile=" << profile.name << " security="
              << (security.enabled ? "on" : "off");
    if (security.enabled) std::cout << " role=" << o.security_role;
    std::cout << "\n" << std::flush;

    for (std::uint32_t waited = 0; waited < o.wait_match_ms && !publisher.matched(); waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const auto period = std::chrono::milliseconds(1000 / o.rate_hz);
    for (std::uint64_t seq = 1; seq <= o.count; ++seq) {
        if (seq == o.drop_at) {
            std::cout << "INJECT drop seq=" << seq << "\n" << std::flush;
            std::this_thread::sleep_for(period);
            continue;
        }
        if (o.stall_at > 0 && seq == o.count / 2) {
            std::cout << "INJECT stall ms=" << o.stall_at << "\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(o.stall_at));
        }

        TelemetrySample s;
        s.source_id = o.source_id;
        s.sequence = seq;
        s.source_timestamp_ns = now_ns();
        s.ingest_timestamp_ns = s.source_timestamp_ns;
        s.temperature_c = 40.0 + static_cast<double>(seq % 5);
        s.voltage_v = 27.5;
        s.schema_version = kCurrentSchemaVersion;

        if (seq == o.stale_at) {
            s.source_timestamp_ns -= 2'000'000'000;
            std::cout << "INJECT stale seq=" << seq << "\n" << std::flush;
        }
        if (seq == o.schema_drift_at) {
            s.schema_version = kCurrentSchemaVersion + 1;
            std::cout << "INJECT schema_drift seq=" << seq << "\n" << std::flush;
        }

        publisher.publish(s);
        if (profile.liveliness == Liveliness::manual_by_topic) publisher.assert_liveliness();

        if (seq == o.duplicate_at) {
            std::cout << "INJECT duplicate seq=" << seq << "\n" << std::flush;
            publisher.publish(s);
        }
        std::this_thread::sleep_for(period);
    }

    std::cout << "PUBLISHER done\n" << metrics.render_prometheus() << std::flush;
    return 0;
}
