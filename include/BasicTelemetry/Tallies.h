#pragma once

// Per-thread tallies of scopes: each TallyScope's count, total and longest time on each thread, recorded only while
// tallies are on. While on, a TallyScope is also a BasicTelemetry Scope, so a session sees the same extents.
// While off, a TallyScope costs one relaxed load. The BasicTelemetry Scope is not opened then, because its constructor
// reads the session through an atomic shared pointer, which takes a lock.
// The callsite must outlive the tallies: TakeTallies reads its name.

#include <BasicTelemetry/Telemetry.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace basic_telemetry
{
/** @brief True while tallies record. A host turns it on at a phase's start, after ResetTallies, and off at its end. */
inline std::atomic<bool> talliesRecording{ false };

// The BasicTelemetry scope is built in place only while tallies record. Its alignment pads the class, hence C4324.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
/** @brief One scope's extent on one thread, tallied while tallies record. */
class TallyScope
{
public:
    explicit TallyScope(const Callsite& callsite) noexcept
    {
        if (talliesRecording.load(std::memory_order_relaxed)) [[unlikely]]
            Begin(callsite);
    }
    ~TallyScope()
    {
        if (m_site) [[unlikely]]
            End();
    }
    TallyScope(const TallyScope&) = delete;
    TallyScope& operator=(const TallyScope&) = delete;

private:
    void Begin(const Callsite& callsite) noexcept;
    void End() noexcept;

    const Callsite* m_site = nullptr;
    std::int64_t m_start = 0;
    bool m_topLevel = false;
    alignas(Scope) std::byte m_storage[sizeof(Scope)];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/** @brief One scope's time on one thread since the last reset. */
struct TallyRow
{
    std::uint32_t threadId = 0;
    std::string thread;  // the thread's description, when it has one
    std::string scope;
    std::uint64_t count = 0, totalNs = 0, maxNs = 0;
    std::uint64_t topLevelNs = 0;  // time with no other tally scope open on the thread, so these add up to its tallied time
};

/** @brief Clears every thread's tallies. */
void ResetTallies();

/** @brief Every thread's tallies since ResetTallies, which this clears. Threads that have exited are included. */
std::vector<TallyRow> TakeTallies();
}
