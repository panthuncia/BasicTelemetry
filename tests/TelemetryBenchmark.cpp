#include <BasicTelemetry/Telemetry.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>

namespace
{
template <class Function>
std::uint64_t Measure(std::size_t iterations, Function&& function)
{
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
        function();
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}
}

int main()
{
    constexpr std::size_t disabledIterations = 1'000'000;
    constexpr std::size_t enabledIterations = 100'000;
    static const basic_telemetry::Callsite callsite("BasicTelemetryBenchmark.Scope");

    const auto baselineNs = Measure(disabledIterations, [] {});
    const auto disabledNs = Measure(disabledIterations, [&] {
        basic_telemetry::Scope scope(callsite);
    });

    std::uint64_t enabledNs{};
    {
        basic_telemetry::Session session({
            .mode = basic_telemetry::CaptureMode::Summary,
            .retainedSamplesPerMetric = 1024,
        });
        enabledNs = Measure(enabledIterations, [&] {
            basic_telemetry::Scope scope(callsite);
        });
    }

    const auto disabledOverhead =
        static_cast<double>(disabledNs - std::min(disabledNs, baselineNs)) /
        disabledIterations;
    const auto enabledOverhead =
        static_cast<double>(enabledNs) / enabledIterations;
    std::cout
        << "baseline_ns_per_iteration="
        << static_cast<double>(baselineNs) / disabledIterations << '\n'
        << "disabled_overhead_ns_per_scope=" << disabledOverhead << '\n'
        << "summary_ns_per_scope=" << enabledOverhead << '\n';

    return disabledOverhead <= 100.0 ? 0 : 1;
}
