// spinlock_test.cpp — tests for atx::core::concurrent::SpinLock
//
// Naming suffix SL on all test-local helper types to avoid ODR collisions
// in the single atx-core-tests binary (spec requirement).
//
// Tests:
//   SpinLock_MutualExclusionCounter  — multithreaded data-race detector
//   SpinLock_TryLockWhenHeld         — try_lock returns false when held, true when free
//   SpinLock_LockUnlockSingleThread  — basic lock/unlock single-threaded contract
//   SpinLock_RaiiLockGuard           — verifies Lockable requirement via lock_guard
//   SpinLock_TryLockSingleThread     — try_lock when free then when held (single thread)

#include <atx/core/concurrent/spinlock.hpp>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

using namespace atx::core::concurrent;

// ============================================================================
//  Seed tests from spec (provided verbatim, names unchanged)
// ============================================================================

// Primary data-race detector: 4 threads each do 10 000 guarded increments.
// If the memory ordering is wrong, increments will be lost and the final
// count will be < 40 000.  Passes deterministically under correct ordering.
TEST(SpinLock, MutualExclusionCounter) {
    SpinLock lk;
    long     counter = 0;

    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 10000; ++i) {
                std::lock_guard<SpinLock> g{lk};
                ++counter;
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }

    EXPECT_EQ(counter, 40000);
}

// try_lock must return false when another owner holds the lock, and true when
// the lock is free.
TEST(SpinLock, TryLockWhenHeld) {
    SpinLock lk;

    lk.lock();
    EXPECT_FALSE(lk.try_lock()); // already locked — must fail
    lk.unlock();

    EXPECT_TRUE(lk.try_lock()); // now free — must succeed
    lk.unlock();
}

// ============================================================================
//  Additional tests (single-thread contract + RAII)
// ============================================================================

// lock/unlock round-trip on a single thread, verifying that a second
// lock() after unlock() can be acquired (not wedged).
TEST(SpinLock, LockUnlockSingleThread) {
    SpinLock lk;

    lk.lock();
    lk.unlock(); // must not deadlock or assert

    lk.lock(); // must succeed again immediately
    lk.unlock();
}

// Verify Lockable requirement: lock_guard must compile and RAII-unlock on
// scope exit.  A second lock_guard must succeed after the first's scope.
TEST(SpinLock, RaiiLockGuard) {
    SpinLock lk;
    long     value = 0;

    {
        std::lock_guard<SpinLock> g{lk};
        value = 1;
    } // unlock on scope exit

    // If RAII failed to unlock, this lock_guard would deadlock.
    {
        std::lock_guard<SpinLock> g{lk};
        value = 2;
    }

    EXPECT_EQ(value, 2);
}

// try_lock: success when free, then correctly reports held.
TEST(SpinLock, TryLockSingleThread) {
    SpinLock lk;

    // Free lock: try_lock must succeed and return true.
    ASSERT_TRUE(lk.try_lock());

    // Now held by us: a second try_lock must fail.
    EXPECT_FALSE(lk.try_lock());

    // Release and verify we can try_lock again.
    lk.unlock();
    EXPECT_TRUE(lk.try_lock());
    lk.unlock();
}

// ============================================================================
//  Stress test: many threads hammering try_lock + lock together
//
//  Uses a mix of try_lock (with retry) and lock_guard to ensure both paths
//  interact correctly under contention.  Counter must reach kThreads*kIter.
// ============================================================================
TEST(SpinLock, MixedTryLockAndLockStress) {
    static constexpr int kThreadsSL = 8;
    static constexpr int kIterSL    = 5000;

    SpinLock lk;
    long     counter = 0;

    std::vector<std::thread> ts;
    ts.reserve(kThreadsSL);

    for (int t = 0; t < kThreadsSL; ++t) {
        const bool use_try = (t % 2 == 0); // even threads use try_lock spin
        ts.emplace_back([&, use_try] {
            for (int i = 0; i < kIterSL; ++i) {
                if (use_try) {
                    // Spin via try_lock until acquired — exercises the try path
                    // under real contention.
                    while (!lk.try_lock()) {
                        // SAFETY: cpu_relax() is called via the spinlock's
                        // internal loop; here we yield to be cooperative.
                        std::this_thread::yield();
                    }
                    ++counter;
                    lk.unlock();
                } else {
                    std::lock_guard<SpinLock> g{lk};
                    ++counter;
                }
            }
        });
    }
    for (auto& t : ts) {
        t.join();
    }

    EXPECT_EQ(counter, static_cast<long>(kThreadsSL) * kIterSL);
}
