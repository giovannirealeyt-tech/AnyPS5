#include "prx/libSceAgcDriver/Graphics/include/ResLookupProbe.hpp"

#include <cstdio>

using namespace AgcDriver::Graphics;

namespace {

bool check(bool passed, const char* what) {
    if (!passed) std::fprintf(stderr, "uncacheable reasons: %s\n", what);
    return passed;
}

ResLookupRecord uncacheableRecord(bool recordable, bool candidate, bool keyed) {
    ResLookupRecord record;
    record.cacheable = false;
    record.recordable = recordable;
    record.candidate = candidate;
    record.keyed = keyed;
    return record;
}

std::uint64_t reasonSum(const ResLookupAggregate& aggregate) {
    std::uint64_t sum = 0;
    for (std::size_t r = 1; r < ResLookupUncacheableReasonCount; ++r) sum += aggregate.uncacheableReason[r];
    return sum;
}

}

int main() {
    bool passed = true;
    const std::uint8_t expected[2][2][2] = {
        {{1, 1}, {1, 1}},
        {{2, 2}, {3, 0}},
    };
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 2; ++k) {
                const auto reason = static_cast<std::uint8_t>(UncacheableReasonOf(r != 0, c != 0, k != 0));
                passed &= check(reason == expected[r][c][k], "priority table of eight gate inputs");
            }
        }
    }

    for (std::size_t r = 1; r < ResLookupUncacheableReasonCount; ++r) {
        const auto* name = ResLookupUncacheableReasonNames[r];
        passed &= check(name != nullptr && name[0] != '\0', "reason names are stable and explicit");
    }

    ResLookupAggregate local{};
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 2; ++k) ResLookupFold(local, uncacheableRecord(r != 0, c != 0, k != 0));
        }
    }
    const auto uncacheable = static_cast<std::size_t>(ResLookupClass::Uncacheable);
    passed &= check(local.classCount[uncacheable] == 8, "eight uncacheable calls folded locally");
    passed &= check(reasonSum(local) + local.uncacheableReason[0] == local.classCount[uncacheable], "local reason sum equals the same aggregate's uncacheable count");
    passed &= check(local.uncacheableReason[static_cast<std::size_t>(ResLookupUncacheableReason::NotRecordable)] == 4, "notRecordable precedence");
    passed &= check(local.uncacheableReason[static_cast<std::size_t>(ResLookupUncacheableReason::NotCandidate)] == 2, "notCandidate precedence");
    passed &= check(local.uncacheableReason[static_cast<std::size_t>(ResLookupUncacheableReason::NotKeyed)] == 1, "notKeyed precedence");
    passed &= check(local.uncacheableReason[0] == 1, "a fully enabled gate has no reason");

    ResLookupSample off(false);
    off.Begin();
    off.NoteGate(false, false, false);
    off.Finish();
    passed &= check(TakeResLookupAggregate().calls == 0, "flag off publishes no reslookup record");

    ResLookupRecord cacheableRecord;
    cacheableRecord.cacheable = true;
    cacheableRecord.findHit = false;
    cacheableRecord.outcome = ResLookupClass::Unclassified;
    cacheableRecord.spanMask = (std::uint64_t{1} << static_cast<std::size_t>(ResLookupSpan::Key)) | (std::uint64_t{1} << static_cast<std::size_t>(ResLookupSpan::Find));
    ResLookupAggregate mixed{};
    ResLookupFold(mixed, cacheableRecord);
    ResLookupFold(mixed, uncacheableRecord(false, false, false));
    passed &= check(mixed.classCount[static_cast<std::size_t>(ResLookupClass::FindMiss)] == 1, "cacheable record stays in its own class");
    passed &= check(reasonSum(mixed) == mixed.classCount[uncacheable], "cacheable records add no uncacheable reason");

    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 2; ++k) PublishResLookupRecord(uncacheableRecord(r != 0, c != 0, k != 0));
        }
    }
    resLookupPending().Flush();
    const auto taken = TakeResLookupAggregate();
    passed &= check(taken.calls == 8 && taken.classCount[uncacheable] == 8, "flush and take of eight sampled calls");
    passed &= check(reasonSum(taken) + taken.uncacheableReason[0] == taken.classCount[uncacheable], "sampled reason sum equals sampled uncacheable count");
    passed &= check(taken.uncacheableReason[static_cast<std::size_t>(ResLookupUncacheableReason::NotRecordable)] == 4, "sampled notRecordable count");
    passed &= check(taken.uncacheableReason[static_cast<std::size_t>(ResLookupUncacheableReason::NotCandidate)] == 2, "sampled notCandidate count");
    passed &= check(taken.uncacheableReason[static_cast<std::size_t>(ResLookupUncacheableReason::NotKeyed)] == 1, "sampled notKeyed count");
    const auto empty = TakeResLookupAggregate();
    passed &= check(empty.calls == 0 && reasonSum(empty) == 0, "take resets the aggregate");
    ResetResLookupAggregate();
    passed &= check(TakeResLookupAggregate().calls == 0, "explicit reset clears the aggregate");
    return passed ? 0 : 1;
}
