#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::uint64_t Span = 128 * Block;
std::uint64_t Base = std::uint64_t{1} << 32;
constexpr std::chrono::seconds Patience{10};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef _WIN32
struct FakeWatch {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> written;
    bool holdNextWalk = false;
    bool holding = false;
    bool released = false;
    bool timedOut = false;
    bool failWalks = false;
    std::size_t maxCoverBytes = 0;
    std::size_t walks = 0;
};

FakeWatch& Fake() {
    static FakeWatch fake;
    return fake;
}
#endif

}

#ifndef _WIN32
namespace GuestWriteWatch {

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix() {
    return true;
}

bool GuestWriteWatchUnregister_nid_postfix(const void*, std::size_t) {
    return true;
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    const auto limit = Fake().maxCoverBytes;
    return address >= Base && bytes <= Span && address - Base <= Span - bytes && (limit == 0 || bytes <= limit);
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
    auto& fake = Fake();
    const auto end = address + bytes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
    {
        std::unique_lock lock(fake.mutex);
        ++fake.walks;
        if (fake.failWalks) return false;
        if (fake.holdNextWalk) {
            fake.holdNextWalk = false;
            fake.holding = true;
            fake.changed.notify_all();
            fake.timedOut = !fake.changed.wait_for(lock, Patience, [&] { return fake.released; });
        }
        for (auto it = fake.written.begin(); it != fake.written.end();) {
            if (it->first < end && address < it->second) {
                runs.emplace_back(std::max(it->first, address), std::min(it->second, end));
                it = fake.written.erase(it);
            } else ++it;
        }
    }
    for (const auto& [from, to] : runs) written(context, from, to);
    return true;
}

}

}
#endif

namespace {

using namespace AgcDriver::GuestMemory;

#ifndef _WIN32
void CheckMemoHitDuringWalk() {
    auto& fake = Fake();
    BumpCollectEpoch();
    const auto collected = CollectWrites(Base, Block);
    Require(collected != 0 && UnchangedSince(Base, Block, collected), "the block was not collected");
    {
        const std::lock_guard lock(fake.mutex);
        fake.written.emplace_back(Base + Page, Base + 2 * Page);
        fake.holdNextWalk = true;
    }
    std::thread walker([] { CollectWritesUncached(Base, 2 * Block); });
    bool holding = false;
    {
        std::unique_lock lock(fake.mutex);
        holding = fake.changed.wait_for(lock, Patience, [&] { return fake.holding; });
    }
    const auto memoized = holding ? CollectWrites(Base, Block) : 0;
    {
        const std::lock_guard lock(fake.mutex);
        fake.released = true;
    }
    fake.changed.notify_all();
    walker.join();
    Require(holding, "the second walk did not reach the write watch");
    Require(!fake.timedOut, "the memoized collect waited for the walk in progress");
    Require(!UnchangedSince(Base + Page, Page, memoized), "a memoized collect returned the generation a walk in progress then stamped a write with");
    BumpCollectEpoch();
    Require(CollectWrites(Base, Block) > memoized && !UnchangedSince(Base, Block, memoized), "a later epoch trusts the write stamped by the walk in progress");
}

std::size_t Walks() {
    auto& fake = Fake();
    const std::lock_guard lock(fake.mutex);
    return fake.walks;
}

void SetFailing(bool failing) {
    auto& fake = Fake();
    const std::lock_guard lock(fake.mutex);
    fake.failWalks = failing;
}

void CheckFailedWalkIsRetried() {
    BumpCollectEpoch();
    const auto address = Base + 11 * Page + 16;
    SetFailing(true);
    Require(CollectWrites(address, Page) == 0, "a failed walk returned a generation");
    SetFailing(false);
    const auto before = Walks();
    Require(CollectWrites(address, Page) != 0 && Walks() > before, "a failed walk was memoized");
}

void Store(std::uint64_t address) {
    auto& fake = Fake();
    const std::lock_guard lock(fake.mutex);
    fake.written.emplace_back(address & ~(Page - 1), (address & ~(Page - 1)) + Page);
}
#else
struct Arena {
    static constexpr std::size_t Bytes = 128 * Block;
    void* memory = nullptr;
    Arena() {
        memory = GuestArena::GuestArenaAllocate_nid_postfix(Bytes, Block);
        Require(memory != nullptr, "the guest arena allocation failed");
        GuestArena::GuestArenaCommit_nid_postfix(memory, Bytes, PAGE_READWRITE, Bytes);
        std::memset(memory, 0x11, Bytes);
        Base = reinterpret_cast<std::uint64_t>(memory);
    }
    ~Arena() {
        GuestArena::GuestArenaReset_nid_postfix(memory, Bytes);
        GuestArena::GuestArenaRelease_nid_postfix(memory, Bytes);
    }
};

void Store(std::uint64_t address) {
    auto* byte = reinterpret_cast<volatile std::uint8_t*>(address);
    *byte = static_cast<std::uint8_t>(*byte + 1);
}
#endif

bool Walked(std::uint64_t address, std::size_t bytes, std::uint64_t probe, bool cached = true) {
    const auto before = TrackerGeneration();
    Store(probe);
    Require((cached ? CollectWrites(address, bytes) : CollectWritesUncached(address, bytes)) != 0, "a collect failed");
    return !UnchangedSince(probe & ~(Page - 1), Page, before);
}

#ifndef _WIN32
void CheckSubBlockCollectThatCannotWidenIsObservedOnce() {
    auto& fake = Fake();
    fake.maxCoverBytes = 4 * Page;
    BumpCollectEpoch();
    const auto address = Base + 40 * Block + 3 * Page + 100;
    const auto probe = Base + 40 * Block + 4 * Page;
    Require(Walked(address, Page, probe), "the first collect did not reach the write watch");
    const auto before = fake.walks;
    Require(!Walked(address, Page, probe), "a sub-block collect that could not widen reached the write watch again in one epoch");
    Require(!Walked(address + 100, 100, probe), "a request inside the exact observation reached the write watch");
    Require(fake.walks == before, "the write watch was walked for an observed exact range");
    Require(Walked(address, 3 * Page, Base + 40 * Block + 6 * Page), "a request reaching past the exact observation was answered from it");
    Unwatch(Base + 90 * Block, Page);
    Require(Walked(address, Page, probe), "an unwatch did not invalidate the exact observation");
    BumpCollectEpoch();
    Require(Walked(address, Page, probe), "a new epoch inherited the exact observation");
    fake.maxCoverBytes = 0;
}
#endif

void CheckUnalignedRepeatReachesKernelOnce() {
    BumpCollectEpoch();
    const auto address = Base + 3 * Page + 100;
    const auto probe = Base + 5 * Page;
    Require(Walked(address, 2 * Page, probe), "the first collect did not reach the write watch");
    for (std::uint64_t i = 0; i < 70; ++i) Require(CollectWrites(Base + (16 + i) * Block + 5 * Page + 8, Page) != 0, "a filler collect failed");
    Require(!Walked(address, 2 * Page, probe), "an identical unaligned request in the same epoch reached the write watch again");
    Require(!Walked(Base + 9 * Page, Page, probe), "a request inside the widened block reached the write watch again");
}

void CheckUncachedWalksKeepCachedEntries() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 2 * Block) != 0, "the large collect failed");
    const auto sub = Base + Block + Page;
    Require(!Walked(sub, Page, sub), "a request inside the large walk reached the write watch");
    for (int i = 0; i < 80; ++i) Require(CollectWritesUncached(Base, Page) != 0, "an uncached collect failed");
    Require(!Walked(sub, Page, sub), "uncached walks evicted a cached entry");
}

void CheckNeighbouringBlockStoreIsFound() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 24 * Block + 5 * Page + 8, Page) != 0, "the block was not collected");
    const auto across = Base + 25 * Block - Page - 100;
    Require(Walked(across, 2 * Page, Base + 25 * Block + 3 * Page), "a request reaching past the retained block trusted it");
}

void CheckInvalidationReachesBlockKeyedEntry() {
    BumpCollectEpoch();
    const auto address = Base + 30 * Block + 3 * Page + 100;
    const auto probe = Base + 30 * Block + 5 * Page;
    Require(Walked(address, Page, probe), "the first collect did not reach the write watch");
    Require(!Walked(address, Page, probe), "the retained entry was not reused");
    Unwatch(Base + 90 * Block, Page);
    Require(Walked(address, Page, probe), "an unwatch did not invalidate the retained entry");
}

void CheckThreadWithoutEpochWalksEveryTime() {
    bool first = false;
    bool second = false;
    const auto address = Base + 35 * Block + 3 * Page + 100;
    std::thread worker([&] {
        first = Walked(address, Page, address);
        second = Walked(address, Page, address);
    });
    worker.join();
    Require(first && second, "a thread that never bumped its epoch reused an entry");
}

void CheckStoreInNewEpochIsFound() {
    BumpCollectEpoch();
    const auto address = Base + 7 * Page + 8;
    Require(Walked(address, Page, address), "the first collect did not reach the write watch");
    BumpCollectEpoch();
    Require(Walked(address, Page, address), "a store in a new epoch was not found");
}

struct Pause {
    std::mutex mutex;
    std::condition_variable changed;
    int point = 0;
    bool reached = false;
    bool released = false;
    bool timedOut = false;
    std::atomic<std::size_t> walks{0};
};

Pause& PauseState() {
    static Pause pause;
    return pause;
}

thread_local bool PauseThisThread = false;

void Hook(int point) {
    auto& pause = PauseState();
    if (point == 1) pause.walks.fetch_add(1);
    if (!PauseThisThread || point != pause.point) return;
    std::unique_lock lock(pause.mutex);
    pause.reached = true;
    pause.changed.notify_all();
    if (!pause.changed.wait_for(lock, Patience, [&] { return pause.released; })) pause.timedOut = true;
}

std::size_t OsWalks() {
    return PauseState().walks.load();
}

class PausedWalk {
public:
    PausedWalk(int point, std::function<void()> work) {
        auto& pause = PauseState();
        {
            const std::lock_guard lock(pause.mutex);
            pause.point = point;
            pause.reached = false;
            pause.released = false;
            pause.timedOut = false;
        }
        thread = std::thread([work = std::move(work)] {
            PauseThisThread = true;
            work();
            PauseThisThread = false;
        });
        std::unique_lock lock(pause.mutex);
        Require(pause.changed.wait_for(lock, Patience, [&] { return pause.reached; }), "the walk did not reach its pause point");
    }
    void Release() {
        auto& pause = PauseState();
        {
            const std::lock_guard lock(pause.mutex);
            pause.released = true;
        }
        pause.changed.notify_all();
        if (thread.joinable()) thread.join();
        Require(!pause.timedOut, "a paused walk timed out");
    }
    ~PausedWalk() {
        if (thread.joinable()) Release();
    }
private:
    std::thread thread;
};

class Waiter {
public:
    Waiter(bool bump, std::uint64_t address, std::size_t bytes) {
        thread = std::thread([this, bump, address, bytes] {
            if (bump) BumpCollectEpoch();
            started.store(true);
            while (!go.load()) std::this_thread::yield();
            result = CollectWrites(address, bytes);
            finished.store(true);
        });
        while (!started.load()) std::this_thread::yield();
    }
    void Go() {
        go.store(true);
    }
    bool Finished(std::chrono::milliseconds wait) {
        const auto until = std::chrono::steady_clock::now() + wait;
        while (!finished.load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return finished.load();
    }
    std::uint64_t Join() {
        if (thread.joinable()) thread.join();
        return result;
    }
    ~Waiter() {
        go.store(true);
        if (thread.joinable()) thread.join();
    }
private:
    std::thread thread;
    std::atomic<bool> started{false};
    std::atomic<bool> go{false};
    std::atomic<bool> finished{false};
    std::uint64_t result = 0;
};

void CheckOwnersShareOneWalk() {
    BumpCollectEpoch();
    const auto before = OsWalks();
    Require(CollectWrites(Base + 41 * Block + 3 * Page + 100, Block + 2 * Page) != 0, "the first owner failed");
    Require(OsWalks() == before + 1, "the first owner did not walk once");
    Require(CollectWrites(Base + 41 * Block, Block) != 0, "the second owner failed");
    Require(CollectWrites(Base + 42 * Block + 9 * Page, Page) != 0, "the third owner failed");
    Require(CollectWrites(Base + 41 * Block + 20 * Page + 1, 3 * Page) != 0, "the fourth owner failed");
    Require(OsWalks() == before + 1, "owners with different ranges over the same blocks walked again in one epoch");
}

void CheckRunsAreWalkedOnce() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 44 * Block + 5 * Page, Page) != 0, "the first block failed");
    Require(CollectWrites(Base + 46 * Block + 5 * Page, Page) != 0, "the third block failed");
    const auto before = OsWalks();
    Require(CollectWrites(Base + 44 * Block, 3 * Block) != 0, "the spanning request failed");
    Require(OsWalks() == before + 1, "a request over two observed blocks and one new one did not walk exactly the new run");
    Require(CollectWrites(Base + 44 * Block, 3 * Block) != 0 && OsWalks() == before + 1, "the spanning request was walked again in the same epoch");
}

void CheckLaterThreadReusesWalk() {
    std::atomic<int> stage{0};
    const auto address = Base + 48 * Block;
    std::thread follower([&] {
        BumpCollectEpoch();
        stage.store(1);
        while (stage.load() != 2) std::this_thread::yield();
        const auto before = OsWalks();
        Require(CollectWrites(address, Block) != 0, "the follower's collect failed");
        stage.store(OsWalks() == before ? 3 : 4);
    });
    while (stage.load() != 1) std::this_thread::yield();
    BumpCollectEpoch();
    Require(CollectWrites(address, Block) != 0, "the later epoch's collect failed");
    stage.store(2);
    follower.join();
    Require(stage.load() == 3, "a walk made after another thread's ordering point did not answer that thread");
}

void CheckEarlierWalkDoesNotAnswerLaterEpoch() {
    const auto address = Base + 49 * Block + 2 * Page;
    BumpCollectEpoch();
    Require(CollectWrites(address, Page) != 0, "the first collect failed");
    const auto observed = TrackerGeneration();
    std::atomic<bool> found{false};
    std::thread other([&] {
        Store(address);
        BumpCollectEpoch();
        const auto before = OsWalks();
        Require(CollectWrites(address, Page) != 0, "the other thread's collect failed");
        found.store(OsWalks() > before && !UnchangedSince(address & ~(Page - 1), Page, observed));
    });
    other.join();
    Require(found.load(), "a walk made before another thread's ordering point answered it and hid a store");
}

void CheckHitDuringWalkReturnsStable() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 50 * Block, Block) != 0, "the block was not collected");
    const auto page = Base + 52 * Block + 3 * Page;
    Store(page);
    std::uint64_t hit = 0;
    {
        PausedWalk walker(1, [&] { CollectWritesUncached(Base + 52 * Block, Block); });
        hit = CollectWrites(Base + 50 * Block, Block);
        walker.Release();
    }
    Require(hit != 0, "a hit during a walk in progress failed");
    Require(!UnchangedSince(page & ~(Page - 1), Page, hit), "a hit during a walk in progress returned a generation the walk stamped a write with");
}

void CheckNoHitBeforePublication(int point) {
    const auto address = Base + 53 * Block + 7 * Page;
    const auto before = OsWalks();
    Waiter waiter(true, address, Page);
    {
        PausedWalk walker(point, [&] { CollectWritesUncached(Base + 53 * Block, Block); });
        waiter.Go();
        Require(!waiter.Finished(std::chrono::milliseconds(150)), "an unpublished walk answered a request");
        walker.Release();
    }
    Require(waiter.Join() != 0, "the waiting request failed");
    Require(OsWalks() == before + 1, "the request after the walk walked again or the walk never reached the write watch");
}

void CheckWaiterNeedingLaterTickWalks() {
    const auto page = Base + 57 * Block + 4 * Page;
    BumpCollectEpoch();
    const auto observed = TrackerGeneration();
    const auto before = OsWalks();
    std::unique_ptr<Waiter> late;
    {
        PausedWalk walker(2, [&] { CollectWritesUncached(Base + 57 * Block, Block); });
        Store(page);
        late = std::make_unique<Waiter>(true, page, Page);
        late->Go();
        Require(!late->Finished(std::chrono::milliseconds(150)), "a request waiting on a walk made before its ordering point did not wait");
        walker.Release();
    }
    Require(late->Join() != 0, "the waiting request failed");
    Require(OsWalks() == before + 2, "a request with an epoch newer than the walk accepted it");
    Require(!UnchangedSince(page & ~(Page - 1), Page, observed), "a store after the walk and before the ordering point was not found");
}

void CheckMappingChangeForcesWalk() {
    BumpCollectEpoch();
    const auto address = Base + 60 * Block + 3 * Page;
    Require(CollectWrites(address, Page) != 0, "the first collect failed");
    const auto before = OsWalks();
    Require(CollectWrites(address, Page) != 0 && OsWalks() == before, "the retained walk was not reused");
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(Base + 100 * Block), Page);
    Require(CollectWrites(address, Page) != 0 && OsWalks() == before + 1, "a mapping change did not force a walk");
    Require(CollectWrites(address, Page) != 0 && OsWalks() == before + 1, "the walk after a mapping change was not retained");
}

void CheckFailedRequestPublishesNothing() {
    BumpCollectEpoch();
#ifdef _WIN32
    const auto address = Base + 127 * Block;
    Require(Observe(address, 3 * Block).status != Observation::Status::Complete, "a request past the committed range completed");
#else
    const auto address = Base + 94 * Block;
    Require(Observe(Base + Span - Page, 2 * Page).status == Observation::Status::Uncovered, "a request past the covered range was not reported uncovered");
    SetFailing(true);
    Require(Observe(address, 2 * Block).status == Observation::Status::Failed, "a failing walk did not report failure");
    SetFailing(false);
#endif
    const auto before = OsWalks();
    Require(CollectWrites(address + Page, Page) != 0 && OsWalks() == before + 1, "a block of an incomplete request was answered without a walk");
}

void CheckPartialGroupIsNotAHit() {
    for (std::uint64_t offset = 0; offset < 64; ++offset) {
        BumpCollectEpoch();
        Require(CollectWrites(Base, 127 * Block) != 0, "the whole range failed");
        BumpCollectEpoch();
        Require(CollectWrites(Base + 63 * Block + Page, Page) != 0, "the single block failed");
        const auto before = OsWalks();
        Require(CollectWrites(Base + offset * Block, 64 * Block) != 0, "the group request failed");
        Require(OsWalks() > before, "a group with one fresh block was answered whole");
    }
}

void CheckInteriorCoverageAfterRingEviction() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 8 * Block) != 0, "the covering collection failed");
    for (std::uint64_t i = 0; i < 70; ++i) Require(CollectWrites(Base + (16 + i) * Block, Block) != 0, "a ring eviction collection failed");
    const auto address = Base + 3 * Block + 3 * Page + 100;
    Require(!Walked(address, 2 * Page, address + Page), "an interior range lost its completed observation after ring eviction");
}

void CheckAdjacentCoverage() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 2 * Block, 2 * Block) != 0, "the right collection failed");
    Require(CollectWrites(Base, 2 * Block) != 0, "the left collection failed");
    Require(!Walked(Base + Page, 4 * Block - 2 * Page, Base + Block + Page), "adjacent completed observations were collected again");

    BumpCollectEpoch();
    Require(CollectWrites(Base, Block) != 0, "the first collection failed");
    Require(CollectWrites(Base + 2 * Block, Block) != 0, "the last collection failed");
    Require(CollectWrites(Base + Block, Block) != 0, "the connecting collection failed");
    Require(!Walked(Base, 3 * Block, Base + Block + Page), "a completed connection did not join both observations");
}

void CheckOverlappingCoverage() {
    BumpCollectEpoch();
    Require(CollectWrites(Base + 4 * Block, 4 * Block) != 0, "the right collection failed");
    Require(CollectWrites(Base + 2 * Block, 4 * Block) != 0, "the overlapping collection failed");
    Require(!Walked(Base + 3 * Block, 4 * Block, Base + 5 * Block + Page), "overlapping completed observations were collected again");
    Require(CollectWrites(Base + Block, 8 * Block) != 0, "the containing collection failed");
    Require(!Walked(Base + 2 * Block, 6 * Block, Base + 6 * Block + Page), "a containing observation lost its interior coverage");
}

void CheckCoverageGapsAndInvalidations() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 2 * Block) != 0, "the left collection failed");
    Require(CollectWrites(Base + 3 * Block, 2 * Block) != 0, "the right collection failed");
    Require(Walked(Base + Block, 3 * Block, Base + 2 * Block + Page), "a hole between observations was treated as observed");
    Require(CollectWrites(Base, 0) == 0, "an empty range acquired a coverage proof");
    Require(CollectWrites(std::numeric_limits<std::uint64_t>::max() - 31, 8) == 0, "page rounding wrapped an invalid address into observed memory");
    Require(CollectWrites(std::numeric_limits<std::uint64_t>::max() - 31, 64) == 0, "an overflowing range acquired a coverage proof");

    BumpCollectEpoch();
    const auto probe = Base + 2 * Block + Page;
    Require(Walked(Base + Block, 3 * Block, probe), "a new epoch inherited an old observation");
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(Base + 90 * Block), Page);
    Require(Walked(Base + Block, 3 * Block, probe), "an allocation change retained an old observation");
    Unwatch(Base + 91 * Block, Page);
    Require(Walked(Base + Block, 3 * Block, probe), "an unwatch retained a merged observation");
}

void CheckCoverageThreadAndDriverWrites() {
    BumpCollectEpoch();
    Require(CollectWrites(Base, 4 * Block) != 0, "the covering collection failed");
    bool walked = false;
    std::thread other([&] {
        BumpCollectEpoch();
        walked = Walked(Base + Block, 2 * Block, Base + Block + Page);
    });
    other.join();
    Require(walked, "a second worker inherited the first worker's observation");
    const auto before = TrackerGeneration();
    const auto written = MarkWritten(Base + 2 * Block, Page);
    Require(written > before, "the driver write did not advance the tracker");
    Require(CollectWrites(Base + Block, 2 * Block) >= written, "an observation hid a completed driver write generation");
    Require(!UnchangedSince(Base + 2 * Block, Page, before), "an observation hid a driver write stamp");
    Require(Walked(Base + Block, 2 * Block, Base + Block + Page, false), "an uncached collection reused an observation");
}

void CheckScopePartitionWalkThenMemo() {
    BumpCollectEpoch();
    const auto address = Base + 92 * Block + 3 * Page + 100;
    const auto probe = Base + 92 * Block + 4 * Page + 64;
    EnterTextureCollectScope();
    const auto before = TrackerGeneration();
    Store(probe);
    const auto walked = CollectWrites(address, 2 * Page);
    const auto memoized = CollectWrites(address, 2 * Page);
    const auto stats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(walked != 0 && memoized != 0, "a scoped collect failed");
    Require(!UnchangedSince(probe & ~(Page - 1), Page, before), "the first scoped collect did not reach the write watch");
    Require(stats.calls == 2, "the scoped collect miscounted its calls");
    Require(stats.earlyExitPreWalk == 0, "a clean scoped range took an early exit");
    Require(stats.walkAttempted == 1, "the first scoped collect was not accounted as a walk attempt");
    Require(stats.memoHits == 1, "the memoized scoped collect was not accounted as a memo hit");
    Require(stats.memoHits == stats.memoHitEpochMap + stats.memoHitThread + stats.memoHitShared, "the memo hit sites do not sum to the memo hits");
    Require(stats.walkFailed <= stats.walkAttempted, "the scoped walk failures exceed the walk attempts");
    Require(stats.calls == stats.earlyExitPreWalk + stats.memoHits + stats.walkAttempted, "the scoped collect partition does not hold");
}

void CheckScopeEarlyExit() {
    EnterTextureCollectScope();
    const auto address = Base + 92 * Block + 3 * Page + 100;
    const auto empty = CollectWrites(address, 0);
    const auto wrapped = CollectWrites(std::numeric_limits<std::uint64_t>::max() - 31, 8);
    const auto stats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(empty == 0 && wrapped == 0, "an empty or overflowing scoped range returned a generation");
    Require(stats.calls == 2 && stats.earlyExitPreWalk == 2, "the scoped early exits were miscounted");
    Require(stats.calls == stats.earlyExitPreWalk + stats.memoHits + stats.walkAttempted, "the scoped early exit partition does not hold");
}

void CheckTakeResetsScopeStats() {
    BumpCollectEpoch();
    EnterTextureCollectScope();
    Require(CollectWrites(Base + 93 * Block + 3 * Page, Page) != 0, "the scoped collect before the take failed");
    const auto first = TakeTextureCollectStats();
    const auto second = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(first.calls == 1 && first.walkAttempted == 1, "the first take did not report the scoped collect");
    Require(second.calls == 0, "the take did not reset the scoped stats");
}

void CheckUnscopedCallsDoNotContaminate() {
    BumpCollectEpoch();
    EnterTextureCollectScope();
    Require(CollectWrites(Base + 93 * Block + 3 * Page, Page) != 0, "the scoped collect failed");
    const auto scoped = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(CollectWrites(Base + 93 * Block + 3 * Page, Page) != 0, "the unscoped collect failed");
    const auto unscoped = TakeTextureCollectStats();
    Require(scoped.calls == 1, "the scoped collect was miscounted");
    Require(unscoped.calls == 0, "an unscoped collect contaminated the texture stats");
}

void CheckScopeStatsAreThreadLocal() {
    BumpCollectEpoch();
    std::uint64_t foreign = 0;
    EnterTextureCollectScope();
    std::thread worker([&] {
        EnterTextureCollectScope();
        Require(CollectWrites(Base + 94 * Block + 3 * Page, Page) != 0, "the scoped collect on the worker failed");
        const auto workerStats = TakeTextureCollectStats();
        LeaveTextureCollectScope();
        foreign = workerStats.calls;
    });
    worker.join();
    Require(CollectWrites(Base + 92 * Block + 7 * Page, Page) != 0, "the scoped collect on the main thread failed");
    const auto local = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(foreign == 1, "the worker scope miscounted its collects");
    Require(local.calls == 1, "the main thread scope counted the worker's collects");
    Require(local.calls == local.earlyExitPreWalk + local.memoHits + local.walkAttempted, "the main thread partition does not hold");
}

void CheckScopedCollectMatchesUnscoped() {
    BumpCollectEpoch();
    const auto address = Base + 92 * Block + 3 * Page + 100;
    const auto probe = Base + 92 * Block + 4 * Page + 64;
    const auto beforeScoped = TrackerGeneration();
    Store(probe);
    EnterTextureCollectScope();
    const auto scoped = CollectWrites(address, 2 * Page);
    const auto scopedStats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    BumpCollectEpoch();
    const auto address2 = Base + 93 * Block + 3 * Page + 100;
    const auto probe2 = Base + 93 * Block + 4 * Page + 64;
    const auto beforeUnscoped = TrackerGeneration();
    Store(probe2);
    const auto unscoped = CollectWrites(address2, 2 * Page);
    Require(scoped != 0 && unscoped != 0, "a scoped or unscoped collect failed");
    Require(scopedStats.walkAttempted == 1 && scopedStats.memoHits == 0, "the scoped collect was not a clean walk");
    Require(!UnchangedSince(probe & ~(Page - 1), Page, beforeScoped), "the scoped collect did not reach the write watch");
    Require(!UnchangedSince(probe2 & ~(Page - 1), Page, beforeUnscoped), "the unscoped collect did not reach the write watch");
}

void CheckNestedScopeTake() {
    BumpCollectEpoch();
    EnterTextureCollectScope();
    Require(CollectWrites(Base + 94 * Block + 3 * Page, Page) != 0, "the first scoped collect failed");
    EnterTextureCollectScope();
    const auto nested = TakeTextureCollectStats();
    Require(nested.calls == 1, "a second enter reset the accumulated scoped stats");
    Require(CollectWrites(Base + 94 * Block + 5 * Page, Page) != 0, "the scoped collect while the depth was two failed");
    LeaveTextureCollectScope();
    const auto outer = TakeTextureCollectStats();
    const auto after = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(outer.calls == 1, "the take while the depth was two miscounted");
    Require(after.calls == 0, "the stats survived the take while the depth was two");
}

// Test-only clock and gate hooks for the FT17-C timing checks: an injectable monotonic ns
// clock (a steady advance per reading) and the gate override, both restored on scope exit so
// the other checks keep the production clock and the env gate.
struct TextureTestClock {
    std::uint64_t now = 1000;
    std::uint64_t step = 0;
    std::uint64_t readings = 0;
    bool retrogradeSoon = false;
    bool retrogradeNext = false;
};

TextureTestClock textureTestClock;

std::uint64_t TextureTestClockNow() {
    auto& clock = textureTestClock;
    ++clock.readings;
    const auto retrograde = clock.retrogradeNext;
    clock.retrogradeNext = false;
    if (clock.retrogradeSoon) {
        clock.retrogradeSoon = false;
        clock.retrogradeNext = true;
    }
    if (retrograde) {
        clock.now = clock.now > 0 ? clock.now - 1 : 0;
        return clock.now;
    }
    const auto reported = clock.now;
    clock.now += clock.step;
    return reported;
}

struct TextureTimingHooks {
    bool enabled = false;
    TextureTimingHooks() = default;
    explicit TextureTimingHooks(bool on) : enabled(on) {
        textureTestClock = TextureTestClock{};
        detail::SetTextureCollectTestClock(&TextureTestClockNow);
        detail::SetTextureCollectTimingOverride(&enabled);
    }
    ~TextureTimingHooks() {
        detail::SetTextureCollectTimingOverride(nullptr);
        detail::SetTextureCollectTestClock(nullptr);
    }
};

void CheckTimingGateOffTakesNoClock() {
    TextureTimingHooks off;
    BumpCollectEpoch();
    EnterTextureCollectScope();
    const auto address = Base + 92 * Block + 3 * Page + 100;
    const auto walked = CollectWrites(address, Page);
    const auto empty = CollectWrites(address, 0);
    const auto stats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(walked != 0 && empty == 0, "the off gate changed the scoped collect's outcome");
    Require(stats.calls == 2 && stats.walkAttempted == 1 && stats.earlyExitPreWalk == 1, "the off gate changed the outcome partition");
    Require(stats.nsMemoHit == 0 && stats.nsWalk == 0 && stats.nsEarlyExit == 0, "the off gate accumulated a timing bucket");
    Require(textureTestClock.readings == 0, "the off gate took a clock reading");
    Require(stats.collectClockViolations == 0, "the off gate counted a clock violation");
}

void CheckTimingBucketsPerReturnCategory() {
    TextureTimingHooks on(true);
    textureTestClock.step = 1000;
    BumpCollectEpoch();
    const auto address = Base + 92 * Block + 3 * Page + 100;
    const auto probe = Base + 92 * Block + 4 * Page + 64;
    EnterTextureCollectScope();
    const auto before = TrackerGeneration();
    Store(probe);
    const auto walked = CollectWrites(address, 2 * Page);
    const auto memoized = CollectWrites(address, 2 * Page);
    const auto empty = CollectWrites(address, 0);
    const auto stats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(walked != 0 && memoized != 0 && empty == 0, "the on gate changed the scoped collect's outcome");
    Require(!UnchangedSince(probe & ~(Page - 1), Page, before), "the on gate changed the scoped collect's write watch behaviour");
    Require(stats.calls == 3, "the on gate miscounted the scoped calls");
    Require(stats.walkAttempted == 1 && stats.memoHits == 1 && stats.earlyExitPreWalk == 1, "the on gate changed the outcome partition");
    Require(stats.calls == stats.earlyExitPreWalk + stats.memoHits + stats.walkAttempted, "the partition does not hold with timing on");
    Require(stats.nsWalk == 1000, "the walk was not timed in its own bucket");
    Require(stats.nsMemoHit == 1000, "the memo hit was not timed in its own bucket");
    Require(stats.nsEarlyExit == 1000, "the early exit was not timed in its own bucket");
    Require(stats.collectClockViolations == 0, "a monotonic test clock raised a violation");
    Require(textureTestClock.readings == 6, "the on gate did not read the clock twice per call");
}

void CheckTimingTakeResetsBuckets() {
    TextureTimingHooks on(true);
    textureTestClock.step = 500;
    BumpCollectEpoch();
    EnterTextureCollectScope();
    Require(CollectWrites(Base + 93 * Block + 3 * Page, Page) != 0, "the scoped collect with timing on failed");
    const auto first = TakeTextureCollectStats();
    const auto second = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(first.calls == 1 && first.nsWalk == 500, "the first take lost the timing bucket");
    Require(second.calls == 0 && second.nsWalk == 0, "the take did not reset the timing buckets");
}

void CheckTimingClockViolation() {
    TextureTimingHooks on(true);
    textureTestClock.retrogradeSoon = true;
    BumpCollectEpoch();
    EnterTextureCollectScope();
    const auto empty = CollectWrites(Base + 92 * Block + 3 * Page + 100, 0);
    const auto stats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(empty == 0, "a retrograde clock changed the scoped early exit's return");
    Require(stats.calls == 1 && stats.earlyExitPreWalk == 1, "a retrograde clock changed the outcome partition");
    Require(stats.collectClockViolations == 1, "a retrograde clock was not counted");
    Require(stats.nsEarlyExit == 0, "a retrograde clock leaked time into a bucket");
}

void CheckTimingNestedScopeAccumulates() {
    TextureTimingHooks on(true);
    textureTestClock.step = 250;
    BumpCollectEpoch();
    EnterTextureCollectScope();
    Require(CollectWrites(Base + 94 * Block + 3 * Page, Page) != 0, "the first scoped collect with timing failed");
    EnterTextureCollectScope();
    const auto nested = TakeTextureCollectStats();
    Require(nested.calls == 1 && nested.nsWalk == 250, "a second enter reset the accumulated timing buckets");
    // A different block: the first walk's block-widened memo entry must not cover this range,
    // so the nested call really walks rather than memo-hitting.
    Require(CollectWrites(Base + 95 * Block + 5 * Page, Page) != 0, "the scoped collect while the depth was two failed");
    LeaveTextureCollectScope();
    const auto outer = TakeTextureCollectStats();
    const auto after = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(outer.calls == 1 && outer.nsWalk == 250, "the take while the depth was two misattributed the timing buckets");
    Require(after.calls == 0 && after.nsWalk == 0, "the timing buckets survived the take while the depth was two");
}

// FT17-L: a memo probe whose mapping recheck mismatches must fall through without claiming a
// TLS memo-hit outcome, leaving the call's single terminal counters and timing bucket coherent
// with the partition. Each check first proves its probe path is live (a real memo hit), then
// arms the one-shot recheck mismatch. Single thread, no real mapping mutation: the seam below
// only replaces the recheck read, and restores it on scope exit.
std::uint64_t RecheckMismatchedMapping() {
    // A value the guest allocation-generation counter never takes in a test run, so the
    // recheck fails while the seam is armed; the real mapping is never touched.
    return ~std::uint64_t{0};
}

struct CollectRecheckSeam {
    CollectRecheckSeam() { detail::SetCollectRecheckMappingOverride(&RecheckMismatchedMapping); }
    ~CollectRecheckSeam() { detail::SetCollectRecheckMappingOverride(nullptr); }
};

void CheckRecheckMissFallsThroughEpochMap() {
    BumpCollectEpoch();
    const auto address = Base + 92 * Block + 3 * Page + 100;
    const auto probe = Base + 92 * Block + 4 * Page + 64;
    Store(probe);
    Require(CollectWrites(address, Page) != 0, "the priming collect failed");
    TextureTimingHooks on(true);
    textureTestClock.step = 1000;
    EnterTextureCollectScope();
    const auto memoized = CollectWrites(address, Page);
    const auto hit = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(memoized != 0, "the memoized collect failed");
    Require(hit.calls == 1 && hit.memoHits == 1 && hit.memoHitEpochMap == 1, "the epoch-map probe was not the live memo path");
    Require(hit.nsMemoHit == 1000, "the memo-hit bucket was not timed");
    CollectRecheckSeam seam;
    EnterTextureCollectScope();
    const auto collected = CollectWrites(address, Page);
    const auto stats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(collected != 0, "the recheck-mismatched collect failed");
    Require(stats.calls == 1, "the recheck-mismatched collect miscounted its calls");
    Require(stats.memoHits == 0 && stats.memoHitEpochMap == 0 && stats.memoHitThread == 0 && stats.memoHitShared == 0,
            "a failed epoch-map probe claimed a TLS memo-hit outcome");
    Require(stats.walkAttempted == 1 && stats.walkFailed == 0, "the failed epoch-map probe did not fall through to a walk");
    Require(stats.calls == stats.earlyExitPreWalk + stats.memoHits + stats.walkAttempted, "the recheck-mismatched partition does not hold");
    Require(stats.nsWalk == 1000 && stats.nsMemoHit == 0, "the timing bucket went to the memo probe instead of the real walk terminal");
    Require(stats.collectClockViolations == 0, "the recheck mismatch raised a clock violation");
}

void CheckRecheckMissFallsThroughThreadMemo() {
    // Same setup as the epoch-map check, but this process runs with the epoch collect map
    // disabled (see --recheck-thread in main), so the probe can only come from the per-thread
    // memo ring: the R4 recheck is exercised on its own.
    BumpCollectEpoch();
    const auto address = Base + 92 * Block + 3 * Page + 100;
    const auto probe = Base + 92 * Block + 4 * Page + 64;
    Store(probe);
    Require(CollectWrites(address, Page) != 0, "the priming collect failed");
    TextureTimingHooks on(true);
    textureTestClock.step = 1000;
    EnterTextureCollectScope();
    const auto memoized = CollectWrites(address, Page);
    const auto hit = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(memoized != 0, "the memoized collect failed");
    Require(hit.calls == 1 && hit.memoHits == 1 && hit.memoHitThread == 1, "the thread-memo probe was not the live memo path");
    Require(hit.nsMemoHit == 1000, "the memo-hit bucket was not timed");
    CollectRecheckSeam seam;
    EnterTextureCollectScope();
    const auto collected = CollectWrites(address, Page);
    const auto stats = TakeTextureCollectStats();
    LeaveTextureCollectScope();
    Require(collected != 0, "the recheck-mismatched collect failed");
    Require(stats.calls == 1, "the recheck-mismatched collect miscounted its calls");
    Require(stats.memoHits == 0 && stats.memoHitThread == 0, "a failed thread-memo probe claimed a TLS memo-hit outcome");
    Require(stats.walkAttempted == 1 && stats.walkFailed == 0, "the failed thread-memo probe did not fall through to a walk");
    Require(stats.calls == stats.earlyExitPreWalk + stats.memoHits + stats.walkAttempted, "the recheck-mismatched partition does not hold");
    Require(stats.nsWalk == 1000 && stats.nsMemoHit == 0, "the timing bucket went to the memo probe instead of the real walk terminal");
    Require(stats.collectClockViolations == 0, "the recheck mismatch raised a clock violation");
}

template <typename Body>
void OnThread(Body body) {
    std::exception_ptr failure;
    std::thread thread([&] {
        try {
            body();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    thread.join();
    if (failure != nullptr) std::rethrow_exception(failure);
}

void CheckSharedWalks() {
    const auto range = Base + 87 * Block;
    const auto probe = range + Page;
    std::uint64_t pointBeforeWalk = 0;
    OnThread([&] {
        BumpCollectEpoch();
        pointBeforeWalk = CollectSince();
        Require(Walked(range, Block, probe), "the first collect of the block did not reach the write watch");
    });
    OnThread([&] {
        BumpCollectEpochSince(pointBeforeWalk);
        Require(!Walked(range, Block, probe), "a collect whose ordering point precedes another thread's walk walked again");
        RaiseCollectSince(SteadyNanoseconds());
        Require(Walked(range, Block, probe), "a raised ordering point reused another thread's walk made before it");
    });
    OnThread([&] {
        BumpCollectEpoch();
        Require(Walked(range, Block, probe), "a collect whose ordering point follows a store reused a walk made before it");
    });
    OnThread([&] {
        BumpCollectEpoch();
        const auto point = CollectSince();
        BumpCollectEpochSince(pointBeforeWalk);
        Require(CollectSince() >= point, "an ordering point moved earlier than the thread's previous one");
        RaiseCollectSince(point + 1);
        Require(CollectSince() >= point + 1, "raising the ordering point did not move it");
    });
    ShareCollectWalks(false);
    OnThread([&] {
        BumpCollectEpochSince(pointBeforeWalk);
        Require(Walked(range, Block, probe), "a collect reused another thread's walk with sharing off");
    });
    ShareCollectWalks(true);
}

void CheckStoreAfterSubmit() {
    const auto range = Base + 91 * Block;
    const auto probe = range + 3 * Page;
    const auto page = probe & ~(Page - 1);
    const auto submitted = SteadyNanoseconds();
    OnThread([&] {
        BumpCollectEpoch();
        Require(CollectWrites(range, Block) != 0, "another queue's collect of the range failed");
    });
    const auto beforeStore = TrackerGeneration();
    Store(probe);
    OnThread([&] {
        BumpCollectEpochSince(submitted);
        Require(CollectWrites(range, Block) != 0, "the collect of the packet failed");
        Require(UnchangedSince(page, Page, beforeStore), "a collect ordered at the submission walked again although another queue walked the range after the submission");
        BumpCollectEpochSince(0);
        Require(CollectWrites(range, Block) != 0, "the collect after the wait failed");
        Require(!UnchangedSince(page, Page, beforeStore), "a collect ordered by a wait the CPU released after its store took a walk made before the store");
    });
    for (const bool inherited : {false, true}) {
        OnThread([&] {
            BumpCollectEpoch();
            Require(CollectWrites(range, Block) != 0, "another queue's collect of the range failed");
        });
        const auto before = TrackerGeneration();
        Store(probe);
        std::uint64_t point = 0;
        if (inherited) {
            OnThread([&] {
                BumpCollectEpoch();
                point = CollectSince();
            });
        } else {
            point = SteadyNanoseconds();
        }
        OnThread([&] {
            BumpCollectEpochSince(point);
            Require(CollectWrites(range, Block) != 0, "the collect of the packet failed");
            Require(!UnchangedSince(page, Page, before), inherited ? "a collect ordered by a label another queue stored after the CPU's store took a walk made before the store" : "a collect of a submission made after the CPU's store took a walk made before the store");
        });
    }

}

bool WaitForPrefetch(std::uint64_t before) {
    const auto until = std::chrono::steady_clock::now() + Patience;
    while (CollectPrefetchWalks() <= before) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void CheckPrefetchAnswersNextPeriod() {
    const auto first = Base + 100 * Block;
    const auto second = Base + 104 * Block;
    for (int round = 0; round < 8; ++round) {
        BumpCollectEpoch();
        Require(CollectWrites(first, Block) != 0, "the first range's collect failed");
        BumpCollectEpoch();
        Require(CollectWrites(second, Block) != 0, "the second range's collect failed");
    }
    SetCollectPrefetch(true);
    const auto probe = first + 3 * Page;
    const auto before = TrackerGeneration();
    Store(probe);
    const auto prefetched = CollectPrefetchWalks();
    BumpCollectEpoch();
    Require(WaitForPrefetch(prefetched), "the prefetch did not walk the range of the previous period");
    const auto walks = OsWalks();
    Require(CollectWrites(first, Block) != 0, "the collect after the prefetch failed");
    Require(OsWalks() == walks, "a range prefetched after the ordering point was walked again");
    Require(!UnchangedSince(probe & ~(Page - 1), Page, before), "a store made before the ordering point was hidden by the prefetched walk");
    const auto next = CollectPrefetchWalks();
    const auto unrelated = Base + 108 * Block;
    BumpCollectEpoch();
    Require(WaitForPrefetch(next), "the prefetch did not walk the second range");
    const auto unrelatedWalks = OsWalks();
    Require(CollectWrites(unrelated, Block) != 0 && OsWalks() == unrelatedWalks + 1, "a range no earlier period walked was not walked by its collect");
    SetCollectPrefetch(false);

}

}

int main(int argc, char** argv) {
    try {
        // The epoch collect map enablement is read once per process (function-local static in
        // GuestMemory.cpp): disabling it here, before the first collect, selects the R4
        // thread-memo probe path for this whole run.
        if (argc == 2 && std::string(argv[1]) == "--recheck-thread") {
            // _putenv (not SetEnvironmentVariable): the CRT's getenv, which the memo helpers
            // read their static config from, only follows the CRT environment.
            _putenv("APS5_NO_EPOCH_COLLECT_MAP=1");
        }
        SetObservationHookForTest(&Hook);
#ifdef _WIN32
        if (!WriteWatched()) {
            std::cout << "collect memo generation test skipped: the guest arena is not write watched\n";
            return 77;
        }
        const Arena arena;
#else
        CheckMemoHitDuringWalk();
        CheckFailedWalkIsRetried();
        CheckSubBlockCollectThatCannotWidenIsObservedOnce();
#endif
        if (argc == 2) {
            const std::string selected = argv[1];
            if (selected == "--interior") CheckInteriorCoverageAfterRingEviction();
            else if (selected == "--unions") {
                CheckAdjacentCoverage();
                CheckOverlappingCoverage();
            } else if (selected == "--invalidation") {
                CheckCoverageGapsAndInvalidations();
                CheckCoverageThreadAndDriverWrites();
            } else if (selected == "--recheck-thread") {
                CheckRecheckMissFallsThroughThreadMemo();
            } else throw std::runtime_error("unknown collect coverage test");
            std::cout << "collect coverage test passed\n";
            return 0;
        }
        CheckPartialGroupIsNotAHit();
        CheckUnalignedRepeatReachesKernelOnce();
        CheckUncachedWalksKeepCachedEntries();
        CheckNeighbouringBlockStoreIsFound();
        CheckInvalidationReachesBlockKeyedEntry();
        CheckThreadWithoutEpochWalksEveryTime();
        CheckStoreInNewEpochIsFound();
CheckScopePartitionWalkThenMemo();
        CheckScopeEarlyExit();
        CheckTakeResetsScopeStats();
        CheckUnscopedCallsDoNotContaminate();
        CheckScopeStatsAreThreadLocal();
        CheckScopedCollectMatchesUnscoped();
        CheckNestedScopeTake();
        CheckTimingGateOffTakesNoClock();
        CheckTimingBucketsPerReturnCategory();
        CheckTimingTakeResetsBuckets();
        CheckTimingClockViolation();
        CheckTimingNestedScopeAccumulates();
        CheckRecheckMissFallsThroughEpochMap();
        CheckOwnersShareOneWalk();
        CheckRunsAreWalkedOnce();
        CheckLaterThreadReusesWalk();
        CheckEarlierWalkDoesNotAnswerLaterEpoch();
        CheckHitDuringWalkReturnsStable();
        CheckNoHitBeforePublication(1);
        CheckNoHitBeforePublication(2);
        CheckWaiterNeedingLaterTickWalks();
        CheckMappingChangeForcesWalk();
        CheckFailedRequestPublishesNothing();
        CheckSharedWalks();
        CheckStoreAfterSubmit();
        if (std::getenv("APS5_ENABLE_COLLECT_PREFETCH") == nullptr) {
            std::fprintf(stderr, "collect prefetch test skipped (APS5_ENABLE_COLLECT_PREFETCH unset)\n");
            return 77;
        }
        CheckPrefetchAnswersNextPeriod();
    } catch (const std::exception& error) {
        std::cerr << "collect memo generation test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "collect memo generation test passed\n";
    return 0;
}
