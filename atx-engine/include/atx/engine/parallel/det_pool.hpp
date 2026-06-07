#pragma once

// atx::engine::parallel — DetPool: deterministic fixed-worker task pool (S2-1).
//
// The engine-local FALLBACK that ships Sprint S2 until the atx-core deterministic
// pool (concurrent/task_pool.hpp) lands. It exposes the SAME API the engine will
// later consume through a single `using Pool = ...;` alias, so the swap is one
// line (see fwd.hpp SWITCH POINT).
//
// DETERMINISM CONTRACT — read before touching this file:
//   The pool itself does NO floating-point math. It guarantees ONLY that
//     (a) every index in [0, n) is processed exactly once by exactly one worker,
//     (b) parallel_for does not return until ALL indices are done (a barrier),
//     (c) a body exception is rethrown deterministically — the LOWEST index that
//         threw wins, regardless of which worker observed it or in what order.
//   Which worker grabs which index is timing-dependent and MUST NOT affect any
//   result: the CALLER's contract is to write only to its own out[i] slot. With
//   that contract, the output is byte-identical across {1,2,4,8} workers BY
//   CONSTRUCTION — there is no cross-worker accumulation here for scheduling
//   order to perturb.
//
// CONCURRENCY MODEL:
//   N persistent std::threads park on a condition_variable between jobs. A
//   monotonically-increasing `generation_` counter distinguishes a real new job
//   from a spurious wake; a `stop_` flag breaks the park loop for destruction.
//   A `job_kind_` selects the dispatch shape: ParallelFor work is dispensed by a
//   single atomic `next_index_` counter (fetch_add) — no queue, no work-stealing;
//   EachWorker runs the body once per worker (each worker invokes body(wid, wid)),
//   used for per-worker setup like warming a stateful Engine. Either way a `busy_`
//   counter tracks outstanding workers; the one that drives it to zero signals the
//   main thread's completion barrier.
//
// Header-only; everything inline. Rule of Five: DetPool owns threads, so it is a
// PINNED object — copy AND move are deleted.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/parallel/fwd.hpp" // class DetPool; (forward-declared)

namespace atx::engine::parallel {

// Deterministic fixed-worker task pool. See file header for the full contract.
class DetPool {
public:
  // n_workers == 0 -> default = max(1, hardware_concurrency() - 2). NOTE:
  // hardware_concurrency may SIZE the pool but must NEVER appear in any result
  // computation (that is the caller's R4 concern) — it only picks the thread
  // count, which by construction cannot affect outputs.
  explicit DetPool(atx::usize n_workers = 0) : n_workers_{resolve_workers(n_workers)} {
    workers_.reserve(n_workers_);
    for (atx::usize wid = 0; wid < n_workers_; ++wid) {
      workers_.emplace_back([this, wid] { worker_loop(wid); });
    }
  }

  // Signals stop, wakes every parked worker, and joins them all (clean shutdown).
  ~DetPool() {
    {
      std::lock_guard<std::mutex> lock{mtx_};
      stop_ = true;
    }
    cv_work_.notify_all();
    for (std::thread& t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  // Owns threads -> pinned object: copy AND move deleted.
  DetPool(const DetPool&) = delete;
  DetPool& operator=(const DetPool&) = delete;
  DetPool(DetPool&&) = delete;
  DetPool& operator=(DetPool&&) = delete;

  [[nodiscard]] atx::usize n_workers() const noexcept { return n_workers_; }

  // Invokes body(usize i, usize worker_id) once per i in [0, n), worker_id in
  // [0, n_workers()). Blocks until all complete. The body must write ONLY to its
  // own slot/index (the caller's determinism contract). Exceptions thrown by body
  // are captured per-worker and, after the barrier, the one from the LOWEST index
  // is rethrown (deterministic failure). n == 0 is a no-op (no wake, no barrier).
  template <class Body>
  void parallel_for(atx::usize n, Body&& body) {
    if (n == 0) {
      return; // nothing to do; do not wake workers or erect a barrier
    }
    dispatch_job(JobKind::ParallelFor, n,
                 [&body](atx::usize i, atx::usize wid) { body(i, wid); });
  }

  // Runs fn(worker_id) exactly once on EACH worker (per-worker setup, e.g.
  // warming a per-thread Engine). This is NOT a parallel_for over the dispenser:
  // the shared next_index_ counter gives no index->worker binding, so a fast
  // worker would drain the indices and a late worker would run fn zero times.
  // The EachWorker job kind instead has every worker invoke the body exactly
  // once with its own id, so fn runs on all n_workers() workers deterministically.
  template <class Fn>
  void for_each_worker(Fn&& fn) {
    dispatch_job(JobKind::EachWorker, n_workers_,
                 [&fn](atx::usize /*i*/, atx::usize wid) { fn(wid); });
  }

private:
  // Selects how a published job is dispatched across the workers.
  enum class JobKind {
    ParallelFor, // drain the atomic next_index_ dispenser over [0, job_n_)
    EachWorker,  // run the body once per worker as body(wid, wid)
  };

  // Publish a job (under the lock), wake the workers, wait on the completion
  // barrier, then rethrow the lowest-index/lowest-wid captured exception. Shared
  // by parallel_for and for_each_worker — only kind + n + body differ.
  void dispatch_job(JobKind kind, atx::usize n,
                    std::function<void(atx::usize, atx::usize)> body) {
    // Reset per-job state under the lock, then publish the new generation. The
    // lock + generation bump establish a happens-before edge from this setup to
    // every worker's wake, so the body the workers read is fully constructed.
    {
      std::unique_lock<std::mutex> lock{mtx_};
      body_ = std::move(body);
      job_kind_ = kind;
      job_n_ = n;
      next_index_.store(0, std::memory_order_relaxed);
      // SAFETY: busy_ is set before the generation is published and read only by
      // workers that have already taken mtx_ on wake, so this plain store is
      // ordered before any worker decrement by the mutex release/acquire. busy_
      // counts ALL workers in both job kinds, so the barrier is identical.
      busy_.store(n_workers_, std::memory_order_relaxed);
      done_ = false;
      ++generation_;
    }
    cv_work_.notify_all();

    // Barrier: wait until the last worker drives busy_ to 0 and sets done_.
    {
      std::unique_lock<std::mutex> lock{mtx_};
      cv_done_.wait(lock, [this] { return done_; });
    }

    rethrow_lowest(); // deterministic: lowest index/wid that threw, or no-op
    clear_captures();
  }

  // Resolve the requested worker count: 0 -> max(1, hw_concurrency - 2).
  [[nodiscard]] static atx::usize resolve_workers(atx::usize requested) noexcept {
    if (requested != 0) {
      return requested;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 3U) {
      return 1; // tiny / unknown (0) machine -> one worker (sequential)
    }
    return static_cast<atx::usize>(hw) - 2;
  }

  // Persistent worker park-and-run loop. One per std::thread.
  void worker_loop(atx::usize wid) {
    atx::usize last_gen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock{mtx_};
        // Wake on a NEW generation (real job) or stop. The generation guard
        // rejects spurious wakes that did not advance the job counter.
        cv_work_.wait(lock, [this, last_gen] { return stop_ || generation_ != last_gen; });
        if (stop_) {
          return;
        }
        last_gen = generation_;
      }
      // job_kind_ was published under mtx_ before this wake, so the read is
      // ordered after the publish by the cv_work_ mutex handshake.
      if (job_kind_ == JobKind::EachWorker) {
        run_each_worker(wid);
      } else {
        run_job(wid);
      }
    }
  }

  // Drain the index dispenser for one job, then participate in the barrier.
  void run_job(atx::usize wid) {
    const atx::usize n = job_n_; // published under the mutex before our wake
    for (;;) {
      // SAFETY: relaxed is sufficient here — fetch_add only CLAIMS a unique
      // index; it publishes no data to other threads. The happens-before for the
      // body's reads/writes comes from the cv_work_ mutex handshake at wake (job
      // setup) and the cv_done_ handshake at the barrier (results), not from this
      // counter. Each index is returned to exactly one worker, so every i in
      // [0, n) is processed exactly once.
      const atx::usize i = next_index_.fetch_add(1, std::memory_order_relaxed);
      if (i >= n) {
        break;
      }
      capture_or_run(i, wid);
    }
    signal_done_if_last();
  }

  // EachWorker job: this worker runs the body EXACTLY once as body(wid, wid),
  // then joins the same barrier. No dispenser is touched — the per-worker fan-out
  // is the whole point (see for_each_worker). An exception in per-worker setup is
  // captured under this worker's wid and rethrown deterministically (lowest wid)
  // after the barrier, exactly like the dispenser path.
  void run_each_worker(atx::usize wid) {
    capture_or_run(wid, wid); // index == wid for the deterministic-rethrow ordering
    signal_done_if_last();
  }

  // Run body_(index, wid); on throw, capture per-worker the LOWEST index this
  // worker saw. No shared write, so no race — the cross-worker minimum is reduced
  // single-threaded after the barrier in rethrow_lowest(). For EachWorker, index
  // == wid, so "lowest index" reduces to "lowest wid".
  void capture_or_run(atx::usize index, atx::usize wid) {
    try {
      body_(index, wid);
    } catch (...) {
      if (!worker_has_exc(wid) || index < captures_[wid].index) {
        captures_[wid].index = index;
        captures_[wid].eptr = std::current_exception();
      }
    }
  }

  // Barrier-completion tail shared by both job kinds. The worker that drives
  // busy_ to zero signals the waiting main thread.
  void signal_done_if_last() {
    // SAFETY: acq_rel on the completion decrement. The release ensures this
    // worker's body effects (its out[i] writes) are visible to the main thread
    // that acquires when it observes the counter hit 0; the acquire pairs with
    // the other workers' releases so the LAST worker sees all prior effects
    // before it signals done_. The fetch_sub returns the PRE-decrement value, so
    // the worker that read 1 is the last one out.
    if (busy_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> lock{mtx_};
      done_ = true;
      cv_done_.notify_one();
    }
  }

  [[nodiscard]] bool worker_has_exc(atx::usize wid) const noexcept {
    return captures_[wid].eptr != nullptr;
  }

  // After the barrier (single-threaded): rethrow the exception from the lowest
  // index across all workers, preserving its original type via exception_ptr.
  void rethrow_lowest() {
    std::exception_ptr chosen;
    atx::usize chosen_index = 0;
    bool found = false;
    for (atx::usize wid = 0; wid < n_workers_; ++wid) {
      if (captures_[wid].eptr && (!found || captures_[wid].index < chosen_index)) {
        chosen = captures_[wid].eptr;
        chosen_index = captures_[wid].index;
        found = true;
      }
    }
    if (found) {
      clear_captures();
      std::rethrow_exception(chosen);
    }
  }

  void clear_captures() noexcept {
    for (Capture& c : captures_) {
      c.eptr = nullptr;
      c.index = 0;
    }
  }

  // Per-worker exception slot: the lowest index that threw on that worker.
  struct Capture {
    std::exception_ptr eptr{};
    atx::usize index{0};
  };

  const atx::usize n_workers_;

  std::mutex mtx_;
  std::condition_variable cv_work_; // workers park here between jobs
  std::condition_variable cv_done_; // main parks here on the barrier

  std::function<void(atx::usize, atx::usize)> body_; // current job body (set under mtx_)
  JobKind job_kind_{JobKind::ParallelFor};           // dispatch shape (set under mtx_)
  atx::usize job_n_{0};                              // current job index count
  std::atomic<atx::usize> next_index_{0};            // work dispenser
  std::atomic<atx::usize> busy_{0};                  // outstanding workers this job
  std::size_t generation_{0};                        // bumped per job (spurious-wake guard)
  bool stop_{false};                                 // destruction flag
  bool done_{false};                                 // barrier predicate for cv_done_

  std::vector<Capture> captures_ = std::vector<Capture>(n_workers_); // per-worker exc slot
  std::vector<std::thread> workers_;
};

} // namespace atx::engine::parallel
