#include "resilientdds/anomaly_detector.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

using namespace resilientdds;

int main() {
    constexpr std::uint64_t samples = 1'000'000;
    constexpr std::int64_t step_ns = 1'000'000; // 1ms
    AnomalyDetector detector({250, 50, kCurrentSchemaVersion});

    std::int64_t now_ns = 1'000'000'000;
    std::uint64_t anomalies = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t seq = 1; seq <= samples; ++seq) {
        now_ns += step_ns;
        TelemetrySample sample{"benchmark-source", seq, now_ns - 100'000, now_ns, 40.0, 28.0, 0, 1};
        anomalies += detector.evaluate(sample, now_ns).size();
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(end - start).count();
    const auto rate = static_cast<double>(samples) / elapsed;

    std::cout << "samples=" << samples << '\n'
              << "anomalies=" << anomalies << '\n'
              << "elapsed_seconds=" << elapsed << '\n'
              << "samples_per_second=" << static_cast<std::uint64_t>(rate) << '\n';
    return anomalies == 0 ? 0 : 1;
}
