#include "resilientdds/audit_sink.hpp"

#include <iomanip>
#include <sstream>

namespace resilientdds {

AuditSink::AuditSink(const std::string& path) : stream_(path, std::ios::app) {}

bool AuditSink::good() const noexcept { return stream_.good(); }

std::string AuditSink::escape_json(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(static_cast<unsigned char>(ch));
                    out += escaped.str();
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

void AuditSink::write(const AnomalyEvent& event, std::int64_t observed_at_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_) return;
    stream_ << "{\"observed_at_ns\":" << observed_at_ns
            << ",\"source_id\":\"" << escape_json(event.source_id)
            << "\",\"sequence\":" << event.sequence
            << ",\"kind\":\"" << to_string(event.kind)
            << "\",\"missing_samples\":" << event.missing_samples
            << ",\"age_ms\":" << event.age_ms
            << ",\"detail\":\"" << escape_json(event.detail) << "\"}\n";
    stream_.flush();
}

} // namespace resilientdds
