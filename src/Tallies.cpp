#include <BasicTelemetry/Tallies.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <chrono>
#include <functional>
#include <thread>
#endif

namespace basic_telemetry
{
namespace
{
struct Tally
{
    std::uint64_t count = 0, totalNs = 0, maxNs = 0, topLevelNs = 0;
};

// One thread's tallies, kept for the process so a reader can take them after the thread is gone.
// The lock is taken only by the thread itself, except when the tallies are reset or taken.
struct ThreadTable
{
    std::mutex mutex;
    std::uint32_t threadId = 0;
    std::uint32_t depth = 0;
    std::unordered_map<const Callsite*, Tally> tallies;
};

std::mutex tablesMutex;
std::vector<std::unique_ptr<ThreadTable>> tables;
thread_local ThreadTable* threadTable = nullptr;

std::uint32_t CurrentThreadId()
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

ThreadTable& CurrentTable()
{
    if (!threadTable) {
        auto table = std::make_unique<ThreadTable>();
        table->threadId = CurrentThreadId();
        threadTable = table.get();
        std::lock_guard lock(tablesMutex);
        tables.push_back(std::move(table));
    }
    return *threadTable;
}

std::int64_t Now()
{
#if defined(_WIN32)
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

double NsPerTick()
{
#if defined(_WIN32)
    static const double ns = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return 1e9 / static_cast<double>(frequency.QuadPart);
    }();
    return ns;
#else
    return 1.0;
#endif
}

std::string ThreadDescription([[maybe_unused]] std::uint32_t threadId)
{
    std::string name;
#if defined(_WIN32)
    if (const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId)) {
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
            if (size > 1) {
                name.resize(static_cast<std::size_t>(size - 1));
                WideCharToMultiByte(CP_UTF8, 0, description, -1, name.data(), size, nullptr, nullptr);
            }
            LocalFree(description);
        }
        CloseHandle(thread);
    }
#endif
    return name;
}
}

void TallyScope::Begin(const Callsite& callsite) noexcept
{
    try {
        auto& table = CurrentTable();
        m_topLevel = table.depth++ == 0;
    } catch (...) {
        return;
    }
    m_site = &callsite;
    ::new (static_cast<void*>(m_storage)) Scope(callsite);
    m_start = Now();
}

void TallyScope::End() noexcept
{
    const auto ns = static_cast<std::uint64_t>(static_cast<double>(Now() - m_start) * NsPerTick());
    std::launder(reinterpret_cast<Scope*>(m_storage))->~Scope();
    auto& table = *threadTable;
    --table.depth;
    try {
        std::lock_guard lock(table.mutex);
        auto& tally = table.tallies[m_site];
        ++tally.count;
        tally.totalNs += ns;
        tally.maxNs = std::max(tally.maxNs, ns);
        if (m_topLevel)
            tally.topLevelNs += ns;
    } catch (...) {
    }
}

void ResetTallies()
{
    std::lock_guard lock(tablesMutex);
    for (auto& table : tables) {
        std::lock_guard tableLock(table->mutex);
        table->tallies.clear();
    }
}

std::vector<TallyRow> TakeTallies()
{
    std::vector<TallyRow> rows;
    std::lock_guard lock(tablesMutex);
    for (auto& table : tables) {
        std::unordered_map<const Callsite*, Tally> tallies;
        {
            std::lock_guard tableLock(table->mutex);
            tallies.swap(table->tallies);
        }
        if (tallies.empty())
            continue;
        const auto thread = ThreadDescription(table->threadId);
        for (const auto& [site, tally] : tallies)
            rows.push_back({ table->threadId, thread, std::string(site->name), tally.count, tally.totalNs, tally.maxNs, tally.topLevelNs });
    }
    return rows;
}
}
