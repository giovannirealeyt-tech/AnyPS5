#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESLOOKUPPROBE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RESLOOKUPPROBE_HPP

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace AgcDriver::Graphics {

enum class ResLookupSpan : std::size_t {
    Key = 0, Find, SpaceRevalidate, Moved, Alias, Count
};

inline constexpr std::size_t ResLookupSpanCount = static_cast<std::size_t>(ResLookupSpan::Count);

enum class ResLookupClass : std::uint8_t {
    Uncacheable = 0, FindMiss, Fallback, Invalidated, Accepted, Unclassified, Count
};

inline constexpr std::size_t ResLookupClassCount = static_cast<std::size_t>(ResLookupClass::Count);

enum class SpacePhase : std::size_t {
    Initial = 0, Completion, FirstFast, Decision, Refresh, Retry, Audit,
    Walk, DiagnosticWalk, Buffer, Bookkeeping, Count
};

inline constexpr std::size_t SpacePhaseCount = static_cast<std::size_t>(SpacePhase::Count);

enum class SpacePath : std::uint8_t {
    NotInvoked = 0, Epoch, Fast, OwnRefreshed, FullWalk, Count
};

enum class SpaceOutcome : std::uint8_t {
    NotInvoked = 0, NotReached, SkippedSpaceHeld, RejectImageViews, RejectNotReusable,
    EpochHit, EpochDiagnosticHit, EpochDiagnosticFailure, FastHit, OwnRefreshedHit,
    FullWalkHit, WalkFailure, BufferFailure, Count
};

inline constexpr std::size_t SpaceOutcomeCount = static_cast<std::size_t>(SpaceOutcome::Count);
inline constexpr std::uint64_t MaxSpacePhaseRepeats = 4;

inline std::uint64_t SpaceNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline bool SpaceSplitEnabled() {
    static const bool enabled = std::getenv("APS5_RESLOOKUP_SPACE") != nullptr;
    return enabled;
}

struct SpaceData {
    bool enabled = false;
    bool invoked = false;
    bool finalResult = false;
    bool clockViolation = false;
    bool outcomeSet = false;
    bool skipped = false;
    bool walkAttempted = false;
    bool walkSucceeded = false;
    bool coherent = false;
    bool owner = false;
    bool epochDiagnostic = false;
    SpacePath path = SpacePath::NotInvoked;
    SpaceOutcome outcome = SpaceOutcome::NotInvoked;
    std::uint64_t spanNs = 0;
    std::int64_t residualNs = 0;
    std::uint64_t phaseNs[SpacePhaseCount]{};
    std::uint64_t phaseCalls[SpacePhaseCount]{};
};

struct SpaceAggregate {
    std::uint64_t enabled = 0;
    std::uint64_t invoked = 0;
    std::uint64_t success = 0;
    std::uint64_t clockViolations = 0;
    std::uint64_t incoherent = 0;
    std::uint64_t spanNs[ResLookupClassCount]{};
    std::int64_t residualNs[ResLookupClassCount]{};
    std::uint64_t outcome[ResLookupClassCount][SpaceOutcomeCount]{};
    std::uint64_t phaseNs[ResLookupClassCount][SpacePhaseCount]{};
    std::uint64_t phaseCalls[ResLookupClassCount][SpacePhaseCount]{};
};

struct SpaceProbeScope;

inline SpaceProbeScope*& activeSpaceProbe() {
    static thread_local SpaceProbeScope* active = nullptr;
    return active;
}

inline bool ValidateSpace(const SpaceData& data);

struct SpaceProbeScope {
    SpaceData* data;
    SpaceProbeScope* previous;
    std::uint64_t started = 0;
    std::uint64_t lastMark = 0;
    std::uint32_t depth = 0;
    bool finalized = false;
    SpacePhase current = SpacePhase::Initial;
    bool phaseOpen = false;
    explicit SpaceProbeScope(SpaceData* target) : data(target), previous(activeSpaceProbe()) {
        if (data == nullptr || !SpaceSplitEnabled()) { data = nullptr; return; }
        data->enabled = true;
        started = SpaceNow();
        depth = 1;
        activeSpaceProbe() = this;
    }
    ~SpaceProbeScope() {
        if (data == nullptr) return;
        Finalize();
        activeSpaceProbe() = previous;
    }
    void Finalize() {
        if (finalized || data == nullptr) return;
        finalized = true;
        ClosePhase(SpaceNow());
        data->spanNs = SpaceNow() - started;
        std::uint64_t sum = 0;
        for (std::size_t p = 0; p < SpacePhaseCount; ++p) sum += data->phaseNs[p];
        data->residualNs = static_cast<std::int64_t>(data->spanNs) - static_cast<std::int64_t>(sum);
        data->coherent = ValidateSpace(*data);
    }
    std::uint32_t Depth() const { return depth; }
    bool Finalized() const { return finalized; }
    void Enter() { ++depth; }
    void Leave() { if (depth > 0) --depth; }
    void ClosePhase(std::uint64_t now) {
        if (!phaseOpen || depth != 1) return;
        if (now < lastMark) { data->clockViolation = true; phaseOpen = false; return; }
        const auto p = static_cast<std::size_t>(current);
        data->phaseNs[p] += now - lastMark;
        ++data->phaseCalls[p];
        phaseOpen = false;
    }
    void EndPhase() {
        if (finalized || depth != 1) return;
        ClosePhase(SpaceNow());
    }
    void Mark(SpacePhase next) {
        if (finalized || depth != 1) return;
        const auto now = SpaceNow();
        ClosePhase(now);
        current = next;
        lastMark = now;
        phaseOpen = true;
    }
    void SetOutcome(bool result, SpacePath path, SpaceOutcome outcome) {
        if (finalized || depth != 1 || data->outcomeSet || !data->owner) return;
        data->invoked = true;
        data->finalResult = result;
        data->path = path;
        data->outcome = outcome;
        data->outcomeSet = true;
    }
    void SetOutcomeFinal(bool result, SpacePath path, SpaceOutcome outcome) {
        if (finalized || depth != 1 || data->outcomeSet || !data->owner) return;
        data->invoked = true;
        data->finalResult = result;
        data->path = path;
        data->outcome = outcome;
        data->outcomeSet = true;
    }
    void ResetOutcome() {
        if (finalized || depth != 1 || !data->owner) return;
        data->outcomeSet = false;
    }
    void SetSkipped(SpaceOutcome outcome) {
        if (finalized || depth != 1 || data->outcomeSet || !data->owner) return;
        data->skipped = true;
        data->invoked = false;
        data->finalResult = false;
        data->path = SpacePath::NotInvoked;
        data->outcome = outcome;
        data->outcomeSet = true;
    }
    void SetWalkAttempted(bool succeeded) {
        if (finalized || depth != 1) return;
        data->walkAttempted = true;
        data->walkSucceeded = succeeded;
    }
    SpaceProbeScope(const SpaceProbeScope&) = delete;
    SpaceProbeScope& operator=(const SpaceProbeScope&) = delete;
};

inline bool ValidateSpace(const SpaceData& data) {
    if (!data.enabled) return true;
    if (!data.outcomeSet) return false;
    const auto o = static_cast<std::size_t>(data.outcome);
    if (o >= SpaceOutcomeCount) return false;
    const auto p = static_cast<std::size_t>(data.path);
    if (p >= static_cast<std::size_t>(SpacePath::Count)) return false;
    if (data.invoked && data.skipped) return false;
    if (!data.invoked && !data.skipped && data.outcome != SpaceOutcome::NotReached) return false;
    if (data.skipped && (data.outcome != SpaceOutcome::SkippedSpaceHeld || data.finalResult || data.path != SpacePath::NotInvoked)) return false;
    const auto failure = [](SpaceOutcome value) {
        return value == SpaceOutcome::WalkFailure || value == SpaceOutcome::BufferFailure || value == SpaceOutcome::RejectImageViews || value == SpaceOutcome::RejectNotReusable || value == SpaceOutcome::EpochDiagnosticFailure;
    };
    if (!data.skipped && data.invoked && data.finalResult == failure(data.outcome)) return false;
    if (data.outcome == SpaceOutcome::WalkFailure && (!data.walkAttempted || data.walkSucceeded)) return false;
    if (data.outcome == SpaceOutcome::BufferFailure && data.walkAttempted && !data.walkSucceeded) return false;
    if (data.outcome == SpaceOutcome::FullWalkHit && (!data.walkAttempted || !data.walkSucceeded || data.path != SpacePath::FullWalk)) return false;
    if (data.outcome == SpaceOutcome::FastHit && data.path != SpacePath::Fast) return false;
    if (data.outcome == SpaceOutcome::OwnRefreshedHit && data.path != SpacePath::OwnRefreshed) return false;
    if (data.outcome == SpaceOutcome::EpochHit && data.path != SpacePath::Epoch) return false;
    if ((data.outcome == SpaceOutcome::EpochDiagnosticHit || data.outcome == SpaceOutcome::EpochDiagnosticFailure) && (data.path != SpacePath::Epoch || !data.epochDiagnostic)) return false;
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < SpacePhaseCount; ++i) {
        sum += data.phaseNs[i];
        if (data.phaseCalls[i] > MaxSpacePhaseRepeats) return false;
    }
    if (static_cast<std::int64_t>(data.spanNs) - static_cast<std::int64_t>(sum) != data.residualNs) return false;
    return true;
}

inline void SpaceMark(SpacePhase next) {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->Mark(next);
}

inline void SpaceEndPhase() {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->EndPhase();
}

inline void SpaceResetOutcome() {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->ResetOutcome();
}

inline void SpaceSetEpochDiagnostic(bool diagnostic) {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr && probe->data != nullptr) probe->data->epochDiagnostic = diagnostic;
}

inline void SpaceSetOutcome(bool result, SpacePath path, SpaceOutcome outcome) {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->SetOutcome(result, path, outcome);
}

inline void SpaceSetOutcomeFinal(bool result, SpacePath path, SpaceOutcome outcome) {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->SetOutcomeFinal(result, path, outcome);
}

inline void SpaceSetSkipped(SpaceOutcome outcome) {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->SetSkipped(outcome);
}

inline void SpaceSetWalkAttempted(bool succeeded) {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->SetWalkAttempted(succeeded);
}

inline void SpaceEnter() {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->Enter();
}

inline void SpaceLeave() {
    auto* probe = activeSpaceProbe();
    if (probe != nullptr) probe->Leave();
}

struct SpaceInvocationGuard {
    SpaceData* data;
    bool previous;
    explicit SpaceInvocationGuard() : data(nullptr), previous(false) {
        auto* probe = activeSpaceProbe();
        if (probe != nullptr && probe->data != nullptr) {
            data = probe->data;
            previous = data->owner;
            data->owner = true;
        }
    }
    ~SpaceInvocationGuard() { if (data != nullptr) data->owner = previous; }
};

struct SpaceNestedGuard {
    SpaceNestedGuard() { SpaceEnter(); }
    ~SpaceNestedGuard() { SpaceLeave(); }
};

inline void FoldSpace(SpaceAggregate& aggregate, std::size_t c, const SpaceData& data) {
    if (!data.enabled) return;
    const bool coherent = ValidateSpace(data);
    ++aggregate.enabled;
    if (data.invoked) ++aggregate.invoked;
    if (data.finalResult) ++aggregate.success;
    if (data.clockViolation) ++aggregate.clockViolations;
    if (!coherent) ++aggregate.incoherent;
    if (!coherent) return;
    aggregate.spanNs[c] += data.spanNs;
    aggregate.residualNs[c] += data.residualNs;
    const auto o = static_cast<std::size_t>(data.outcome);
    if (o < SpaceOutcomeCount) ++aggregate.outcome[c][o];
    for (std::size_t p = 0; p < SpacePhaseCount; ++p) {
        aggregate.phaseNs[c][p] += data.phaseNs[p];
        aggregate.phaseCalls[c][p] += data.phaseCalls[p];
    }
}

struct ResLookupRecord {
    std::uint64_t totalNs = 0;
    std::uint64_t spanNs[ResLookupSpanCount]{};
    std::uint64_t spanMask = 0;
    bool cacheable = false;
    bool findHit = false;
    bool clockViolation = false;
    bool shapeError = false;
    bool duplicateSpan = false;
    ResLookupClass outcome = ResLookupClass::Unclassified;
    SpaceData space;
};

struct ResLookupAggregate {
    std::uint64_t calls = 0;
    std::uint64_t clockViolations = 0;
    std::uint64_t shapeErrors = 0;
    std::uint64_t classClockViolations[ResLookupClassCount]{};
    std::uint64_t classCount[ResLookupClassCount]{};
    std::uint64_t classTotalNs[ResLookupClassCount]{};
    std::uint64_t spanNs[ResLookupClassCount][ResLookupSpanCount]{};
    std::uint64_t spanCalls[ResLookupClassCount][ResLookupSpanCount]{};
    SpaceAggregate space;
};

inline ResLookupClass ResLookupClassOf(const ResLookupRecord& record) {
    if (!record.cacheable) return ResLookupClass::Uncacheable;
    if (!record.findHit) return ResLookupClass::FindMiss;
    switch (record.outcome) {
        case ResLookupClass::Fallback:
        case ResLookupClass::Invalidated:
        case ResLookupClass::Accepted:
            return record.outcome;
        default:
            return ResLookupClass::Unclassified;
    }
}

inline int64_t ResLookupResidualNs(const ResLookupAggregate& aggregate, ResLookupClass cls) {
    const auto c = static_cast<std::size_t>(cls);
    std::uint64_t spans = 0;
    for (std::size_t s = 0; s < ResLookupSpanCount; ++s) spans += aggregate.spanNs[c][s];
    return static_cast<int64_t>(aggregate.classTotalNs[c]) - static_cast<int64_t>(spans);
}

inline void ValidateResLookupRecord(ResLookupRecord& record) {
    std::uint64_t spanSum = 0;
    for (std::size_t s = 0; s < ResLookupSpanCount; ++s) {
        if ((record.spanMask & (std::uint64_t{1} << s)) != 0) spanSum += record.spanNs[s];
    }
    if (spanSum > record.totalNs) record.clockViolation = true;
    record.shapeError = record.shapeError || record.duplicateSpan;
    const auto has = [&](ResLookupSpan span) {
        return (record.spanMask & (std::uint64_t{1} << static_cast<std::size_t>(span))) != 0;
    };
    if (!record.cacheable) {
        if (record.spanMask != 0) record.shapeError = true;
        return;
    }
    if (!has(ResLookupSpan::Key) || !has(ResLookupSpan::Find)) record.shapeError = true;
    const bool hasSpace = has(ResLookupSpan::SpaceRevalidate);
    const bool hasMoved = has(ResLookupSpan::Moved);
    const bool hasAlias = has(ResLookupSpan::Alias);
    if (!record.findHit) {
        if (hasSpace || hasMoved || hasAlias) record.shapeError = true;
    } else {
        if (!hasSpace) record.shapeError = true;
        if (record.outcome == ResLookupClass::Accepted) {
            if (!hasMoved) record.shapeError = true;
        } else if (record.outcome == ResLookupClass::Fallback || record.outcome == ResLookupClass::Invalidated) {
            if (hasAlias) record.shapeError = true;
        } else {
            record.shapeError = true;
        }
    }
}

inline std::uint64_t ResLookupNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline bool ResLookupSplitEnabled() {
    static const bool enabled = std::getenv("APS5_RESLOOKUP_SPLIT") != nullptr;
    return enabled;
}

inline void PublishResLookupRecord(const ResLookupRecord& record);

struct ResLookupSample {
    bool active;
    std::uint64_t started = 0;
    std::uint64_t cursor = 0;
    ResLookupRecord record;
    explicit ResLookupSample(bool enabled) : active(enabled) {}
    void Begin() {
        if (!active) return;
        record = ResLookupRecord{};
        started = ResLookupNow();
    }
    void NoteCacheable() {
        if (active) record.cacheable = true;
    }
    void NoteFindHit(bool hit) {
        if (active) record.findHit = hit;
    }
    void Note(ResLookupClass outcome) {
        if (active) record.outcome = outcome;
    }
    void Span(ResLookupSpan span, std::uint64_t before, std::uint64_t after) {
        if (!active) return;
        const auto s = static_cast<std::size_t>(span);
        record.duplicateSpan = record.duplicateSpan || ((record.spanMask & (std::uint64_t{1} << s)) != 0);
        record.spanMask |= std::uint64_t{1} << s;
        if (after < before) {
            record.clockViolation = true;
            return;
        }
        record.spanNs[s] += after - before;
    }
    void SpanBegin() {
        if (active) cursor = ResLookupNow();
    }
    void SpanEnd(ResLookupSpan span) {
        if (active) Span(span, cursor, ResLookupNow());
    }
    void Finish() {
        if (!active) return;
        active = false;
        const auto end = ResLookupNow();
        if (end < started) {
            record.clockViolation = true;
            record.totalNs = 0;
        } else {
            record.totalNs = end - started;
        }
        PublishResLookupRecord(record);
    }
};

inline constexpr std::uint32_t ResLookupFlushRecords = 64;

struct ResLookupPending {
    ResLookupRecord records[ResLookupFlushRecords];
    std::uint32_t count = 0;
    void Flush();
};

struct ResLookupState {
    std::mutex mutex;
    ResLookupAggregate aggregate;
};

inline ResLookupPending& resLookupPending() {
    static thread_local ResLookupPending pending;
    return pending;
}

inline ResLookupState& resLookupState() {
    static ResLookupState state;
    return state;
}

inline void ResLookupFold(ResLookupAggregate& aggregate, const ResLookupRecord& record) {
    ResLookupRecord validated = record;
    ValidateResLookupRecord(validated);
    const bool clockViolation = record.clockViolation || validated.clockViolation;
    const bool shapeError = record.shapeError || validated.shapeError;
    ++aggregate.calls;
    if (clockViolation) ++aggregate.clockViolations;
    if (shapeError) ++aggregate.shapeErrors;
    const auto c = static_cast<std::size_t>(ResLookupClassOf(validated));
    if (clockViolation) ++aggregate.classClockViolations[c];
    ++aggregate.classCount[c];
    aggregate.classTotalNs[c] += validated.totalNs;
    FoldSpace(aggregate.space, c, validated.space);
    for (std::size_t s = 0; s < ResLookupSpanCount; ++s) {
        if ((validated.spanMask & (std::uint64_t{1} << s)) == 0) continue;
        ++aggregate.spanCalls[c][s];
        aggregate.spanNs[c][s] += validated.spanNs[s];
    }
}

inline void ResLookupPending::Flush() {
    auto& state = resLookupState();
    std::lock_guard lock(state.mutex);
    for (std::uint32_t i = 0; i < count; ++i) ResLookupFold(state.aggregate, records[i]);
    count = 0;
}

inline void PublishResLookupRecord(const ResLookupRecord& record) {
    auto& pending = resLookupPending();
    pending.records[pending.count++] = record;
    if (pending.count >= ResLookupFlushRecords) pending.Flush();
}

inline ResLookupAggregate TakeResLookupAggregate() {
    auto& state = resLookupState();
    std::lock_guard lock(state.mutex);
    const auto taken = state.aggregate;
    state.aggregate = ResLookupAggregate{};
    return taken;
}

inline void ResetResLookupAggregate() {
    auto& state = resLookupState();
    std::lock_guard lock(state.mutex);
    state.aggregate = ResLookupAggregate{};
}

} // namespace AgcDriver::Graphics

#endif
