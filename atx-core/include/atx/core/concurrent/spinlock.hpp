#pragma once

// atx::core::concurrent::SpinLock — TTAS (test-and-test-and-set) spinlock.
//
// Implements the C++ BasicLockable and Lockable named requirements so it is
// usable directly with std::lock_guard<SpinLock> and std::scoped_lock.
//
// ============================================================================
//  Memory-ordering rationale (agent.md §5 — justify every ordering)
// ============================================================================
//
//  lock():
//    Outer relaxed-load (the "test" of TTAS): we only want to know whether the
//    flag LOOKS free before paying the cost of a bus-locked RMW. A stale
//    relaxed read of `true` is harmless — we just spin more. A relaxed read of
//    `false` leads us to attempt the exchange; correctness is guaranteed by the
//    subsequent acquire exchange, not by this load. No synchronisation is
//    required on the spin reads themselves.
//
//    Inner exchange(true, acquire): this IS the lock acquisition.  The acquire
//    semantics pair with the release store in unlock().  C++20 [intro.races]:
//    the release-store in unlock() synchronises-with the acquire-exchange in
//    the thread that observes the lock becoming free.  Everything the previous
//    holder wrote inside the critical section before calling unlock() is
//    happens-before the new holder's reads after lock() returns.
//
//  try_lock():
//    Single exchange(true, acquire) — same acquire pairing as above.
//    No relaxed pre-read: we are not spinning, so there is nothing to save.
//
//  unlock():
//    store(false, release) — the release makes all preceding critical-section
//    writes visible to any thread that subsequently acquires with acquire.
//    seq_cst is unnecessary: the only thread that needs to see those writes is
//    the one that wins the next lock() exchange, and the acquire on that
//    exchange provides the required ordering.  seq_cst would add a full memory
//    fence that no correctness argument requires.
//
//  cpu_relax():
//    On x86-64, PAUSE lowers speculative-execution pressure and reduces memory
//    bus traffic on the spin-wait loop.  It has no effect on the C++ abstract
//    machine's memory model — it is purely a microarchitectural hint.
//
// ============================================================================
//  False-sharing elimination
// ============================================================================
//  The atomic flag is `ATX_CACHE_ALIGNED` so it occupies its own cache line.
//  Without this, a spinlock embedded in a struct would share a cache line with
//  adjacent data, causing spurious invalidations on competing cores.
//
// ============================================================================
//  Lockable contract
// ============================================================================
//  lock()     — acquire the lock; blocks until available.
//  try_lock() — attempt once; returns true if acquired, false if already held.
//  unlock()   — release the lock; undefined behaviour if not currently held.
//
// ============================================================================
//  Thread-safety
// ============================================================================
//  The SpinLock object itself is safe to use from multiple threads
//  simultaneously. The data protected by the lock is the caller's
//  responsibility — as with std::mutex.

#include <atomic>

#include "atx/core/macro.hpp"
#include "atx/core/platform.hpp"

// x86/x86-64: _mm_pause is the canonical spin-wait hint (Intel SDM Vol.2B).
// clang-cl on Windows defines _MSC_VER as well as __clang__, and both expose
// <xmmintrin.h> via ATX_PLATFORM_HAS_MM_PREFETCH, but _mm_pause lives in
// <immintrin.h> on MSVC/clang-cl.  Guard directly on the arch macro.
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#  include <immintrin.h>
#  define ATX_HAS_CPU_PAUSE 1
#else
#  define ATX_HAS_CPU_PAUSE 0
#endif

namespace atx::core::concurrent {

// ============================================================================
//  cpu_relax() — yield the CPU pipeline during a spin-wait
//
//  On x86-64: emits the PAUSE instruction via _mm_pause().
//  SAFETY: _mm_pause / PAUSE is a pure microarchitectural hint.  It does NOT
//  affect the C++ memory model or observable program state.  It reduces the
//  penalty of a memory-order violation on out-of-order CPUs and lowers power
//  consumption during spin loops.  Using it here is safe regardless of the
//  memory ordering on surrounding operations.
//
//  On non-x86: compiles to nothing — spin loops are still correct, just
//  slightly less efficient.
// ============================================================================
[[gnu::always_inline]] inline void cpu_relax() noexcept {
#if ATX_HAS_CPU_PAUSE
    // SAFETY: pure pipeline hint, no memory ordering side-effects.
    _mm_pause();
#endif
}

// ============================================================================
//  SpinLock
// ============================================================================
class SpinLock {
  public:
    SpinLock() noexcept = default;
    ~SpinLock()         = default;

    // A lock is an identity — copying or moving it would alias the live lock
    // state, producing two objects that both believe they are "the" spinlock.
    // (agent.md §1 Rule of Five — explicit delete via ATX_DISABLE_COPY_MOVE.)
    ATX_DISABLE_COPY_MOVE(SpinLock);

    // -------------------------------------------------------------------------
    //  lock() — TTAS acquire
    //
    //  The "test-and-test-and-set" pattern avoids a cache-coherency RMW
    //  (exchange) on every iteration:
    //    1. Relaxed-load until the flag LOOKS free (the "test").
    //    2. Attempt exchange(true, acquire) — the "test-and-set".
    //    3. If the exchange returns false, we own the lock; done.
    //    4. Otherwise another thread snuck in; back to step 1.
    //
    //  Between each failed attempt we call cpu_relax() to reduce bus traffic
    //  and allow the lock-holding core to complete its critical section faster.
    //
    //  The outer loop's relaxed load is an unconditional read for the spin
    //  wait.  See file-header ordering rationale for full justification.
    // -------------------------------------------------------------------------
    void lock() noexcept {
        for (;;) {
            // SAFETY: memory_order_relaxed — we only need to observe *some*
            // recent value.  This load does not participate in any
            // synchronisation; it is a cheap pre-check to avoid the expensive
            // locked exchange when the flag is clearly held.
            if (!locked_.load(std::memory_order_relaxed)) {
                // SAFETY: memory_order_acquire — if the exchange succeeds (we
                // set the flag from false to true), the acquire pairs with the
                // release in unlock().  The previous holder's critical-section
                // writes become visible to us before we enter our own critical
                // section.  If it fails, acquire on a failed exchange has no
                // ordering effect, but the cost is negligible.
                if (!locked_.exchange(true, std::memory_order_acquire)) {
                    return; // acquired
                }
            }
            // SAFETY: PAUSE instruction — pure microarchitectural hint; see
            // cpu_relax() documentation above.
            cpu_relax();
        }
    }

    // -------------------------------------------------------------------------
    //  try_lock() — single attempt, non-blocking
    //
    //  Returns true if the lock was free and is now held by the caller.
    //  Returns false if the lock was already held; caller does NOT own it.
    //
    //  SAFETY: memory_order_acquire — same pairing as lock() on success.
    //  On failure the exchange is a relaxed read in terms of the memory model
    //  (no synchronisation needed because we don't enter the critical section).
    // -------------------------------------------------------------------------
    [[nodiscard]] bool try_lock() noexcept {
        // SAFETY: memory_order_acquire on success establishes the critical
        // section's happens-before: pairs with the release in unlock().
        // On failure (returns true from exchange, meaning already locked),
        // acquire has no harmful effect — it is conservative.
        return !locked_.exchange(true, std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    //  unlock() — release
    //
    //  Precondition: the calling thread holds the lock (i.e., the thread
    //  previously returned from lock() or try_lock() == true without an
    //  intervening unlock()). Violating this is undefined behaviour identical
    //  to unlocking a std::mutex you don't own.
    //
    //  SAFETY: memory_order_release — all stores performed by this thread
    //  inside the critical section are visible to the thread that subsequently
    //  acquires the lock with acquire semantics.  No full fence is needed:
    //  only the acquiring thread needs those writes, and the acquire on its
    //  exchange provides the ordering.
    // -------------------------------------------------------------------------
    void unlock() noexcept {
        // SAFETY: memory_order_release — publishes all critical-section writes
        // to the next thread that acquires with memory_order_acquire.
        locked_.store(false, std::memory_order_release);
    }

  private:
    // Cache-line padded so the lock flag does not share a cache line with
    // adjacent data, preventing false-sharing invalidations on competing cores.
    // ATX_CACHE_ALIGNED expands to alignas(::atx::core::kCacheLineSize).
    ATX_CACHE_ALIGNED std::atomic<bool> locked_{false};
};

// Verify the atomic is always lock-free on this platform.
// A lock implemented via a mutex would be circular (a mutex using itself).
static_assert(std::atomic<bool>::is_always_lock_free,
              "SpinLock requires std::atomic<bool> to be lock-free");

} // namespace atx::core::concurrent
