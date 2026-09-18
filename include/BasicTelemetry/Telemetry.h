#pragma once

#include <BasicTelemetry/Sampling.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace basic_telemetry
{
enum class CaptureMode : std::uint8_t
{
    Off,
    Summary,
    Trace
};

enum class AllocationDomain : std::uint8_t
{
    CpuGeneral,
    CpuCompile,
    Gpu,
    Custom
};

using StableId = std::uint64_t;

struct SessionConfig
{
    CaptureMode mode{ CaptureMode::Off };
    bool trackAllocations{ false };
    std::size_t retainedSamplesPerMetric{ 16'384 };
    std::size_t maximumTraceEvents{ 1'000'000 };
    bool measureThreadCpuTime{ false };
    bool retainStallEvents{ false };
    std::uint64_t stallEventThresholdNs{ 4'000'000 };
    std::size_t maximumStallEvents{ 100'000 };
    std::unordered_map<std::string, std::string> metadata;
    std::vector<SamplingTargetConfig> samplingTargets;
    // Empty retains every trace event. Otherwise retain only these scopes and
    // their descendants, including descendants reached through ContextToken.
    // Summary distributions remain complete and are not filtered.
    std::vector<std::string> traceRootScopes;
};

struct ScopeDefinition
{
    StableId id{};
    std::string name;
    std::string category;
    std::string file;
    std::string function;
    std::uint32_t line{};
};

struct DistributionSnapshot
{
    std::uint64_t count{};
    std::uint64_t total{};
    std::uint64_t minimum{};
    std::uint64_t maximum{};
    std::vector<std::uint64_t> retainedSamples;
};

struct ScopeAggregateSnapshot
{
    StableId scopeId{};
    DistributionSnapshot inclusiveNs;
    DistributionSnapshot selfNs;
    DistributionSnapshot threadCpuNs;
    DistributionSnapshot nonRunningNs;
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

struct ScopeEventSnapshot
{
    std::uint64_t eventId{};
    StableId scopeId{};
    std::uint64_t parentEventId{};
    std::uint64_t frameId{};
    std::uint64_t threadId{};
    std::uint64_t startNs{};
    std::uint64_t inclusiveNs{};
    std::uint64_t selfNs{};
    std::uint64_t threadCpuNs{};
    std::uint64_t nonRunningNs{};
    std::string text;
    std::uint64_t value{};
    bool hasValue{};
};

struct MetricSnapshot
{
    StableId id{};
    std::string name;
    std::string kind;
    std::int64_t current{};
    std::int64_t maximum{};
    DistributionSnapshot distribution;
};

struct FrameSnapshot
{
    std::uint64_t id{};
    std::string name;
    bool warmup{};
    std::uint64_t startNs{};
    std::uint64_t durationNs{};
    std::unordered_map<std::string, std::int64_t> dimensions;
};

struct AllocationEventSnapshot
{
    std::uint64_t allocationId{};
    AllocationDomain domain{};
    StableId ownerScopeId{};
    std::uint64_t ownerEventId{};
    std::uint64_t ownerFrameId{};
    std::uint64_t size{};
    std::uint64_t allocatedAtNs{};
    std::uint64_t freedAtNs{};
    std::uint64_t freedScopeEventId{};
};

struct SessionSnapshot
{
    std::uint32_t schemaVersion{ 2 };
    CaptureMode mode{ CaptureMode::Off };
    bool allocationTrackingEnabled{};
    std::uint64_t startedAtNs{};
    std::uint64_t endedAtNs{};
    std::uint64_t droppedEvents{};
    std::uint64_t droppedContendedScopeSamples{};
    std::uint64_t allocationTrackingOverflows{};
    std::uint64_t unknownFrees{};
    std::unordered_map<std::string, std::string> metadata;
    std::vector<ScopeDefinition> scopeDefinitions;
    std::vector<ScopeAggregateSnapshot> scopes;
    std::vector<ScopeEventSnapshot> events;
    std::vector<MetricSnapshot> metrics;
    std::vector<FrameSnapshot> frames;
    std::vector<AllocationEventSnapshot> allocations;
    std::vector<SamplingSummary> sampling;
};

class Sink
{
public:
    virtual ~Sink() = default;
    virtual void OnSessionEnd(const SessionSnapshot& snapshot) = 0;
    [[nodiscard]] virtual std::string Error() const { return {}; }
};

class Session
{
public:
    explicit Session(SessionConfig config, std::vector<std::shared_ptr<Sink>> sinks = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] SessionSnapshot Snapshot() const;
    [[nodiscard]] std::vector<SamplingSummary> SamplingStatus() const;
    [[nodiscard]] bool SamplingConverged() const;
    [[nodiscard]] bool SamplingComplete() const;
    void Flush();

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

struct Callsite
{
    StableId id{};
    std::string_view name;
    std::string_view category;
    std::source_location location;

    explicit Callsite(
        std::string_view scopeName,
        std::string_view scopeCategory = {},
        std::source_location source = std::source_location::current()) noexcept;
};

class ContextToken
{
public:
    ContextToken() = default;
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend class Scope;
    friend class ContextBinding;
    friend ContextToken CaptureCurrentContext() noexcept;
    std::shared_ptr<void> m_session;
    std::uint64_t m_parentEventId{};
    std::uint64_t m_frameId{};
    bool m_traceSelected{};
};

class ContextBinding
{
public:
    explicit ContextBinding(const ContextToken& token) noexcept;
    ~ContextBinding();
    ContextBinding(const ContextBinding&) = delete;
    ContextBinding& operator=(const ContextBinding&) = delete;

private:
    struct State;
    alignas(std::max_align_t) std::byte m_storage[96]{};
    State* m_state{};
};

class Scope
{
public:
    explicit Scope(const Callsite& callsite) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void Text(std::string_view text);
    void Value(std::uint64_t value) noexcept;
    [[nodiscard]] ContextToken CaptureContext() const noexcept;
    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] StableId CallsiteId() const noexcept;
    [[nodiscard]] std::uint64_t EventId() const noexcept;
    [[nodiscard]] std::uint64_t FrameId() const noexcept;

private:
    struct Impl;
    alignas(std::max_align_t) std::byte m_storage[256]{};
    Impl* m_impl{};
};

class Frame
{
public:
    Frame(std::string_view name, bool warmup = false);
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    void SetDimension(std::string_view name, std::int64_t value);
    [[nodiscard]] std::uint64_t Id() const noexcept;

private:
    struct Impl;
    alignas(std::max_align_t) std::byte m_storage[256]{};
    Impl* m_impl{};
};

class Counter
{
public:
    explicit Counter(std::string_view name);
    void Add(std::int64_t delta = 1) const noexcept;
    void Set(std::int64_t value) const noexcept;
private:
    StableId m_id{};
    std::string m_name;
};

class Gauge
{
public:
    explicit Gauge(std::string_view name);
    void Set(std::int64_t value) const noexcept;
    void Max(std::int64_t value) const noexcept;
private:
    StableId m_id{};
    std::string m_name;
};

class Distribution
{
public:
    explicit Distribution(std::string_view name);
    void Record(std::uint64_t value) const noexcept;
private:
    StableId m_id{};
    std::string m_name;
};

[[nodiscard]] bool Enabled() noexcept;
[[nodiscard]] CaptureMode CurrentMode() noexcept;
[[nodiscard]] std::uint64_t NowNs() noexcept;
[[nodiscard]] std::uint64_t CurrentThreadCpuTimeNs() noexcept;
[[nodiscard]] ContextToken CaptureCurrentContext() noexcept;
void AnnotateCurrentScope(std::string_view text);
void SetCurrentScopeValue(std::uint64_t value) noexcept;
void Record(std::string_view name, std::uint64_t value) noexcept;
void AddCounter(std::string_view name, std::int64_t delta = 1) noexcept;
void SetGauge(std::string_view name, std::int64_t value) noexcept;
void MaxGauge(std::string_view name, std::int64_t value) noexcept;

struct AllocationToken
{
    std::uint64_t sessionId{};
    std::uint64_t id{};
    StableId ownerScopeId{};
    std::uint64_t ownerEventId{};
    std::uint64_t ownerFrameId{};
    std::uint64_t size{};
    std::uint64_t allocatedAtNs{};
    AllocationDomain domain{ AllocationDomain::CpuGeneral };
    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

[[nodiscard]] AllocationToken RecordAllocation(
    std::size_t size,
    AllocationDomain domain = AllocationDomain::CpuGeneral) noexcept;
void RecordFree(AllocationToken token) noexcept;
void RecordAllocationTrackingOverflow() noexcept;
void RecordUnknownFree() noexcept;

class TrackingMemoryResource final : public std::pmr::memory_resource
{
public:
    explicit TrackingMemoryResource(
        std::pmr::memory_resource* upstream = std::pmr::get_default_resource(),
        AllocationDomain domain = AllocationDomain::CpuGeneral);
    ~TrackingMemoryResource() override;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    std::pmr::memory_resource* m_upstream;
    AllocationDomain m_domain;
    struct AllocationMap;
    std::unique_ptr<AllocationMap> m_allocations;
};
}

#define BASIC_TELEMETRY_JOIN_IMPL(a, b) a##b
#define BASIC_TELEMETRY_JOIN(a, b) BASIC_TELEMETRY_JOIN_IMPL(a, b)
#define BASIC_TELEMETRY_SCOPE(name) \
    static const ::basic_telemetry::Callsite BASIC_TELEMETRY_JOIN(_btCallsite, __LINE__){ name }; \
    ::basic_telemetry::Scope BASIC_TELEMETRY_JOIN(_btScope, __LINE__){ BASIC_TELEMETRY_JOIN(_btCallsite, __LINE__) }
#define BASIC_TELEMETRY_SCOPE_CATEGORY(name, category) \
    static const ::basic_telemetry::Callsite BASIC_TELEMETRY_JOIN(_btCallsite, __LINE__){ name, category }; \
    ::basic_telemetry::Scope BASIC_TELEMETRY_JOIN(_btScope, __LINE__){ BASIC_TELEMETRY_JOIN(_btCallsite, __LINE__) }
#define BASIC_TELEMETRY_TEXT(text) ::basic_telemetry::AnnotateCurrentScope(text)
#define BASIC_TELEMETRY_VALUE(value) ::basic_telemetry::SetCurrentScopeValue(static_cast<std::uint64_t>(value))
