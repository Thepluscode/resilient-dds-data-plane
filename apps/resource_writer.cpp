// Milestone 4B probe: bounded RELIABLE writer behavior when a matched reader
// stops acknowledging data. The reusable adapter intentionally remains unchanged
// until this failure mode is measured against Fast DDS itself.
#include "SystemTelemetry.h"
#include "SystemTelemetryPubSubTypes.h"
#include "resilientdds/telemetry.hpp"

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace fdds = eprosima::fastdds::dds;
using eprosima::fastrtps::types::ReturnCode_t;
using resilientdds::kCurrentSchemaVersion;

namespace {

struct WriterListener final : fdds::DataWriterListener {
    std::atomic<int> matches{0};
    void on_publication_matched(fdds::DataWriter*, const fdds::PublicationMatchedStatus& status) override {
        matches.store(status.current_count);
    }
};

struct Options {
    int domain{191};
    std::string topic{"ResourceBoundTelemetry"};
    std::string source_id{"resource-channel-01"};
    std::uint64_t count{600};
    std::uint32_t rate_hz{200};
    std::uint32_t history_limit{8};
    std::uint32_t max_blocking_ms{50};
    std::uint32_t wait_match_ms{5000};
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc - 1; ++i) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--domain") o.domain = std::atoi(v.c_str());
        else if (k == "--topic") o.topic = v;
        else if (k == "--source-id") o.source_id = v;
        else if (k == "--count") o.count = std::stoull(v);
        else if (k == "--rate-hz") o.rate_hz = static_cast<std::uint32_t>(std::stoul(v));
        else if (k == "--history-limit") o.history_limit = static_cast<std::uint32_t>(std::stoul(v));
        else if (k == "--max-blocking-ms") o.max_blocking_ms = static_cast<std::uint32_t>(std::stoul(v));
        else if (k == "--wait-match-ms") o.wait_match_ms = static_cast<std::uint32_t>(std::stoul(v));
    }
    if (o.rate_hz == 0) o.rate_hz = 1;
    if (o.history_limit == 0) o.history_limit = 1;
    return o;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

eprosima::fastrtps::Duration_t duration_ms(std::uint32_t value) {
    return {static_cast<std::int32_t>(value / 1000u),
            static_cast<std::uint32_t>((value % 1000u) * 1'000'000u)};
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    auto* factory = fdds::DomainParticipantFactory::get_instance();

    auto participant_qos = fdds::PARTICIPANT_QOS_DEFAULT;
    participant_qos.transport().use_builtin_transports = false;
    participant_qos.transport().user_transports.push_back(
        std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>());

    auto* participant = factory->create_participant(o.domain, participant_qos);
    if (participant == nullptr) {
        std::cerr << "SETUP_FAILED entity=participant domain=" << o.domain << "\n";
        return 2;
    }

    fdds::TypeSupport type(new resilientdds::SystemTelemetryPubSubType());
    type.register_type(participant);
    auto* topic = participant->create_topic(o.topic, type.get_type_name(), fdds::TOPIC_QOS_DEFAULT);
    auto* publisher = participant->create_publisher(fdds::PUBLISHER_QOS_DEFAULT);
    if (topic == nullptr || publisher == nullptr) {
        std::cerr << "SETUP_FAILED entity=" << (topic == nullptr ? "topic" : "publisher") << "\n";
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    auto qos = fdds::DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = fdds::RELIABLE_RELIABILITY_QOS;
    qos.reliability().max_blocking_time = duration_ms(o.max_blocking_ms);
    qos.durability().kind = fdds::VOLATILE_DURABILITY_QOS;
    qos.history().kind = fdds::KEEP_ALL_HISTORY_QOS;
    qos.resource_limits().max_samples = static_cast<std::int32_t>(o.history_limit);
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = static_cast<std::int32_t>(o.history_limit);
    qos.resource_limits().allocated_samples = static_cast<std::int32_t>(o.history_limit);
    qos.resource_limits().extra_samples = 0;

    WriterListener listener;
    auto* writer = publisher->create_datawriter(topic, qos, &listener);
    if (writer == nullptr) {
        std::cerr << "SETUP_FAILED entity=datawriter history_limit=" << o.history_limit
                  << " max_blocking_ms=" << o.max_blocking_ms << "\n";
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    std::cout << "RESOURCE_WRITER_READY history_limit=" << o.history_limit
              << " max_blocking_ms=" << o.max_blocking_ms << "\n" << std::flush;

    for (std::uint32_t waited = 0; waited < o.wait_match_ms && listener.matches.load() == 0; waited += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (listener.matches.load() == 0) {
        std::cerr << "MATCH_TIMEOUT readers=0\n";
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 3;
    }
    std::cout << "RESOURCE_WRITER_MATCHED readers=" << listener.matches.load() << "\n" << std::flush;

    const auto period = std::chrono::microseconds(1'000'000 / o.rate_hz);
    std::uint64_t successes = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t errors = 0;
    std::int64_t max_write_us = 0;
    bool seen_timeout = false;
    bool recovered = false;

    for (std::uint64_t seq = 1; seq <= o.count; ++seq) {
        resilientdds::SystemTelemetry sample;
        sample.source_id(o.source_id);
        sample.sequence_number(seq);
        const auto stamp = now_ns();
        sample.source_timestamp_ns(stamp);
        sample.ingest_timestamp_ns(stamp);
        sample.temperature_c(40.0 + static_cast<double>(seq % 5));
        sample.voltage_v(27.5);
        sample.health_flags(0);
        sample.schema_version(kCurrentSchemaVersion);

        const auto before = std::chrono::steady_clock::now();
        const ReturnCode_t rc = writer->write(&sample, fdds::HANDLE_NIL);
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count();
        max_write_us = std::max(max_write_us, elapsed_us);

        if (rc == ReturnCode_t::RETCODE_OK) {
            ++successes;
            if (seen_timeout && !recovered) {
                recovered = true;
                std::cout << "WRITE_RECOVERED seq=" << seq << " elapsed_us=" << elapsed_us
                          << " wall_ms=" << wall_ms() << "\n" << std::flush;
            }
        } else if (rc == ReturnCode_t::RETCODE_TIMEOUT) {
            ++timeouts;
            seen_timeout = true;
            std::cout << "WRITE_TIMEOUT seq=" << seq << " elapsed_us=" << elapsed_us
                      << " wall_ms=" << wall_ms() << "\n" << std::flush;
        } else {
            ++errors;
            std::cout << "WRITE_ERROR seq=" << seq << " code=" << rc()
                      << " elapsed_us=" << elapsed_us << " wall_ms=" << wall_ms() << "\n"
                      << std::flush;
        }

        std::this_thread::sleep_for(period);
    }

    std::cout << "RESOURCE_SUMMARY attempts=" << o.count
              << " success=" << successes
              << " timeouts=" << timeouts
              << " errors=" << errors
              << " max_write_us=" << max_write_us
              << " recovered=" << (recovered ? 1 : 0)
              << "\n" << std::flush;

    participant->delete_contained_entities();
    factory->delete_participant(participant);
    return errors == 0 ? 0 : 4;
}
