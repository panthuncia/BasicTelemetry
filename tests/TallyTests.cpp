// These tests check everything with assert, so keep it active in optimized builds.
#undef NDEBUG

#include <BasicTelemetry/Tallies.h>
#include <BasicTelemetry/ThreadBudget.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace basic_telemetry;

const TallyRow* Find(const std::vector<TallyRow>& rows, std::string_view scope, std::uint32_t threadId)
{
    for (const auto& row : rows)
        if (row.scope == scope && row.threadId == threadId)
            return &row;
    return nullptr;
}

std::uint32_t OnlyThread(const std::vector<TallyRow>& rows, std::string_view scope)
{
    std::uint32_t id = 0, found = 0;
    for (const auto& row : rows)
        if (row.scope == scope)
            id = row.threadId, ++found;
    assert(found == 1);
    return id;
}

void TalliesRecordOnlyWhileOn()
{
    static const Callsite site("tally.off");
    ResetTallies();
    talliesRecording.store(false);
    {
        TallyScope scope(site);
    }
    assert(TakeTallies().empty());
}

void TalliesNestAndClear()
{
    static const Callsite outer("tally.outer");
    static const Callsite inner("tally.inner");
    ResetTallies();
    talliesRecording.store(true);
    for (int i = 0; i < 3; ++i) {
        TallyScope a(outer);
        TallyScope b(inner);
        TallyScope c(inner);
    }
    talliesRecording.store(false);
    const auto rows = TakeTallies();
    const std::uint32_t self = OnlyThread(rows, "tally.outer");
    const TallyRow* o = Find(rows, "tally.outer", self);
    const TallyRow* n = Find(rows, "tally.inner", self);
    assert(o && n);
    assert(o->count == 3 && n->count == 6);
    // Only the outermost scope is top-level.
    assert(o->topLevelNs == o->totalNs);
    assert(n->topLevelNs == 0);
    assert(o->maxNs <= o->totalNs && n->maxNs <= n->totalNs);
    // Taking the tallies clears them.
    assert(TakeTallies().empty());
}

void TalliesPerThread()
{
    static const Callsite site("tally.thread");
    ResetTallies();
    talliesRecording.store(true);
    {
        TallyScope here(site);
    }
    std::thread other([] {
        for (int i = 0; i < 2; ++i) {
            TallyScope there(site);
        }
    });
    other.join();
    talliesRecording.store(false);
    auto rows = TakeTallies();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const TallyRow& row) { return row.scope != "tally.thread"; }), rows.end());
    // The exited thread's tallies are kept.
    assert(rows.size() == 2);
    std::sort(rows.begin(), rows.end(), [](const TallyRow& a, const TallyRow& b) { return a.count < b.count; });
    assert(rows[0].count == 1 && rows[1].count == 2);
    assert(rows[0].threadId != rows[1].threadId);
}

void ResetClearsTallies()
{
    static const Callsite site("tally.reset");
    talliesRecording.store(true);
    {
        TallyScope scope(site);
    }
    ResetTallies();
    talliesRecording.store(false);
    assert(TakeTallies().empty());
}

enum class Bucket : std::uint8_t
{
    Prepare,
    Submit,
    Rest,
    Count
};

enum class Site : std::uint8_t
{
    Frame,
    Draw,
    Count
};

double splitSubmit = 0.0;

ThreadBudget<Bucket, Site, 1>::SplitTotals Splits()
{
    return { splitSubmit };
}

using Budget = ThreadBudget<Bucket, Site, 1>;

Budget::Desc BudgetDesc()
{
    return { Site::Frame, Bucket::Rest, { Bucket::Submit }, &Splits };
}

void BudgetCountsNothingBeforeItsThreadIsClaimed()
{
    Budget budget(BudgetDesc());
    {
        Budget::Hook hook(budget, Site::Draw);
        Budget::Part part(budget, Bucket::Prepare);
    }
    budget.EndFrame();
    assert(!budget.Take());
}

void BudgetHooksPartsAndSplits()
{
    splitSubmit = 0.0;
    Budget budget(BudgetDesc());
    {
        Budget::Hook frame(budget, Site::Frame);
        {
            // A nested hook counts to the outermost's site.
            Budget::Hook nested(budget, Site::Draw);
            Budget::Part part(budget, Bucket::Prepare);
        }
        // Another component reports 1000 us inside the hook.
        splitSubmit += 1000.0;
    }
    {
        Budget::Hook draw(budget, Site::Draw);
    }
    {
        // Outside any hook a part is its own time.
        Budget::Part loose(budget, Bucket::Prepare);
    }
    // Another thread counts nothing.
    std::thread other([&budget] {
        Budget::Hook hook(budget, Site::Draw);
        Budget::Part part(budget, Bucket::Prepare);
        budget.EndFrame();
    });
    other.join();
    budget.EndFrame();

    const auto& last = budget.LastFrame();
    assert(last.bucketUs[static_cast<std::size_t>(Bucket::Submit)] == 1000.0);
    assert(last.totalUs >= 0.0);

    const auto totals = budget.Take();
    assert(totals);
    assert(totals->frames == 1);
    assert(totals->siteCalls[static_cast<std::size_t>(Site::Frame)] == 1);
    assert(totals->siteCalls[static_cast<std::size_t>(Site::Draw)] == 1);
    assert(totals->bucketUs[static_cast<std::size_t>(Bucket::Submit)] == 1000.0);
    assert(totals->bucketUs[static_cast<std::size_t>(Bucket::Rest)] >= 0.0);
    assert(totals->frameMaxUs == totals->totalUs);
    // Taking clears the sums, and the next take has no frame.
    assert(!budget.Take());
    budget.EndFrame();
    const auto next = budget.Take();
    assert(next && next->frames == 1 && next->totalUs == 0.0);
    assert(next->siteCalls[static_cast<std::size_t>(Site::Frame)] == 0);
}

void BudgetWithoutSplits()
{
    ThreadBudget<Bucket, Site> budget({ Site::Frame, Bucket::Rest });
    {
        ThreadBudget<Bucket, Site>::Hook hook(budget, Site::Frame);
    }
    budget.EndFrame();
    const auto totals = budget.Take();
    assert(totals && totals->frames == 1);
    assert(totals->siteCalls[static_cast<std::size_t>(Site::Frame)] == 1);
    assert(totals->bucketUs[static_cast<std::size_t>(Bucket::Submit)] == 0.0);
}
}

int main()
{
    TalliesRecordOnlyWhileOn();
    TalliesNestAndClear();
    TalliesPerThread();
    ResetClearsTallies();
    BudgetCountsNothingBeforeItsThreadIsClaimed();
    BudgetHooksPartsAndSplits();
    BudgetWithoutSplits();
    return 0;
}
