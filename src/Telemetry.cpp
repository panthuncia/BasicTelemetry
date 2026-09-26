#include <BasicTelemetry/Telemetry.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__)
#include <time.h>
#endif

namespace basic_telemetry
{
namespace
{
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t HashAppend(std::uint64_t hash, std::string_view text) noexcept
{
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= kFnvPrime;
    }
    return hash;
}

StableId MakeStableId(
    std::string_view name,
    std::string_view file = {},
    std::string_view discriminator = {}) noexcept
{
    auto hash = HashAppend(kFnvOffset, name);
    hash = HashAppend(hash, file);
    hash = HashAppend(hash, discriminator);
    return hash == 0 ? 1 : hash;
}

struct DistributionData
{
    std::uint64_t count{};
    std::uint64_t total{};
    std::uint64_t minimum{ std::numeric_limits<std::uint64_t>::max() };
    std::uint64_t maximum{};
    std::vector<std::uint64_t> samples;

    void Record(std::uint64_t value, std::size_t capacity)
    {
        const auto sampleIndex = count++;
        total += value;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        if (capacity == 0) {
            return;
        }
        if (samples.size() < capacity) {
            samples.push_back(value);
        }
        else {
            samples[static_cast<std::size_t>(sampleIndex % capacity)] = value;
        }
    }

    [[nodiscard]] DistributionSnapshot Snapshot() const
    {
        return {
            .count = count,
            .total = total,
            .minimum = count == 0 ? 0 : minimum,
            .maximum = maximum,
            .retainedSamples = samples,
        };
    }

    void Merge(const DistributionData& other, std::size_t capacity)
    {
        if (other.count == 0) return;
        const auto oldCount = count;
        count += other.count;
        total += other.total;
        minimum = oldCount == 0 ? other.minimum : std::min(minimum, other.minimum);
        maximum = std::max(maximum, other.maximum);
        if (capacity == 0) return;
        for (const auto sample : other.samples) {
            if (samples.size() < capacity) samples.push_back(sample);
            else samples[static_cast<std::size_t>((oldCount + samples.size()) % capacity)] = sample;
        }
    }
};

struct ScopeAggregateData
{
    DistributionData inclusive;
    DistributionData self;
    DistributionData threadCpu;
    DistributionData nonRunning;
    std::uint64_t allocatedCount{};
    std::uint64_t allocatedBytes{};
    std::uint64_t freedCount{};
    std::uint64_t freedBytes{};
    std::uint64_t liveBytes{};
    std::uint64_t peakLiveBytes{};
    std::uint64_t largestAllocation{};
    std::uint64_t freedInSameScope{};
    std::uint64_t freedInSameFrame{};
    std::uint64_t freedLater{};
    std::uint64_t stillLive{};
};

struct MetricData
{
    StableId id{};
    std::string name;
    std::string kind;
    std::int64_t current{};
    std::int64_t maximum{ std::numeric_limits<std::int64_t>::min() };
    DistributionData distribution;
};

struct TelemetryState
{
    static constexpr std::size_t kEventsPerChunk = 1'024;
    static constexpr std::size_t kInlineTextBytes = 128;

    struct TraceEventRecord {
        const Callsite* callsite{};
        std::uint64_t eventId{};
        std::uint64_t parentEventId{};
        std::uint64_t frameId{};
        std::uint64_t threadId{};
        std::uint64_t startNs{};
        std::uint64_t inclusiveNs{};
        std::uint64_t selfNs{};
        std::uint64_t threadCpuNs{};
        std::uint64_t nonRunningNs{};
        std::uint64_t value{};
        std::uint16_t textSize{};
        bool hasValue{};
        std::array<char, kInlineTextBytes> text{};
    };
    static_assert(std::is_trivially_copyable_v<TraceEventRecord>);

    struct TraceChunk {
        std::array<TraceEventRecord, kEventsPerChunk> events{};
        std::size_t size{};
        std::size_t capacity{};
    };

    struct ThreadShard {
        std::mutex chunkMutex;
        std::vector<std::unique_ptr<TraceChunk>> chunks;
        TraceChunk* current{};
        std::unordered_map<StableId, ScopeAggregateData> scopeAggregates;
        std::unordered_map<StableId, const Callsite*> definitions;
        std::atomic<std::uint64_t> droppedEvents{};
        std::uint64_t nextEventId{};
        std::uint64_t eventIdEnd{};
        bool quotaExhausted{};
    };

    struct SamplingTargetData
    {
        explicit SamplingTargetData(SamplingTargetConfig value)
            : config(std::move(value))
            , sampler(config.sampling)
        {
        }

        SamplingTargetConfig config;
        StatisticalSampler sampler;
    };

    TelemetryState(SessionConfig value, std::uint64_t id)
        : config(std::move(value))
        , startedAtNs(NowNs())
        , sessionId(id)
    {
        config.metadata.try_emplace("git_commit", BASICTELEMETRY_GIT_COMMIT);
#if defined(_MSC_FULL_VER)
        config.metadata.try_emplace("compiler", "msvc-" + std::to_string(_MSC_FULL_VER));
#elif defined(__clang_version__)
        config.metadata.try_emplace("compiler", std::string("clang-") + __clang_version__);
#else
        config.metadata.try_emplace("compiler", "unknown");
#endif
#if defined(NDEBUG)
        config.metadata.try_emplace("build_configuration", "optimized");
#else
        config.metadata.try_emplace("build_configuration", "debug");
#endif
        config.metadata.try_emplace(
            "hardware_concurrency",
            std::to_string(std::thread::hardware_concurrency()));
#if defined(_M_X64) || defined(__x86_64__)
        config.metadata.try_emplace("architecture", "x86_64");
#elif defined(_M_ARM64) || defined(__aarch64__)
        config.metadata.try_emplace("architecture", "arm64");
#else
        config.metadata.try_emplace("architecture", "unknown");
#endif
        samplingTargets.reserve(config.samplingTargets.size());
        for (const auto& target : config.samplingTargets) {
            if (target.name.empty()) {
                throw std::invalid_argument("telemetry sampling target name cannot be empty");
            }
            samplingTargets.emplace_back(target);
        }
    }

    SessionConfig config;
    std::uint64_t startedAtNs{};
    std::uint64_t sessionId{};
    std::atomic<bool> accepting{ true };
    std::atomic<std::uint64_t> nextEventId{ 1 };
    std::atomic<std::uint64_t> nextFrameId{ 1 };
    std::atomic<std::uint64_t> nextAllocationId{ 1 };
    mutable std::mutex mutex;
    std::unordered_map<StableId, ScopeDefinition> definitions;
    std::unordered_map<StableId, ScopeAggregateData> scopeAggregates;
    std::unordered_map<StableId, MetricData> metrics;
    std::vector<FrameSnapshot> frames;
    std::vector<AllocationEventSnapshot> allocations;
    std::unordered_map<std::uint64_t, std::size_t> allocationEventIndices;
    std::vector<SamplingTargetData> samplingTargets;
    mutable std::mutex shardsMutex;
    std::vector<std::shared_ptr<ThreadShard>> shards;
    std::atomic_size_t reservedEvents{};
    std::atomic<std::uint64_t> droppedEvents{};
    std::atomic<std::uint64_t> droppedContendedScopeSamples{};
    std::uint64_t allocationTrackingOverflows{};
    std::uint64_t unknownFrees{};

    ThreadShard* ThreadLocalShard()
    {
        struct CacheEntry {
            const TelemetryState* state{};
            std::uint64_t sessionId{};
            ThreadShard* shard{};
        };
        thread_local CacheEntry cache;
        if (cache.state == this && cache.sessionId == sessionId) return cache.shard;
        auto shard = std::make_shared<ThreadShard>();
        auto* result = shard.get();
        {
            std::scoped_lock lock(shardsMutex);
            shards.push_back(std::move(shard));
        }
        cache = { this, sessionId, result };
        return result;
    }

    std::uint64_t NextEventId()
    {
        auto* shard = ThreadLocalShard();
        if (shard->nextEventId == shard->eventIdEnd) {
            constexpr std::uint64_t idsPerBlock = 1'024;
            const auto begin = nextEventId.fetch_add(idsPerBlock, std::memory_order_relaxed);
            shard->nextEventId = begin;
            shard->eventIdEnd = begin + idsPerBlock;
        }
        return shard->nextEventId++;
    }

    void AppendTraceEvent(TraceEventRecord record, std::size_t maximumEvents) noexcept
    {
        try {
            auto* shard = ThreadLocalShard();
            std::scoped_lock lock(shard->chunkMutex);
            if (!shard->current || shard->current->size == shard->current->capacity) {
                if (shard->quotaExhausted) {
                    shard->droppedEvents.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const auto begin = reservedEvents.fetch_add(kEventsPerChunk, std::memory_order_relaxed);
                if (begin >= maximumEvents) {
                    shard->quotaExhausted = true;
                    shard->droppedEvents.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                auto chunk = std::make_unique<TraceChunk>();
                chunk->capacity = std::min(kEventsPerChunk, maximumEvents - begin);
                shard->current = chunk.get();
                shard->chunks.push_back(std::move(chunk));
            }
            shard->current->events[shard->current->size++] = record;
        }
        catch (...) {
            droppedEvents.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

std::atomic<std::shared_ptr<TelemetryState>> g_activeState;
std::atomic<std::uint64_t> g_nextSessionId{ 1 };
// True only while the active session tracks allocations. RecordAllocation runs
// on every heap allocation in hooked processes; checking this before touching
// g_activeState keeps untracked sessions from serializing all allocating
// threads on the atomic<shared_ptr> load's internal lock.
std::atomic<bool> g_allocationTrackingActive{ false };

struct ThreadContext
{
    std::shared_ptr<TelemetryState> boundState;
    Scope* currentScope{};
    std::uint64_t parentEventId{};
    std::uint64_t frameId{};
    bool warmup{};
    bool traceSelected{};
};

thread_local ThreadContext g_threadContext;

std::uint64_t ThreadId() noexcept
{
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

std::shared_ptr<TelemetryState> CurrentState() noexcept
{
    if (g_threadContext.boundState) {
        return g_threadContext.boundState;
    }
    return g_activeState.load(std::memory_order_acquire);
}

void RegisterDefinition(TelemetryState& state, const Callsite& callsite)
{
    if (state.definitions.contains(callsite.id)) {
        return;
    }
    state.definitions.emplace(
        callsite.id,
        ScopeDefinition{
            .id = callsite.id,
            .name = std::string(callsite.name),
            .category = std::string(callsite.category),
            .file = callsite.location.file_name(),
            .function = callsite.location.function_name(),
            .line = callsite.location.line(),
        });
}

SessionSnapshot SnapshotState(const TelemetryState& state, std::uint64_t endedAt)
{
    std::scoped_lock lock(state.mutex);
    std::vector<std::shared_ptr<TelemetryState::ThreadShard>> shards;
    {
        std::scoped_lock shardsLock(state.shardsMutex);
        shards = state.shards;
    }
    auto mergedAggregates = state.scopeAggregates;
    auto mergedDefinitions = state.definitions;
    const auto mergeAggregate = [&](ScopeAggregateData& destination, const ScopeAggregateData& source) {
        const auto capacity = state.config.retainedSamplesPerMetric;
        destination.inclusive.Merge(source.inclusive, capacity);
        destination.self.Merge(source.self, capacity);
        destination.threadCpu.Merge(source.threadCpu, capacity);
        destination.nonRunning.Merge(source.nonRunning, capacity);
        destination.allocatedCount += source.allocatedCount;
        destination.allocatedBytes += source.allocatedBytes;
        destination.freedCount += source.freedCount;
        destination.freedBytes += source.freedBytes;
        destination.liveBytes += source.liveBytes;
        destination.peakLiveBytes = std::max(destination.peakLiveBytes, source.peakLiveBytes);
        destination.largestAllocation = std::max(destination.largestAllocation, source.largestAllocation);
        destination.freedInSameScope += source.freedInSameScope;
        destination.freedInSameFrame += source.freedInSameFrame;
        destination.freedLater += source.freedLater;
        destination.stillLive += source.stillLive;
    };
    for (const auto& shard : shards) {
        std::scoped_lock shardLock(shard->chunkMutex);
        for (const auto& [id, aggregate] : shard->scopeAggregates)
            mergeAggregate(mergedAggregates[id], aggregate);
        for (const auto& [id, callsite] : shard->definitions) {
            if (callsite && !mergedDefinitions.contains(id)) {
                mergedDefinitions.emplace(id, ScopeDefinition{
                    .id = id,
                    .name = std::string(callsite->name),
                    .category = std::string(callsite->category),
                    .file = callsite->location.file_name(),
                    .function = callsite->location.function_name(),
                    .line = callsite->location.line(),
                });
            }
        }
    }
    SessionSnapshot result;
    result.mode = state.config.mode;
    result.allocationTrackingEnabled = state.config.trackAllocations;
    result.startedAtNs = state.startedAtNs;
    result.endedAtNs = endedAt;
    result.droppedEvents = state.droppedEvents.load(std::memory_order_relaxed);
    for (const auto& shard : shards)
        result.droppedEvents += shard->droppedEvents.load(std::memory_order_relaxed);
    result.droppedContendedScopeSamples =
        state.droppedContendedScopeSamples.load(std::memory_order_relaxed);
    result.allocationTrackingOverflows = state.allocationTrackingOverflows;
    result.unknownFrees = state.unknownFrees;
    result.metadata = state.config.metadata;

    result.scopeDefinitions.reserve(mergedDefinitions.size());
    for (const auto& [id, definition] : mergedDefinitions) {
        result.scopeDefinitions.push_back(definition);
    }
    std::ranges::sort(result.scopeDefinitions, {}, &ScopeDefinition::id);

    result.scopes.reserve(mergedAggregates.size());
    for (const auto& [id, aggregate] : mergedAggregates) {
        result.scopes.push_back({
            .scopeId = id,
            .inclusiveNs = aggregate.inclusive.Snapshot(),
            .selfNs = aggregate.self.Snapshot(),
            .threadCpuNs = aggregate.threadCpu.Snapshot(),
            .nonRunningNs = aggregate.nonRunning.Snapshot(),
            .allocatedCount = aggregate.allocatedCount,
            .allocatedBytes = aggregate.allocatedBytes,
            .freedCount = aggregate.freedCount,
            .freedBytes = aggregate.freedBytes,
            .liveBytes = aggregate.liveBytes,
            .peakLiveBytes = aggregate.peakLiveBytes,
            .largestAllocation = aggregate.largestAllocation,
            .freedInSameScope = aggregate.freedInSameScope,
            .freedInSameFrame = aggregate.freedInSameFrame,
            .freedLater = aggregate.freedLater,
            .stillLive = aggregate.stillLive,
        });
    }
    std::ranges::sort(result.scopes, {}, &ScopeAggregateSnapshot::scopeId);

    result.metrics.reserve(state.metrics.size());
    for (const auto& [id, metric] : state.metrics) {
        result.metrics.push_back({
            .id = id,
            .name = metric.name,
            .kind = metric.kind,
            .current = metric.current,
            .maximum = metric.maximum == std::numeric_limits<std::int64_t>::min()
                ? metric.current
                : metric.maximum,
            .distribution = metric.distribution.Snapshot(),
        });
    }
    std::ranges::sort(result.metrics, {}, &MetricSnapshot::id);
    for (const auto& shard : shards) {
        std::scoped_lock chunkLock(shard->chunkMutex);
        for (const auto& chunk : shard->chunks) {
            for (std::size_t index = 0; index < chunk->size; ++index) {
                const auto& event = chunk->events[index];
                result.events.push_back({
                    .eventId = event.eventId,
                    .scopeId = event.callsite ? event.callsite->id : 0,
                    .parentEventId = event.parentEventId,
                    .frameId = event.frameId,
                    .threadId = event.threadId,
                    .startNs = event.startNs,
                    .inclusiveNs = event.inclusiveNs,
                    .selfNs = event.selfNs,
                    .threadCpuNs = event.threadCpuNs,
                    .nonRunningNs = event.nonRunningNs,
                    .text = std::string(event.text.data(), event.textSize),
                    .value = event.value,
                    .hasValue = event.hasValue,
                });
            }
        }
    }
    std::ranges::sort(result.events, {}, &ScopeEventSnapshot::eventId);
    result.frames = state.frames;
    result.allocations = state.allocations;
    result.sampling.reserve(state.samplingTargets.size());
    for (const auto& target : state.samplingTargets) {
        result.sampling.push_back(target.sampler.Summarize(target.config.name, target.config.category));
    }
    return result;
}

MetricData& FindMetric(TelemetryState& state, StableId id, std::string_view name, std::string_view kind)
{
    auto [it, inserted] = state.metrics.try_emplace(id);
    if (inserted) {
        it->second.id = id;
        it->second.name = name;
        it->second.kind = kind;
    }
    return it->second;
}
}

struct Session::Impl
{
    std::shared_ptr<TelemetryState> state;
    std::vector<std::shared_ptr<Sink>> sinks;
    bool flushed{};
};

Session::Session(SessionConfig config, std::vector<std::shared_ptr<Sink>> sinks)
    : m_impl(std::make_shared<Impl>())
{
    m_impl->state = std::make_shared<TelemetryState>(
        std::move(config),
        g_nextSessionId.fetch_add(1, std::memory_order_relaxed));
    m_impl->sinks = std::move(sinks);
    if (m_impl->state->config.mode == CaptureMode::Off) {
        m_impl->state->accepting.store(false, std::memory_order_release);
        return;
    }
    std::shared_ptr<TelemetryState> expected;
    if (!g_activeState.compare_exchange_strong(expected, m_impl->state)) {
        throw std::runtime_error("BasicTelemetry supports one active process session");
    }
    g_allocationTrackingActive.store(m_impl->state->config.trackAllocations, std::memory_order_release);
}

Session::~Session()
{
    Flush();
}

bool Session::IsActive() const noexcept
{
    return m_impl && m_impl->state->accepting.load(std::memory_order_acquire);
}

SessionSnapshot Session::Snapshot() const
{
    return SnapshotState(*m_impl->state, NowNs());
}

std::vector<SamplingSummary> Session::SamplingStatus() const
{
    if (!m_impl) {
        return {};
    }
    std::scoped_lock lock(m_impl->state->mutex);
    std::vector<SamplingSummary> result;
    result.reserve(m_impl->state->samplingTargets.size());
    for (const auto& target : m_impl->state->samplingTargets) {
        result.push_back(target.sampler.Summarize(target.config.name, target.config.category));
    }
    return result;
}

bool Session::SamplingConverged() const
{
    if (!m_impl) {
        return false;
    }
    std::scoped_lock lock(m_impl->state->mutex);
    bool foundRequired = false;
    for (const auto& target : m_impl->state->samplingTargets) {
        if (!target.config.required) {
            continue;
        }
        foundRequired = true;
        if (!target.sampler.Converged()) {
            return false;
        }
    }
    return foundRequired;
}

bool Session::SamplingComplete() const
{
    if (!m_impl) {
        return false;
    }
    std::scoped_lock lock(m_impl->state->mutex);
    bool foundRequired = false;
    for (const auto& target : m_impl->state->samplingTargets) {
        if (!target.config.required) {
            continue;
        }
        foundRequired = true;
        if (!target.sampler.Complete()) {
            return false;
        }
    }
    return foundRequired;
}

void Session::Flush()
{
    if (!m_impl || m_impl->flushed) {
        return;
    }
    m_impl->flushed = true;
    m_impl->state->accepting.store(false, std::memory_order_release);
    g_allocationTrackingActive.store(false, std::memory_order_release);
    auto expected = m_impl->state;
    std::shared_ptr<TelemetryState> empty;
    g_activeState.compare_exchange_strong(expected, empty);
    const auto snapshot = SnapshotState(*m_impl->state, NowNs());
    for (const auto& sink : m_impl->sinks) {
        if (sink) {
            sink->OnSessionEnd(snapshot);
        }
    }
}

Callsite::Callsite(
    std::string_view scopeName,
    std::string_view scopeCategory,
    std::source_location source) noexcept
    : id(MakeStableId(scopeName, source.file_name(), source.function_name()))
    , name(scopeName)
    , category(scopeCategory)
    , location(source)
{
}

ContextToken::operator bool() const noexcept
{
    return static_cast<bool>(m_session);
}

struct ContextBinding::State
{
    ThreadContext previous;
};

ContextBinding::ContextBinding(const ContextToken& token) noexcept
{
    static_assert(sizeof(State) <= sizeof(m_storage));
    m_state = ::new (static_cast<void*>(m_storage)) State;
    m_state->previous = g_threadContext;
    g_threadContext.boundState = std::static_pointer_cast<TelemetryState>(token.m_session);
    g_threadContext.currentScope = nullptr;
    g_threadContext.parentEventId = token.m_parentEventId;
    g_threadContext.frameId = token.m_frameId;
    g_threadContext.traceSelected = token.m_traceSelected;
}

ContextBinding::~ContextBinding()
{
    if (m_state) {
        g_threadContext = std::move(m_state->previous);
        m_state->~State();
        m_state = nullptr;
    }
}

struct Scope::Impl
{
    std::shared_ptr<TelemetryState> state;
    const Callsite* callsite{};
    Scope* previousScope{};
    std::uint64_t eventId{};
    std::uint64_t parentEventId{};
    std::uint64_t frameId{};
    std::uint64_t startNs{};
    std::uint64_t startThreadCpuNs{};
    std::uint64_t childNs{};
    std::uint64_t threadId{};
    bool warmup{};
    bool traceSelected{};
    std::array<char, TelemetryState::kInlineTextBytes> text{};
    std::uint16_t textSize{};
    std::uint64_t value{};
    bool hasValue{};
};

Scope::Scope(const Callsite& callsite) noexcept
{
    auto state = CurrentState();
    if (!state || !state->accepting.load(std::memory_order_acquire)) {
        return;
    }
    try {
        static_assert(sizeof(Impl) <= sizeof(m_storage));
        m_impl = ::new (static_cast<void*>(m_storage)) Impl;
        m_impl->state = std::move(state);
        m_impl->callsite = &callsite;
        m_impl->previousScope = g_threadContext.currentScope;
        m_impl->parentEventId = m_impl->previousScope && m_impl->previousScope->m_impl
            ? m_impl->previousScope->m_impl->eventId
            : g_threadContext.parentEventId;
        m_impl->frameId = g_threadContext.frameId;
        m_impl->warmup = g_threadContext.warmup;
        const auto& roots = m_impl->state->config.traceRootScopes;
        m_impl->traceSelected = roots.empty()
            || (m_impl->previousScope && m_impl->previousScope->m_impl
                ? m_impl->previousScope->m_impl->traceSelected : g_threadContext.traceSelected)
            || std::ranges::find(roots, callsite.name) != roots.end();
        m_impl->eventId = m_impl->state->NextEventId();
        m_impl->threadId = ThreadId();
        m_impl->startNs = NowNs();
        if (m_impl->state->config.measureThreadCpuTime) {
            m_impl->startThreadCpuNs = CurrentThreadCpuTimeNs();
        }
        g_threadContext.currentScope = this;
    }
    catch (...) {
        if (m_impl) {
            m_impl->~Impl();
            m_impl = nullptr;
        }
    }
}

Scope::~Scope()
{
    if (!m_impl) {
        return;
    }
    const auto endNs = NowNs();
    const auto inclusiveNs = endNs - m_impl->startNs;
    const auto selfNs = inclusiveNs >= m_impl->childNs ? inclusiveNs - m_impl->childNs : 0;
    const auto endThreadCpuNs = m_impl->state->config.measureThreadCpuTime
        ? CurrentThreadCpuTimeNs()
        : 0;
    const auto threadCpuNs = endThreadCpuNs >= m_impl->startThreadCpuNs
        ? endThreadCpuNs - m_impl->startThreadCpuNs
        : 0;
    const auto nonRunningNs = inclusiveNs >= threadCpuNs ? inclusiveNs - threadCpuNs : 0;
    g_threadContext.currentScope = m_impl->previousScope;
    if (m_impl->previousScope && m_impl->previousScope->m_impl) {
        m_impl->previousScope->m_impl->childNs += inclusiveNs;
    }

    try {
        auto* shard = m_impl->state->ThreadLocalShard();
        {
            std::scoped_lock shardLock(shard->chunkMutex);
            shard->definitions.try_emplace(m_impl->callsite->id, m_impl->callsite);
            if (!m_impl->warmup) {
                auto& aggregate = shard->scopeAggregates[m_impl->callsite->id];
                aggregate.inclusive.Record(
                    inclusiveNs,
                    m_impl->state->config.retainedSamplesPerMetric);
                aggregate.self.Record(
                    selfNs,
                    m_impl->state->config.retainedSamplesPerMetric);
                if (m_impl->state->config.measureThreadCpuTime) {
                    aggregate.threadCpu.Record(
                        threadCpuNs,
                        m_impl->state->config.retainedSamplesPerMetric);
                    aggregate.nonRunning.Record(
                        nonRunningNs,
                        m_impl->state->config.retainedSamplesPerMetric);
                }
            }
        }
        if (!m_impl->warmup && !m_impl->state->samplingTargets.empty()) {
            std::scoped_lock lock(m_impl->state->mutex);
            for (auto& target : m_impl->state->samplingTargets) {
                if (target.config.name == m_impl->callsite->name
                    && (target.config.category.empty() || target.config.category == m_impl->callsite->category)) {
                    target.sampler.AddSample(static_cast<double>(inclusiveNs));
                }
            }
        }
        const bool retainStall = m_impl->state->config.retainStallEvents
            && nonRunningNs >= m_impl->state->config.stallEventThresholdNs;
        if (m_impl->traceSelected && (m_impl->state->config.mode == CaptureMode::Trace || retainStall)) {
            const auto maximumEvents = m_impl->state->config.mode == CaptureMode::Trace
                ? m_impl->state->config.maximumTraceEvents
                : m_impl->state->config.maximumStallEvents;
            TelemetryState::TraceEventRecord event{
                .callsite = m_impl->callsite,
                .eventId = m_impl->eventId,
                .parentEventId = m_impl->parentEventId,
                .frameId = m_impl->frameId,
                .threadId = m_impl->threadId,
                .startNs = m_impl->startNs - m_impl->state->startedAtNs,
                .inclusiveNs = inclusiveNs,
                .selfNs = selfNs,
                .threadCpuNs = threadCpuNs,
                .nonRunningNs = nonRunningNs,
                .value = m_impl->value,
                .textSize = m_impl->textSize,
                .hasValue = m_impl->hasValue,
            };
            if (m_impl->textSize != 0)
                std::memcpy(event.text.data(), m_impl->text.data(), m_impl->textSize);
            m_impl->state->AppendTraceEvent(std::move(event), maximumEvents);
        }
    }
    catch (...) {
    }
    m_impl->~Impl();
    m_impl = nullptr;
}

void Scope::Text(std::string_view text)
{
    if (m_impl && (m_impl->state->config.mode == CaptureMode::Trace
        || m_impl->state->config.retainStallEvents)) {
        m_impl->textSize = static_cast<std::uint16_t>(
            std::min(text.size(), m_impl->text.size()));
        if (m_impl->textSize != 0)
            std::memcpy(m_impl->text.data(), text.data(), m_impl->textSize);
    }
}

void Scope::Value(std::uint64_t value) noexcept
{
    if (m_impl) {
        m_impl->value = value;
        m_impl->hasValue = true;
    }
}

ContextToken Scope::CaptureContext() const noexcept
{
    ContextToken token;
    if (m_impl) {
        token.m_session = m_impl->state;
        token.m_parentEventId = m_impl->eventId;
        token.m_frameId = m_impl->frameId;
        token.m_traceSelected = m_impl->traceSelected;
    }
    return token;
}

bool Scope::IsActive() const noexcept
{
    return static_cast<bool>(m_impl);
}

StableId Scope::CallsiteId() const noexcept
{
    return m_impl ? m_impl->callsite->id : 0;
}

std::uint64_t Scope::EventId() const noexcept
{
    return m_impl ? m_impl->eventId : 0;
}

std::uint64_t Scope::FrameId() const noexcept
{
    return m_impl ? m_impl->frameId : 0;
}

struct Frame::Impl
{
    std::shared_ptr<TelemetryState> state;
    FrameSnapshot snapshot;
    std::uint64_t previousFrameId{};
    bool previousWarmup{};
};

Frame::Frame(std::string_view name, bool warmup)
{
    auto state = CurrentState();
    if (!state || !state->accepting.load(std::memory_order_acquire)) {
        return;
    }
    static_assert(sizeof(Impl) <= sizeof(m_storage));
    m_impl = ::new (static_cast<void*>(m_storage)) Impl;
    m_impl->state = std::move(state);
    m_impl->snapshot.id = m_impl->state->nextFrameId.fetch_add(1, std::memory_order_relaxed);
    m_impl->snapshot.name = name;
    m_impl->snapshot.warmup = warmup;
    m_impl->snapshot.startNs = NowNs();
    m_impl->previousFrameId = g_threadContext.frameId;
    m_impl->previousWarmup = g_threadContext.warmup;
    g_threadContext.frameId = m_impl->snapshot.id;
    g_threadContext.warmup = warmup;
}

Frame::~Frame()
{
    if (!m_impl) {
        return;
    }
    m_impl->snapshot.durationNs = NowNs() - m_impl->snapshot.startNs;
    m_impl->snapshot.startNs -= m_impl->state->startedAtNs;
    g_threadContext.frameId = m_impl->previousFrameId;
    g_threadContext.warmup = m_impl->previousWarmup;
    std::scoped_lock lock(m_impl->state->mutex);
    m_impl->state->frames.push_back(std::move(m_impl->snapshot));
    m_impl->~Impl();
    m_impl = nullptr;
}

void Frame::SetDimension(std::string_view name, std::int64_t value)
{
    if (m_impl) {
        m_impl->snapshot.dimensions[std::string(name)] = value;
    }
}

std::uint64_t Frame::Id() const noexcept
{
    return m_impl ? m_impl->snapshot.id : 0;
}

Counter::Counter(std::string_view name)
    : m_id(MakeStableId(name))
    , m_name(name)
{
}

void Counter::Add(std::int64_t delta) const noexcept
{
    if (auto state = CurrentState(); state && state->accepting.load(std::memory_order_acquire)) {
        try {
            std::scoped_lock lock(state->mutex);
            auto& metric = FindMetric(*state, m_id, m_name, "counter");
            metric.current += delta;
            metric.maximum = std::max(metric.maximum, metric.current);
        }
        catch (...) {
        }
    }
}

void Counter::Set(std::int64_t value) const noexcept
{
    if (auto state = CurrentState(); state && state->accepting.load(std::memory_order_acquire)) {
        try {
            std::scoped_lock lock(state->mutex);
            auto& metric = FindMetric(*state, m_id, m_name, "counter");
            metric.current = value;
            metric.maximum = std::max(metric.maximum, value);
        }
        catch (...) {
        }
    }
}

Gauge::Gauge(std::string_view name)
    : m_id(MakeStableId(name))
    , m_name(name)
{
}

void Gauge::Set(std::int64_t value) const noexcept
{
    if (auto state = CurrentState(); state && state->accepting.load(std::memory_order_acquire)) {
        try {
            std::scoped_lock lock(state->mutex);
            auto& metric = FindMetric(*state, m_id, m_name, "gauge");
            metric.current = value;
            metric.maximum = std::max(metric.maximum, value);
            metric.distribution.Record(
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, value)),
                state->config.retainedSamplesPerMetric);
        }
        catch (...) {
        }
    }
}

void Gauge::Max(std::int64_t value) const noexcept
{
    if (auto state = CurrentState(); state && state->accepting.load(std::memory_order_acquire)) {
        try {
            std::scoped_lock lock(state->mutex);
            auto& metric = FindMetric(*state, m_id, m_name, "gauge");
            metric.maximum = std::max(metric.maximum, value);
            metric.current = metric.maximum;
        }
        catch (...) {
        }
    }
}

Distribution::Distribution(std::string_view name)
    : m_id(MakeStableId(name))
    , m_name(name)
{
}

void Distribution::Record(std::uint64_t value) const noexcept
{
    if (auto state = CurrentState(); state && state->accepting.load(std::memory_order_acquire)) {
        try {
            std::scoped_lock lock(state->mutex);
            auto& metric = FindMetric(*state, m_id, m_name, "distribution");
            metric.distribution.Record(value, state->config.retainedSamplesPerMetric);
        }
        catch (...) {
        }
    }
}

bool Enabled() noexcept
{
    const auto state = CurrentState();
    return state && state->accepting.load(std::memory_order_relaxed);
}

CaptureMode CurrentMode() noexcept
{
    const auto state = CurrentState();
    return state ? state->config.mode : CaptureMode::Off;
}

std::uint64_t NowNs() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint64_t CurrentThreadCpuTimeNs() noexcept
{
#if defined(_WIN32)
    // GetThreadTimes advances in scheduler quanta (15.6 ms), which makes the
    // per-scope non-running time meaningless for scopes shorter than that.
    // QueryThreadCycleTime is exact; cycles are converted with a ratio measured
    // once while this thread is known to be running.
    static const double nsPerCycle = [] {
        ULONG64 cyclesBegin = 0, cyclesEnd = 0;
        LARGE_INTEGER frequency{}, begin{}, now{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);
        QueryThreadCycleTime(GetCurrentThread(), &cyclesBegin);
        do { QueryPerformanceCounter(&now); } while ((now.QuadPart - begin.QuadPart) * 1000 < frequency.QuadPart * 4); // ~4 ms
        QueryThreadCycleTime(GetCurrentThread(), &cyclesEnd);
        const double elapsedNs = static_cast<double>(now.QuadPart - begin.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart);
        const double cycles = static_cast<double>(cyclesEnd - cyclesBegin);
        return cycles > 0 ? elapsedNs / cycles : 0.0;
    }();
    ULONG64 cycles = 0;
    if (nsPerCycle <= 0 || !QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        return 0;
    }
    return static_cast<std::uint64_t>(static_cast<double>(cycles) * nsPerCycle);
#elif defined(__linux__)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ull
        + static_cast<std::uint64_t>(value.tv_nsec);
#else
    return 0;
#endif
}

ContextToken CaptureCurrentContext() noexcept
{
    if (g_threadContext.currentScope) {
        return g_threadContext.currentScope->CaptureContext();
    }
    ContextToken token;
    if (auto state = CurrentState()) {
        token.m_session = std::move(state);
        token.m_parentEventId = g_threadContext.parentEventId;
        token.m_frameId = g_threadContext.frameId;
        token.m_traceSelected = g_threadContext.traceSelected;
    }
    return token;
}

void AnnotateCurrentScope(std::string_view text)
{
    if (g_threadContext.currentScope) {
        g_threadContext.currentScope->Text(text);
    }
}

void SetCurrentScopeValue(std::uint64_t value) noexcept
{
    if (g_threadContext.currentScope) {
        g_threadContext.currentScope->Value(value);
    }
}

void Record(std::string_view name, std::uint64_t value) noexcept
{
    Distribution(name).Record(value);
}

void AddCounter(std::string_view name, std::int64_t delta) noexcept
{
    Counter(name).Add(delta);
}

void SetGauge(std::string_view name, std::int64_t value) noexcept
{
    Gauge(name).Set(value);
}

void MaxGauge(std::string_view name, std::int64_t value) noexcept
{
    Gauge(name).Max(value);
}

AllocationToken RecordAllocation(std::size_t size, AllocationDomain domain) noexcept
{
    AllocationToken token;
    if (!g_threadContext.currentScope ||
        !g_allocationTrackingActive.load(std::memory_order_acquire)) {
        return token;
    }
    auto state = CurrentState();
    if (!state || !state->accepting.load(std::memory_order_acquire) ||
        !state->config.trackAllocations || !g_threadContext.currentScope ||
        !g_threadContext.currentScope->IsActive()) {
        return token;
    }

    try {
        token.id = state->nextAllocationId.fetch_add(1, std::memory_order_relaxed);
        token.sessionId = state->sessionId;
        token.size = static_cast<std::uint64_t>(size);
        token.domain = domain;
        token.allocatedAtNs = NowNs();
        token.ownerEventId = g_threadContext.currentScope->EventId();
        token.ownerFrameId = g_threadContext.currentScope->FrameId();
        token.ownerScopeId = g_threadContext.currentScope->CallsiteId();

        std::scoped_lock lock(state->mutex);
        auto& aggregate = state->scopeAggregates[token.ownerScopeId];
        ++aggregate.allocatedCount;
        aggregate.allocatedBytes += token.size;
        aggregate.liveBytes += token.size;
        aggregate.peakLiveBytes = std::max(aggregate.peakLiveBytes, aggregate.liveBytes);
        aggregate.largestAllocation = std::max(aggregate.largestAllocation, token.size);
        ++aggregate.stillLive;
        if (state->config.mode == CaptureMode::Trace &&
            state->allocations.size() < state->config.maximumTraceEvents) {
            state->allocationEventIndices.emplace(token.id, state->allocations.size());
            state->allocations.push_back({
                .allocationId = token.id,
                .domain = domain,
                .ownerScopeId = token.ownerScopeId,
                .ownerEventId = token.ownerEventId,
                .ownerFrameId = token.ownerFrameId,
                .size = token.size,
                .allocatedAtNs = token.allocatedAtNs - state->startedAtNs,
            });
        }
        return token;
    }
    catch (...) {
        return {};
    }
}

void RecordFree(AllocationToken token) noexcept
{
    if (!token) {
        return;
    }
    auto state = g_activeState.load(std::memory_order_acquire);
    if (!state || state->sessionId != token.sessionId ||
        !state->accepting.load(std::memory_order_acquire)) {
        return;
    }
    try {
        const auto freedAt = NowNs();
        const auto currentEvent = g_threadContext.currentScope &&
                g_threadContext.currentScope->IsActive()
            ? g_threadContext.currentScope->EventId()
            : 0;
        const auto currentFrame = g_threadContext.frameId;
        std::scoped_lock lock(state->mutex);
        auto& aggregate = state->scopeAggregates[token.ownerScopeId];
        ++aggregate.freedCount;
        aggregate.freedBytes += token.size;
        aggregate.liveBytes = aggregate.liveBytes >= token.size
            ? aggregate.liveBytes - token.size
            : 0;
        if (aggregate.stillLive > 0) {
            --aggregate.stillLive;
        }
        if (currentEvent == token.ownerEventId) {
            ++aggregate.freedInSameScope;
        }
        else if (currentFrame != 0 && currentFrame == token.ownerFrameId) {
            ++aggregate.freedInSameFrame;
        }
        else {
            ++aggregate.freedLater;
        }
        if (const auto it = state->allocationEventIndices.find(token.id);
            it != state->allocationEventIndices.end()) {
            auto& event = state->allocations[it->second];
            event.freedAtNs = freedAt - state->startedAtNs;
            event.freedScopeEventId = currentEvent;
            state->allocationEventIndices.erase(it);
        }
    }
    catch (...) {
    }
}

void RecordAllocationTrackingOverflow() noexcept
{
    if (auto state = CurrentState()) {
        std::scoped_lock lock(state->mutex);
        ++state->allocationTrackingOverflows;
    }
}

void RecordUnknownFree() noexcept
{
    if (auto state = CurrentState()) {
        std::scoped_lock lock(state->mutex);
        ++state->unknownFrees;
    }
}

struct TrackingMemoryResource::AllocationMap
{
    std::mutex mutex;
    std::unordered_map<void*, AllocationToken> tokens;
};

TrackingMemoryResource::TrackingMemoryResource(
    std::pmr::memory_resource* upstream,
    AllocationDomain domain)
    : m_upstream(upstream)
    , m_domain(domain)
    , m_allocations(std::make_unique<AllocationMap>())
{
}

TrackingMemoryResource::~TrackingMemoryResource() = default;

void* TrackingMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment)
{
    void* pointer = m_upstream->allocate(bytes, alignment);
    auto token = RecordAllocation(bytes, m_domain);
    if (token) {
        std::scoped_lock lock(m_allocations->mutex);
        m_allocations->tokens.emplace(pointer, std::move(token));
    }
    return pointer;
}

void TrackingMemoryResource::do_deallocate(
    void* pointer,
    std::size_t bytes,
    std::size_t alignment)
{
    AllocationToken token;
    {
        std::scoped_lock lock(m_allocations->mutex);
        if (const auto it = m_allocations->tokens.find(pointer);
            it != m_allocations->tokens.end()) {
            token = std::move(it->second);
            m_allocations->tokens.erase(it);
        }
    }
    RecordFree(std::move(token));
    m_upstream->deallocate(pointer, bytes, alignment);
}

bool TrackingMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept
{
    return this == &other;
}
}
