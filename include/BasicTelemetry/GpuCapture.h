#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The data of a GPU hardware-counter capture over named ranges (a GPU profiler such as NVIDIA's Nsight Perf, driven by
// OpenRenderGraph's Telemetry/NvPerfCapture): what to measure, where, and what came back. Backend-neutral, so the code
// that plans and analyses captures (sampling, reports) does not depend on the renderer that takes them.
namespace basic_telemetry::gpu_capture
{
// One metric, as the profiler's metrics evaluator names it (e.g. "sm__throughput"); the numeric fields are the
// evaluator's enums (metric type, rollup, submetric).
struct MetricRequest
{
    std::string id;
    std::string name;
    std::string outputName;
    std::string unit;
    std::uint8_t metricType = 0;
    std::uint8_t rollupOp = 0;
    std::uint16_t submetric = 0;
    bool required = true;
};

// A range to measure: its name, exactly, or every name starting with it when it ends in '*'; and the queue it runs on
// (empty: any).
struct PassFilter
{
    std::string name;
    std::string queue;
};

struct CaptureConfiguration
{
    std::vector<MetricRequest> metrics;
    std::vector<PassFilter> passes;
    std::string controllerQueue = "Graphics";
    std::uint32_t syncTimeoutMs = 10000;
};

// One measured range: the metrics' values in the configuration's order. A name that recurs in a frame is told apart by
// its occurrence.
struct RangeResult
{
    std::string queue;
    std::string passName;
    std::uint32_t occurrence = 0;
    std::uint32_t rangeIndex = 0;
    std::vector<double> values;
};

struct CaptureResult
{
    std::uint64_t sampleId = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    std::size_t scheduledPasses = 0;
    std::uint64_t droppedRanges = 0;
    std::uint64_t droppedTraceBytes = 0;
    bool success = false;
    std::string error;
    std::string chipName;
    std::vector<MetricRequest> metrics;
    std::vector<MetricRequest> unsupportedMetrics;
    std::vector<RangeResult> ranges;
};
}  // namespace basic_telemetry::gpu_capture
