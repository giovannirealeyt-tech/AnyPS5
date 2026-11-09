#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDCOUNTERS_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace AgcDriver::Graphics {

// Low-overhead counters for the Vulkan command volume the committer records.
// They exist to answer "how many calls per draw" without APS5_PROFILE_DRAW,
// whose packet-profile path disables the handoff to the committer thread;
// APS5_PIPELINE_STATS alone enables the report without changing the pipeline.
struct CommandCallCounters {
    std::atomic<std::uint64_t> pipelineBinds{0};
    std::atomic<std::uint64_t> libraryStates{0}; // each one issues the dynamic-state set below
    std::atomic<std::uint64_t> descriptorSetBinds{0};
    std::atomic<std::uint64_t> pushConstantCalls{0};
    std::atomic<std::uint64_t> drawCalls{0};
    // "Would dedupe" counters: how many of those calls would be skipped if the
    // exact same value were already bound in the same command buffer. Nothing is
    // skipped here, so they are a lower bound on what deduplication could remove.
    std::atomic<std::uint64_t> pipelineBindsRepeated{0};
    std::atomic<std::uint64_t> descriptorSetBindsRepeated{0};
    std::atomic<std::uint64_t> pushConstantsRepeated{0};
};

inline CommandCallCounters& commandCallCounters() {
    static CommandCallCounters counters;
    return counters;
}

// Number of vkCmdSet* calls one Pipeline::setLibraryState emits (cull mode, front
// face, depth bias, depth bounds, depth test, depth write, depth compare, depth
// bounds test, stencil test, and op/compare/write/reference for both faces).
inline constexpr std::uint64_t LibraryStateCommands = 17;

// APS5_PUSH_SINGLE_RANGE=1 declares one push-constant range covering both slots
// (default off). With two ranges the driver must issue two vkCmdPushConstants per
// draw, because a call may not write outside a stage's declared range.
inline bool SinglePushRange() {
    static const bool enabled = std::getenv("APS5_PUSH_SINGLE_RANGE") != nullptr;
    return enabled;
}

// APS5_COMMIT_SPLIT: coarse split of one draw commit, measured on the committer
// thread. APS5_PROFILE_DRAW must be absent from the environment (the driver tests
// getenv != nullptr).
//
// Corrections from the review of the first attempt:
//  - classified and unclassified samples are accumulated separately, so the gap is
//    always total(classified) - sum(phases) over the SAME draws;
//  - the gap is signed and a violation counter catches sum > total (uint64 underflow);
//  - marks respect depth and path, so a nested call cannot overwrite the outer one;
//  - a draw that reuses the render pass has no Begin, which is legitimate: only the
//    record and draw marks are required for a sample to be classified.
// Graphics::Draw recurses for the tessellation geometry path, so only the outermost
// call measures.
struct CommitSplitCounters {
    std::atomic<std::uint64_t> classifiedNs{0};
    std::atomic<std::uint64_t> classified{0};
    std::atomic<std::uint64_t> unclassifiedNs{0};
    std::atomic<std::uint64_t> unclassified{0};
    std::atomic<std::uint64_t> setupNs{0};
    std::atomic<std::uint64_t> beginNs{0};
    std::atomic<std::uint64_t> setsNs{0};
    std::atomic<std::uint64_t> ldsNs{0};
    std::atomic<std::uint64_t> pushNs{0};
    std::atomic<std::uint64_t> recordNs{0};
    std::atomic<std::uint64_t> finishNs{0};
    std::atomic<std::uint64_t> noBegin{0};      // classified draws with a reused pass
    std::atomic<std::uint64_t> violations{0};   // sum(phases) > total
    std::atomic<std::uint64_t> recipeClassified{0};
    std::atomic<std::uint64_t> recipeUnclassified{0};
};

inline CommitSplitCounters& commitSplitCounters() {
    static CommitSplitCounters counters;
    return counters;
}

inline bool CommitSplitEnabled() {
    static const bool enabled = std::getenv("APS5_COMMIT_SPLIT") != nullptr;
    return enabled;
}

// Version 2 of the same scope: one mark per boundary, and every phase accumulated
// only on the draws that reach its mark. The first version derived setup/begin by
// subtraction from a start that moved with hasBegin, so prepareDraw was counted in
// setup for a draw that opened its render pass and in begin for one that reused it,
// and the two means could not be compared. A phase whose mark is not reached is not
// counted at all; its time joins the next reached phase, so different populations
// never mix. Each phase keeps its own sample count.
//
//   inputs      prepareDrawInputs: validation, index and vertex staging copies
//   targets     the color/depth target loop: resident lookups, DCC, transfers
//   resLookup   resolveDrawResources before a build: key, Find, Revalidate
//   resBuild    the ShaderResources build of a miss (bindings, upload, sets)
//   pipeline    indirect records, cache insert, masks, stencil pipelines, pipeline
//   framebuffer owners, AcquireFramebuffer, pass depth locations
//   descWrites  PrepareDrawBindings: this draw's vkUpdateDescriptorSets
//   preBegin    barriers, proxies, staged clears before Pipeline::Begin
//   passBegin   Pipeline::Begin alone (only draws that open a render pass)
//   descBind    the vkCmdBindDescriptorSets of this draw's sets
//   ldsBind     pixel-function LDS and mesh window binds
//   push        vkCmdPushConstants
//   record      the vkCmdDraw* calls, stencil replay, face/channel splits
//   finish      everything after the last mark (keep, completion, the draw's dtor)
//
// Samples are accumulated per thread and merged every CommitSplitFlushDraws draws,
// so no phase costs an atomic per draw; the report lags by up to that many draws
// per thread, as the resource build profile already does.
enum class CommitSplitPhase : std::size_t {
    Inputs = 0, Targets, ResLookup, ResBuild, Pipeline, Framebuffer, DescWrites,
    PreBegin, PassBegin, DescBind, LdsBind, Push, Record, Finish, Count
};

inline constexpr std::size_t CommitSplitPhaseCount = static_cast<std::size_t>(CommitSplitPhase::Count);
// 0 = draw that opened its render pass, 1 = draw that reused it, 2 = everything
// else (synchronous draws, early exits, DrawWithRecipe).
inline constexpr std::size_t CommitSplitPopulations = 3;
inline constexpr std::uint32_t CommitSplitFlushDraws = 64;

struct CommitSplitV2Counters {
    std::atomic<std::uint64_t> ns[CommitSplitPopulations][CommitSplitPhaseCount]{};
    std::atomic<std::uint64_t> count[CommitSplitPopulations][CommitSplitPhaseCount]{};
    std::atomic<std::uint64_t> draws[CommitSplitPopulations]{};
    std::atomic<std::uint64_t> totalNs[CommitSplitPopulations]{};
};

inline CommitSplitV2Counters& commitSplitV2Counters() {
    static CommitSplitV2Counters counters;
    return counters;
}

struct CommitSplitPending {
    std::uint64_t ns[CommitSplitPopulations][CommitSplitPhaseCount]{};
    std::uint64_t count[CommitSplitPopulations][CommitSplitPhaseCount]{};
    std::uint64_t draws[CommitSplitPopulations]{};
    std::uint64_t totalNs[CommitSplitPopulations]{};
    std::uint32_t drawsPending = 0;
    void Flush() {
        auto& counters = commitSplitV2Counters();
        for (std::size_t population = 0; population < CommitSplitPopulations; ++population) {
            counters.draws[population].fetch_add(draws[population], std::memory_order_relaxed);
            counters.totalNs[population].fetch_add(totalNs[population], std::memory_order_relaxed);
            for (std::size_t phase = 0; phase < CommitSplitPhaseCount; ++phase) {
                counters.ns[population][phase].fetch_add(ns[population][phase], std::memory_order_relaxed);
                counters.count[population][phase].fetch_add(count[population][phase], std::memory_order_relaxed);
            }
        }
        *this = CommitSplitPending{};
    }
};

inline CommitSplitPending& commitSplitPending() {
    static thread_local CommitSplitPending pending;
    return pending;
}

struct CommitSplitState {
    std::uint64_t started = 0;
    std::uint64_t marks[CommitSplitPhaseCount]{};
    std::uint32_t reached = 0;
    bool active = false;
    int depth = 0; // Graphics::Draw recurses; only the outermost call measures
    int path = 0;  // 0 = Draw, 1 = DrawWithRecipe
};

inline CommitSplitState& commitSplitState() {
    static thread_local CommitSplitState state;
    return state;
}

inline std::uint64_t CommitSplitNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void CommitSplitStart(int path) {
    if (!CommitSplitEnabled()) return;
    auto& state = commitSplitState();
    if (state.depth++ != 0) return;
    state = CommitSplitState{};
    state.depth = 1;
    state.path = path;
    state.active = true;
    state.started = CommitSplitNow();
}

inline void CommitSplitMark(CommitSplitPhase phase) {
    auto& state = commitSplitState();
    if (!state.active || state.depth != 1) return;
    const auto bit = std::uint32_t{1} << static_cast<std::uint32_t>(phase);
    if ((state.reached & bit) != 0) return;
    state.marks[static_cast<std::size_t>(phase)] = CommitSplitNow();
    state.reached |= bit;
}

inline bool CommitSplitReached(const CommitSplitState& state, CommitSplitPhase phase) {
    return (state.reached & (std::uint32_t{1} << static_cast<std::uint32_t>(phase))) != 0;
}

inline std::uint64_t CommitSplitMarkAt(const CommitSplitState& state, CommitSplitPhase phase) {
    return state.marks[static_cast<std::size_t>(phase)];
}

// Version 1 keeps its original marks and formulas, so a run stays comparable with
// the 22.6-23.4 us baseline; it is now expressed with the version 2 mark array.
inline void CommitSplitMarkBegin() { CommitSplitMark(CommitSplitPhase::PreBegin); }
inline void CommitSplitMarkSets() { CommitSplitMark(CommitSplitPhase::DescBind); }
inline void CommitSplitMarkLds() { CommitSplitMark(CommitSplitPhase::LdsBind); }
inline void CommitSplitMarkRecord() { CommitSplitMark(CommitSplitPhase::Push); }
inline void CommitSplitMarkDraw() { CommitSplitMark(CommitSplitPhase::Record); }

inline void CommitSplitFinish() {
    if (!CommitSplitEnabled()) return;
    auto& state = commitSplitState();
    if (state.depth <= 0) return;
    if (--state.depth > 0) return;
    if (!state.active) return;
    state.active = false;
    const auto end = CommitSplitNow();
    const auto total = end - state.started;
    auto& counters = commitSplitCounters();
    const bool hasBegin = CommitSplitReached(state, CommitSplitPhase::PreBegin);
    const bool hasSets = CommitSplitReached(state, CommitSplitPhase::DescBind);
    const bool hasLds = CommitSplitReached(state, CommitSplitPhase::LdsBind);
    const bool hasRecord = CommitSplitReached(state, CommitSplitPhase::Push);
    const bool hasDraw = CommitSplitReached(state, CommitSplitPhase::Record);

    // Version 1 line: unchanged formulas, including its own classification rules.
    if (!hasRecord || !hasDraw || CommitSplitMarkAt(state, CommitSplitPhase::Push) < state.started || CommitSplitMarkAt(state, CommitSplitPhase::Record) < CommitSplitMarkAt(state, CommitSplitPhase::Push) || end < CommitSplitMarkAt(state, CommitSplitPhase::Record)) {
        counters.unclassifiedNs.fetch_add(total, std::memory_order_relaxed);
        counters.unclassified.fetch_add(1, std::memory_order_relaxed);
        if (state.path == 1) counters.recipeUnclassified.fetch_add(1, std::memory_order_relaxed);
    } else {
        const auto startOfRecord = hasBegin ? CommitSplitMarkAt(state, CommitSplitPhase::PreBegin) : state.started;
        const auto markSets = CommitSplitMarkAt(state, CommitSplitPhase::DescBind);
        const auto markLds = CommitSplitMarkAt(state, CommitSplitPhase::LdsBind);
        const auto markRecord = CommitSplitMarkAt(state, CommitSplitPhase::Push);
        const auto markDraw = CommitSplitMarkAt(state, CommitSplitPhase::Record);
        if (!hasBegin) counters.noBegin.fetch_add(1, std::memory_order_relaxed);
        if (markRecord < startOfRecord || !hasSets || !hasLds || markSets < startOfRecord || markLds < markSets || markRecord < markLds) {
            counters.unclassifiedNs.fetch_add(total, std::memory_order_relaxed);
            counters.unclassified.fetch_add(1, std::memory_order_relaxed);
            if (state.path == 1) counters.recipeUnclassified.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto setup = startOfRecord - state.started;
            const auto begin = markSets - startOfRecord;
            const auto sets = markLds - markSets;
            const auto push = markRecord - markLds;
            const auto record = markDraw - markRecord;
            const auto finish = end - markDraw;
            counters.setupNs.fetch_add(setup, std::memory_order_relaxed);
            counters.beginNs.fetch_add(begin, std::memory_order_relaxed);
            counters.setsNs.fetch_add(sets, std::memory_order_relaxed);
            counters.pushNs.fetch_add(push, std::memory_order_relaxed);
            counters.recordNs.fetch_add(record, std::memory_order_relaxed);
            counters.finishNs.fetch_add(finish, std::memory_order_relaxed);
            counters.classifiedNs.fetch_add(total, std::memory_order_relaxed);
            counters.classified.fetch_add(1, std::memory_order_relaxed);
            if (state.path == 1) counters.recipeClassified.fetch_add(1, std::memory_order_relaxed);
            if (setup + begin + sets + push + record + finish > total) counters.violations.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Version 2: forward differences from the last reached mark. The population is
    // known only here: a reused pass skips every mark of the new-pass block.
    const std::size_t population = hasSets ? (hasBegin ? 0u : 1u) : 2u;
    auto& pending = commitSplitPending();
    ++pending.draws[population];
    pending.totalNs[population] += total;
    std::uint64_t previous = state.started;
    for (std::size_t phase = 0; phase < static_cast<std::size_t>(CommitSplitPhase::Finish); ++phase) {
        if ((state.reached & (std::uint32_t{1} << phase)) == 0) continue;
        const auto at = state.marks[phase];
        if (at > previous) {
            pending.ns[population][phase] += at - previous;
            previous = at;
        }
        ++pending.count[population][phase];
    }
    if (end > previous) pending.ns[population][static_cast<std::size_t>(CommitSplitPhase::Finish)] += end - previous;
    ++pending.count[population][static_cast<std::size_t>(CommitSplitPhase::Finish)];
    if (++pending.drawsPending >= CommitSplitFlushDraws) pending.Flush();
}

// Scope guard: starts at the entry of Graphics::Draw / DrawWithRecipe and always
// finishes, including the early returns and the exception path.
struct CommitSplitScope {
    explicit CommitSplitScope(int path) { CommitSplitStart(path); }
    ~CommitSplitScope() { CommitSplitFinish(); }
    CommitSplitScope(const CommitSplitScope&) = delete;
    CommitSplitScope& operator=(const CommitSplitScope&) = delete;
};

enum MovedSizeBucket : std::size_t {
    MovedSize0 = 0, MovedSize1, MovedSize2to4, MovedSize5to8, MovedSize9to16, MovedSize17to32, MovedSize33plus, MovedSizeBucketCount
};

inline std::size_t MovedSizeBucketOf(std::size_t size) {
    if (size == 0) return MovedSize0;
    if (size == 1) return MovedSize1;
    if (size <= 4) return MovedSize2to4;
    if (size <= 8) return MovedSize5to8;
    if (size <= 16) return MovedSize9to16;
    if (size <= 32) return MovedSize17to32;
    return MovedSize33plus;
}

inline bool MovedStatsEnabled() {
    static const bool enabled = std::getenv("APS5_MOVED_STATS") != nullptr;
    return enabled;
}

inline bool MovedStatsReportEnabled() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr;
    return profile || stats || MovedStatsEnabled();
}

struct MovedStatsAggregate {
    std::uint64_t calls = 0, earlyExit = 0, loopCalls = 0, movedEmpty = 0, movedNonEmpty = 0, comparisons = 0;
    std::uint64_t allocations[MovedSizeBucketCount]{}, moved[MovedSizeBucketCount]{};
    std::uint64_t maxAllocations = 0, maxMoved = 0;
};

struct MovedStatsState {
    std::mutex mutex;
    MovedStatsAggregate aggregate;
};

inline MovedStatsState& movedStatsState() {
    static MovedStatsState state;
    return state;
}

inline void PublishMovedStats(const MovedStatsAggregate& batch) {
    auto& state = movedStatsState();
    std::lock_guard lock(state.mutex);
    auto& aggregate = state.aggregate;
    aggregate.calls += batch.calls;
    aggregate.earlyExit += batch.earlyExit;
    aggregate.loopCalls += batch.loopCalls;
    aggregate.movedEmpty += batch.movedEmpty;
    aggregate.movedNonEmpty += batch.movedNonEmpty;
    aggregate.comparisons += batch.comparisons;
    for (std::size_t bucket = 0; bucket < MovedSizeBucketCount; ++bucket) {
        aggregate.allocations[bucket] += batch.allocations[bucket];
        aggregate.moved[bucket] += batch.moved[bucket];
    }
    if (batch.maxAllocations > aggregate.maxAllocations) aggregate.maxAllocations = batch.maxAllocations;
    if (batch.maxMoved > aggregate.maxMoved) aggregate.maxMoved = batch.maxMoved;
}

inline MovedStatsAggregate TakeMovedStats() {
    auto& state = movedStatsState();
    std::lock_guard lock(state.mutex);
    const auto taken = state.aggregate;
    state.aggregate = MovedStatsAggregate{};
    return taken;
}

inline void ResetMovedStats() {
    auto& state = movedStatsState();
    std::lock_guard lock(state.mutex);
    state.aggregate = MovedStatsAggregate{};
}

inline constexpr std::uint32_t MovedStatsFlushCalls = 64;

struct MovedStatsPending {
    MovedStatsAggregate batch;
    std::uint32_t pending = 0;
    void Flush();
};

inline MovedStatsPending& movedStatsPending() {
    static thread_local MovedStatsPending pending;
    return pending;
}

inline void MovedStatsPending::Flush() {
    PublishMovedStats(batch);
    batch = MovedStatsAggregate{};
    pending = 0;
}

inline void RecordMovedStats(bool earlyExit, std::size_t allocationsSize, std::size_t movedSize, std::size_t comparisons) {
    auto& pending = movedStatsPending();
    auto& batch = pending.batch;
    ++batch.calls;
    batch.comparisons += comparisons;
    if (earlyExit) {
        ++batch.earlyExit;
    } else {
        ++batch.loopCalls;
        ++(movedSize == 0 ? batch.movedEmpty : batch.movedNonEmpty);
        ++batch.allocations[MovedSizeBucketOf(allocationsSize)];
        ++batch.moved[MovedSizeBucketOf(movedSize)];
        if (allocationsSize > batch.maxAllocations) batch.maxAllocations = allocationsSize;
        if (movedSize > batch.maxMoved) batch.maxMoved = movedSize;
    }
    if (++pending.pending >= MovedStatsFlushCalls) pending.Flush();
}

// Last bind state per recording thread and command buffer. Bindings persist only
// inside a command buffer, so a different command buffer invalidates everything.
struct LastBoundState {
    const void* commandBuffer = nullptr;
    const void* pipeline = nullptr;
    const void* set0 = nullptr;
    const void* setFragment = nullptr;
    const void* setLayout = nullptr;
    std::uint64_t pushHash = 0;
    bool hasPipeline = false;
    bool hasSet0 = false;
    bool hasSetFragment = false;
    bool hasPush = false;
};

inline LastBoundState& lastBoundState() {
    static thread_local LastBoundState state;
    return state;
}

inline void NoteCommandBuffer(const void* commandBuffer) {
    auto& state = lastBoundState();
    if (state.commandBuffer == commandBuffer) return;
    state = LastBoundState{};
    state.commandBuffer = commandBuffer;
}

inline std::uint64_t Fnv1a(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

// Records what the next bind will look like and counts a repetition when the exact
// same value is already bound in this command buffer. Nothing is skipped here.

// Counts the call and the repetition. Never skips: the bind sites outside
// Pipeline::Continue (second channel, back faces, presentation, compute) would
// leave this cache stale, and a naive skip crashes the title.
inline void NotePipelineBind(const void* commandBuffer, const void* handle) {
    NoteCommandBuffer(commandBuffer);
    auto& state = lastBoundState();
    auto& counters = commandCallCounters();
    if (state.hasPipeline && state.pipeline == handle) counters.pipelineBindsRepeated.fetch_add(1, std::memory_order_relaxed);
    state.pipeline = handle;
    state.hasPipeline = true;
    counters.pipelineBinds.fetch_add(1, std::memory_order_relaxed);
}

inline void NoteDescriptorSetBind(const void* commandBuffer, const void* layout, const void* set, bool fragment) {
    NoteCommandBuffer(commandBuffer);
    auto& state = lastBoundState();
    auto& counters = commandCallCounters();
    if (fragment) {
        if (state.hasSetFragment && state.setFragment == set && state.setLayout == layout) counters.descriptorSetBindsRepeated.fetch_add(1, std::memory_order_relaxed);
        state.setFragment = set;
        state.hasSetFragment = true;
    } else {
        if (state.hasSet0 && state.set0 == set && state.setLayout == layout) counters.descriptorSetBindsRepeated.fetch_add(1, std::memory_order_relaxed);
        state.set0 = set;
        state.hasSet0 = true;
    }
    state.setLayout = layout;
    counters.descriptorSetBinds.fetch_add(1, std::memory_order_relaxed);
}

inline void NotePushConstants(const void* commandBuffer, const void* bytes, std::size_t size, std::size_t calls) {
    NoteCommandBuffer(commandBuffer);
    auto& state = lastBoundState();
    auto& counters = commandCallCounters();
    const auto hash = Fnv1a(bytes, size);
    if (state.hasPush && state.pushHash == hash) counters.pushConstantsRepeated.fetch_add(calls, std::memory_order_relaxed);
    state.pushHash = hash;
    state.hasPush = true;
    counters.pushConstantCalls.fetch_add(calls, std::memory_order_relaxed);
}

} // namespace AgcDriver::Graphics

#endif
