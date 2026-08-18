#pragma once

#include "resilientdds/anomaly_detector.hpp"

#include <fstream>
#include <mutex>
#include <string>

namespace resilientdds {

class AuditSink {
public:
    explicit AuditSink(const std::string& path);
    void write(const AnomalyEvent& event, std::int64_t observed_at_ns);
    bool good() const noexcept;

private:
    static std::string escape_json(const std::string& input);
    mutable std::mutex mutex_;
    std::ofstream stream_;
};

} // namespace resilientdds
