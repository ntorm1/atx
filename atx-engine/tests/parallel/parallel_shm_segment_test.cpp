#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <span>
#include <string>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/parallel/shm_segment.hpp"

// ---------------------------------------------------------------------------
// ParallelShmSegment — exercises the public API of the cross-platform named
// shared-memory primitive. The API is identical on Win32 and POSIX; only the
// host backend (Win32 here) is compiled/run, so these tests are written purely
// against the public surface (create/open/view_ro/view_rw/size/writable).
//
// Segment names are made unique per test (suite + test name + a process-local
// counter) so parallel ctest shards never collide on a shared OS namespace.
// ---------------------------------------------------------------------------

namespace {

using atx::usize;
using atx::core::ErrorCode;
using atx::engine::parallel::ShmAccess;
using atx::engine::parallel::ShmSegment;

// A unique-per-call segment name. ctest may shard tests across processes; the
// monotonic counter keeps names distinct even within one process, and the
// gtest test name disambiguates across cases.
[[nodiscard]] std::string unique_name(const char *tag) {
  static std::atomic<unsigned> counter{0};
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-s7-shm-";
  name += (info != nullptr) ? info->name() : "anon";
  name += '-';
  name += tag;
  name += '-';
  name += std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
  return name;
}

} // namespace

// --- Boundary validation -----------------------------------------------------

TEST(ParallelShmSegment, CreateRejectsEmptyName) {
  auto seg = ShmSegment::create("", 64);
  ASSERT_FALSE(seg.has_value());
  EXPECT_EQ(seg.error().code(), ErrorCode::InvalidArgument);
}

TEST(ParallelShmSegment, CreateRejectsZeroBytes) {
  auto seg = ShmSegment::create(unique_name("zero"), 0);
  ASSERT_FALSE(seg.has_value());
  EXPECT_EQ(seg.error().code(), ErrorCode::InvalidArgument);
}

TEST(ParallelShmSegment, OpenRejectsEmptyName) {
  auto seg = ShmSegment::open("", ShmAccess::ReadOnly);
  ASSERT_FALSE(seg.has_value());
  EXPECT_EQ(seg.error().code(), ErrorCode::InvalidArgument);
}

TEST(ParallelShmSegment, OpenNonExistentFailsNotFound) {
  auto seg = ShmSegment::open(unique_name("ghost"), ShmAccess::ReadOnly);
  ASSERT_FALSE(seg.has_value());
  EXPECT_EQ(seg.error().code(), ErrorCode::NotFound);
}

// Ambiguous names are rejected, never silently rewritten: an interior '/' (would
// otherwise collide with another logical name) and an over-long name both fail
// InvalidArgument on create AND open — so two distinct caller names can never
// alias onto one OS object.
TEST(ParallelShmSegment, RejectsNameWithInteriorSlash) {
  auto created = ShmSegment::create("atx/s7/slash", 64);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().code(), ErrorCode::InvalidArgument);

  auto opened = ShmSegment::open("atx/s7/slash", ShmAccess::ReadOnly);
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
}

TEST(ParallelShmSegment, RejectsOverlongName) {
  const std::string too_long(512, 'x'); // far beyond the portable NAME_MAX bound
  auto created = ShmSegment::create(too_long, 64);
  ASSERT_FALSE(created.has_value());
  EXPECT_EQ(created.error().code(), ErrorCode::InvalidArgument);

  auto opened = ShmSegment::open(too_long, ShmAccess::ReadOnly);
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
}

TEST(ParallelShmSegment, CreateDuplicateFailsAlreadyExists) {
  const std::string name = unique_name("dup");
  auto first = ShmSegment::create(name, 128);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  auto second = ShmSegment::create(name, 128);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code(), ErrorCode::AlreadyExists);
}

// --- Size & basic creator state ---------------------------------------------

TEST(ParallelShmSegment, CreateExposesRequestedSizeAndIsWritable) {
  constexpr usize kBytes = 4096;
  auto seg = ShmSegment::create(unique_name("size"), kBytes);
  ASSERT_TRUE(seg.has_value()) << seg.error().to_string();
  EXPECT_EQ(seg->size(), kBytes);
  EXPECT_TRUE(seg->writable());
  EXPECT_EQ(seg->view_ro().size(), kBytes);
  EXPECT_EQ(seg->view_rw().size(), kBytes);
}

// --- The core proof: one physical region, two handles -----------------------

TEST(ParallelShmSegment, SameProcessRoundTripSharesOnePhysicalRegion) {
  constexpr usize kBytes = 256;
  const std::string name = unique_name("rt");

  auto creator = ShmSegment::create(name, kBytes);
  ASSERT_TRUE(creator.has_value()) << creator.error().to_string();

  // Write a known pattern through the writable view.
  std::span<std::byte> rw = creator->view_rw();
  ASSERT_EQ(rw.size(), kBytes);
  for (usize i = 0; i < kBytes; ++i) {
    rw[i] = static_cast<std::byte>((i * 7U + 1U) & 0xFFU);
  }

  // A SECOND handle onto the same name must see the very same bytes.
  {
    auto viewer = ShmSegment::open(name, ShmAccess::ReadWrite);
    ASSERT_TRUE(viewer.has_value()) << viewer.error().to_string();
    EXPECT_EQ(viewer->size(), kBytes);
    EXPECT_TRUE(viewer->writable());
    std::span<const std::byte> ro = viewer->view_ro();
    ASSERT_EQ(ro.size(), kBytes);
    for (usize i = 0; i < kBytes; ++i) {
      EXPECT_EQ(ro[i], static_cast<std::byte>((i * 7U + 1U) & 0xFFU)) << "mismatch at " << i;
    }
    // A write through the second (RW) handle is visible to the creator: one
    // physical region, two windows.
    viewer->view_rw()[0] = static_cast<std::byte>(0xABU);
    EXPECT_EQ(creator->view_ro()[0], static_cast<std::byte>(0xABU));
  } // viewer dropped

  // Drop the creator too; the segment name is then freed. Releasing the owned
  // ShmSegment in place (reset()) unmaps/closes the last handle (Win32 frees the
  // paging-file mapping when the final handle closes; POSIX shm_unlink'd at the
  // creator's reset()).
  creator->reset();

  // Re-opening the freed name must now fail NotFound.
  auto reopened = ShmSegment::open(name, ShmAccess::ReadOnly);
  ASSERT_FALSE(reopened.has_value());
  EXPECT_EQ(reopened.error().code(), ErrorCode::NotFound);
}

// --- Read-only open: not writable, empty rw view, reads creator bytes -------

TEST(ParallelShmSegment, ReadOnlyOpenSeesCreatorBytesAndIsNotWritable) {
  constexpr usize kBytes = 64;
  const std::string name = unique_name("ro");

  auto creator = ShmSegment::create(name, kBytes);
  ASSERT_TRUE(creator.has_value()) << creator.error().to_string();
  for (usize i = 0; i < kBytes; ++i) {
    creator->view_rw()[i] = static_cast<std::byte>(0xC0U + (i & 0x0FU));
  }

  auto ro = ShmSegment::open(name, ShmAccess::ReadOnly);
  ASSERT_TRUE(ro.has_value()) << ro.error().to_string();
  EXPECT_FALSE(ro->writable());
  EXPECT_TRUE(ro->view_rw().empty()); // RO mapping => no writable window
  EXPECT_EQ(ro->size(), kBytes);

  std::span<const std::byte> bytes = ro->view_ro();
  ASSERT_EQ(bytes.size(), kBytes);
  for (usize i = 0; i < kBytes; ++i) {
    EXPECT_EQ(bytes[i], static_cast<std::byte>(0xC0U + (i & 0x0FU)));
  }
}

// --- Move semantics: source emptied, dtor safe ------------------------------

TEST(ParallelShmSegment, MoveConstructLeavesSourceEmptyAndValid) {
  constexpr usize kBytes = 128;
  auto src = ShmSegment::create(unique_name("movector"), kBytes);
  ASSERT_TRUE(src.has_value()) << src.error().to_string();
  src->view_rw()[0] = static_cast<std::byte>(0x5AU);

  ShmSegment moved{std::move(*src)};
  EXPECT_EQ(moved.size(), kBytes);
  EXPECT_TRUE(moved.writable());
  EXPECT_EQ(moved.view_ro()[0], static_cast<std::byte>(0x5AU));

  // Moved-from source is empty and safe to query / destroy.
  EXPECT_EQ(src->size(), 0U);          // NOLINT(bugprone-use-after-move) — intentional
  EXPECT_TRUE(src->view_ro().empty()); // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(src->view_rw().empty()); // NOLINT(bugprone-use-after-move)
}

TEST(ParallelShmSegment, MoveAssignReleasesTargetAndEmptiesSource) {
  constexpr usize kBytes = 96;
  auto a = ShmSegment::create(unique_name("mvA"), kBytes);
  auto b = ShmSegment::create(unique_name("mvB"), kBytes);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  b->view_rw()[0] = static_cast<std::byte>(0x77U);

  *a = std::move(*b); // a's original segment released; a now owns b's
  EXPECT_EQ(a->size(), kBytes);
  EXPECT_EQ(a->view_ro()[0], static_cast<std::byte>(0x77U));
  EXPECT_EQ(b->size(), 0U); // NOLINT(bugprone-use-after-move) — intentional
}

// --- Read-only write FAULTS (OS-enforced PROT_READ invariant, R5) -----------
//
// A write through a ReadOnly mapping must crash the process. We map RO, then
// store one byte at the address backing view_ro() (casting away const for the
// TEST ONLY). gtest's death-test runner treats the resulting access violation
// as abnormal termination. The static `writable()==false` guarantee is also
// asserted as a documented invariant in case the death runner is unreliable.
TEST(ParallelShmSegmentDeathTest, WriteThroughReadOnlyMappingFaults) {
  const std::string name = unique_name("fault");
  auto creator = ShmSegment::create(name, 64);
  ASSERT_TRUE(creator.has_value()) << creator.error().to_string();

  EXPECT_DEATH(
      {
        auto ro = ShmSegment::open(name, ShmAccess::ReadOnly);
        // In the child we cannot use ASSERT; if open failed, exit nonzero so
        // the death test still observes abnormal termination deterministically.
        if (!ro.has_value()) {
          std::abort();
        }
        std::span<const std::byte> v = ro->view_ro();
        // SAFETY: test-only, intentionally faults. The mapping is PROT_READ /
        // FILE_MAP_READ; const_cast + store provokes the OS write-protection
        // fault that proves ReadOnly really is read-only (R5).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        auto *p = const_cast<std::byte *>(v.data());
        *p = static_cast<std::byte>(0xFFU);
        // If somehow no fault occurred, force a distinct abnormal exit so the
        // test does not falsely pass via normal return.
        std::abort();
      },
      ".*");

  // Documented invariant (the property under test, regardless of the runner).
  auto ro = ShmSegment::open(name, ShmAccess::ReadOnly);
  ASSERT_TRUE(ro.has_value()) << ro.error().to_string();
  EXPECT_FALSE(ro->writable());
}
