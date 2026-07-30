#include <BasicTelemetry/Sampling.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <ranges>
#include <stdexcept>

namespace basic_telemetry
{
namespace
{
double Percentile(std::vector<double> values, double probability)
{
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const double index = probability * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(index));
    const auto upper = static_cast<std::size_t>(std::ceil(index));
    if (lower == upper) {
        return values[lower];
    }
    const double fraction = index - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

// Peter J. Acklam's inverse-normal approximation. The subsequent expansion
// converts the normal quantile to a Student-t quantile.
double InverseNormal(double probability)
{
    constexpr double a1 = -3.969683028665376e+01;
    constexpr double a2 = 2.209460984245205e+02;
    constexpr double a3 = -2.759285104469687e+02;
    constexpr double a4 = 1.383577518672690e+02;
    constexpr double a5 = -3.066479806614716e+01;
    constexpr double a6 = 2.506628277459239e+00;
    constexpr double b1 = -5.447609879822406e+01;
    constexpr double b2 = 1.615858368580409e+02;
    constexpr double b3 = -1.556989798598866e+02;
    constexpr double b4 = 6.680131188771972e+01;
    constexpr double b5 = -1.328068155288572e+01;
    constexpr double c1 = -7.784894002430293e-03;
    constexpr double c2 = -3.223964580411365e-01;
    constexpr double c3 = -2.400758277161838e+00;
    constexpr double c4 = -2.549732539343734e+00;
    constexpr double c5 = 4.374664141464968e+00;
    constexpr double c6 = 2.938163982698783e+00;
    constexpr double d1 = 7.784695709041462e-03;
    constexpr double d2 = 3.224671290700398e-01;
    constexpr double d3 = 2.445134137142996e+00;
    constexpr double d4 = 3.754408661907416e+00;
    constexpr double low = 0.02425;
    constexpr double high = 1.0 - low;

    if (probability <= 0.0 || probability >= 1.0) {
        throw std::invalid_argument("normal quantile probability must be between zero and one");
    }
    if (probability < low) {
        const double q = std::sqrt(-2.0 * std::log(probability));
        return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6)
            / ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }
    if (probability > high) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - probability));
        return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6)
            / ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }
    const double q = probability - 0.5;
    const double r = q * q;
    return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q
        / (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
}

double StudentTCritical(double confidenceLevel, double degreesOfFreedom)
{
    const double probability = 0.5 + confidenceLevel / 2.0;
    if (degreesOfFreedom <= 1.0) {
        return std::tan(std::numbers::pi * (probability - 0.5));
    }
    const double z = InverseNormal(probability);
    const double z2 = z * z;
    const double z3 = z2 * z;
    const double z5 = z3 * z2;
    const double z7 = z5 * z2;
    const double z9 = z7 * z2;
    const double inverseDf = 1.0 / degreesOfFreedom;
    return z
        + (z3 + z) * inverseDf / 4.0
        + (5.0 * z5 + 16.0 * z3 + 3.0 * z) * inverseDf * inverseDf / 96.0
        + (3.0 * z7 + 19.0 * z5 + 17.0 * z3 - 15.0 * z)
            * inverseDf * inverseDf * inverseDf / 384.0
        + (79.0 * z9 + 776.0 * z7 + 1482.0 * z5 - 1920.0 * z3 - 945.0 * z)
            * inverseDf * inverseDf * inverseDf * inverseDf / 92'160.0;
}
}

StatisticalSampler::StatisticalSampler(SamplingConfig config)
    : m_config(std::move(config))
{
    if (m_config.minimumSamples < 2 || m_config.maximumSamples < m_config.minimumSamples) {
        throw std::invalid_argument("invalid statistical sampling minimum/maximum sample counts");
    }
    if (!(m_config.confidenceLevel > 0.0 && m_config.confidenceLevel < 1.0)) {
        throw std::invalid_argument("sampling confidence level must be between zero and one");
    }
    if (m_config.relativeHalfWidth < 0.0
        || (m_config.absoluteHalfWidth && *m_config.absoluteHalfWidth < 0.0)) {
        throw std::invalid_argument("sampling half-width targets cannot be negative");
    }
    m_samples.reserve(m_config.maximumSamples);
}

void StatisticalSampler::AddSample(double value)
{
    if (!std::isfinite(value)) {
        throw std::invalid_argument("statistical samples must be finite");
    }
    if (m_samples.size() < m_config.maximumSamples) {
        m_samples.push_back(value);
    }
}

void StatisticalSampler::Reset()
{
    m_samples.clear();
}

const SamplingConfig& StatisticalSampler::Config() const noexcept
{
    return m_config;
}

const std::vector<double>& StatisticalSampler::Samples() const noexcept
{
    return m_samples;
}

SamplingSummary StatisticalSampler::Summarize(std::string name, std::string category) const
{
    SamplingSummary result{
        .name = std::move(name),
        .category = std::move(category),
        .rawSampleCount = static_cast<std::uint32_t>(m_samples.size()),
        .minimumSamples = m_config.minimumSamples,
        .maximumSamples = m_config.maximumSamples,
        .confidenceLevel = m_config.confidenceLevel,
        .targetRelativeHalfWidth = m_config.relativeHalfWidth,
        .targetAbsoluteHalfWidth = m_config.absoluteHalfWidth,
        .reachedMaximum = m_samples.size() >= m_config.maximumSamples,
    };
    if (m_samples.empty()) {
        return result;
    }

    const double n = static_cast<double>(m_samples.size());
    result.mean = std::accumulate(m_samples.begin(), m_samples.end(), 0.0) / n;
    double squared = 0.0;
    for (double value : m_samples) {
        squared += (value - result.mean) * (value - result.mean);
    }
    const double sampleVariance = m_samples.size() > 1 ? squared / (n - 1.0) : 0.0;
    result.standardDeviation = std::sqrt(sampleVariance);
    result.coefficientOfVariation = result.mean != 0.0
        ? result.standardDeviation / std::abs(result.mean)
        : 0.0;
    result.median = Percentile(m_samples, 0.5);
    result.p05 = Percentile(m_samples, 0.05);
    result.p95 = Percentile(m_samples, 0.95);

    double longRunVariance = m_samples.size() > 1 ? squared / n : 0.0;
    const std::size_t bandwidth = m_samples.size() > 2
        ? std::min(m_samples.size() - 1, std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::floor(4.0 * std::pow(n / 100.0, 2.0 / 9.0)))))
        : 0;
    double lagOneCovariance = 0.0;
    for (std::size_t lag = 1; lag <= bandwidth; ++lag) {
        double covariance = 0.0;
        for (std::size_t index = lag; index < m_samples.size(); ++index) {
            covariance += (m_samples[index] - result.mean) * (m_samples[index - lag] - result.mean);
        }
        covariance /= n;
        if (lag == 1) {
            lagOneCovariance = covariance;
        }
        const double weight = 1.0 - static_cast<double>(lag) / static_cast<double>(bandwidth + 1);
        longRunVariance += 2.0 * weight * covariance;
    }
    longRunVariance = std::max(longRunVariance, 0.0);
    const double populationVariance = m_samples.size() > 1 ? squared / n : 0.0;
    result.lag1Autocorrelation = populationVariance > 0.0
        ? lagOneCovariance / populationVariance
        : 0.0;
    result.effectiveSampleCount = longRunVariance > 0.0 && sampleVariance > 0.0
        ? std::clamp(n * sampleVariance / longRunVariance, 1.0, n)
        : n;

    const double standardError = std::sqrt(longRunVariance / n);
    const double degreesOfFreedom = std::max(1.0, result.effectiveSampleCount - 1.0);
    const double halfWidth = StudentTCritical(m_config.confidenceLevel, degreesOfFreedom) * standardError;
    result.confidenceLow = result.mean - halfWidth;
    result.confidenceHigh = result.mean + halfWidth;
    result.relativeHalfWidth = result.mean != 0.0
        ? halfWidth / std::abs(result.mean)
        : (halfWidth == 0.0 ? 0.0 : std::numeric_limits<double>::infinity());

    const double mad = Percentile([&] {
        std::vector<double> deviations;
        deviations.reserve(m_samples.size());
        for (double value : m_samples) {
            deviations.push_back(std::abs(value - result.median));
        }
        return deviations;
    }(), 0.5);
    if (mad > 0.0) {
        for (double value : m_samples) {
            if (0.67448975 * std::abs(value - result.median) / mad > 3.5) {
                ++result.madOutlierCount;
            }
        }
    }

    const bool precisionMet = halfWidth == 0.0
        || (m_config.absoluteHalfWidth && halfWidth <= *m_config.absoluteHalfWidth)
        || (result.mean != 0.0 && result.relativeHalfWidth <= m_config.relativeHalfWidth);
    result.converged =
        result.rawSampleCount >= m_config.minimumSamples
        && result.effectiveSampleCount >= static_cast<double>(m_config.minimumSamples)
        && precisionMet;
    return result;
}

bool StatisticalSampler::Converged() const
{
    return Summarize().converged;
}

bool StatisticalSampler::Complete() const
{
    const auto summary = Summarize();
    return summary.converged || summary.reachedMaximum;
}
}
