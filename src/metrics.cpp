#include "resilientdds/metrics.hpp"

#include <iomanip>
#include <sstream>

namespace resilientdds {

void MetricsRegistry::increment(const std::string& name, std::uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[name] += value;
}

void MetricsRegistry::gauge(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[name] = value;
}

std::string MetricsRegistry::render_prometheus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    for (const auto& [name, value] : counters_) {
        out << name << " " << value << "\n";
    }
    out << std::fixed << std::setprecision(3);
    for (const auto& [name, value] : gauges_) {
        out << name << " " << value << "\n";
    }
    return out.str();
}

} // namespace resilientdds
