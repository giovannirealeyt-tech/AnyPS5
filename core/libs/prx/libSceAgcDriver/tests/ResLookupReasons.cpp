#include "prx/libSceAgcDriver/Graphics/include/ResLookupProbe.hpp"

#include <cstdio>
#include <string_view>

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

    ResLookupRecord fastRecord;
    fastRecord.cacheable = true;
    fastRecord.findHit = true;
    fastRecord.outcome = ResLookupClass::Accepted;
    fastRecord.spanMask = (std::uint64_t{1} << static_cast<std::size_t>(ResLookupSpan::Key)) | (std::uint64_t{1} << static_cast<std::size_t>(ResLookupSpan::Find)) | (std::uint64_t{1} << static_cast<std::size_t>(ResLookupSpan::SpaceRevalidate)) | (std::uint64_t{1} << static_cast<std::size_t>(ResLookupSpan::Moved));
    fastRecord.space.enabled = true;
    fastRecord.space.invoked = true;
    fastRecord.space.finalResult = true;
    fastRecord.space.outcomeSet = true;
    fastRecord.space.path = SpacePath::Fast;
    fastRecord.space.outcome = SpaceOutcome::FastHit;
    fastRecord.space.spanNs = 1000;
    fastRecord.space.phaseNs[static_cast<std::size_t>(SpacePhase::FirstFast)] = 600;
    fastRecord.space.residualNs = 400;
    auto& first = fastRecord.space.fastFirst;
    first.invocations = 2;
    first.eachFallback = 1;
    first.textures = 7;
    first.storage = 3;
    first.queries = 9;
    first.pending = 4;
    first.pendingSerialUnchanged = 1;
    first.pendingSerialChanged = 1;
    first.ns[static_cast<std::size_t>(FastPart::TexturePrep)] = 350;
    first.calls[static_cast<std::size_t>(FastPart::TexturePrep)] = 2;
    first.ns[static_cast<std::size_t>(FastPart::ScanPending)] = 150;
    first.calls[static_cast<std::size_t>(FastPart::ScanPending)] = 2;
    auto& retryStats = fastRecord.space.fastRetry;
    retryStats.invocations = 1;
    retryStats.ns[static_cast<std::size_t>(FastPart::Finalize)] = 40;
    retryStats.calls[static_cast<std::size_t>(FastPart::Finalize)] = 1;
    auto& textureFirst = fastRecord.space.textureFirst;
    textureFirst.ns[static_cast<std::size_t>(TexturePart::Controls)] = 70;
    textureFirst.calls[static_cast<std::size_t>(TexturePart::Controls)] = 7;
    textureFirst.depth = 1;
    textureFirst.source = 4;
    textureFirst.snapshot = 2;
    textureFirst.collectZero = 3;
    textureFirst.refresh = 2;
    textureFirst.compare = 1;
    textureFirst.proofScan = 0;
    textureFirst.pendingCalls = 2;
    textureFirst.calls[static_cast<std::size_t>(TexturePart::Proofs)] = 6;
    auto& textureRetry = fastRecord.space.textureRetry;
    textureRetry.ns[static_cast<std::size_t>(TexturePart::Prep)] = 5;
    textureRetry.calls[static_cast<std::size_t>(TexturePart::Prep)] = 1;

    ResLookupAggregate fastAggregate{};
    ResLookupFold(fastAggregate, fastRecord);
    const auto accepted = static_cast<std::size_t>(ResLookupClass::Accepted);
    const auto folded = fastAggregate.space.fastFirst[accepted];
    passed &= check(folded.invocations == 2 && folded.eachFallback == 1, "fast counters fold once per record");
    passed &= check(folded.textures == 7 && folded.storage == 3 && folded.queries == 9 && folded.pending == 4, "visited element counts fold");
    passed &= check(folded.pendingSerialUnchanged == 1 && folded.pendingSerialChanged == 1, "pending-serial branches fold separately from the each fallback");
    passed &= check(folded.ns[static_cast<std::size_t>(FastPart::TexturePrep)] == 350 && folded.calls[static_cast<std::size_t>(FastPart::TexturePrep)] == 2, "first-call part time folds");
    passed &= check(fastAggregate.space.fastRetry[accepted].invocations == 1 && fastAggregate.space.fastRetry[accepted].ns[static_cast<std::size_t>(FastPart::Finalize)] == 40, "retry parts fold away from the first call");
    passed &= check(fastAggregate.space.fastFirst[static_cast<std::size_t>(ResLookupClass::FindMiss)].invocations == 0, "fast stats stay in their own class");
    std::uint64_t partSum = 0;
    for (std::size_t p = 0; p < FastPartCount; ++p) partSum += folded.ns[p];
    passed &= check(partSum <= fastRecord.space.phaseNs[static_cast<std::size_t>(SpacePhase::FirstFast)], "disjoint fast parts stay inside the firstFast parent");
    for (std::size_t p = 0; p < FastPartCount; ++p) passed &= check(folded.calls[p] <= folded.invocations, "a part is recorded at most once per invocation");

    const auto& foldedTexture = fastAggregate.space.textureFirst[accepted];
    passed &= check(foldedTexture.ns[static_cast<std::size_t>(TexturePart::Controls)] == 70 && foldedTexture.calls[static_cast<std::size_t>(TexturePart::Controls)] == 7, "texture part time and visits fold");
    passed &= check(foldedTexture.depth == 1 && foldedTexture.source == 4 && foldedTexture.snapshot == 2, "texture depth and source-or-snapshot branches fold");
    passed &= check(foldedTexture.collectZero == 3 && foldedTexture.refresh == 2 && foldedTexture.compare == 1, "collect refusal and its two continuations fold");
    passed &= check(foldedTexture.pendingCalls == 2, "calls with a non-empty pending set fold");
    passed &= check(foldedTexture.proofScan + foldedTexture.source + foldedTexture.snapshot == foldedTexture.calls[static_cast<std::size_t>(TexturePart::Proofs)], "the three proof branches cover the proof section's visits");
    passed &= check(foldedTexture.depth + foldedTexture.calls[static_cast<std::size_t>(TexturePart::Proofs)] <= foldedTexture.calls[static_cast<std::size_t>(TexturePart::Controls)], "depth plus proofs never exceeds the visited textures");
    passed &= check(foldedTexture.calls[static_cast<std::size_t>(TexturePart::Controls)] > folded.invocations, "a per-texture part counts visits, so it can exceed invocations");
    passed &= check(fastAggregate.space.textureRetry[accepted].calls[static_cast<std::size_t>(TexturePart::Prep)] == 1, "retry texture parts stay away from the first call");
    passed &= check(fastAggregate.space.textureFirst[static_cast<std::size_t>(ResLookupClass::FindMiss)].depth == 0, "texture stats stay in their own class");

    TextureStats firstVisit{};
    firstVisit.collectCalls = 5;
    firstVisit.collectNsMemoHit = 100;
    firstVisit.collectNsWalk = 200;
    firstVisit.collectNsEarlyExit = 50;
    firstVisit.collectClockViolations = 1;
    TextureStats retryVisit{};
    retryVisit.collectCalls = 2;
    retryVisit.collectNsMemoHit = 40;
    retryVisit.collectNsWalk = 60;
    retryVisit.collectNsEarlyExit = 20;
    retryVisit.collectClockViolations = 0;
    TextureStats firstAggregate{}, retryAggregate{};
    FoldTexture(firstAggregate, firstVisit);
    FoldTexture(retryAggregate, retryVisit);
    passed &= check(firstAggregate.collectNsMemoHit == 100 && firstAggregate.collectNsWalk == 200 && firstAggregate.collectNsEarlyExit == 50, "the timing buckets fold into the first-pass aggregate");
    passed &= check(retryAggregate.collectNsMemoHit == 40 && retryAggregate.collectNsWalk == 60 && retryAggregate.collectNsEarlyExit == 20, "the timing buckets fold into the retry aggregate, away from the first pass");
    passed &= check(firstAggregate.collectClockViolations == 1 && retryAggregate.collectClockViolations == 0, "clock violations fold into their own pass");
    passed &= check(firstAggregate.collectCalls == 5 && retryAggregate.collectCalls == 2, "the timing population folds with the collect calls");
    FoldTexture(firstAggregate, retryVisit);
    passed &= check(firstAggregate.collectNsMemoHit == 140 && firstAggregate.collectNsWalk == 260 && firstAggregate.collectCalls == 7, "folding again accumulates the timing buckets");

    constexpr std::string_view companionPrefix = "[resspace-timing-v1]";
    passed &= check(companionPrefix.find("[resspace]") == std::string_view::npos, "the timing companion prefix is invisible to the legacy [resspace] parser");

    passed &= check(ActiveFastStats(false) == nullptr && ActiveFastStats(true) == nullptr, "no active probe exposes no fast stats");
    passed &= check(ActiveTextureStats(false) == nullptr && ActiveTextureStats(true) == nullptr, "no active probe exposes no texture stats");
#if defined(_WIN32)
    _putenv_s("APS5_RESLOOKUP_SPACE", "1");
#else
    setenv("APS5_RESLOOKUP_SPACE", "1", 1);
#endif
    SpaceData activeData;
    {
        SpaceProbeScope activeScope(&activeData);
        passed &= check(ActiveFastStats(false) == &activeData.fastFirst, "first-call stats hang off the active probe");
        passed &= check(ActiveFastStats(true) == &activeData.fastRetry, "retry stats hang off the active probe");
        passed &= check(ActiveTextureStats(false) == &activeData.textureFirst, "first-call texture stats hang off the active probe");
        passed &= check(ActiveTextureStats(true) == &activeData.textureRetry, "retry texture stats hang off the active probe");
        if (auto* stats = ActiveFastStats(false); stats != nullptr) {
            ++stats->invocations;
            stats->textures += 2;
        }
        const FastSection part(ActiveFastStats(true), FastPart::StoragePrep);
    }
    passed &= check(activeData.fastFirst.invocations == 1 && activeData.fastFirst.textures == 2, "writes through the active probe reach the first-call stats");
    passed &= check(activeData.fastRetry.calls[static_cast<std::size_t>(FastPart::StoragePrep)] == 1 && activeData.fastRetry.ns[static_cast<std::size_t>(FastPart::StoragePrep)] <= activeData.spanNs, "an enabled part lands in the retry stats");
    FastStats untouched{};
    {
        const FastSection none(nullptr, FastPart::TexturePrep);
    }
    passed &= check(untouched.ns[static_cast<std::size_t>(FastPart::TexturePrep)] == 0 && untouched.calls[static_cast<std::size_t>(FastPart::TexturePrep)] == 0, "a disabled section records nothing");
    {
        const FastSection one(&untouched, FastPart::StoragePrep);
    }
    passed &= check(untouched.calls[static_cast<std::size_t>(FastPart::StoragePrep)] == 1, "an enabled section records one call");

    TextureStats textureTiming{};
    {
        TextureSection twice(&textureTiming, TexturePart::Collect);
        twice.Stop();
        twice.Stop();
    }
    passed &= check(textureTiming.calls[static_cast<std::size_t>(TexturePart::Collect)] == 1, "a texture section stops once even when stopped twice");
    {
        TextureSection early(&textureTiming, TexturePart::FlushDepth);
    }
    passed &= check(textureTiming.calls[static_cast<std::size_t>(TexturePart::FlushDepth)] == 1, "an unfinished texture section closes with its destructor");
    {
        TextureSection disabled(nullptr, TexturePart::Proofs);
        disabled.Stop();
    }
    passed &= check(textureTiming.calls[static_cast<std::size_t>(TexturePart::Proofs)] == 0 && textureTiming.ns[static_cast<std::size_t>(TexturePart::Proofs)] == 0, "a disabled texture section records nothing");

    const auto cleared = TakeResLookupAggregate();
    passed &= check(cleared.space.fastFirst[accepted].invocations == 0, "take clears fast aggregates");
    passed &= check(cleared.space.textureFirst[accepted].depth == 0 && cleared.space.textureRetry[accepted].pendingCalls == 0, "take clears texture aggregates");
    PublishResLookupRecord(fastRecord);
    resLookupPending().Flush();
    const auto published = TakeResLookupAggregate();
    passed &= check(published.space.fastFirst[accepted].invocations == 2 && published.space.fastRetry[accepted].invocations == 1, "take preserves published fast aggregates before reset");
    passed &= check(published.space.textureFirst[accepted].pendingCalls == 2 && published.space.textureRetry[accepted].calls[static_cast<std::size_t>(TexturePart::Prep)] == 1, "take preserves published texture aggregates before reset");
    passed &= check(TakeResLookupAggregate().space.fastFirst[accepted].invocations == 0, "published fast aggregates are cleared by take");
    const auto drained = TakeResLookupAggregate();
    passed &= check(drained.space.textureFirst[accepted].pendingCalls == 0, "published texture aggregates are cleared by take");
    return passed ? 0 : 1;
}
