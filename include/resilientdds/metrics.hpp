#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace resilientdds {

class MetricsRegistry {
public:
    void increment(const std::string& name, std::uint64_t value = 1);
    void gauge(const std::string& name, double value);
    std::string render_prometheus() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> counters_;
    std::map<std::string, double> gauges_;
};

} // namespace resilientdds
