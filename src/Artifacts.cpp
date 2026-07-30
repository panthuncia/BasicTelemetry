#include <BasicTelemetry/Artifacts.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace basic_telemetry
{
namespace
{
using json = nlohmann::json;

const char* ModeName(CaptureMode mode)
{
    switch (mode) {
    case CaptureMode::Off:
        return "off";
    case CaptureMode::Summary:
        return "summary";
    case CaptureMode::Trace:
        return "trace";
    }
    return "unknown";
}

const char* DomainName(AllocationDomain domain)
{
    switch (domain) {
    case AllocationDomain::CpuGeneral:
        return "cpu_general";
    case AllocationDomain::CpuCompile:
        return "cpu_compile";
    case AllocationDomain::Gpu:
        return "gpu";
    case AllocationDomain::Custom:
        return "custom";
    }
    return "unknown";
}

std::uint64_t Percentile(const DistributionSnapshot& distribution, double percentile)
{
    if (distribution.retainedSamples.empty()) {
        return 0;
    }
    auto samples = distribution.retainedSamples;
    std::ranges::sort(samples);
    const auto scaled = percentile * static_cast<double>(samples.size() - 1);
    return samples[static_cast<std::size_t>(scaled + 0.5)];
}

json DistributionJson(const DistributionSnapshot& distribution)
{
    return {
        { "count", distribution.count },
        { "total", distribution.total },
        { "mean", distribution.count == 0
                ? 0.0
                : static_cast<double>(distribution.total) /
                    static_cast<double>(distribution.count) },
        { "min", distribution.minimum },
        { "median", Percentile(distribution, 0.50) },
        { "p95", Percentile(distribution, 0.95) },
        { "p99", Percentile(distribution, 0.99) },
        { "max", distribution.maximum },
        { "retained_samples", distribution.retainedSamples.size() },
    };
}

bool WriteJson(const std::filesystem::path& path, const json& value, std::string& error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "failed to open " + path.string();
        return false;
    }
    output << std::setw(2) << value << '\n';
    if (!output) {
        error = "failed to write " + path.string();
        return false;
    }
    return true;
}

bool Execute(sqlite3* database, const char* sql, std::string& error)
{
    char* sqliteError = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &sqliteError) != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(database);
        sqlite3_free(sqliteError);
        return false;
    }
    return true;
}

class Statement
{
public:
    Statement(sqlite3* database, const char* sql, std::string& error)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &m_statement, nullptr) != SQLITE_OK) {
            error = sqlite3_errmsg(database);
        }
    }

    ~Statement()
    {
        sqlite3_finalize(m_statement);
    }

    [[nodiscard]] sqlite3_stmt* Get() const { return m_statement; }
    [[nodiscard]] explicit operator bool() const { return m_statement != nullptr; }

private:
    sqlite3_stmt* m_statement{};
};

void BindText(sqlite3_stmt* statement, int index, std::string_view value)
{
    sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
}

bool StepAndReset(sqlite3_stmt* statement, std::string& error)
{
    if (sqlite3_step(statement) != SQLITE_DONE) {
        error = sqlite3_errmsg(sqlite3_db_handle(statement));
        return false;
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return true;
}

bool WriteSqlite(
    const SessionSnapshot& snapshot,
    const ArtifactOptions& options,
    const std::unordered_map<StableId, ScopeDefinition>& definitions,
    std::string& error)
{
    const auto path = options.outputDirectory / "profile.sqlite";
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    sqlite3* rawDatabase{};
    if (sqlite3_open(path.string().c_str(), &rawDatabase) != SQLITE_OK) {
        error = rawDatabase ? sqlite3_errmsg(rawDatabase) : "failed to open SQLite database";
        sqlite3_close(rawDatabase);
        return false;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(rawDatabase, sqlite3_close);

    constexpr const char* schema = R"SQL(
PRAGMA journal_mode = OFF;
PRAGMA synchronous = OFF;
BEGIN;
CREATE TABLE session (
    schema_version INTEGER NOT NULL,
    mode TEXT NOT NULL,
    started_ns INTEGER NOT NULL,
    ended_ns INTEGER NOT NULL,
    dropped_events INTEGER NOT NULL,
    allocation_overflows INTEGER NOT NULL,
    unknown_frees INTEGER NOT NULL
);
CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE scope_definitions (
    scope_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    category TEXT NOT NULL,
    file TEXT NOT NULL,
    function TEXT NOT NULL,
    line INTEGER NOT NULL
);
CREATE TABLE scope_summary (
    scope_id INTEGER PRIMARY KEY,
    count INTEGER NOT NULL,
    inclusive_total_ns INTEGER NOT NULL,
    inclusive_mean_ns REAL NOT NULL,
    inclusive_median_ns INTEGER NOT NULL,
    inclusive_p95_ns INTEGER NOT NULL,
    inclusive_p99_ns INTEGER NOT NULL,
    inclusive_max_ns INTEGER NOT NULL,
    self_total_ns INTEGER NOT NULL,
    self_mean_ns REAL NOT NULL,
    self_median_ns INTEGER NOT NULL,
    self_p95_ns INTEGER NOT NULL,
    self_p99_ns INTEGER NOT NULL,
    self_max_ns INTEGER NOT NULL,
    allocated_count INTEGER NOT NULL,
    allocated_bytes INTEGER NOT NULL,
    freed_count INTEGER NOT NULL,
    freed_bytes INTEGER NOT NULL,
    live_bytes INTEGER NOT NULL,
    peak_live_bytes INTEGER NOT NULL,
    largest_allocation INTEGER NOT NULL,
    FOREIGN KEY(scope_id) REFERENCES scope_definitions(scope_id)
);
CREATE TABLE frames (
    frame_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    warmup INTEGER NOT NULL,
    start_ns INTEGER NOT NULL,
    duration_ns INTEGER NOT NULL,
    dimensions_json TEXT NOT NULL
);
CREATE TABLE scope_events (
    event_id INTEGER PRIMARY KEY,
    scope_id INTEGER NOT NULL,
    parent_event_id INTEGER NOT NULL,
    frame_id INTEGER NOT NULL,
    thread_id INTEGER NOT NULL,
    start_ns INTEGER NOT NULL,
    inclusive_ns INTEGER NOT NULL,
    self_ns INTEGER NOT NULL,
    text TEXT NOT NULL,
    value INTEGER,
    FOREIGN KEY(scope_id) REFERENCES scope_definitions(scope_id)
);
CREATE TABLE metric_samples (
    metric_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    kind TEXT NOT NULL,
    current_value INTEGER NOT NULL,
    maximum_value INTEGER NOT NULL,
    count INTEGER NOT NULL,
    total INTEGER NOT NULL,
    median INTEGER NOT NULL,
    p95 INTEGER NOT NULL,
    p99 INTEGER NOT NULL
);
CREATE TABLE allocations (
    allocation_id INTEGER PRIMARY KEY,
    domain TEXT NOT NULL,
    owner_scope_id INTEGER NOT NULL,
    owner_event_id INTEGER NOT NULL,
    owner_frame_id INTEGER NOT NULL,
    size INTEGER NOT NULL,
    allocated_at_ns INTEGER NOT NULL,
    freed_at_ns INTEGER NOT NULL,
    freed_scope_event_id INTEGER NOT NULL
);
CREATE TABLE sampling_summary (
    name TEXT NOT NULL,
    category TEXT NOT NULL,
    raw_sample_count INTEGER NOT NULL,
    minimum_samples INTEGER NOT NULL,
    maximum_samples INTEGER NOT NULL,
    confidence_level REAL NOT NULL,
    target_relative_half_width REAL NOT NULL,
    target_absolute_half_width REAL,
    effective_sample_count REAL NOT NULL,
    mean REAL NOT NULL,
    standard_deviation REAL NOT NULL,
    coefficient_of_variation REAL NOT NULL,
    median REAL NOT NULL,
    p05 REAL NOT NULL,
    p95 REAL NOT NULL,
    confidence_low REAL NOT NULL,
    confidence_high REAL NOT NULL,
    relative_half_width REAL NOT NULL,
    lag1_autocorrelation REAL NOT NULL,
    mad_outlier_count INTEGER NOT NULL,
    converged INTEGER NOT NULL,
    reached_maximum INTEGER NOT NULL
);
CREATE VIEW timing_hotspots AS
SELECT d.name, d.category, s.*
FROM scope_summary s JOIN scope_definitions d USING(scope_id)
ORDER BY s.self_total_ns DESC;
CREATE VIEW allocation_hotspots AS
SELECT d.name, d.category, s.scope_id, s.allocated_count, s.allocated_bytes,
       s.peak_live_bytes, s.live_bytes, s.largest_allocation
FROM scope_summary s JOIN scope_definitions d USING(scope_id)
ORDER BY s.allocated_bytes DESC;
CREATE VIEW worst_frames AS
SELECT * FROM frames WHERE warmup = 0 ORDER BY duration_ns DESC;
)SQL";
    if (!Execute(database.get(), schema, error)) {
        return false;
    }

    Statement sessionStatement(
        database.get(),
        "INSERT INTO session VALUES(?,?,?,?,?,?,?)",
        error);
    if (!sessionStatement) {
        return false;
    }
    sqlite3_bind_int(sessionStatement.Get(), 1, snapshot.schemaVersion);
    BindText(sessionStatement.Get(), 2, ModeName(snapshot.mode));
    sqlite3_bind_int64(sessionStatement.Get(), 3, snapshot.startedAtNs);
    sqlite3_bind_int64(sessionStatement.Get(), 4, snapshot.endedAtNs);
    sqlite3_bind_int64(sessionStatement.Get(), 5, snapshot.droppedEvents);
    sqlite3_bind_int64(sessionStatement.Get(), 6, snapshot.allocationTrackingOverflows);
    sqlite3_bind_int64(sessionStatement.Get(), 7, snapshot.unknownFrees);
    if (!StepAndReset(sessionStatement.Get(), error)) {
        return false;
    }

    Statement metadataStatement(
        database.get(),
        "INSERT INTO metadata VALUES(?,?)",
        error);
    for (const auto& [key, value] : snapshot.metadata) {
        BindText(metadataStatement.Get(), 1, key);
        BindText(metadataStatement.Get(), 2, value);
        if (!StepAndReset(metadataStatement.Get(), error)) {
            return false;
        }
    }

    Statement definitionStatement(
        database.get(),
        "INSERT INTO scope_definitions VALUES(?,?,?,?,?,?)",
        error);
    for (const auto& definition : snapshot.scopeDefinitions) {
        sqlite3_bind_int64(definitionStatement.Get(), 1, definition.id);
        BindText(definitionStatement.Get(), 2, definition.name);
        BindText(definitionStatement.Get(), 3, definition.category);
        BindText(definitionStatement.Get(), 4, definition.file);
        BindText(definitionStatement.Get(), 5, definition.function);
        sqlite3_bind_int(definitionStatement.Get(), 6, definition.line);
        if (!StepAndReset(definitionStatement.Get(), error)) {
            return false;
        }
    }

    Statement scopeStatement(
        database.get(),
        "INSERT INTO scope_summary VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        error);
    for (const auto& scope : snapshot.scopes) {
        const auto inclusiveMean = scope.inclusiveNs.count == 0
            ? 0.0
            : static_cast<double>(scope.inclusiveNs.total) / scope.inclusiveNs.count;
        const auto selfMean = scope.selfNs.count == 0
            ? 0.0
            : static_cast<double>(scope.selfNs.total) / scope.selfNs.count;
        int column = 1;
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.scopeId);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.inclusiveNs.count);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.inclusiveNs.total);
        sqlite3_bind_double(scopeStatement.Get(), column++, inclusiveMean);
        sqlite3_bind_int64(scopeStatement.Get(), column++, Percentile(scope.inclusiveNs, 0.50));
        sqlite3_bind_int64(scopeStatement.Get(), column++, Percentile(scope.inclusiveNs, 0.95));
        sqlite3_bind_int64(scopeStatement.Get(), column++, Percentile(scope.inclusiveNs, 0.99));
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.inclusiveNs.maximum);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.selfNs.total);
        sqlite3_bind_double(scopeStatement.Get(), column++, selfMean);
        sqlite3_bind_int64(scopeStatement.Get(), column++, Percentile(scope.selfNs, 0.50));
        sqlite3_bind_int64(scopeStatement.Get(), column++, Percentile(scope.selfNs, 0.95));
        sqlite3_bind_int64(scopeStatement.Get(), column++, Percentile(scope.selfNs, 0.99));
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.selfNs.maximum);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.allocatedCount);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.allocatedBytes);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.freedCount);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.freedBytes);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.liveBytes);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.peakLiveBytes);
        sqlite3_bind_int64(scopeStatement.Get(), column++, scope.largestAllocation);
        if (!StepAndReset(scopeStatement.Get(), error)) {
            return false;
        }
    }

    Statement frameStatement(
        database.get(),
        "INSERT INTO frames VALUES(?,?,?,?,?,?)",
        error);
    for (const auto& frame : snapshot.frames) {
        sqlite3_bind_int64(frameStatement.Get(), 1, frame.id);
        BindText(frameStatement.Get(), 2, frame.name);
        sqlite3_bind_int(frameStatement.Get(), 3, frame.warmup ? 1 : 0);
        sqlite3_bind_int64(frameStatement.Get(), 4, frame.startNs);
        sqlite3_bind_int64(frameStatement.Get(), 5, frame.durationNs);
        BindText(frameStatement.Get(), 6, json(frame.dimensions).dump());
        if (!StepAndReset(frameStatement.Get(), error)) {
            return false;
        }
    }

    Statement eventStatement(
        database.get(),
        "INSERT INTO scope_events VALUES(?,?,?,?,?,?,?,?,?,?)",
        error);
    for (const auto& event : snapshot.events) {
        sqlite3_bind_int64(eventStatement.Get(), 1, event.eventId);
        sqlite3_bind_int64(eventStatement.Get(), 2, event.scopeId);
        sqlite3_bind_int64(eventStatement.Get(), 3, event.parentEventId);
        sqlite3_bind_int64(eventStatement.Get(), 4, event.frameId);
        sqlite3_bind_int64(eventStatement.Get(), 5, event.threadId);
        sqlite3_bind_int64(eventStatement.Get(), 6, event.startNs);
        sqlite3_bind_int64(eventStatement.Get(), 7, event.inclusiveNs);
        sqlite3_bind_int64(eventStatement.Get(), 8, event.selfNs);
        BindText(eventStatement.Get(), 9, event.text);
        if (event.hasValue) {
            sqlite3_bind_int64(eventStatement.Get(), 10, event.value);
        }
        else {
            sqlite3_bind_null(eventStatement.Get(), 10);
        }
        if (!StepAndReset(eventStatement.Get(), error)) {
            return false;
        }
    }

    Statement metricStatement(
        database.get(),
        "INSERT INTO metric_samples VALUES(?,?,?,?,?,?,?,?,?,?)",
        error);
    for (const auto& metric : snapshot.metrics) {
        sqlite3_bind_int64(metricStatement.Get(), 1, metric.id);
        BindText(metricStatement.Get(), 2, metric.name);
        BindText(metricStatement.Get(), 3, metric.kind);
        sqlite3_bind_int64(metricStatement.Get(), 4, metric.current);
        sqlite3_bind_int64(metricStatement.Get(), 5, metric.maximum);
        sqlite3_bind_int64(metricStatement.Get(), 6, metric.distribution.count);
        sqlite3_bind_int64(metricStatement.Get(), 7, metric.distribution.total);
        sqlite3_bind_int64(metricStatement.Get(), 8, Percentile(metric.distribution, 0.50));
        sqlite3_bind_int64(metricStatement.Get(), 9, Percentile(metric.distribution, 0.95));
        sqlite3_bind_int64(metricStatement.Get(), 10, Percentile(metric.distribution, 0.99));
        if (!StepAndReset(metricStatement.Get(), error)) {
            return false;
        }
    }

    Statement allocationStatement(
        database.get(),
        "INSERT INTO allocations VALUES(?,?,?,?,?,?,?,?,?)",
        error);
    for (const auto& allocation : snapshot.allocations) {
        sqlite3_bind_int64(allocationStatement.Get(), 1, allocation.allocationId);
        BindText(allocationStatement.Get(), 2, DomainName(allocation.domain));
        sqlite3_bind_int64(allocationStatement.Get(), 3, allocation.ownerScopeId);
        sqlite3_bind_int64(allocationStatement.Get(), 4, allocation.ownerEventId);
        sqlite3_bind_int64(allocationStatement.Get(), 5, allocation.ownerFrameId);
        sqlite3_bind_int64(allocationStatement.Get(), 6, allocation.size);
        sqlite3_bind_int64(allocationStatement.Get(), 7, allocation.allocatedAtNs);
        sqlite3_bind_int64(allocationStatement.Get(), 8, allocation.freedAtNs);
        sqlite3_bind_int64(allocationStatement.Get(), 9, allocation.freedScopeEventId);
        if (!StepAndReset(allocationStatement.Get(), error)) {
            return false;
        }
    }

    Statement samplingStatement(
        database.get(),
        "INSERT INTO sampling_summary VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        error);
    for (const auto& sampling : snapshot.sampling) {
        int column = 1;
        BindText(samplingStatement.Get(), column++, sampling.name);
        BindText(samplingStatement.Get(), column++, sampling.category);
        sqlite3_bind_int64(samplingStatement.Get(), column++, sampling.rawSampleCount);
        sqlite3_bind_int64(samplingStatement.Get(), column++, sampling.minimumSamples);
        sqlite3_bind_int64(samplingStatement.Get(), column++, sampling.maximumSamples);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.confidenceLevel);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.targetRelativeHalfWidth);
        if (sampling.targetAbsoluteHalfWidth) {
            sqlite3_bind_double(samplingStatement.Get(), column++, *sampling.targetAbsoluteHalfWidth);
        }
        else {
            sqlite3_bind_null(samplingStatement.Get(), column++);
        }
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.effectiveSampleCount);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.mean);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.standardDeviation);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.coefficientOfVariation);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.median);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.p05);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.p95);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.confidenceLow);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.confidenceHigh);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.relativeHalfWidth);
        sqlite3_bind_double(samplingStatement.Get(), column++, sampling.lag1Autocorrelation);
        sqlite3_bind_int64(samplingStatement.Get(), column++, sampling.madOutlierCount);
        sqlite3_bind_int(samplingStatement.Get(), column++, sampling.converged ? 1 : 0);
        sqlite3_bind_int(samplingStatement.Get(), column++, sampling.reachedMaximum ? 1 : 0);
        if (!StepAndReset(samplingStatement.Get(), error)) {
            return false;
        }
    }
    return Execute(
        database.get(),
        "CREATE INDEX scope_events_scope_id ON scope_events(scope_id);"
        "CREATE INDEX scope_events_parent_event_id ON scope_events(parent_event_id);"
        "CREATE INDEX scope_events_frame_id ON scope_events(frame_id);"
        "CREATE INDEX allocations_owner_scope_id ON allocations(owner_scope_id);"
        "COMMIT;",
        error);
}

class ArtifactSink final : public Sink
{
public:
    explicit ArtifactSink(ArtifactOptions options)
        : m_options(std::move(options))
    {
    }

    void OnSessionEnd(const SessionSnapshot& snapshot) override
    {
        (void)WriteArtifacts(snapshot, m_options, &m_error);
    }

    [[nodiscard]] std::string Error() const override
    {
        return m_error;
    }

private:
    ArtifactOptions m_options;
    std::string m_error;
};
}

std::shared_ptr<Sink> MakeArtifactSink(ArtifactOptions options)
{
    return std::make_shared<ArtifactSink>(std::move(options));
}

bool WriteArtifacts(
    const SessionSnapshot& snapshot,
    const ArtifactOptions& options,
    std::string* errorOut)
{
    std::string error;
    std::error_code directoryError;
    std::filesystem::create_directories(options.outputDirectory, directoryError);
    if (directoryError) {
        error = "failed to create output directory: " + directoryError.message();
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }

    std::unordered_map<StableId, ScopeDefinition> definitions;
    for (const auto& definition : snapshot.scopeDefinitions) {
        definitions.emplace(definition.id, definition);
    }

    json manifest{
        { "schema_version", snapshot.schemaVersion },
        { "mode", ModeName(snapshot.mode) },
        { "allocation_tracking", snapshot.allocationTrackingEnabled },
        { "started_ns", snapshot.startedAtNs },
        { "ended_ns", snapshot.endedAtNs },
        { "duration_ns", snapshot.endedAtNs - snapshot.startedAtNs },
        { "dropped_events", snapshot.droppedEvents },
        { "allocation_tracking_overflows", snapshot.allocationTrackingOverflows },
        { "unknown_frees", snapshot.unknownFrees },
        { "sampling_target_count", snapshot.sampling.size() },
        { "metadata", snapshot.metadata },
    };
    if (!WriteJson(options.outputDirectory / "manifest.json", manifest, error)) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }

    std::vector<json> scopeRows;
    for (const auto& scope : snapshot.scopes) {
        const auto definitionIt = definitions.find(scope.scopeId);
        const auto name = definitionIt == definitions.end() ? std::string{} : definitionIt->second.name;
        const auto category = definitionIt == definitions.end() ? std::string{} : definitionIt->second.category;
        scopeRows.push_back({
            { "scope_id", scope.scopeId },
            { "name", name },
            { "category", category },
            { "inclusive_ns", DistributionJson(scope.inclusiveNs) },
            { "self_ns", DistributionJson(scope.selfNs) },
            { "allocations", {
                { "allocated_count", scope.allocatedCount },
                { "allocated_bytes", scope.allocatedBytes },
                { "freed_count", scope.freedCount },
                { "freed_bytes", scope.freedBytes },
                { "live_bytes", scope.liveBytes },
                { "peak_live_bytes", scope.peakLiveBytes },
                { "largest_allocation", scope.largestAllocation },
                { "freed_same_scope", scope.freedInSameScope },
                { "freed_same_frame", scope.freedInSameFrame },
                { "freed_later", scope.freedLater },
                { "still_live", scope.stillLive },
            } },
        });
    }
    std::ranges::sort(scopeRows, [](const json& left, const json& right) {
        return left["self_ns"]["total"].get<std::uint64_t>() >
            right["self_ns"]["total"].get<std::uint64_t>();
    });
    json scopes = scopeRows;

    json metrics = json::array();
    for (const auto& metric : snapshot.metrics) {
        metrics.push_back({
            { "metric_id", metric.id },
            { "name", metric.name },
            { "kind", metric.kind },
            { "current", metric.current },
            { "maximum", metric.maximum },
            { "distribution", DistributionJson(metric.distribution) },
        });
    }
    json sampling = json::array();
    for (const auto& sample : snapshot.sampling) {
        sampling.push_back({
            { "name", sample.name },
            { "category", sample.category },
            { "raw_sample_count", sample.rawSampleCount },
            { "minimum_samples", sample.minimumSamples },
            { "maximum_samples", sample.maximumSamples },
            { "confidence_level", sample.confidenceLevel },
            { "target_relative_half_width", sample.targetRelativeHalfWidth },
            { "target_absolute_half_width", sample.targetAbsoluteHalfWidth
                ? json(*sample.targetAbsoluteHalfWidth)
                : json(nullptr) },
            { "effective_sample_count", sample.effectiveSampleCount },
            { "mean", sample.mean },
            { "standard_deviation", sample.standardDeviation },
            { "coefficient_of_variation", sample.coefficientOfVariation },
            { "median", sample.median },
            { "p05", sample.p05 },
            { "p95", sample.p95 },
            { "confidence_low", sample.confidenceLow },
            { "confidence_high", sample.confidenceHigh },
            { "relative_half_width", sample.relativeHalfWidth },
            { "lag1_autocorrelation", sample.lag1Autocorrelation },
            { "mad_outlier_count", sample.madOutlierCount },
            { "converged", sample.converged },
            { "reached_maximum", sample.reachedMaximum },
        });
    }
    json summary{
        { "schema_version", snapshot.schemaVersion },
        { "scopes", std::move(scopes) },
        { "metrics", std::move(metrics) },
        { "sampling", std::move(sampling) },
    };
    if (!WriteJson(options.outputDirectory / "summary.json", summary, error)) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }

    {
        std::ofstream output(options.outputDirectory / "frames.jsonl", std::ios::binary | std::ios::trunc);
        for (const auto& frame : snapshot.frames) {
            output << json{
                { "frame_id", frame.id },
                { "name", frame.name },
                { "warmup", frame.warmup },
                { "start_ns", frame.startNs },
                { "duration_ns", frame.durationNs },
                { "dimensions", frame.dimensions },
            }.dump() << '\n';
        }
        if (!output) {
            error = "failed to write frames.jsonl";
        }
    }
    if (!error.empty()) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }

    if (snapshot.mode == CaptureMode::Trace) {
        std::ofstream output(options.outputDirectory / "events.jsonl", std::ios::binary | std::ios::trunc);
        for (const auto& event : snapshot.events) {
            output << json{
                { "type", "scope" },
                { "event_id", event.eventId },
                { "scope_id", event.scopeId },
                { "parent_event_id", event.parentEventId },
                { "frame_id", event.frameId },
                { "thread_id", event.threadId },
                { "start_ns", event.startNs },
                { "inclusive_ns", event.inclusiveNs },
                { "self_ns", event.selfNs },
                { "text", event.text },
                { "value", event.hasValue ? json(event.value) : json(nullptr) },
            }.dump() << '\n';
        }
        for (const auto& allocation : snapshot.allocations) {
            output << json{
                { "type", "allocation" },
                { "allocation_id", allocation.allocationId },
                { "domain", DomainName(allocation.domain) },
                { "owner_scope_id", allocation.ownerScopeId },
                { "owner_event_id", allocation.ownerEventId },
                { "owner_frame_id", allocation.ownerFrameId },
                { "size", allocation.size },
                { "allocated_at_ns", allocation.allocatedAtNs },
                { "freed_at_ns", allocation.freedAtNs },
                { "freed_scope_event_id", allocation.freedScopeEventId },
            }.dump() << '\n';
        }
        if (!output) {
            error = "failed to write events.jsonl";
            if (errorOut) {
                *errorOut = error;
            }
            return false;
        }
    }

    if (options.writeSqlite && !WriteSqlite(snapshot, options, definitions, error)) {
        if (errorOut) {
            *errorOut = error;
        }
        return false;
    }

    if (options.writeMarkdown) {
        std::ofstream output(options.outputDirectory / "summary.md", std::ios::binary | std::ios::trunc);
        output << "# Telemetry summary\n\n";
        output << "- Mode: " << ModeName(snapshot.mode) << "\n";
        output << "- Frames: " << snapshot.frames.size() << "\n";
        output << "- Dropped events: " << snapshot.droppedEvents << "\n";
        output << "- Allocation overflows: " << snapshot.allocationTrackingOverflows << "\n\n";
        if (!snapshot.sampling.empty()) {
            output << "## Statistical sampling\n\n";
            output << "| Region | Samples | Effective | Mean (ms) | Confidence interval (ms) | Relative half-width | Converged |\n";
            output << "|---|---:|---:|---:|---:|---:|---:|\n";
            for (const auto& sample : snapshot.sampling) {
                output << "| " << sample.name
                       << " | " << sample.rawSampleCount
                       << " | " << sample.effectiveSampleCount
                       << " | " << sample.mean / 1'000'000.0
                       << " | [" << sample.confidenceLow / 1'000'000.0
                       << ", " << sample.confidenceHigh / 1'000'000.0 << "]"
                       << " | " << sample.relativeHalfWidth
                       << " | " << (sample.converged ? "yes" : "no")
                       << " |\n";
            }
            output << "\n";
        }
        output << "| Scope | Self total (ms) | Inclusive p95 (ms) | Allocated (bytes) |\n";
        output << "|---|---:|---:|---:|\n";
        const auto& summaryScopes = summary["scopes"];
        const auto count = std::min<std::size_t>(20, summaryScopes.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& scope = summaryScopes[index];
            output << "| " << scope["name"].get<std::string>()
                   << " | " << scope["self_ns"]["total"].get<double>() / 1'000'000.0
                   << " | " << scope["inclusive_ns"]["p95"].get<double>() / 1'000'000.0
                   << " | " << scope["allocations"]["allocated_bytes"].get<std::uint64_t>()
                   << " |\n";
        }
    }

    if (errorOut) {
        errorOut->clear();
    }
    return true;
}
}
