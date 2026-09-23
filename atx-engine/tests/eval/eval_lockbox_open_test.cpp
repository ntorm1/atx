// Lane 4 (l4-mtest): the single audited lockbox open — single-use, bound to
// the sealed panel's content address, with a tamper-evident receipt chain.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/eval/lockbox.hpp"

namespace atx_test_l4_mtest_lockbox_open {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::ErrorCode;
using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;
using namespace atx::engine::eval;

Panel build_panel(usize dates, usize insts, u64 seed) {
  std::vector<f64> close(dates * insts);
  u64 s = seed;
  for (usize c = 0; c < close.size(); ++c) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    close[c] = 100.0 + static_cast<f64>(s >> 40U) * 1e-6;
  }
  auto r = Panel::create(dates, insts, {"close"}, {close}, {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

OpenRequest request(u64 candidate) {
  OpenRequest q;
  q.purpose = "final OOS evaluation";
  q.requester = "l4-test";
  q.candidate_hash = candidate;
  return q;
}

TEST(EvalLockboxOpen, OpensOnceAndReturnsTheHeldOutSlice) {
  const Panel full = build_panel(100U, 3U, 1U);
  auto sealed = reserve_lockbox(full, 0.2, usize{2});
  ASSERT_TRUE(sealed.has_value());
  const u64 addr = sealed->reservation().content_address;
  InMemoryLockboxAudit audit;
  auto opened = open_lockbox(std::move(*sealed), full, request(0xabcU), audit);
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  const Panel &h = opened->holdout;
  ASSERT_EQ(h.dates(), 20U);
  ASSERT_EQ(h.instruments(), 3U);
  for (usize t = 0; t < 20U; ++t) {
    const auto want = full.field_cross_section(FieldId{0}, 80U + t);
    const auto got = h.field_cross_section(FieldId{0}, t);
    for (usize j = 0; j < 3U; ++j) {
      EXPECT_EQ(got[j], want[j]);
    }
  }
  const LockboxReceipt &r = opened->receipt;
  EXPECT_EQ(r.content_address, addr);
  EXPECT_EQ(r.candidate_hash, 0xabcU);
  EXPECT_EQ(r.holdout_begin, 80U);
  EXPECT_EQ(r.holdout_end, 100U);
  EXPECT_EQ(r.sequence, 0U);
  EXPECT_TRUE(verify_receipt(r));
  ASSERT_EQ(audit.receipts().size(), 1U);
  EXPECT_EQ(audit.receipts()[0].receipt_hash, r.receipt_hash);
}

TEST(EvalLockboxOpen, SecondOpenIsRefusedWithTypedErr) {
  const Panel full = build_panel(60U, 2U, 2U);
  InMemoryLockboxAudit audit;
  auto s1 = reserve_lockbox(full, 0.25, usize{1});
  ASSERT_TRUE(s1.has_value());
  ASSERT_TRUE(open_lockbox(std::move(*s1), full, request(1U), audit).has_value());
  // A fresh SealedPanel over the same panel + geometry is the same lockbox.
  auto s2 = reserve_lockbox(full, 0.25, usize{1});
  ASSERT_TRUE(s2.has_value());
  const auto again = open_lockbox(std::move(*s2), full, request(2U), audit);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code(), ErrorCode::AlreadyExists);
  EXPECT_EQ(audit.receipts().size(), 1U);
}

TEST(EvalLockboxOpen, MismatchedPanelAndMissingCommitmentAreRefused) {
  const Panel full = build_panel(50U, 2U, 3U);
  const Panel other = build_panel(50U, 2U, 4U);
  InMemoryLockboxAudit audit;
  auto s = reserve_lockbox(full, 0.2, usize{1});
  ASSERT_TRUE(s.has_value());
  const auto wrong = open_lockbox(SealedPanel{*s}, other, request(9U), audit);
  ASSERT_FALSE(wrong.has_value());
  EXPECT_EQ(wrong.error().code(), ErrorCode::PermissionDenied);
  const auto uncommitted = open_lockbox(SealedPanel{*s}, full, request(0U), audit);
  ASSERT_FALSE(uncommitted.has_value());
  EXPECT_EQ(uncommitted.error().code(), ErrorCode::InvalidArgument);
  EXPECT_TRUE(audit.receipts().empty()); // refusals consume nothing
  EXPECT_TRUE(open_lockbox(std::move(*s), full, request(9U), audit).has_value());
}

TEST(EvalLockboxOpen, ReceiptHashIsBoundToEveryField) {
  const Panel full = build_panel(40U, 2U, 5U);
  InMemoryLockboxAudit audit;
  auto s = reserve_lockbox(full, 0.25, usize{0});
  ASSERT_TRUE(s.has_value());
  auto opened = open_lockbox(std::move(*s), full, request(77U), audit);
  ASSERT_TRUE(opened.has_value());
  const LockboxReceipt good = opened->receipt;
  ASSERT_TRUE(verify_receipt(good));
  LockboxReceipt bad = good;
  bad.candidate_hash = 78U;
  EXPECT_FALSE(verify_receipt(bad));
  bad = good;
  bad.content_address ^= 1U;
  EXPECT_FALSE(verify_receipt(bad));
  bad = good;
  bad.purpose += "!";
  EXPECT_FALSE(verify_receipt(bad));
  bad = good;
  bad.holdout_end -= 1U;
  EXPECT_FALSE(verify_receipt(bad));
}

TEST(EvalLockboxOpen, InteriorWindowOpensOnlyItsSlice) {
  const Panel full = build_panel(80U, 2U, 6U);
  auto s = reserve_window(full, 50U, 10U, 3U);
  ASSERT_TRUE(s.has_value());
  InMemoryLockboxAudit audit;
  OpenRequest q = request(5U);
  q.holdout_len = 10U;
  auto opened = open_lockbox(std::move(*s), full, q, audit);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->holdout.dates(), 10U);
  EXPECT_EQ(opened->receipt.holdout_begin, 50U);
  EXPECT_EQ(opened->receipt.holdout_end, 60U);
  // Past the panel end is refused.
  auto s2 = reserve_window(build_panel(80U, 2U, 7U), 75U, 5U, 1U);
  ASSERT_TRUE(s2.has_value());
  OpenRequest far = request(5U);
  far.holdout_len = 6U;
  const auto bad = open_lockbox(std::move(*s2), build_panel(80U, 2U, 7U), far, audit);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
}

TEST(EvalLockboxOpen, FileAuditPersistsAndDetectsTampering) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "atx_l4_lockbox_audit.log";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  const Panel full = build_panel(40U, 2U, 8U);
  {
    auto audit = FileLockboxAudit::open(path);
    ASSERT_TRUE(audit.has_value()) << audit.error().to_string();
    auto s = reserve_lockbox(full, 0.25, usize{1});
    ASSERT_TRUE(s.has_value());
    OpenRequest q = request(3U);
    q.purpose = "tab\tand\nnewline"; // sanitized into the single-line log
    auto opened = open_lockbox(std::move(*s), full, q, *audit);
    ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
    EXPECT_TRUE(verify_receipt(opened->receipt));
  }
  {
    // A "new process": the log alone refuses the second open.
    auto audit = FileLockboxAudit::open(path);
    ASSERT_TRUE(audit.has_value()) << audit.error().to_string();
    ASSERT_EQ(audit->receipts().size(), 1U);
    EXPECT_TRUE(verify_receipt(audit->receipts()[0]));
    auto s = reserve_lockbox(full, 0.25, usize{1});
    ASSERT_TRUE(s.has_value());
    const auto again = open_lockbox(std::move(*s), full, request(4U), *audit);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::AlreadyExists);
  }
  // Editing the committed log breaks its hash chain: the sink refuses to load.
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekg(12);
    const int c = f.get();
    f.seekp(12);
    f.put(c == 'f' ? '0' : 'f');
  }
  EXPECT_FALSE(FileLockboxAudit::open(path).has_value());
  std::filesystem::remove(path, ec);
}

} // namespace atx_test_l4_mtest_lockbox_open
