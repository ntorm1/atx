// WriterLock -- cross-process manifest writer lock (Task D3,
// backtest-production-lakehouse sprint). detail/writer_lock.hpp.
//
// Unconditionally registered (unlike catalog_test.cpp): BacktestDb/SurfaceDb
// -- and therefore WriterLock, which their persist_locked() now goes
// through -- are core atx-vol surface, compiled and shipped regardless of
// ATX_VOL_LAKEHOUSE. Gating this file the same way catalog_test.cpp is
// gated would leave the minimal (ATX_VOL_LAKEHOUSE=OFF) build's copy of this
// exact code path untested.
//
// Two halves:
//   WriterLockTest.*             the raw primitive: acquire/release,
//                                 real cross-handle contention (never
//                                 same-handle -- acquire() has no reentrant
//                                 mode, so any two acquire() calls ARE two
//                                 independent OS handles/fds), RAII release
//                                 on a thrown exception, stale-lock takeover
//                                 via a PID that is not a live process.
//   BacktestDbWriterLockTest / SurfaceDbWriterLockTest
//                                 the brief's Step 1(c) regression: two
//                                 independent handles (not two threads
//                                 racing one instance -- two full
//                                 create()/open() handles on the same root,
//                                 the shape a second process would present)
//                                 writing through persist_locked(). Asserts
//                                 the on-disk generation sequence is
//                                 gap-free and BOTH writers' content
//                                 survives -- the last-rename-wins clobber
//                                 this task fixes would instead have either
//                                 silently discarded one writer's update or
//                                 landed two different files at the same
//                                 generation number.

#include "storage/writer_lock.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <gtest/gtest.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "storage/backtest_db.hpp"
#include "atx/vol/api/storage/surface_db.hpp"

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
using atx::vol::detail::WriterLock;

[[nodiscard]] fs::path scratch_dir(std::string_view tag) {
  static std::atomic<std::uint64_t> sequence{0};
  const fs::path dir = fs::temp_directory_path() /
                       ("atx_writer_lock_" + std::string(tag) + "_" +
                        std::to_string(sequence.fetch_add(1)));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

void write_text(const fs::path &p, std::string_view text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

// ── WriterLockTest: the raw primitive ───────────────────────────────────

TEST(WriterLockTest, AcquireCreatesLockFileReleaseRemovesIt) {
  const fs::path lock = scratch_dir("basic") / "manifest.lock";
  ASSERT_FALSE(fs::exists(lock));
  {
    auto guard = WriterLock::acquire(lock.string());
    ASSERT_TRUE(guard.has_value()) << (guard ? "" : guard.error().to_string());
    EXPECT_TRUE(guard->held());
    EXPECT_TRUE(fs::exists(lock));
  }
  EXPECT_FALSE(fs::exists(lock)) << "destructor did not release the lock";
}

TEST(WriterLockTest, ExplicitReleaseIsIdempotentAndRemovesTheFile) {
  const fs::path lock = scratch_dir("explicit-release") / "manifest.lock";
  auto guard = WriterLock::acquire(lock.string());
  ASSERT_TRUE(guard.has_value());
  guard->release();
  EXPECT_FALSE(guard->held());
  EXPECT_FALSE(fs::exists(lock));
  guard->release(); // no-op, must not crash or double-free
  guard->release();
}

// Real cross-handle exclusion: `first` and `second` are two SEPARATE
// WriterLock objects, each backed by its own OS handle/fd from its own
// CREATE_NEW/O_EXCL call -- acquire() has no notion of a reentrant
// same-handle re-lock, so this is never accidentally testing "the same
// handle against itself".
TEST(WriterLockTest, SecondAcquireFailsWhileFirstHeldRealCrossHandle) {
  const fs::path lock = scratch_dir("contend") / "manifest.lock";
  auto first = WriterLock::acquire(lock.string());
  ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());

  // timeout=0: try exactly once, so this assertion does not wait out any
  // retry budget -- it proves the SECOND handle is refused while the FIRST
  // is still alive, not merely that it eventually gives up.
  auto second = WriterLock::acquire(lock.string(), std::chrono::milliseconds{0});
  ASSERT_FALSE(second.has_value())
      << "a second, independent handle acquired the SAME lock path while the first was "
         "still held";
  EXPECT_EQ(second.error().code(), atx::core::ErrorCode::Unavailable);

  EXPECT_TRUE(fs::exists(lock)) << "the first holder's lock file must be untouched";
}

TEST(WriterLockTest, LockIsReleasedAfterFirstReleasesSoAThirdAcquireSucceeds) {
  const fs::path lock = scratch_dir("release-then-reacquire") / "manifest.lock";
  auto first = WriterLock::acquire(lock.string());
  ASSERT_TRUE(first.has_value());

  auto blocked = WriterLock::acquire(lock.string(), std::chrono::milliseconds{0});
  ASSERT_FALSE(blocked.has_value());

  first->release();

  auto third = WriterLock::acquire(lock.string());
  ASSERT_TRUE(third.has_value()) << (third ? "" : third.error().to_string())
                                  << " -- lock never freed after the first holder released";
  EXPECT_TRUE(fs::exists(lock));
}

// RAII: the guard lives on the stack inside a try-block that throws. Stack
// unwinding must run WriterLock's destructor exactly like a normal return
// would -- the lock is not leaked just because the scope exited via an
// exception instead of falling off the end.
TEST(WriterLockTest, ReleasedOnScopeExitEvenOnException) {
  const fs::path lock = scratch_dir("exception-unwind") / "manifest.lock";
  bool caught = false;
  try {
    auto guard = WriterLock::acquire(lock.string());
    ASSERT_TRUE(guard.has_value());
    ASSERT_TRUE(fs::exists(lock));
    throw std::runtime_error("deliberate: prove RAII release survives stack unwinding");
  } catch (const std::runtime_error &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  EXPECT_FALSE(fs::exists(lock)) << "lock leaked across a thrown exception";
}

// Stale-lock takeover: a lock file pre-exists (simulating an abandoned lock
// from a writer that crashed before its WriterLock destructor could run),
// naming a PID that is not currently a running process. `acquire` must
// detect this via the liveness probe and take over rather than treating it
// as live contention.
TEST(WriterLockTest, StaleLockWithDeadOwnerPidIsTakenOver) {
  const fs::path lock = scratch_dir("stale") / "manifest.lock";
  // Not a real PID on any host this test runs on; process_alive()'s
  // liveness probe (OpenProcess/kill(pid,0)) reports it as dead, which is
  // exactly the condition under test -- this is not a timing-dependent
  // "spawn and hope it exited" probe.
  write_text(lock, "999999999");
  ASSERT_TRUE(fs::exists(lock));

  auto guard = WriterLock::acquire(lock.string(), std::chrono::milliseconds{200});
  ASSERT_TRUE(guard.has_value())
      << (guard ? "" : guard.error().to_string()) << " -- stale lock (dead owner) was not taken over";
  EXPECT_TRUE(fs::exists(lock));
}

#if defined(_WIN32)
// The previous holder's release deletes the lock file. Between the delete disposition
// being set and that handle closing (inside fs::remove), the name is DELETE-PENDING and
// CREATE_NEW on it fails with ERROR_ACCESS_DENIED. `delete_pending_lock` holds that state
// open deterministically: a lock file naming this (live) process, opened with DELETE access
// and marked for deletion. Closing the returned handle completes the delete. nullptr (with
// `why`) when the state cannot be produced on this file system.
[[nodiscard]] HANDLE delete_pending_lock(const fs::path &lock, std::string &why) {
  write_text(lock, std::to_string(::GetCurrentProcessId()));
  HANDLE h = ::CreateFileW(lock.wstring().c_str(), DELETE | GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    why = "open for delete failed: " + std::to_string(::GetLastError());
    return nullptr;
  }
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (::SetFileInformationByHandle(h, FileDispositionInfo, &disposition, sizeof disposition) ==
      FALSE) {
    why = "delete disposition refused: " + std::to_string(::GetLastError());
    ::CloseHandle(h);
    return nullptr;
  }
  const DWORD attrs = ::GetFileAttributesW(lock.wstring().c_str());
  const DWORD error = attrs == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : 0;
  if (error != ERROR_ACCESS_DENIED) {
    ::CloseHandle(h);
    why = "no delete-pending state on this file system (attributes error " +
          std::to_string(error) + ")";
    return nullptr;
  }
  return h;
}
#endif

// Measured before this test existed: PersistDirectory.LockSerializesConcurrentPublishers
// lost 2 of 60 repeats to an IoError "win32 error=5" from exactly this state. A
// delete-pending lock file is the previous holder still leaving, not a failure: acquire
// retries it inside its timeout and takes the lock once the name clears.
TEST(WriterLockTest, DeletePendingLockFileIsRetriedUntilItClears) {
#if !defined(_WIN32)
  GTEST_SKIP() << "delete-pending is a Windows file state";
#else
  const fs::path lock = scratch_dir("delete-pending") / "manifest.lock";
  std::string why;
  HANDLE probe = delete_pending_lock(lock, why);
  if (probe == nullptr) GTEST_SKIP() << why;
  std::thread closer([probe] {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    ::CloseHandle(probe);
  });
  auto next = WriterLock::acquire(lock.string(), std::chrono::milliseconds{5000});
  closer.join();
  ASSERT_TRUE(next.has_value()) << next.error().to_string()
                                << " -- a delete-pending lock file was treated as a failure";
  EXPECT_TRUE(fs::exists(lock));
#endif
}

// ...but a refusal that outlasts the timeout is not contention: it surfaces as IoError
// naming the Win32 code (a genuine permission failure stays distinguishable from
// Unavailable), and only after the retry budget was spent.
TEST(WriterLockTest, PersistentAccessDeniedIsIoErrorAfterTheDeadline) {
#if !defined(_WIN32)
  GTEST_SKIP() << "delete-pending is a Windows file state";
#else
  const fs::path lock = scratch_dir("access-denied") / "manifest.lock";
  std::string why;
  HANDLE probe = delete_pending_lock(lock, why);
  if (probe == nullptr) GTEST_SKIP() << why;
  const auto t0 = std::chrono::steady_clock::now();
  auto next = WriterLock::acquire(lock.string(), std::chrono::milliseconds{150});
  const auto waited = std::chrono::steady_clock::now() - t0;
  ::CloseHandle(probe);
  ASSERT_FALSE(next.has_value());
  EXPECT_EQ(next.error().code(), atx::core::ErrorCode::IoError);
  EXPECT_NE(next.error().to_string().find("win32 error=5"), std::string::npos)
      << next.error().to_string();
  EXPECT_GE(waited, std::chrono::milliseconds{140}) << "refused before the deadline";
#endif
}

TEST(WriterLockTest, EmptyLockPathIsInvalidArgument) {
  auto guard = WriterLock::acquire("");
  ASSERT_FALSE(guard.has_value());
  EXPECT_EQ(guard.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── BacktestDbWriterLockTest: brief Step 1(c) regression ───────────────────

[[nodiscard]] fs::path btdb_root(std::string_view tag) { return scratch_dir(std::string("btdb_") + std::string(tag)); }

[[nodiscard]] BacktestStrategyTemplate btdb_template(std::string id) {
  auto made = make_40_delta_3_calendar_month_strangle_template();
  EXPECT_TRUE(made.has_value()) << (made ? "" : made.error().to_string());
  BacktestStrategyTemplate value = made ? std::move(*made) : BacktestStrategyTemplate{};
  value.id = std::move(id);
  value.name = "writer-lock regression template";
  return value;
}

TEST(BacktestDbWriterLockTest, TwoHandlesGenerationSequenceIsGapFreeNoDataLoss) {
  const fs::path root = btdb_root("gapfree");
  auto created = BacktestDb::create(root.string());
  ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
  EXPECT_EQ(created->generation(), 1u);

  auto handle_a = BacktestDb::open(root.string());
  auto handle_b = BacktestDb::open(root.string());
  ASSERT_TRUE(handle_a.has_value());
  ASSERT_TRUE(handle_b.has_value());
  ASSERT_EQ(handle_a->generation(), 1u);
  ASSERT_EQ(handle_b->generation(), 1u);

  // A commits first: generation 1 -> 2.
  ASSERT_TRUE(handle_a->register_template(btdb_template("template-a")).has_value());
  EXPECT_EQ(handle_a->generation(), 2u);

  // B was ALSO opened at generation 1 and decided its write against that
  // stale snapshot. Without the D3 fix this would silently clobber A's
  // generation-2 manifest with a generation-2 manifest of its own that
  // never saw template-a. With the fix it must fail cleanly instead.
  auto conflict = handle_b->register_template(btdb_template("template-b"));
  ASSERT_FALSE(conflict.has_value())
      << "B's write, decided against a stale snapshot, must not silently clobber A's commit";
  EXPECT_EQ(conflict.error().code(), atx::core::ErrorCode::Unavailable);

  // persist_locked resyncs `snapshot_` on that conflict -- B's very next
  // attempt (same call, no explicit refresh()) sees A's commit and succeeds
  // at the next generation.
  EXPECT_EQ(handle_b->generation(), 2u) << "conflict path did not resync the caller's snapshot";
  ASSERT_TRUE(handle_b->register_template(btdb_template("template-b")).has_value());
  EXPECT_EQ(handle_b->generation(), 3u);

  // Gap-free, and nothing lost: reload a THIRD, fresh handle straight off
  // disk and confirm both templates independently survived.
  auto verify = BacktestDb::open(root.string());
  ASSERT_TRUE(verify.has_value());
  EXPECT_EQ(verify->generation(), 3u);
  EXPECT_TRUE(verify->find_template("template-a").has_value());
  EXPECT_TRUE(verify->find_template("template-b").has_value());
}

TEST(BacktestDbWriterLockTest, LockFileDoesNotSurviveASuccessfulPersist) {
  const fs::path root = btdb_root("nolockleak");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->register_template(btdb_template("t1")).has_value());

  const fs::path lock = root / (std::string(kBacktestDbManifestName) + ".lock");
  EXPECT_FALSE(fs::exists(lock)) << "manifest writer lock leaked after a successful persist";
}

// ── SurfaceDbWriterLockTest: same retrofit, surface_db.cpp:1070 ───────────

[[nodiscard]] fs::path sdb_root(std::string_view tag) { return scratch_dir(std::string("sdb_") + std::string(tag)); }

TEST(SurfaceDbWriterLockTest, TwoHandlesGenerationSequenceIsGapFreeNoDataLoss) {
  const fs::path root = sdb_root("gapfree");
  auto created = SurfaceDb::create(root.string());
  ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().to_string());
  EXPECT_EQ(created->generation(), 1u);

  auto handle_a = SurfaceDb::open(root.string());
  auto handle_b = SurfaceDb::open(root.string());
  ASSERT_TRUE(handle_a.has_value());
  ASSERT_TRUE(handle_b.has_value());
  ASSERT_EQ(handle_a->generation(), 1u);
  ASSERT_EQ(handle_b->generation(), 1u);

  ASSERT_TRUE(handle_a->upsert_symbol("AAPL", SymbolFitConfig{}).has_value());
  EXPECT_EQ(handle_a->generation(), 2u);

  auto conflict = handle_b->upsert_symbol("MSFT", SymbolFitConfig{});
  ASSERT_FALSE(conflict.has_value())
      << "B's write, decided against a stale snapshot, must not silently clobber A's commit";
  EXPECT_EQ(conflict.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_EQ(handle_b->generation(), 2u) << "conflict path did not resync the caller's snapshot";

  ASSERT_TRUE(handle_b->upsert_symbol("MSFT", SymbolFitConfig{}).has_value());
  EXPECT_EQ(handle_b->generation(), 3u);

  auto verify = SurfaceDb::open(root.string());
  ASSERT_TRUE(verify.has_value());
  EXPECT_EQ(verify->generation(), 3u);
  EXPECT_TRUE(verify->symbol_config("AAPL").has_value());
  EXPECT_TRUE(verify->symbol_config("MSFT").has_value());
}

TEST(SurfaceDbWriterLockTest, LockFileDoesNotSurviveASuccessfulPersist) {
  const fs::path root = sdb_root("nolockleak");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->upsert_symbol("AAPL", SymbolFitConfig{}).has_value());

  const fs::path lock = root / (std::string(kSurfaceDbManifestName) + ".lock");
  EXPECT_FALSE(fs::exists(lock)) << "manifest writer lock leaked after a successful persist";
}

} // namespace
} // namespace atx::vol
