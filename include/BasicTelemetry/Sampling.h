#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace basic_telemetry
{
struct SamplingConfig
{
    std::uint32_t minimumSamples{ 30 };
    std::uint32_t maximumSamples{ 1'000 };
    double confidenceLevel{ 0.95 };
    double relativeHalfWidth{ 0.02 };
    std::optional<double> absoluteHalfWidth;
};

struct SamplingTargetConfig
{
    std::string name;
    std::string category;
    SamplingConfig sampling;
    bool required{ true };
};

struct SamplingSummary
{
    std::string name;
    std::string category;
    std::uint32_t rawSampleCount{};
    std::uint32_t minimumSamples{};
    std::uint32_t maximumSamples{};
    double confidenceLevel{};
    double targetRelativeHalfWidth{};
    std::optional<double> targetAbsoluteHalfWidth;
    double effectiveSampleCount{};
    double mean{};
    double standardDeviation{};
    double coefficientOfVariation{};
    double median{};
    double p05{};
    double p95{};
    double confidenceLow{};
    double confidenceHigh{};
    double relativeHalfWidth{};
    double lag1Autocorrelation{};
    std::uint32_t madOutlierCount{};
    bool converged{};
    bool reachedMaximum{};
};

class StatisticalSampler
{
public:
    explicit StatisticalSampler(SamplingConfig config = {});

    void AddSample(double value);
    void Reset();

    [[nodiscard]] const SamplingConfig& Config() const noexcept;
    [[nodiscard]] const std::vector<double>& Samples() const noexcept;
    [[nodiscard]] SamplingSummary Summarize(
        std::string name = {},
        std::string category = {}) const;
    [[nodiscard]] bool Converged() const;
    [[nodiscard]] bool Complete() const;

private:
    SamplingConfig m_config;
    std::vector<double> m_samples;
};
}
