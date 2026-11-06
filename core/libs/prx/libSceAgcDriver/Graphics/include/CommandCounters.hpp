#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMANDCOUNTERS_HPP

#include <atomic>
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
