#pragma once

// One thread's time per frame, by bucket and by site. Every entry the thread makes into the host is a Hook, counted to
// its site. Parts nested in a hook go to their own bucket. Splits are time another component reports inside the hook,
// as running totals, which go to their buckets too. The rest of the hook goes to the rest bucket.
// The thread is the one that last opened a hook at the claiming site. Hooks and parts on other threads count nothing.
// Use it from that thread only.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

namespace basic_telemetry
{
/**
 * @brief Bucket and Site are enums with a Count member. Splits is the number of running totals the host reports from
 * inside hooks.
 */
template <class Bucket, class Site, std::size_t Splits = 0>
class ThreadBudget
{
public:
    static constexpr std::size_t kBuckets = static_cast<std::size_t>(Bucket::Count);
    static constexpr std::size_t kSites = static_cast<std::size_t>(Site::Count);
    using SplitTotals = std::array<double, Splits>;

    struct Desc
    {
        /** @brief A hook at this site makes its thread the budget's thread. */
        Site claimSite{};
        /** @brief A hook's time that no part or split takes. */
        Bucket restBucket{};
        /** @brief The bucket each split's time goes to. */
        std::array<Bucket, Splits> splitBuckets{};
        /** @brief The splits' running totals in microseconds, read when an outermost hook opens and closes. */
        SplitTotals (*splitTotals)() = nullptr;
    };

    /** @brief One frame's total and buckets, in microseconds. */
    struct FrameTime
    {
        double totalUs = 0.0;
        std::array<double, kBuckets> bucketUs{};
    };

    /** @brief The sums since the last Take, in microseconds. */
    struct Totals
    {
        std::uint32_t frames = 0;
        double totalUs = 0.0;
        double frameMaxUs = 0.0;
        std::array<double, kBuckets> bucketUs{};
        std::array<double, kSites> siteUs{};
        std::array<std::uint32_t, kSites> siteCalls{};
    };

    explicit ThreadBudget(const Desc& desc) : m_desc(desc) {}
    ThreadBudget(const ThreadBudget&) = delete;
    ThreadBudget& operator=(const ThreadBudget&) = delete;

    /** @brief A host entry's whole call. Nested hooks count once, to the outermost's site. */
    class Hook
    {
    public:
        Hook(ThreadBudget& budget, Site site) : m_budget(budget)
        {
            auto& b = m_budget;
            if (site == b.m_desc.claimSite)
                b.m_thread = std::this_thread::get_id();
            if (!b.OnThread())
                return;
            m_counted = true;
            if (b.m_depth++)
                return;
            b.m_site = site;
            b.m_hookPartsUs = 0.0;
            b.m_splitStart = b.ReadSplits();
            b.m_hookStart = std::chrono::steady_clock::now();
        }
        ~Hook()
        {
            auto& b = m_budget;
            if (!m_counted || --b.m_depth)
                return;
            const double us = Microseconds(std::chrono::steady_clock::now() - b.m_hookStart);
            const SplitTotals splitEnd = b.ReadSplits();
            SplitTotals split{};
            double other = us - b.m_hookPartsUs;
            for (std::size_t i = 0; i < Splits; ++i) {
                split[i] = splitEnd[i] - b.m_splitStart[i];
                other -= split[i];
            }
            other = (std::max)(0.0, other);
            for (std::size_t i = 0; i < Splits; ++i)
                b.m_bucketUs[Index(b.m_desc.splitBuckets[i])] += split[i];
            b.m_bucketUs[Index(b.m_desc.restBucket)] += other;
            for (std::size_t i = 0; i < Splits; ++i)
                b.m_frameBucketUs[Index(b.m_desc.splitBuckets[i])] += split[i];
            b.m_frameBucketUs[Index(b.m_desc.restBucket)] += other;
            b.m_siteUs[Index(b.m_site)] += us;
            ++b.m_siteCalls[Index(b.m_site)];
            b.m_frameUs += us;
            b.m_totalUs += us;
        }
        Hook(const Hook&) = delete;
        Hook& operator=(const Hook&) = delete;

    private:
        ThreadBudget& m_budget;
        bool m_counted = false;
    };

    /** @brief A part of a hook counted to its bucket rather than the rest. Outside any hook it is its own time. */
    class Part
    {
    public:
        Part(ThreadBudget& budget, Bucket bucket) : m_budget(budget), m_bucket(bucket)
        {
            if (!m_budget.OnThread())
                return;
            m_counted = true;
            m_start = std::chrono::steady_clock::now();
        }
        ~Part()
        {
            if (!m_counted)
                return;
            auto& b = m_budget;
            const double us = Microseconds(std::chrono::steady_clock::now() - m_start);
            b.m_bucketUs[Index(m_bucket)] += us;
            b.m_frameBucketUs[Index(m_bucket)] += us;
            if (b.m_depth) {
                b.m_hookPartsUs += us;
                return;
            }
            b.m_frameUs += us;
            b.m_totalUs += us;
        }
        Part(const Part&) = delete;
        Part& operator=(const Part&) = delete;

    private:
        ThreadBudget& m_budget;
        Bucket m_bucket;
        bool m_counted = false;
        std::chrono::steady_clock::time_point m_start;
    };

    /** @brief At the frame's end, on the budget's thread. The frame's total joins the maximum. */
    void EndFrame()
    {
        if (!OnThread())
            return;
        m_frameMaxUs = (std::max)(m_frameMaxUs, m_frameUs);
        m_lastFrame = { m_frameUs, m_frameBucketUs };
        m_frameUs = 0.0;
        m_frameBucketUs = {};
        ++m_frames;
    }

    /** @brief The last frame EndFrame closed. */
    const FrameTime& LastFrame() const { return m_lastFrame; }

    /** @brief The sums since the last Take, which this clears, or nothing when no frame ended. */
    std::optional<Totals> Take()
    {
        if (!m_frames)
            return std::nullopt;
        Totals totals{ m_frames, m_totalUs, m_frameMaxUs, m_bucketUs, m_siteUs, m_siteCalls };
        m_bucketUs = {};
        m_siteUs = {};
        m_siteCalls = {};
        m_frameMaxUs = 0.0;
        m_totalUs = 0.0;
        m_frames = 0;
        return totals;
    }

private:
    template <class E>
    static constexpr std::size_t Index(E value)
    {
        return static_cast<std::size_t>(value);
    }

    static double Microseconds(std::chrono::steady_clock::duration duration)
    {
        return std::chrono::duration<double, std::micro>(duration).count();
    }

    bool OnThread() const { return m_thread == std::this_thread::get_id(); }

    SplitTotals ReadSplits() const { return m_desc.splitTotals ? m_desc.splitTotals() : SplitTotals{}; }

    Desc m_desc;
    std::thread::id m_thread{};
    std::uint32_t m_depth = 0;
    Site m_site{};
    std::chrono::steady_clock::time_point m_hookStart;
    double m_hookPartsUs = 0.0;
    SplitTotals m_splitStart{};

    std::array<double, kBuckets> m_bucketUs{};
    std::array<double, kSites> m_siteUs{};
    std::array<std::uint32_t, kSites> m_siteCalls{};
    double m_frameUs = 0.0;
    std::array<double, kBuckets> m_frameBucketUs{};
    FrameTime m_lastFrame;
    double m_frameMaxUs = 0.0;
    double m_totalUs = 0.0;
    std::uint32_t m_frames = 0;
};
}
