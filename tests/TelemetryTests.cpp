#include <BasicTelemetry/Artifacts.h>
#include <BasicTelemetry/Telemetry.h>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <memory_resource>
#include <thread>
#include <vector>

namespace
{
using namespace basic_telemetry;

void NestedScopesAndMetrics()
{
    Session session({
        .mode = CaptureMode::Trace,
        .trackAllocations = true,
        .retainedSamplesPerMetric = 32,
        .maximumTraceEvents = 128,
    });
    Counter counter("test.counter");
    Gauge gauge("test.gauge");
    Distribution distribution("test.distribution");
    {
        Frame frame("test.frame");
        frame.SetDimension("passes", 4);
        static const Callsite outerCallsite("outer");
        Scope outer(outerCallsite);
        outer.Text("outer annotation");
        outer.Value(7);
        {
            static const Callsite innerCallsite("inner");
            Scope inner(innerCallsite);
            counter.Add(2);
            gauge.Set(9);
            distribution.Record(11);
        }
    }
    const auto snapshot = session.Snapshot();
    assert(snapshot.scopes.size() == 2);
    assert(snapshot.events.size() == 2);
    assert(snapshot.frames.size() == 1);
    assert(snapshot.metrics.size() == 3);
    const auto outer = std::ranges::find_if(snapshot.events, [](const auto& event) {
        return event.parentEventId == 0;
    });
    const auto inner = std::ranges::find_if(snapshot.events, [](const auto& event) {
        return event.parentEventId != 0;
    });
    assert(outer != snapshot.events.end());
    assert(inner != snapshot.events.end());
    assert(inner->parentEventId == outer->eventId);
    assert(outer->inclusiveNs >= outer->selfNs);
}

void AsyncContext()
{
    Session session({ .mode = CaptureMode::Trace });
    static const Callsite parentCallsite("async.parent");
    Scope parent(parentCallsite);
    const auto token = parent.CaptureContext();
    std::thread worker([token] {
        ContextBinding binding(token);
        static const Callsite childCallsite("async.child");
        Scope child(childCallsite);
    });
    worker.join();
    const auto snapshot = session.Snapshot();
    assert(snapshot.events.size() == 1); // Parent is still active.
    assert(snapshot.events.front().parentEventId == parent.EventId());
}

void AllocationOwnership()
{
    Session session({
        .mode = CaptureMode::Trace,
        .trackAllocations = true,
    });
    AllocationToken token;
    {
        Frame frame("allocation.frame");
        static const Callsite ownerCallsite("allocation.owner");
        Scope owner(ownerCallsite);
        token = RecordAllocation(64, AllocationDomain::CpuCompile);
        assert(token);
    }
    {
        static const Callsite freeCallsite("allocation.free");
        Scope freeScope(freeCallsite);
        RecordFree(token);
    }
    const auto snapshot = session.Snapshot();
    const auto owner = std::ranges::find_if(snapshot.scopes, [&](const auto& scope) {
        return scope.scopeId == token.ownerScopeId;
    });
    assert(owner != snapshot.scopes.end());
    assert(owner->allocatedBytes == 64);
    assert(owner->freedBytes == 64);
    assert(owner->liveBytes == 0);
    assert(owner->freedLater == 1);
}

void MemoryResourceAndArtifacts()
{
    const auto output = std::filesystem::temp_directory_path() / "basic-telemetry-tests";
    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);
    auto sink = MakeArtifactSink({ .outputDirectory = output });
    {
        Session session(
            {
                .mode = CaptureMode::Trace,
                .trackAllocations = true,
                .metadata = { { "test", "true" } },
            },
            { sink });
        static const Callsite callsite("pmr.scope");
        Scope scope(callsite);
        TrackingMemoryResource resource;
        std::pmr::vector<int> values(&resource);
        values.resize(128);
    }
    assert(sink->Error().empty());
    assert(std::filesystem::exists(output / "manifest.json"));
    assert(std::filesystem::exists(output / "summary.json"));
    assert(std::filesystem::exists(output / "frames.jsonl"));
    assert(std::filesystem::exists(output / "events.jsonl"));
    assert(std::filesystem::exists(output / "profile.sqlite"));
    assert(std::filesystem::exists(output / "summary.md"));
    std::filesystem::remove_all(output, ignored);
}

void StatisticalSampling()
{
    StatisticalSampler constant({
        .minimumSamples = 20,
        .maximumSamples = 100,
        .confidenceLevel = 0.95,
        .relativeHalfWidth = 0.01,
    });
    for (int index = 0; index < 20; ++index) {
        constant.AddSample(100.0);
    }
    const auto constantSummary = constant.Summarize("constant");
    assert(constantSummary.converged);
    assert(constantSummary.confidenceLow == 100.0);
    assert(constantSummary.confidenceHigh == 100.0);

    StatisticalSampler correlated({
        .minimumSamples = 20,
        .maximumSamples = 100,
        .confidenceLevel = 0.95,
        .relativeHalfWidth = 0.01,
    });
    double value = 0.0;
    for (int index = 0; index < 80; ++index) {
        value = 0.95 * value + std::sin(static_cast<double>(index) * 0.17);
        correlated.AddSample(100.0 + value);
    }
    const auto correlatedSummary = correlated.Summarize("correlated");
    assert(correlatedSummary.effectiveSampleCount <= correlatedSummary.rawSampleCount);
    assert(correlatedSummary.lag1Autocorrelation > 0.0);

    Session session({
        .mode = CaptureMode::Summary,
        .samplingTargets = {
            {
                .name = "sample.roi",
                .category = "test",
                .sampling = {
                    .minimumSamples = 2,
                    .maximumSamples = 2,
                    .relativeHalfWidth = 0.0,
                },
            },
        },
    });
    for (int index = 0; index < 2; ++index) {
        static const Callsite roi("sample.roi", "test");
        Scope scope(roi);
    }
    const auto status = session.SamplingStatus();
    assert(status.size() == 1);
    assert(status.front().rawSampleCount == 2);
    assert(session.SamplingComplete());
}
}

int main()
{
    assert(!Enabled());
    NestedScopesAndMetrics();
    assert(!Enabled());
    AsyncContext();
    AllocationOwnership();
    MemoryResourceAndArtifacts();
    StatisticalSampling();
    return 0;
}
