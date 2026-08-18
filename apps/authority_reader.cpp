// Milestone 4A probe: observe which DataWriter actually owns a keyed instance.
#include "SystemTelemetry.h"
#include "SystemTelemetryPubSubTypes.h"

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
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
#include <unordered_map>

namespace dds = eprosima::fastdds::dds;

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

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct ReaderListener final : dds::DataReaderListener {
    std::atomic<std::uint64_t> samples{0};
    std::atomic<int> matches{0};
    std::unordered_map<std::string, std::string> visible_owner;

    void on_subscription_matched(dds::DataReader*, const dds::SubscriptionMatchedStatus& status) override {
        matches.store(status.current_count);
        std::cout << "MATCH writers=" << status.current_count << " at_ns=" << now_ns() << "\n"
                  << std::flush;
    }

    void on_data_available(dds::DataReader* reader) override {
        resilientdds::SystemTelemetry wire;
        dds::SampleInfo info;
        while (reader->take_next_sample(&wire, &info) ==
               eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) continue;
            const std::string source = wire.source_id().to_string();
            const std::string writer = handle_hex(info.publication_handle);
            const auto observed = now_ns();
            const auto age_ms = (observed - wire.source_timestamp_ns()) / 1'000'000;
            const auto current = visible_owner.find(source);
            if (current == visible_owner.end()) {
                visible_owner[source] = writer;
                std::cout << "OWNER_ACTIVE source=" << source
                          << " current=" << writer
                          << " seq=" << wire.sequence_number()
                          << " age_ms=" << age_ms
                          << " at_ns=" << observed << "\n" << std::flush;
            } else if (current->second != writer) {
                const std::string previous = current->second;
                current->second = writer;
                std::cout << "OWNER_CHANGE source=" << source
                          << " previous=" << previous
                          << " current=" << writer
                          << " seq=" << wire.sequence_number()
                          << " age_ms=" << age_ms
                          << " at_ns=" << observed << "\n" << std::flush;
            }
            ++samples;
        }
    }
};

struct Options {
    int domain{181};
    std::string topic{"SystemTelemetry"};
    OwnershipMode ownership{OwnershipMode::exclusive};
    int duration_s{10};
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc - 1; ++i) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--domain") o.domain = std::atoi(v.c_str());
        else if (k == "--topic") o.topic = v;
        else if (k == "--duration-s") o.duration_s = std::atoi(v.c_str());
        else if (k == "--ownership") {
            o.ownership = v == "shared" ? OwnershipMode::shared : OwnershipMode::exclusive;
        }
    }
    return o;
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
    auto* subscriber = participant->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT);
    if (topic == nullptr || subscriber == nullptr) {
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    auto reader_qos = dds::DATAREADER_QOS_DEFAULT;
    reader_qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
    reader_qos.durability().kind = dds::TRANSIENT_LOCAL_DURABILITY_QOS;
    reader_qos.history().kind = dds::KEEP_LAST_HISTORY_QOS;
    reader_qos.history().depth = 8;
    reader_qos.deadline().period = {0, 100'000'000};
    reader_qos.liveliness().kind = dds::AUTOMATIC_LIVELINESS_QOS;
    reader_qos.liveliness().lease_duration = {0, 500'000'000};
    reader_qos.ownership().kind = ownership_kind;

    ReaderListener listener;
    auto* reader = subscriber->create_datareader(topic, reader_qos, &listener);
    if (reader == nullptr) {
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    std::cout << "READER_READY ownership="
              << (o.ownership == OwnershipMode::exclusive ? "exclusive" : "shared")
              << " at_ns=" << now_ns() << "\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::seconds(o.duration_s));
    std::cout << "READER_DONE samples=" << listener.samples.load()
              << " matches=" << listener.matches.load() << "\n" << std::flush;

    participant->delete_contained_entities();
    factory->delete_participant(participant);
    return listener.samples.load() == 0 ? 1 : 0;
}
