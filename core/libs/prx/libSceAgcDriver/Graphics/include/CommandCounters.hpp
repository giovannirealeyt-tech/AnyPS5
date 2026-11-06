#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDCOUNTERS_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

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

struct CommitSplitState {
    std::uint64_t started = 0, markBegin = 0, markRecord = 0, markDraw = 0;
    bool active = false, hasBegin = false, hasRecord = false, hasDraw = false;
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

inline void CommitSplitMarkBegin() {
    auto& state = commitSplitState();
    if (!state.active || state.depth != 1 || state.hasBegin) return;
    state.markBegin = CommitSplitNow();
    state.hasBegin = true;
}

inline void CommitSplitMarkRecord() {
    auto& state = commitSplitState();
    if (!state.active || state.depth != 1 || state.hasRecord) return;
    state.markRecord = CommitSplitNow();
    state.hasRecord = true;
}

inline void CommitSplitMarkDraw() {
    auto& state = commitSplitState();
    if (!state.active || state.depth != 1 || state.hasDraw) return;
    state.markDraw = CommitSplitNow();
    state.hasDraw = true;
}

inline void CommitSplitFinish() {
    if (!CommitSplitEnabled()) return;
    auto& state = commitSplitState();
    if (state.depth <= 0) return;
    if (--state.depth > 0) return;
    if (!state.active) return;
    state.active = false;
    const auto end = CommitSplitNow();
    auto& counters = commitSplitCounters();
    const auto total = end - state.started;
    if (!state.hasRecord || !state.hasDraw || state.markRecord < state.started || state.markDraw < state.markRecord || end < state.markDraw) {
        counters.unclassifiedNs.fetch_add(total, std::memory_order_relaxed);
        counters.unclassified.fetch_add(1, std::memory_order_relaxed);
        if (state.path == 1) counters.recipeUnclassified.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto startOfRecord = state.hasBegin && state.markBegin >= state.started ? state.markBegin : state.started;
    if (!state.hasBegin) counters.noBegin.fetch_add(1, std::memory_order_relaxed);
    if (state.markRecord < startOfRecord) {
        counters.unclassifiedNs.fetch_add(total, std::memory_order_relaxed);
        counters.unclassified.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto setup = startOfRecord - state.started;
    const auto begin = state.markRecord - startOfRecord;
    const auto record = state.markDraw - state.markRecord;
    const auto finish = end - state.markDraw;
    counters.setupNs.fetch_add(setup, std::memory_order_relaxed);
    counters.beginNs.fetch_add(begin, std::memory_order_relaxed);
    counters.recordNs.fetch_add(record, std::memory_order_relaxed);
    counters.finishNs.fetch_add(finish, std::memory_order_relaxed);
    counters.classifiedNs.fetch_add(total, std::memory_order_relaxed);
    counters.classified.fetch_add(1, std::memory_order_relaxed);
    if (state.path == 1) counters.recipeClassified.fetch_add(1, std::memory_order_relaxed);
    if (setup + begin + record + finish > total) counters.violations.fetch_add(1, std::memory_order_relaxed);
}

// Scope guard: starts at the entry of Graphics::Draw / DrawWithRecipe and always
// finishes, including the early returns and the exception path.
struct CommitSplitScope {
    explicit CommitSplitScope(int path) { CommitSplitStart(path); }
    ~CommitSplitScope() { CommitSplitFinish(); }
    CommitSplitScope(const CommitSplitScope&) = delete;
    CommitSplitScope& operator=(const CommitSplitScope&) = delete;
};

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
