#pragma once

// WriterLock -- cross-process mutual-exclusion guard for a "manifest
// read-modify-publish" window (Task D3, backtest-production-lakehouse
// sprint). BacktestDb::persist_locked and SurfaceDb::persist_locked each
// serialize mutations WITHIN one process via their own `mu_`, but that is a
// per-instance std::mutex: two separate BacktestDb/SurfaceDb handles on the
// SAME on-disk root -- whether two handles in one process or one handle each
// in two different processes -- do not share a `mu_` and can both read the
// manifest at generation N, both compute generation N+1, and both
// `flush_and_publish_file` onto the same destination. The atomic rename
// itself never corrupts anything (it is still one clean file at all times),
// but the LAST rename simply wins: the other writer's update is silently
// discarded rather than reported. `WriterLock` closes that window with an
// OS-level lock a second process can actually observe, unlike an in-process
// mutex.
//
// MECHANISM. `lock_path` (by convention "<resource>.lock", a sibling of the
// file being protected) is created with CREATE_NEW (Win32 `CreateFileW`) /
// O_EXCL (POSIX `open`) semantics -- existence of the file IS the lock, which
// is what makes it observable across a process boundary. The body of the
// file is the owning process's PID, decimal ASCII, written and flushed
// before `acquire` returns. The handle/fd stays OPEN for the lifetime of the
// `WriterLock` object (opened with read-sharing only, so a contending
// process can still read the PID for a liveness probe, but cannot delete or
// overwrite the file out from under the holder); `release()` (also run by
// the destructor) closes it and removes the file, so a normal return, an
// early return, or an exception unwinding through the holder's scope all
// release the lock -- this is a plain RAII guard, not a try/finally the
// caller has to remember.
//
// CONTENTION. `acquire` retries with a short fixed backoff for up to
// `timeout` while `lock_path` is held by a LIVE process, then returns
// `Err(ErrorCode::Unavailable)` -- never blocks forever. `timeout = 0ms`
// tries exactly once (useful for a synchronous "is it held right now" test).
//
// STALE-LOCK TAKEOVER. If CREATE_NEW/O_EXCL fails because `lock_path`
// already exists, `acquire` reads the PID inside it and probes whether that
// process is still alive (Win32 `OpenProcess` + `GetExitCodeProcess`; POSIX
// `kill(pid, 0)`). A DEAD owner (the prior writer crashed, or was killed,
// before its `WriterLock` destructor ran) is stale: this call removes the
// abandoned file and retries CREATE_NEW immediately, without waiting out
// `timeout`. KNOWN, ACCEPTED LIMITATIONS (out of this task's scope to close
// completely):
//   * PID reuse -- if the OS recycles the dead owner's PID for an unrelated
//     live process before the probe runs, the stale lock reads as live and
//     this call reports contention (fails safe, not silent, but pessimistic)
//     rather than taking over.
//   * A narrow takeover-vs-takeover race -- between reading a stale PID and
//     deleting the file, a DIFFERENT process could already have taken it
//     over; this call re-reads the PID immediately before deleting and
//     aborts the takeover (falls back into the normal contention retry) if
//     it no longer matches what was probed, which closes most but not
//     provably all of the window. Closing it completely needs a
//     process-start-time or random nonce in the lock body, which is more
//     mechanism than a rarely-contended manifest lock warrants here.
//
// THREAD-SAFETY. One `WriterLock` object is not safe to acquire/release from
// multiple threads concurrently (it has no internal synchronization of its
// own) -- exactly like the `std::mutex` it complements. Different
// `WriterLock` objects (any thread, any process) contending for the same
// `lock_path` are the whole point and are safe.
//
// LOCK ORDERING (binding invariant, all call sites): acquire the WriterLock
// BEFORE taking any in-process `std::mutex` (BacktestDb/SurfaceDb's `mu_`),
// and hold it until after that mutex is released. NEVER the other way
// around. Rationale: `acquire`'s wait against a LIVE contender is bounded by
// `timeout` (default 5000ms) -- real wall-clock time, not a fast in-process
// spin. If a writer took its `mu_` first and then called `acquire` while
// still holding it (this was Task D3's original shape, fixed after review),
// every OTHER in-process caller of a `mu_`-guarded method -- readers
// included, e.g. `BacktestDb::templates()`/`generation()` -- would stall for
// up to that same 5000ms merely because a DIFFERENT process happened to be
// mid-write. Acquiring the file lock first keeps that wait entirely outside
// `mu_`: once held, `mu_` only ever guards the fast in-process
// read-modify-publish sequence, exactly as it did before this lock existed.
// A single, consistent order across every caller (file-lock -> mu_) is also
// what makes this deadlock-free -- two locks taken in the same order by
// every thread/process can never form a cycle.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "atx/core/error.hpp" // atx::core::Result

namespace atx::vol::detail {

// Task D6 reuse: the SAME "is this PID still a running process" probe
// WriterLock's own stale-owner takeover uses (Win32 OpenProcess +
// GetExitCodeProcess; POSIX kill(pid, 0) -- see the anonymous-namespace
// implementation in writer_lock.cpp for the exact semantics, including the
// "indeterminate reads as alive" conservative-direction rule). Exposed here
// so OTHER PID-liveness advisory mechanisms (the track lakehouse Catalog's
// reader_marks, BacktestReaderMark) can share ONE platform-specific
// implementation instead of re-deriving a second Win32/POSIX liveness probe
// -- the same "one implementation, not a second copy that can drift" rule
// this sprint already applied to SHA-256 (Task D1). Unlike WriterLock
// itself, callers of these two functions are not implementing mutual
// exclusion: multiple concurrent PID-tagged marks are the whole point for a
// many-reader registration.
[[nodiscard]] std::uint64_t current_process_id() noexcept;
[[nodiscard]] bool process_alive(std::uint64_t pid) noexcept;

class WriterLock {
public:
  // Bounded retry budget against a LIVE contender. Never waited out against a
  // STALE (dead-owner) lock, which is taken over immediately instead.
  static constexpr std::chrono::milliseconds kDefaultTimeout{5000};

  [[nodiscard]] static atx::core::Result<WriterLock>
  acquire(std::string_view lock_path, std::chrono::milliseconds timeout = kDefaultTimeout);

  ~WriterLock();
  WriterLock(WriterLock &&other) noexcept;
  WriterLock &operator=(WriterLock &&other) noexcept;
  WriterLock(const WriterLock &) = delete;
  WriterLock &operator=(const WriterLock &) = delete;

  // Release early. Idempotent -- a no-op if already released or moved-from.
  // The destructor calls this, so callers normally never need to.
  void release() noexcept;

  [[nodiscard]] bool held() const noexcept { return native_handle_ != kInvalidHandle; }

private:
  static constexpr std::intptr_t kInvalidHandle = -1;

  WriterLock() = default;
  WriterLock(std::string lock_path, std::intptr_t native_handle) noexcept
      : lock_path_{std::move(lock_path)}, native_handle_{native_handle} {}

  std::string lock_path_;
  // Win32 HANDLE or POSIX fd, whichever this platform's acquire() opened,
  // cast to std::intptr_t so this header never names a platform type (the
  // house discipline archive_util.hpp/.cpp already follows: platform headers
  // stay confined to the .cpp). kInvalidHandle means "not held" -- the
  // default-constructed and moved-from state.
  std::intptr_t native_handle_{kInvalidHandle};
};

} // namespace atx::vol::detail
