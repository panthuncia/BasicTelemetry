#include <BasicTelemetry/Artifacts.h>
#include <BasicTelemetry/Telemetry.h>

#include <cassert>
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
}

int main()
{
    assert(!Enabled());
    NestedScopesAndMetrics();
    assert(!Enabled());
    AsyncContext();
    AllocationOwnership();
    MemoryResourceAndArtifacts();
    return 0;
}
