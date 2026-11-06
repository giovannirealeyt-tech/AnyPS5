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

// APS5_COMMIT_SPLIT=1: coarse split of one draw commit, measured on the committer
// thread without APS5_PROFILE_DRAW (which would disable the handoff). The total is
// taken around Graphics::Draw with an RAII guard, so early returns and exceptions
// stay inside it; the phase marks are inside recordDrawCommandsImpl. A draw whose
// marks were not all reached is counted as unclassified and contributes only to the
// total, which is what makes total == setup + begin + record + finish + unclassified.
struct CommitSplitCounters {
    std::atomic<std::uint64_t> totalNs{0};
    std::atomic<std::uint64_t> setupNs{0};
    std::atomic<std::uint64_t> beginNs{0};
    std::atomic<std::uint64_t> recordNs{0};
    std::atomic<std::uint64_t> finishNs{0};
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> unclassified{0};
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
};

inline CommitSplitState& commitSplitState() {
    static thread_local CommitSplitState state;
    return state;
}

inline std::uint64_t CommitSplitNow() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void CommitSplitStart() {
    auto& state = commitSplitState();
    state = CommitSplitState{};
    if (!CommitSplitEnabled()) return;
    state.active = true;
    state.started = CommitSplitNow();
}

inline void CommitSplitMarkBegin() {
    auto& state = commitSplitState();
    if (!state.active) return;
    state.markBegin = CommitSplitNow();
    state.hasBegin = true;
}

inline void CommitSplitMarkRecord() {
    auto& state = commitSplitState();
    if (!state.active) return;
    state.markRecord = CommitSplitNow();
    state.hasRecord = true;
}

inline void CommitSplitMarkDraw() {
    auto& state = commitSplitState();
    if (!state.active) return;
    state.markDraw = CommitSplitNow();
    state.hasDraw = true;
}

inline void CommitSplitFinish() {
    auto& state = commitSplitState();
    if (!state.active) return;
    state.active = false;
    const auto end = CommitSplitNow();
    auto& counters = commitSplitCounters();
    const auto total = end - state.started;
    counters.totalNs.fetch_add(total, std::memory_order_relaxed);
    if (!state.hasBegin || !state.hasRecord || !state.hasDraw || state.markBegin < state.started || state.markRecord < state.markBegin || state.markDraw < state.markRecord || end < state.markDraw) {
        counters.unclassified.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto setup = state.markBegin - state.started;
    const auto begin = state.markRecord - state.markBegin;
    const auto record = state.markDraw - state.markRecord;
    const auto finish = end - state.markDraw;
    counters.setupNs.fetch_add(setup, std::memory_order_relaxed);
    counters.beginNs.fetch_add(begin, std::memory_order_relaxed);
    counters.recordNs.fetch_add(record, std::memory_order_relaxed);
    counters.finishNs.fetch_add(finish, std::memory_order_relaxed);
    counters.draws.fetch_add(1, std::memory_order_relaxed);
}

// Scope guard: starts at Graphics::Draw entry and always finishes, including the
// early returns and the exception path.
struct CommitSplitScope {
    CommitSplitScope() { CommitSplitStart(); }
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
