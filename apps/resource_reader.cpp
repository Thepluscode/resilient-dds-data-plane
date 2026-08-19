// Milestone 4B companion reader. The harness freezes this entire process with
// SIGSTOP so protocol threads stop acknowledging samples while the DDS match
// remains established. That distinguishes writer-history pressure from a merely
// slow application callback.
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
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace fdds = eprosima::fastdds::dds;

namespace {

struct Options {
    int domain{191};
    std::string topic{"ResourceBoundTelemetry"};
    std::uint32_t duration_s{30};
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc - 1; ++i) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--domain") o.domain = std::atoi(v.c_str());
        else if (k == "--topic") o.topic = v;
        else if (k == "--duration-s") o.duration_s = static_cast<std::uint32_t>(std::stoul(v));
    }
    return o;
}

struct ReaderListener final : fdds::DataReaderListener {
    std::atomic<std::uint64_t> received{0};
    std::atomic<int> matches{0};

    void on_subscription_matched(fdds::DataReader*, const fdds::SubscriptionMatchedStatus& status) override {
        matches.store(status.current_count);
        std::cout << "RESOURCE_READER_MATCHED writers=" << status.current_count << "\n" << std::flush;
    }

    void on_data_available(fdds::DataReader* reader) override {
        resilientdds::SystemTelemetry sample;
        fdds::SampleInfo info;
        while (reader->take_next_sample(&sample, &info) ==
               eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) continue;
            const auto n = received.fetch_add(1) + 1;
            if (n == 1 || n % 100 == 0) {
                std::cout << "RESOURCE_READER_PROGRESS received=" << n
                          << " seq=" << sample.sequence_number() << "\n" << std::flush;
            }
        }
    }
};

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
    auto* subscriber = participant->create_subscriber(fdds::SUBSCRIBER_QOS_DEFAULT);
    if (topic == nullptr || subscriber == nullptr) {
        std::cerr << "SETUP_FAILED entity=" << (topic == nullptr ? "topic" : "subscriber") << "\n";
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    auto qos = fdds::DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = fdds::RELIABLE_RELIABILITY_QOS;
    qos.durability().kind = fdds::VOLATILE_DURABILITY_QOS;
    qos.history().kind = fdds::KEEP_ALL_HISTORY_QOS;
    qos.resource_limits().max_samples = 5000;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 5000;
    qos.resource_limits().allocated_samples = 256;

    ReaderListener listener;
    auto* reader = subscriber->create_datareader(topic, qos, &listener);
    if (reader == nullptr) {
        std::cerr << "SETUP_FAILED entity=datareader\n";
        participant->delete_contained_entities();
        factory->delete_participant(participant);
        return 2;
    }

    std::cout << "RESOURCE_READER_READY duration_s=" << o.duration_s << "\n" << std::flush;
    for (std::uint32_t second = 0; second < o.duration_s; ++second) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "RESOURCE_READER_DONE received=" << listener.received.load() << "\n" << std::flush;
    participant->delete_contained_entities();
    factory->delete_participant(participant);
    return 0;
}
