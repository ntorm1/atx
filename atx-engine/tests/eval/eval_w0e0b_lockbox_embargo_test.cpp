// W0-E0b (trial accounting): the lockbox embargo tied to the label horizon
// (findings E-17) and the lockbox audit chain head exported outside the log
// (the status-table "L4" gap). Suites EvalLockboxEmbargo_*.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/eval/cpcv.hpp"
#include "atx/engine/eval/lockbox.hpp"

namespace atx_test_w0_e0b_lockbox_embargo {

using namespace atx::engine::eval;
using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::ErrorCode;
using atx::engine::alpha::Panel;

Panel build_panel(usize dates, usize insts, u64 seed) {
  std::vector<f64> close(dates * insts);
  u64 s = seed;
  for (f64 &c : close) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    c = 100.0 + static_cast<f64>(s >> 40U) * 1e-6;
  }
  auto r = Panel::create(dates, insts, {"close"}, {close}, {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

LockboxEmbargo horizon(usize h, usize delay) {
  LockboxEmbargo e;
  e.max_label_horizon = h;
  e.delay = delay;
  return e;
}

TEST(EvalLockboxEmbargo_Len, EmbargoIsLabelHorizonPlusDelay) {
  EXPECT_EQ(LockboxEmbargo{}.rule, EmbargoRule::LabelHorizonV2); // the default moved (E-17)
  // V2: independent of the panel length.
  for (const usize t : {usize{100}, usize{1750}, usize{5000}}) {
    const auto len = lockbox_embargo_len(horizon(21U, 1U), t);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 22U) << "T=" << t;
  }
  EXPECT_EQ(*lockbox_embargo_len(horizon(63U, 0U), 1750U), 63U);
  // V1 reproduces the legacy ceil(0.01 * T) width exactly.
  LockboxEmbargo v1;
  v1.rule = EmbargoRule::CpcvFractionV1;
  EXPECT_EQ(*lockbox_embargo_len(v1, 1750U), 18U);
  EXPECT_EQ(*lockbox_embargo_len(v1, 100U), 1U);
  EXPECT_EQ(*lockbox_embargo_len(v1, 1750U),
            detail::embargo_len_from_cpcv(CpcvConfig{}.embargo, 1750U));
  // An undeclared horizon and an overflowing width are refused.
  const auto undeclared = lockbox_embargo_len(horizon(0U, 1U), 1750U);
  ASSERT_FALSE(undeclared.has_value());
  EXPECT_EQ(undeclared.error().code(), ErrorCode::InvalidArgument);
  EXPECT_FALSE(
      lockbox_embargo_len(horizon(2U, std::numeric_limits<usize>::max()), 1750U).has_value());
  LockboxEmbargo unknown;
  unknown.rule = static_cast<EmbargoRule>(9U);
  EXPECT_FALSE(lockbox_embargo_len(unknown, 1750U).has_value());
}

TEST(EvalLockboxEmbargo_Reserve, ReservationUsesTheDeclaredHorizon) {
  const Panel panel = build_panel(200U, 3U, 1U);
  auto sealed = reserve_lockbox(panel, 0.2, horizon(5U, 1U));
  ASSERT_TRUE(sealed.has_value()) << sealed.error().to_string();
  const SealedReservation &r = sealed->reservation();
  EXPECT_EQ(r.lockbox_begin, 160U);
  EXPECT_EQ(r.embargo_len, 6U);
  EXPECT_EQ(r.visible_len, 154U);
  // Identical to the explicit-length reservation (same address, same cut).
  auto same = reserve_lockbox(panel, 0.2, usize{6});
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(same->reservation().content_address, r.content_address);
  // The legacy rule reproduces the CpcvConfig overload byte-for-byte.
  LockboxEmbargo v1;
  v1.rule = EmbargoRule::CpcvFractionV1;
  auto legacy = reserve_lockbox(panel, 0.2, v1);
  auto cpcv = reserve_lockbox(panel, 0.2, CpcvConfig{});
  ASSERT_TRUE(legacy.has_value() && cpcv.has_value());
  EXPECT_EQ(legacy->reservation().embargo_len, 2U); // ceil(0.01 * 200)
  EXPECT_EQ(legacy->reservation().content_address, cpcv->reservation().content_address);
  // reserve_window takes the same embargo.
  auto win = reserve_window(panel, 120U, 30U, horizon(10U, 2U));
  ASSERT_TRUE(win.has_value());
  EXPECT_EQ(win->reservation().embargo_len, 12U);
  EXPECT_EQ(win->reservation().visible_len, 108U);
  auto win_same = reserve_window(panel, 120U, 30U, usize{12});
  ASSERT_TRUE(win_same.has_value());
  EXPECT_EQ(win_same->reservation().content_address, win->reservation().content_address);
  // Errors propagate: undeclared horizon, and a gap that empties the visible region.
  EXPECT_FALSE(reserve_lockbox(panel, 0.2, horizon(0U, 0U)).has_value());
  EXPECT_FALSE(reserve_lockbox(panel, 0.2, horizon(160U, 0U)).has_value());
}

// The property the embargo exists for: a label formed at ANY visible date t
// (horizon h, delay d) reads returns only up to t + d + h, which must stay
// before the lockbox. V2 guarantees it for every (h, d); the legacy width does
// not (E-17).
TEST(EvalLockboxEmbargo_Reserve, NoVisibleLabelReadsTheLockbox) {
  const Panel panel = build_panel(200U, 2U, 2U);
  usize v1_leaks = 0U;
  for (const usize h : {usize{1}, usize{5}, usize{21}, usize{63}}) {
    for (const usize d : {usize{0}, usize{1}, usize{2}}) {
      auto s = reserve_lockbox(panel, 0.2, horizon(h, d));
      ASSERT_TRUE(s.has_value());
      const SealedReservation &r = s->reservation();
      const usize last_visible = r.visible_len - 1U;
      EXPECT_LT(last_visible + d + h, r.lockbox_begin) << "h=" << h << " d=" << d;
      LockboxEmbargo v1;
      v1.rule = EmbargoRule::CpcvFractionV1;
      auto l = reserve_lockbox(panel, 0.2, v1);
      ASSERT_TRUE(l.has_value());
      const SealedReservation &lr = l->reservation();
      if (lr.visible_len - 1U + d + h >= lr.lockbox_begin) {
        ++v1_leaks;
      }
    }
  }
  // Under the legacy 2-date gap, every (h, d) with h + d >= 3 leaks.
  EXPECT_EQ(v1_leaks, 10U);
}

// ---------------------------------------------------------------------------
//  Chain head exported outside the audit log.
// ---------------------------------------------------------------------------
OpenRequest request(u64 candidate) {
  OpenRequest q;
  q.purpose = "w0e0b chain head";
  q.requester = "w0e0b-test";
  q.candidate_hash = candidate;
  return q;
}

TEST(EvalLockboxEmbargo_ChainHead, AnchorDetectsRemovedAndReplacedReceipts) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "atx_w0e0b_lockbox_chain_head.log";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  LockboxChainHead anchor;
  std::vector<LockboxReceipt> committed;
  {
    auto audit = FileLockboxAudit::open(path);
    ASSERT_TRUE(audit.has_value()) << audit.error().to_string();
    EXPECT_EQ(lockbox_chain_head(*audit), LockboxChainHead{});
    for (u64 k = 0; k < 2U; ++k) {
      const Panel full = build_panel(100U, 2U, 10U + k); // disjoint held-out data
      auto sealed = reserve_lockbox(full, 0.2, horizon(1U, 1U));
      ASSERT_TRUE(sealed.has_value());
      auto opened = open_lockbox(std::move(*sealed), full, request(100U + k), *audit);
      ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
    }
    anchor = lockbox_chain_head(*audit);
    committed = audit->receipts();
  }
  ASSERT_EQ(anchor.receipts, 2U);
  EXPECT_EQ(anchor.head, committed.back().receipt_hash);
  EXPECT_TRUE(verify_lockbox_chain_head(committed, anchor).has_value());
  // An earlier anchor stays valid as receipts are added after it.
  EXPECT_TRUE(
      verify_lockbox_chain_head(committed, LockboxChainHead{1U, committed[0].receipt_hash})
          .has_value());
  EXPECT_TRUE(verify_lockbox_chain_head(committed, LockboxChainHead{}).has_value());

  // Delete the last receipt line: the chain prefix is still valid in-log ...
  std::string text;
  {
    std::ifstream in(path, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const usize first_nl = text.find('\n');
  ASSERT_NE(first_nl, std::string::npos);
  std::filesystem::resize_file(path, first_nl + 1U);
  auto shortened = FileLockboxAudit::open(path);
  ASSERT_TRUE(shortened.has_value()) << "a truncated log still verifies on its own";
  EXPECT_EQ(shortened->receipts().size(), 1U);
  // ... and only the exported head reveals the removal.
  const auto removed = verify_lockbox_chain_head(shortened->receipts(), anchor);
  ASSERT_FALSE(removed.has_value());
  EXPECT_EQ(removed.error().code(), ErrorCode::ParseError);

  // A replaced last receipt (different hash at the anchored position) fails too.
  std::vector<LockboxReceipt> replaced = committed;
  replaced.back().candidate_hash ^= 1U;
  replaced.back().receipt_hash = lockbox_receipt_hash(replaced.back());
  EXPECT_FALSE(verify_lockbox_chain_head(replaced, anchor).has_value());
  std::filesystem::remove(path, ec);
}

} // namespace atx_test_w0_e0b_lockbox_embargo
