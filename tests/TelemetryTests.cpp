// These tests check everything with assert, so keep it active in optimized builds.
#undef NDEBUG

#include <BasicTelemetry/Artifacts.h>
#include <BasicTelemetry/Telemetry.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <string_view>
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

void FilteredTraceRoots()
{
    // Quota is reserved per thread in 1024-event chunks; two threads record here.
    Session session({ .mode = CaptureMode::Trace, .maximumTraceEvents = 2 * 1024,
        .traceRootScopes = { "selected.root" } });
    static const Callsite outsideCallsite("outside");
    static const Callsite rootCallsite("selected.root");
    static const Callsite childCallsite("selected.child");
    static const Callsite workerCallsite("selected.worker");
    {
        Scope outside(outsideCallsite);
        {
            Scope root(rootCallsite);
            const auto token = CaptureCurrentContext();
            std::thread worker([token] {
                ContextBinding binding(token);
                Scope child(workerCallsite);
            });
            { Scope child(childCallsite); }
            worker.join();
        }
        // Selection must not leak after the root closes, including through a
        // context captured outside the selected subtree.
        const auto token = CaptureCurrentContext();
        std::thread worker([token] {
            ContextBinding binding(token);
            Scope child(workerCallsite);
        });
        worker.join();
    }
    const auto snapshot = session.Snapshot();
    assert(snapshot.events.size() == 3);
    assert(snapshot.scopes.size() == 4); // Summaries still include outside.
    const auto root = std::ranges::find_if(snapshot.events, [&](const auto& e) {
        return e.scopeId == rootCallsite.id;
    });
    assert(root != snapshot.events.end());
    for (const auto& event : snapshot.events)
        if (event.scopeId != rootCallsite.id) assert(event.parentEventId == root->eventId);
    const auto workers = std::ranges::find_if(snapshot.scopes, [&](const auto& s) {
        return s.scopeId == workerCallsite.id;
    });
    assert(workers != snapshot.scopes.end());
    assert(workers->inclusiveNs.count == 2);
    assert(snapshot.droppedEvents == 0);
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

void ConcurrentChunkedTrace()
{
    constexpr std::size_t threadCount = 8;
    constexpr std::size_t eventsPerThread = 5'000;
    Session session({
        .mode = CaptureMode::Trace,
        .retainedSamplesPerMetric = 64,
        .maximumTraceEvents = 50'000,
    });
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        workers.emplace_back([] {
            static const Callsite callsite("chunked.concurrent");
            const std::string annotation(256, 'x');
            for (std::size_t index = 0; index < eventsPerThread; ++index) {
                Scope scope(callsite);
                scope.Text(annotation);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto snapshot = session.Snapshot();
    assert(snapshot.events.size() == threadCount * eventsPerThread);
    assert(snapshot.droppedEvents == 0);
    assert(snapshot.droppedContendedScopeSamples == 0);
    assert(!snapshot.events.empty());
    assert(snapshot.events.front().text.size() == 128);
    const auto aggregate = std::ranges::find_if(snapshot.scopes, [](const auto& scope) {
        return scope.inclusiveNs.count == threadCount * eventsPerThread;
    });
    assert(aggregate != snapshot.scopes.end());
}

void ConcurrentQuotaExhaustion()
{
    constexpr std::size_t threadCount = 8;
    constexpr std::size_t eventsPerThread = 4'000;
    Session session({ .mode = CaptureMode::Trace, .maximumTraceEvents = 1'024 });
    std::vector<std::thread> workers;
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        workers.emplace_back([] {
            static const Callsite callsite("chunked.quota");
            for (std::size_t index = 0; index < eventsPerThread; ++index) Scope scope(callsite);
        });
    }
    for (auto& worker : workers) worker.join();
    const auto snapshot = session.Snapshot();
    assert(snapshot.events.size() == 1'024);
    assert(snapshot.droppedEvents == threadCount * eventsPerThread - snapshot.events.size());
    assert(snapshot.droppedContendedScopeSamples == 0);
}

void MetricsMergeAcrossThreads()
{
    constexpr std::size_t threadCount = 8;
    constexpr std::int64_t additionsPerThread = 1'000;
    Session session({ .mode = CaptureMode::Summary, .retainedSamplesPerMetric = 16 });
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        workers.emplace_back([threadIndex] {
            for (std::int64_t index = 0; index < additionsPerThread; ++index) {
                AddCounter("merge.named_counter");
                Record("merge.distribution", 5);
            }
            MaxGauge("merge.max_gauge", static_cast<std::int64_t>(threadIndex));
        });
    }
    for (auto& worker : workers) worker.join();
    SetGauge("merge.gauge", 7);
    SetGauge("merge.gauge", 3);

    const auto snapshot = session.Snapshot();
    const auto find = [&snapshot](std::string_view name) {
        const auto metric = std::ranges::find(snapshot.metrics, name, &MetricSnapshot::name);
        assert(metric != snapshot.metrics.end());
        return *metric;
    };
    const auto counter = find("merge.named_counter");
    assert(counter.kind == "counter");
    assert(counter.current == threadCount * additionsPerThread);
    assert(counter.maximum == counter.current);
    const auto distribution = find("merge.distribution");
    assert(distribution.distribution.count == threadCount * additionsPerThread);
    assert(distribution.distribution.total == 5 * threadCount * additionsPerThread);
    assert(distribution.distribution.retainedSamples.size() == 16);
    const auto maxGauge = find("merge.max_gauge");
    assert(maxGauge.current == threadCount - 1);
    const auto gauge = find("merge.gauge");
    assert(gauge.current == 3);
    assert(gauge.maximum == 7);
    assert(gauge.distribution.count == 2);
}

void SessionReplacedWhileScopeOpen()
{
    static const Callsite outerCallsite("replaced.outer");
    static const Callsite innerCallsite("replaced.inner");
    auto first = std::make_unique<Session>(SessionConfig{ .mode = CaptureMode::Summary });
    {
        // The outer scope's session ends and is destroyed while the scope is open.
        Scope outer(outerCallsite);
        assert(outer.IsActive());
        first.reset();
        assert(!Enabled());
        Session second({ .mode = CaptureMode::Summary });
        assert(Enabled());
        {
            Scope inner(innerCallsite);
            assert(inner.IsActive());
        }
        const auto snapshot = second.Snapshot();
        assert(snapshot.scopes.size() == 1);
        assert(snapshot.scopes[0].scopeId == innerCallsite.id);
    }
    assert(!Enabled());
    // No session has been created on this new thread until now.
    Session third({ .mode = CaptureMode::Summary });
    std::thread worker([] {
        assert(Enabled());
        Scope scope(innerCallsite);
        assert(scope.IsActive());
    });
    worker.join();
    assert(third.Snapshot().scopes.size() == 1);
}

void ConcurrentSnapshots()
{
    constexpr std::size_t threadCount = 8;
    Session session({
        .mode = CaptureMode::Trace,
        .retainedSamplesPerMetric = 64,
        .maximumTraceEvents = 100'000,
    });
    std::atomic<bool> stop{};
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        workers.emplace_back([&stop] {
            static const Callsite callsite("snapshot.concurrent");
            while (!stop.load(std::memory_order_relaxed)) Scope scope(callsite);
        });
    }
    for (std::size_t index = 0; index < 100; ++index) {
        const auto snapshot = session.Snapshot();
        assert(snapshot.droppedContendedScopeSamples == 0);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& worker : workers) worker.join();
    const auto snapshot = session.Snapshot();
    assert(!snapshot.scopes.empty());
    assert(!snapshot.events.empty());
}
}

int main()
{
    assert(!Enabled());
    NestedScopesAndMetrics();
    assert(!Enabled());
    AsyncContext();
    FilteredTraceRoots();
    AllocationOwnership();
    MemoryResourceAndArtifacts();
    StatisticalSampling();
    ConcurrentChunkedTrace();
    ConcurrentQuotaExhaustion();
    ConcurrentSnapshots();
    MetricsMergeAcrossThreads();
    SessionReplacedWhileScopeOpen();
    return 0;
}
