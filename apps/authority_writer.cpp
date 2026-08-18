// Milestone 4A probe: one DDS writer participating in SHARED or EXCLUSIVE
// ownership. This is deliberately separate from the reusable transport adapter
// until the authority semantics are proven by the scenario harness.
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace dds = eprosima::fastdds::dds;
using resilientdds::kCurrentSchemaVersion;

namespace {

enum class OwnershipMode { shared, exclusive };

std::string handle_hex(const dds::InstanceHandle_t& handle) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < 16; ++i) {
        out << std::setw(2) << static_cast<unsigned>(handle.value[i]);
    }
    return out.str();
}

struct WriterListener final : dds::DataWriterListener {
    std::atomic<int> matches{0};
    void on_publication_matched(dds::DataWriter*, const dds::PublicationMatchedStatus& status) override {
        matches.store(status.current_count);
    }
};

struct Options {
    int domain{181};
    std::string topic{"SystemTelemetry"};
    std::string source_id{"command-channel-01"};
    std::string role{"writer"};
    OwnershipMode ownership{OwnershipMode::exclusive};
    std::uint32_t strength{0};
    std::uint64_t count{10000};
    std::uint32_t rate_hz{50};
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc - 1; ++i) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--domain") o.domain = std::atoi(v.c_str());
        else if (k == "--topic") o.topic = v;
        else if (k == "--source-id") o.source_id = v;
        else if (k == "--role") o.role = v;
        else if (k == "--strength") o.strength = static_cast<std::uint32_t>(std::stoul(v));
        else if (k == "--count") o.count = std::stoull(v);
        else if (k == "--rate-hz") o.rate_hz = static_cast<std::uint32_t>(std::stoul(v));
        else if (k == "--ownership") {
            o.ownership = v == "shared" ? OwnershipMode::shared : OwnershipMode::exclusive;
        }
    }
    if (o.rate_hz == 0) o.rate_hz = 1;
    return o;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    auto* factory = dds::DomainParticipantFactory::get_instance();

    auto participant_qos = dds::PARTICIPANT_QOS_DEFAULT;
    participant_qos.transport().use_builtin_transports = false;
    participant_qos.transport().user_transports.push_back(
        std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>());

    auto* participant = factory->create_participant(o.domain, participant_qos);
    if (participant == nullptr) return 2;

    dds::TypeSupport type(new resilientdds::SystemTelemetryPubSubType());
    type.register_type(participant);

    const auto ownership_kind = o.ownership == OwnershipMode::exclusive
                                    ? dds::EXCLUSIVE_OWNERSHIP_QOS
                                    : dds::SHARED_OWNERSHIP_QOS;
    auto topic_qos = dds::TOPIC_QOS_DEFAULT;
    topic_qos.ownership().kind = ownership_kind;
    auto* topic = participant->create_topic(o.topic, type.get_type_name(), topic_qos);
    auto* publisher = participant->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
    if (topic == nullptr || publisher == nullptr) {
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    auto writer_qos = dds::DATAWRITER_QOS_DEFAULT;
    writer_qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
    writer_qos.durability().kind = dds::TRANSIENT_LOCAL_DURABILITY_QOS;
    writer_qos.history().kind = dds::KEEP_LAST_HISTORY_QOS;
    writer_qos.history().depth = 8;
    writer_qos.deadline().period = {0, 100'000'000};
    writer_qos.liveliness().kind = dds::AUTOMATIC_LIVELINESS_QOS;
    writer_qos.liveliness().lease_duration = {0, 500'000'000};
    writer_qos.ownership().kind = ownership_kind;
    writer_qos.ownership_strength().value = o.strength;

    WriterListener listener;
    auto* writer = publisher->create_datawriter(topic, writer_qos, &listener);
    if (writer == nullptr) {
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    std::cout << "WRITER_READY role=" << o.role
              << " handle=" << handle_hex(writer->get_instance_handle())
              << " strength=" << o.strength
              << " ownership=" << (o.ownership == OwnershipMode::exclusive ? "exclusive" : "shared")
              << "\n" << std::flush;

    for (int waited = 0; waited < 60 && listener.matches.load() == 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const auto period = std::chrono::microseconds(1'000'000 / o.rate_hz);
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
        if (!writer->write(&sample)) {
            std::cerr << "WRITE_FAILED role=" << o.role << " seq=" << seq << "\n";
            break;
        }
        std::this_thread::sleep_for(period);
    }

    std::cout << "WRITER_DONE role=" << o.role << "\n" << std::flush;
    participant->delete_contained_entities();
    factory->delete_participant(participant);
    return 0;
}
