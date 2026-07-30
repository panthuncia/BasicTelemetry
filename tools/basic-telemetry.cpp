#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using json = nlohmann::json;

json ReadJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return json::parse(input);
}

std::vector<json> ReadJsonLines(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::vector<json> values;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            values.push_back(json::parse(line));
        }
    }
    return values;
}

std::string Option(
    const std::vector<std::string>& arguments,
    std::string_view name,
    std::string fallback = {})
{
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == name) {
            return arguments[index + 1];
        }
    }
    return fallback;
}

bool Has(const std::vector<std::string>& arguments, std::string_view name)
{
    return std::ranges::find(arguments, name) != arguments.end();
}

std::string GlobToLike(std::string value)
{
    std::ranges::replace(value, '*', '%');
    std::ranges::replace(value, '?', '_');
    return value;
}

struct Database
{
    sqlite3* value{};
    explicit Database(const std::filesystem::path& path)
    {
        if (sqlite3_open_v2(path.string().c_str(), &value, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            const std::string error = value ? sqlite3_errmsg(value) : "cannot open database";
            sqlite3_close(value);
            value = nullptr;
            throw std::runtime_error(error);
        }
    }
    ~Database() { sqlite3_close(value); }
};

json Query(
    sqlite3* database,
    std::string_view sql,
    std::string_view parameter = {})
{
    sqlite3_stmt* raw{};
    if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &raw, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (!parameter.empty()) {
        sqlite3_bind_text(
            statement.get(), 1, parameter.data(), static_cast<int>(parameter.size()), SQLITE_TRANSIENT);
    }
    json rows = json::array();
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        json row;
        for (int column = 0; column < sqlite3_column_count(statement.get()); ++column) {
            const auto* name = sqlite3_column_name(statement.get(), column);
            switch (sqlite3_column_type(statement.get(), column)) {
            case SQLITE_INTEGER:
                row[name] = sqlite3_column_int64(statement.get(), column);
                break;
            case SQLITE_FLOAT:
                row[name] = sqlite3_column_double(statement.get(), column);
                break;
            case SQLITE_TEXT:
                row[name] = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), column));
                break;
            default:
                row[name] = nullptr;
                break;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void PrintRows(const json& rows, bool asJson)
{
    if (asJson) {
        std::cout << rows.dump(2) << '\n';
        return;
    }
    for (const auto& row : rows) {
        bool first = true;
        for (const auto& [key, value] : row.items()) {
            if (!first) {
                std::cout << "  ";
            }
            first = false;
            std::cout << key << '=' << value;
        }
        std::cout << '\n';
    }
}

int Compare(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& candidatePath,
    double threshold,
    bool asJson)
{
    const auto baseline = ReadJson(baselinePath / "summary.json");
    const auto candidate = ReadJson(candidatePath / "summary.json");
    const auto baselineManifest = ReadJson(baselinePath / "manifest.json");
    const auto candidateManifest = ReadJson(candidatePath / "manifest.json");
    const auto baselineMetadata = baselineManifest.value("metadata", json::object());
    const auto candidateMetadata = candidateManifest.value("metadata", json::object());
    static constexpr std::string_view compatibilityKeys[]{
        "component",
        "workload",
        "build_configuration",
        "compiler",
        "architecture",
        "backend",
        "world_form_id",
        "position",
        "radius",
    };
    json mismatchedMetadata = json::array();
    for (const auto key : compatibilityKeys) {
        const auto baselineValue = baselineMetadata.value(std::string(key), std::string{});
        const auto candidateValue = candidateMetadata.value(std::string(key), std::string{});
        if (baselineValue != candidateValue) {
            mismatchedMetadata.push_back({
                { "key", key },
                { "baseline", baselineValue },
                { "candidate", candidateValue },
            });
        }
    }
    const bool compatible = mismatchedMetadata.empty();
    std::map<std::uint64_t, json> baselineScopes;
    for (const auto& scope : baseline["scopes"]) {
        baselineScopes.emplace(scope["scope_id"].get<std::uint64_t>(), scope);
    }
    std::vector<json> comparisons;
    bool regressed = false;
    for (const auto& scope : candidate["scopes"]) {
        const auto name = scope["name"].get<std::string>();
        const auto scopeId = scope["scope_id"].get<std::uint64_t>();
        const auto it = baselineScopes.find(scopeId);
        if (it == baselineScopes.end()) {
            continue;
        }
        const auto fraction = [](double baselineValue, double candidateValue) {
            return baselineValue == 0.0 ? 0.0 :
                (candidateValue - baselineValue) / baselineValue;
        };
        const auto oldSelfMedian = (*it).second["self_ns"]["median"].get<double>();
        const auto newSelfMedian = scope["self_ns"]["median"].get<double>();
        const auto oldSelfP95 = (*it).second["self_ns"]["p95"].get<double>();
        const auto newSelfP95 = scope["self_ns"]["p95"].get<double>();
        const auto oldInclusiveMedian = (*it).second["inclusive_ns"]["median"].get<double>();
        const auto newInclusiveMedian = scope["inclusive_ns"]["median"].get<double>();
        const auto oldInclusiveP95 = (*it).second["inclusive_ns"]["p95"].get<double>();
        const auto newInclusiveP95 = scope["inclusive_ns"]["p95"].get<double>();
        const auto oldAllocated = (*it).second["allocations"]["allocated_bytes"].get<double>();
        const auto newAllocated = scope["allocations"]["allocated_bytes"].get<double>();
        const auto selfMedianFraction = fraction(oldSelfMedian, newSelfMedian);
        const auto selfP95Fraction = fraction(oldSelfP95, newSelfP95);
        const auto inclusiveMedianFraction = fraction(oldInclusiveMedian, newInclusiveMedian);
        const auto inclusiveP95Fraction = fraction(oldInclusiveP95, newInclusiveP95);
        const auto allocatedFraction = fraction(oldAllocated, newAllocated);
        const bool scopeRegressed =
            selfMedianFraction > threshold ||
            selfP95Fraction > threshold ||
            inclusiveMedianFraction > threshold ||
            inclusiveP95Fraction > threshold ||
            allocatedFraction > threshold;
        regressed = regressed || scopeRegressed;
        comparisons.push_back({
            { "name", name },
            { "scope_id", scopeId },
            { "baseline_count", (*it).second["self_ns"]["count"] },
            { "candidate_count", scope["self_ns"]["count"] },
            { "self_median_ns", {
                { "baseline", oldSelfMedian },
                { "candidate", newSelfMedian },
                { "delta_fraction", selfMedianFraction },
            } },
            { "self_p95_ns", {
                { "baseline", oldSelfP95 },
                { "candidate", newSelfP95 },
                { "delta_fraction", selfP95Fraction },
            } },
            { "inclusive_median_ns", {
                { "baseline", oldInclusiveMedian },
                { "candidate", newInclusiveMedian },
                { "delta_fraction", inclusiveMedianFraction },
            } },
            { "inclusive_p95_ns", {
                { "baseline", oldInclusiveP95 },
                { "candidate", newInclusiveP95 },
                { "delta_fraction", inclusiveP95Fraction },
            } },
            { "allocated_bytes", {
                { "baseline", oldAllocated },
                { "candidate", newAllocated },
                { "delta_fraction", allocatedFraction },
            } },
            { "maximum_delta_fraction", std::max({
                selfMedianFraction,
                selfP95Fraction,
                inclusiveMedianFraction,
                inclusiveP95Fraction,
                allocatedFraction,
            }) },
            { "regression", scopeRegressed },
        });
    }
    std::ranges::sort(comparisons, [](const json& left, const json& right) {
        return left["maximum_delta_fraction"].get<double>() >
            right["maximum_delta_fraction"].get<double>();
    });
    const json result{
        { "threshold", threshold },
        { "metadata_compatible", compatible },
        { "metadata_mismatches", std::move(mismatchedMetadata) },
        { "regression", compatible && regressed },
        { "scopes", json(comparisons) },
    };
    if (asJson) {
        std::cout << result.dump(2) << '\n';
    }
    else {
        std::cout << (regressed ? "REGRESSION" : "PASS")
                  << " threshold=" << threshold << '\n';
        PrintRows(result["scopes"], false);
    }
    if (!compatible) {
        return 3;
    }
    return regressed ? 2 : 0;
}

void Usage()
{
    std::cerr
        << "basic-telemetry summary <profile> [--json]\n"
        << "basic-telemetry query <profile> [--scope glob] [--metric self|inclusive] [--top N] [--json]\n"
        << "basic-telemetry frames <profile> [--worst N] [--json]\n"
        << "basic-telemetry allocations <profile> [--top N] [--json]\n"
        << "basic-telemetry compare <baseline> <candidate> [--threshold fraction] [--json]\n"
        << "basic-telemetry build-index <profile>\n";
}
}

int main(int argc, char** argv)
{
    try {
        std::vector<std::string> arguments(argv + 1, argv + argc);
        if (arguments.size() < 2) {
            Usage();
            return 1;
        }
        const auto& command = arguments[0];
        const bool asJson = Has(arguments, "--json");
        if (command == "compare") {
            if (arguments.size() < 3) {
                Usage();
                return 1;
            }
            return Compare(
                arguments[1],
                arguments[2],
                std::stod(Option(arguments, "--threshold", "0.05")),
                asJson);
        }

        const std::filesystem::path profile = arguments[1];
        if (command == "build-index") {
            if (!std::filesystem::exists(profile / "profile.sqlite")) {
                throw std::runtime_error(
                    "profile.sqlite is missing; regenerate artifacts from the canonical session snapshot");
            }
            std::cout << (profile / "profile.sqlite").string() << '\n';
            return 0;
        }
        if (command == "summary") {
            const auto summary = ReadJson(profile / "summary.json");
            if (asJson) {
                std::cout << summary.dump(2) << '\n';
            }
            else {
                const auto count = std::min<std::size_t>(20, summary["scopes"].size());
                for (std::size_t index = 0; index < count; ++index) {
                    const auto& scope = summary["scopes"][index];
                    std::cout << scope["name"].get<std::string>()
                              << " self_total_ns=" << scope["self_ns"]["total"]
                              << " inclusive_p95_ns=" << scope["inclusive_ns"]["p95"]
                              << " allocated_bytes=" << scope["allocations"]["allocated_bytes"]
                              << '\n';
                }
            }
            return 0;
        }

        Database database(profile / "profile.sqlite");
        const auto top = std::max(1, std::stoi(Option(
            arguments,
            command == "frames" ? "--worst" : "--top",
            "20")));
        if (command == "query") {
            const auto pattern = GlobToLike(Option(arguments, "--scope", "*"));
            const auto metric = Option(arguments, "--metric", "self");
            const auto orderColumn = metric == "inclusive"
                ? "inclusive_total_ns"
                : "self_total_ns";
            const auto sql =
                std::string("SELECT name, category, count, inclusive_total_ns, inclusive_median_ns, "
                "inclusive_p95_ns, self_total_ns, self_median_ns, self_p95_ns, "
                "allocated_bytes FROM timing_hotspots WHERE name LIKE ? ORDER BY ") +
                orderColumn + " DESC LIMIT " + std::to_string(top);
            PrintRows(Query(database.value, sql, pattern), asJson);
            return 0;
        }
        if (command == "frames") {
            PrintRows(
                Query(
                    database.value,
                    "SELECT * FROM worst_frames LIMIT " + std::to_string(top)),
                asJson);
            return 0;
        }
        if (command == "allocations") {
            PrintRows(
                Query(
                    database.value,
                    "SELECT * FROM allocation_hotspots LIMIT " + std::to_string(top)),
                asJson);
            return 0;
        }
        Usage();
        return 1;
    }
    catch (const std::exception& exception) {
        std::cerr << "basic-telemetry: " << exception.what() << '\n';
        return 1;
    }
}
