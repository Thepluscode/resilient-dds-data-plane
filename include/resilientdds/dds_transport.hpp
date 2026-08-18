#pragma once

#include "resilientdds/anomaly_detector.hpp"
#include "resilientdds/audit_sink.hpp"
#include "resilientdds/health_monitor.hpp"
#include "resilientdds/metrics.hpp"
#include "resilientdds/qos_profiles.hpp"
#include "resilientdds/telemetry.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace resilientdds {

// Transport selection. Fast DDS prefers shared memory between local endpoints,
// which silently bypasses any `tc netem` impairment applied to a network
// interface. Network-degradation tests MUST run udp_only or they measure
// nothing.
enum class TransportMode { defaults, udp_only };

// DDS Security material is kept out of the application model. Paths refer to
// ephemeral test PKI in CI or externally managed credentials in deployment.
// The library never generates or stores credentials itself.
struct DdsSecurityConfig {
    bool enabled{false};
    bool encryption{true};
    std::string identity_ca;
    std::string identity_certificate;
    std::string private_key;
    std::string permissions_ca;
    std::string governance;
    std::string permissions;

    bool complete() const noexcept {
        return !enabled || (!identity_ca.empty() && !identity_certificate.empty() &&
                            !private_key.empty() && !permissions_ca.empty() &&
                            !governance.empty() && !permissions.empty());
    }
};

// Convention used by the security harness:
//   <root>/identity_ca.pem
//   <root>/permissions_ca.pem
//   <root>/governance.smime
//   <root>/<role>-cert.pem
//   <root>/<role>-key.pem
//   <root>/<role>-permissions.smime
DdsSecurityConfig security_from_directory(const std::string& root,
                                          const std::string& role,
                                          bool encryption = true);

// Vendor seam. Fast DDS headers stay inside the .cpp behind a pimpl so the
// trustworthiness layer, the tests and the apps never include vendor headers.
// This is what makes an RTI Connext backend a new .cpp rather than a rewrite.

class TelemetryPublisher {
public:
    explicit TelemetryPublisher(MetricsRegistry& metrics);
    ~TelemetryPublisher();
    TelemetryPublisher(const TelemetryPublisher&) = delete;
    TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;

    bool start(int domain_id, const std::string& topic, const QosProfile& profile,
               TransportMode transport = TransportMode::udp_only,
               const DdsSecurityConfig* security = nullptr);
    bool publish(const TelemetrySample& sample);
    void assert_liveliness();
    bool matched() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TelemetrySubscriber {
public:
    TelemetrySubscriber(AnomalyDetector& detector,
                        HealthMonitor& health,
                        MetricsRegistry& metrics,
                        AuditSink* audit);
    ~TelemetrySubscriber();
    TelemetrySubscriber(const TelemetrySubscriber&) = delete;
    TelemetrySubscriber& operator=(const TelemetrySubscriber&) = delete;

    // Status callbacks (match, deadline, liveliness) carry no instance handle we
    // can resolve to a source, so the monitored source must be named up front or
    // those events attach to a phantom "unknown" source and are never seen.
    void set_source(const std::string& source_id);

    // Simulate a consumer that cannot keep up. A perfectly healthy network with
    // a subscriber slower than the publisher is its own failure mode, and it
    // looks nothing like packet loss.
    void set_processing_delay_us(std::int64_t delay_us);

    bool start(int domain_id, const std::string& topic, const QosProfile& profile,
               TransportMode transport = TransportMode::udp_only,
               const DdsSecurityConfig* security = nullptr);

    // End-to-end write->read latency observed on this reader, in microseconds.
    // Derived from the publisher's source_timestamp, so it is only meaningful
    // while both endpoints share a clock (same host, or NTP/PTP disciplined).
    struct Latency {
        std::uint64_t count{0};
        std::int64_t min_us{0};
        std::int64_t p50_us{0};
        std::int64_t p95_us{0};
        std::int64_t p99_us{0};
        std::int64_t p999_us{0};
        std::int64_t max_us{0};
        double mean_us{0.0};
        double stddev_us{0.0};
    };
    Latency latency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Wall-clock nanoseconds. Publisher and subscriber compare timestamps across
// processes, so this must be system_clock, not steady_clock.
std::int64_t now_ns();

} // namespace resilientdds
