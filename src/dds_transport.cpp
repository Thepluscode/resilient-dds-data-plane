#include "resilientdds/dds_transport.hpp"

#include "SystemTelemetry.h"
#include "SystemTelemetryPubSubTypes.h"

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace ddsapi = eprosima::fastdds::dds;

namespace resilientdds {
namespace {

eprosima::fastrtps::Duration_t ms_to_duration(std::uint32_t ms) {
    return {static_cast<std::int32_t>(ms / 1000u),
            static_cast<std::uint32_t>((ms % 1000u) * 1'000'000u)};
}

// One mapping function, used by both endpoints. Writer/reader QoS drifting apart
// is exactly the bug that shows up as an unexplained incompatible-QoS event.
template <typename QosT>
void apply_profile(QosT& qos, const QosProfile& profile) {
    qos.reliability().kind = profile.reliability == Reliability::reliable
                                 ? ddsapi::RELIABLE_RELIABILITY_QOS
                                 : ddsapi::BEST_EFFORT_RELIABILITY_QOS;
    // How long write() waits for history space before failing. This is the
    // knob that turns a full writer history into an observable event instead of
    // an indefinite stall.
    qos.reliability().max_blocking_time = ms_to_duration(profile.max_blocking_ms);
    qos.durability().kind = profile.durability == Durability::transient_local
                                ? ddsapi::TRANSIENT_LOCAL_DURABILITY_QOS
                                : ddsapi::VOLATILE_DURABILITY_QOS;
    qos.history().kind = profile.history == History::keep_all ? ddsapi::KEEP_ALL_HISTORY_QOS
                                                              : ddsapi::KEEP_LAST_HISTORY_QOS;
    qos.history().depth = static_cast<std::int32_t>(profile.history_depth);
    if (profile.max_samples > 0) {
        // Fast DDS enforces max_samples >= max_instances * max_samples_per_instance
        // and rejects the writer otherwise. Bounding the per-instance limit
        // without also bounding instances fails that check, so pin instances to
        // the single keyed source this lab publishes.
        qos.resource_limits().max_samples = static_cast<std::int32_t>(profile.max_samples);
        qos.resource_limits().max_instances = 1;
        qos.resource_limits().max_samples_per_instance = static_cast<std::int32_t>(profile.max_samples);
    }
    if (profile.deadline_ms > 0) {
        qos.deadline().period = ms_to_duration(profile.deadline_ms);
    }
    qos.liveliness().kind = profile.liveliness == Liveliness::manual_by_topic
                                ? ddsapi::MANUAL_BY_TOPIC_LIVELINESS_QOS
                                : ddsapi::AUTOMATIC_LIVELINESS_QOS;
    if (profile.liveliness_lease_ms > 0) {
        qos.liveliness().lease_duration = ms_to_duration(profile.liveliness_lease_ms);
        qos.liveliness().announcement_period = ms_to_duration(profile.liveliness_lease_ms / 2);
    }
}

std::string file_uri(const std::string& value) {
    if (value.rfind("file://", 0) == 0) return value;
    return std::string("file://") + std::filesystem::absolute(value).string();
}

bool security_config_ok(const DdsSecurityConfig* security) {
    if (security == nullptr || !security->enabled) return true;
    if (!security->complete()) return false;
    const std::string* paths[] = {
        &security->identity_ca,
        &security->identity_certificate,
        &security->private_key,
        &security->permissions_ca,
        &security->governance,
        &security->permissions,
    };
    for (const auto* path : paths) {
        if (path->rfind("file://", 0) == 0) continue;
        if (!std::filesystem::is_regular_file(*path)) return false;
    }
    return true;
}

void apply_security(ddsapi::DomainParticipantQos& qos, const DdsSecurityConfig& security) {
    auto& props = qos.properties().properties();

    props.emplace_back("dds.sec.auth.plugin", "builtin.PKI-DH");
    props.emplace_back("dds.sec.auth.builtin.PKI-DH.identity_ca", file_uri(security.identity_ca));
    props.emplace_back("dds.sec.auth.builtin.PKI-DH.identity_certificate",
                       file_uri(security.identity_certificate));
    props.emplace_back("dds.sec.auth.builtin.PKI-DH.private_key", file_uri(security.private_key));
    props.emplace_back("dds.sec.auth.builtin.PKI-DH.preferred_key_agreement", "ECDH");

    props.emplace_back("dds.sec.access.plugin", "builtin.Access-Permissions");
    props.emplace_back("dds.sec.access.builtin.Access-Permissions.permissions_ca",
                       file_uri(security.permissions_ca));
    props.emplace_back("dds.sec.access.builtin.Access-Permissions.governance",
                       file_uri(security.governance));
    props.emplace_back("dds.sec.access.builtin.Access-Permissions.permissions",
                       file_uri(security.permissions));

    if (security.encryption) {
        props.emplace_back("dds.sec.crypto.plugin", "builtin.AES-GCM-GMAC");
    }
}

ddsapi::DomainParticipantQos participant_qos(TransportMode transport,
                                              const DdsSecurityConfig* security) {
    auto qos = ddsapi::PARTICIPANT_QOS_DEFAULT;
    if (transport == TransportMode::udp_only) {
        qos.transport().use_builtin_transports = false;
        qos.transport().user_transports.push_back(
            std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>());
    }
    if (security != nullptr && security->enabled) apply_security(qos, *security);
    return qos;
}

} // namespace

DdsSecurityConfig security_from_directory(const std::string& root,
                                          const std::string& role,
                                          bool encryption) {
    DdsSecurityConfig out;
    if (root.empty()) return out;
    const std::filesystem::path base(root);
    out.enabled = true;
    out.encryption = encryption;
    out.identity_ca = (base / "identity_ca.pem").string();
    out.identity_certificate = (base / (role + "-cert.pem")).string();
    out.private_key = (base / (role + "-key.pem")).string();
    out.permissions_ca = (base / "permissions_ca.pem").string();
    out.governance = (base / "governance.smime").string();
    out.permissions = (base / (role + "-permissions.smime")).string();
    return out;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------- publisher

struct TelemetryPublisher::Impl : public ddsapi::DataWriterListener {
    explicit Impl(MetricsRegistry& m) : metrics(m) {}

    void on_publication_matched(ddsapi::DataWriter*,
                                const ddsapi::PublicationMatchedStatus& status) override {
        match_count.store(status.current_count);
        metrics.gauge("rdtf_publication_matched", static_cast<double>(status.current_count));
    }

    void on_offered_deadline_missed(ddsapi::DataWriter*,
                                    const ddsapi::OfferedDeadlineMissedStatus&) override {
        metrics.increment("rdtf_offered_deadline_missed_total");
    }

    void on_offered_incompatible_qos(ddsapi::DataWriter*,
                                     const ddsapi::OfferedIncompatibleQosStatus& status) override {
        metrics.increment("rdtf_offered_incompatible_qos_total");
        std::cerr << "QOS offered-incompatible last_policy_id=" << status.last_policy_id << "\n";
    }

    MetricsRegistry& metrics;
    std::atomic<int> match_count{0};
    std::atomic<std::uint64_t> total_blocked_us{0};
    std::atomic<std::uint64_t> max_blocked_us{0};
    ddsapi::DomainParticipant* participant{nullptr};
    ddsapi::Publisher* publisher{nullptr};
    ddsapi::Topic* topic{nullptr};
    ddsapi::DataWriter* writer{nullptr};
    ddsapi::TypeSupport type{new resilientdds::SystemTelemetryPubSubType()};
};

TelemetryPublisher::TelemetryPublisher(MetricsRegistry& metrics)
    : impl_(std::make_unique<Impl>(metrics)) {}

TelemetryPublisher::~TelemetryPublisher() {
    if (impl_->participant != nullptr) {
        impl_->participant->delete_contained_entities();
        ddsapi::DomainParticipantFactory::get_instance()->delete_participant(impl_->participant);
    }
}

bool TelemetryPublisher::start(int domain_id, const std::string& topic_name,
                               const QosProfile& profile, TransportMode transport,
                               const DdsSecurityConfig* security) {
    const auto errors = validate(profile);
    if (!errors.empty()) {
        for (const auto& e : errors) std::cerr << "QOS invalid: " << e << "\n";
        impl_->metrics.increment("rdtf_adapter_init_failures_total");
        return false;
    }
    if (!security_config_ok(security)) {
        std::cerr << "DDS security configuration is incomplete or references missing files\n";
        impl_->metrics.increment("rdtf_security_config_failures_total");
        return false;
    }

    auto* factory = ddsapi::DomainParticipantFactory::get_instance();
    impl_->participant = factory->create_participant(domain_id, participant_qos(transport, security));
    if (impl_->participant == nullptr) {
        impl_->metrics.increment("rdtf_adapter_init_failures_total");
        return false;
    }

    impl_->type.register_type(impl_->participant);
    impl_->topic = impl_->participant->create_topic(topic_name, impl_->type.get_type_name(),
                                                    ddsapi::TOPIC_QOS_DEFAULT);
    if (impl_->topic == nullptr) return false;

    impl_->publisher = impl_->participant->create_publisher(ddsapi::PUBLISHER_QOS_DEFAULT);
    if (impl_->publisher == nullptr) return false;

    auto qos = ddsapi::DATAWRITER_QOS_DEFAULT;
    apply_profile(qos, profile);
    impl_->writer = impl_->publisher->create_datawriter(impl_->topic, qos, impl_.get());
    if (impl_->writer == nullptr) return false;

    impl_->metrics.increment("rdtf_adapter_initializations_total");
    if (security != nullptr && security->enabled) {
        impl_->metrics.increment("rdtf_secure_participant_initializations_total");
    }
    return true;
}

bool TelemetryPublisher::publish(const TelemetrySample& sample) {
    if (impl_->writer == nullptr) return false;
    resilientdds::SystemTelemetry wire;
    wire.source_id(sample.source_id);
    wire.sequence_number(sample.sequence);
    wire.source_timestamp_ns(sample.source_timestamp_ns);
    wire.ingest_timestamp_ns(sample.ingest_timestamp_ns);
    wire.temperature_c(sample.temperature_c);
    wire.voltage_v(sample.voltage_v);
    wire.health_flags(sample.health_flags);
    wire.schema_version(sample.schema_version);

    // A blocked write is the producer feeling backpressure. Timing it is the
    // only way to tell "delivered promptly" from "stalled just under the cap".
    const auto started = std::chrono::steady_clock::now();
    const bool ok = impl_->writer->write(&wire);
    const auto blocked_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - started).count();

    impl_->total_blocked_us.fetch_add(static_cast<std::uint64_t>(blocked_us));
    auto prev = impl_->max_blocked_us.load();
    while (blocked_us > static_cast<std::int64_t>(prev) &&
           !impl_->max_blocked_us.compare_exchange_weak(prev, static_cast<std::uint64_t>(blocked_us))) {
    }
    impl_->metrics.increment(ok ? "rdtf_samples_published_total" : "rdtf_publish_failures_total");
    return ok;
}

void TelemetryPublisher::assert_liveliness() {
    if (impl_->writer != nullptr) impl_->writer->assert_liveliness();
}

bool TelemetryPublisher::matched() const { return impl_->match_count.load() > 0; }

TelemetryPublisher::WriteStats TelemetryPublisher::write_stats() const {
    return {impl_->total_blocked_us.load(), impl_->max_blocked_us.load()};
}

// --------------------------------------------------------------- subscriber

struct TelemetrySubscriber::Impl : public ddsapi::DataReaderListener {
    Impl(AnomalyDetector& d, HealthMonitor& h, MetricsRegistry& m, AuditSink* a)
        : detector(d), health(h), metrics(m), audit(a) {}

    void on_data_available(ddsapi::DataReader* reader) override {
        resilientdds::SystemTelemetry wire;
        ddsapi::SampleInfo info;
        while (reader->take_next_sample(&wire, &info) ==
               eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) continue;

            TelemetrySample sample;
            sample.source_id = wire.source_id().to_string();
            sample.sequence = wire.sequence_number();
            sample.source_timestamp_ns = wire.source_timestamp_ns();
            sample.ingest_timestamp_ns = now_ns();
            sample.temperature_c = wire.temperature_c();
            sample.voltage_v = wire.voltage_v();
            sample.health_flags = wire.health_flags();
            sample.schema_version = wire.schema_version();

            source_hint = sample.source_id;
            if (!first_seen) {
                first_seen = true;
                std::cout << "FIRST_SAMPLE seq=" << sample.sequence << " age_ms="
                          << (now_ns() - sample.source_timestamp_ns) / 1'000'000 << "\n"
                          << std::flush;
            }
            const auto observed = sample.ingest_timestamp_ns;
            {
                std::lock_guard<std::mutex> lock(latency_mutex);
                if (latency_us.size() < kLatencyCap) {
                    latency_us.push_back((observed - sample.source_timestamp_ns) / 1000);
                }
            }
            metrics.increment("rdtf_samples_received_total");
            health.on_sample(sample.source_id, observed,
                             (observed - sample.source_timestamp_ns) / 1'000'000);

            if (processing_delay_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(processing_delay_us));
            }

            for (const auto& event : detector.evaluate(sample, observed)) {
                metrics.increment(std::string("rdtf_anomaly_") + to_string(event.kind) + "_total");
                if (audit != nullptr) audit->write(event, observed);
                std::cout << "ANOMALY source=" << event.source_id
                          << " seq=" << event.sequence
                          << " kind=" << to_string(event.kind)
                          << " missing=" << event.missing_samples
                          << " detail=\"" << event.detail << "\"\n"
                          << std::flush;
            }
        }
    }

    void on_requested_deadline_missed(ddsapi::DataReader*,
                                      const ddsapi::RequestedDeadlineMissedStatus& status) override {
        metrics.increment("rdtf_deadline_missed_total",
                          static_cast<std::uint64_t>(status.total_count_change));
        health.on_deadline_missed(last_source());
        std::cout << "DEADLINE_MISSED total=" << status.total_count << "\n" << std::flush;
    }

    void on_liveliness_changed(ddsapi::DataReader*,
                               const ddsapi::LivelinessChangedStatus& status) override {
        if (status.not_alive_count_change > 0) {
            metrics.increment("rdtf_liveliness_lost_total",
                              static_cast<std::uint64_t>(status.not_alive_count_change));
            health.on_liveliness_lost(last_source());
            std::cout << "LIVELINESS_LOST alive=" << status.alive_count << "\n" << std::flush;
        }
    }

    void on_requested_incompatible_qos(ddsapi::DataReader*,
                                       const ddsapi::RequestedIncompatibleQosStatus& status) override {
        metrics.increment("rdtf_incompatible_qos_total");
        health.on_incompatible_qos(last_source());
        std::cout << "INCOMPATIBLE_QOS last_policy_id=" << status.last_policy_id << "\n"
                  << std::flush;
    }

    void on_sample_lost(ddsapi::DataReader*, const ddsapi::SampleLostStatus& status) override {
        metrics.increment("rdtf_middleware_sample_lost_total",
                          static_cast<std::uint64_t>(status.total_count_change));
    }

    void on_subscription_matched(ddsapi::DataReader*,
                                 const ddsapi::SubscriptionMatchedStatus& status) override {
        metrics.gauge("rdtf_subscription_matched", static_cast<double>(status.current_count));
        health.on_writer_matched(last_source(), status.current_count > 0);
        std::cout << "MATCH writers=" << status.current_count << "\n" << std::flush;
    }

    const std::string& last_source() const { return source_hint; }

    static constexpr std::size_t kLatencyCap = 1'000'000;
    mutable std::mutex latency_mutex;
    std::vector<std::int64_t> latency_us;

    AnomalyDetector& detector;
    HealthMonitor& health;
    MetricsRegistry& metrics;
    AuditSink* audit;
    std::string source_hint{"unknown"};
    bool first_seen{false};
    std::int64_t processing_delay_us{0};

    ddsapi::DomainParticipant* participant{nullptr};
    ddsapi::Subscriber* subscriber{nullptr};
    ddsapi::Topic* topic{nullptr};
    ddsapi::DataReader* reader{nullptr};
    ddsapi::TypeSupport type{new resilientdds::SystemTelemetryPubSubType()};
};

TelemetrySubscriber::TelemetrySubscriber(AnomalyDetector& detector, HealthMonitor& health,
                                         MetricsRegistry& metrics, AuditSink* audit)
    : impl_(std::make_unique<Impl>(detector, health, metrics, audit)) {}

void TelemetrySubscriber::set_source(const std::string& source_id) {
    impl_->source_hint = source_id;
}

void TelemetrySubscriber::set_processing_delay_us(std::int64_t delay_us) {
    impl_->processing_delay_us = delay_us;
}

TelemetrySubscriber::~TelemetrySubscriber() {
    if (impl_->participant != nullptr) {
        impl_->participant->delete_contained_entities();
        ddsapi::DomainParticipantFactory::get_instance()->delete_participant(impl_->participant);
    }
}

bool TelemetrySubscriber::start(int domain_id, const std::string& topic_name,
                                const QosProfile& profile, TransportMode transport,
                                const DdsSecurityConfig* security) {
    const auto errors = validate(profile);
    if (!errors.empty()) {
        for (const auto& e : errors) std::cerr << "QOS invalid: " << e << "\n";
        impl_->metrics.increment("rdtf_adapter_init_failures_total");
        return false;
    }
    if (!security_config_ok(security)) {
        std::cerr << "DDS security configuration is incomplete or references missing files\n";
        impl_->metrics.increment("rdtf_security_config_failures_total");
        return false;
    }

    auto* factory = ddsapi::DomainParticipantFactory::get_instance();
    impl_->participant = factory->create_participant(domain_id, participant_qos(transport, security));
    if (impl_->participant == nullptr) {
        impl_->metrics.increment("rdtf_adapter_init_failures_total");
        return false;
    }

    impl_->type.register_type(impl_->participant);
    impl_->topic = impl_->participant->create_topic(topic_name, impl_->type.get_type_name(),
                                                    ddsapi::TOPIC_QOS_DEFAULT);
    if (impl_->topic == nullptr) return false;

    impl_->subscriber = impl_->participant->create_subscriber(ddsapi::SUBSCRIBER_QOS_DEFAULT);
    if (impl_->subscriber == nullptr) return false;

    auto qos = ddsapi::DATAREADER_QOS_DEFAULT;
    apply_profile(qos, profile);
    impl_->reader = impl_->subscriber->create_datareader(impl_->topic, qos, impl_.get());
    if (impl_->reader == nullptr) return false;

    impl_->metrics.increment("rdtf_adapter_initializations_total");
    if (security != nullptr && security->enabled) {
        impl_->metrics.increment("rdtf_secure_participant_initializations_total");
    }
    return true;
}

TelemetrySubscriber::Latency TelemetrySubscriber::latency() const {
    std::lock_guard<std::mutex> lock(impl_->latency_mutex);
    Latency out;
    auto samples = impl_->latency_us;
    if (samples.empty()) return out;
    std::sort(samples.begin(), samples.end());

    const auto pct = [&samples](double p) {
        const auto n = static_cast<double>(samples.size());
        auto rank = static_cast<std::size_t>(std::ceil(p * n / 100.0));
        if (rank == 0) rank = 1;
        return samples[std::min(rank, samples.size()) - 1];
    };

    out.count = samples.size();
    out.min_us = samples.front();
    out.max_us = samples.back();
    out.p50_us = pct(50.0);
    out.p95_us = pct(95.0);
    out.p99_us = pct(99.0);
    out.p999_us = pct(99.9);
    const double sum = static_cast<double>(
        std::accumulate(samples.begin(), samples.end(), std::int64_t{0}));
    out.mean_us = sum / static_cast<double>(samples.size());
    double sq = 0.0;
    for (const auto v : samples) {
        const double d = static_cast<double>(v) - out.mean_us;
        sq += d * d;
    }
    out.stddev_us = std::sqrt(sq / static_cast<double>(samples.size()));
    return out;
}

} // namespace resilientdds
