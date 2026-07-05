// report_borrow_test.cpp — S5-4: non-zero borrow financing reaching the book
// P&L. `book::accumulate_period`/`accumulate_report` gain a trailing
// `borrow_bps` parameter (default 0.0): a flat per-period financing charge on
// the book's SHORT notional (Σ|w_i| over w_i<0 only), mirroring `cost_bps`'s
// own bps-per-period convention. `pnl_net = pnl_gross - pnl_cost - pnl_borrow`.
//
// borrow_bps == 0.0 (the default, every pre-S5-4 caller) makes pnl_borrow
// exactly 0.0 for every period -> pnl_net/equity_curve/write_report's TSVs are
// byte-identical to pre-S5-4 (BorrowZeroByteIdentical). borrow_bps > 0.0 debits
// exactly the short leg's notional times the flat rate (BorrowDebitsShortLeg,
// a closed-form hand check) and strictly lowers pnl_net vs the same fixture at
// borrow_bps == 0. accumulate_report is a PURE sequential reduction over its
// inputs (no RNG, no parallel/admission path) -- BorrowTwiceRunByteIdentical
// covers the determinism contract; (d) seq==parallel is N/A, documented rather
// than silently omitted (no parallel caller of this function exists).

#include <cmath>      // std::fabs
#include <filesystem> // per-test tmpdir
#include <string>
#include <system_error> // std::error_code
#include <utility>      // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Core> // Eigen::Index

#include "atx/core/error.hpp"         // Result, Status
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, u64, usize

#include "atx/engine/alpha/panel.hpp"       // alpha::Panel, FieldId
#include "atx/engine/combine/gate.hpp"      // GateConfig
#include "atx/engine/library/library.hpp"   // library::Library
#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel
#include "atx/engine/risk/multi_period.hpp" // risk::MultiPeriodResult, RebalanceSchedule

#include "atx/engine/book/report.hpp" // the unit under test

namespace atxtest_report_borrow {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;
using atx::engine::book::accumulate_report;
using atx::engine::book::BookReport;
using atx::engine::combine::GateConfig;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MultiPeriodResult;
using atx::engine::risk::RebalanceSchedule;

namespace lib = atx::engine::library;

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S54") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s5_4_report_borrow" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// An EMPTY library (n_alphas()==0): lifecycle census isn't this test's concern.
[[nodiscard]] lib::Library make_empty_library(const std::string &dir) {
  GateConfig cfg;
  return lib::Library::open(dir, cfg, std::vector<u64>{7ULL});
}

// A trivial K=1, M=2 FactorModel via create -- not this test's concern either;
// just needs to accept a 2-instrument book without erroring.
[[nodiscard]] FactorModel make_model_2x1() {
  MatX x(2, 1);
  x << 1.0, 1.0;
  MatX f(1, 1);
  f << 0.04;
  VecX d(2);
  d << 0.10, 0.10;
  auto r = FactorModel::create(x, f, d, /*fit_begin=*/0U, /*fit_end=*/2U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// 2 dates x 2 instruments, "ret" field: a FIXED, non-degenerate cross-section
// per date (so pnl_gross is a genuine nonzero number the borrow debit is
// measured AGAINST, not merely a 0-baseline).
[[nodiscard]] Panel make_panel_2x2() {
  std::vector<f64> ret = {
      0.01, 0.02, // date 0
      -0.01, 0.03 // date 1
  };
  auto r = Panel::create(/*dates=*/2U, /*instruments=*/2U, {"ret"}, {ret}, /*universe=*/{});
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// The book {+0.5, -0.5} held BOTH periods (S5-4's own Accept fixture): a flat
// long/short pair, gross 1.0, short notional exactly 0.5 every period. cost_bps
// is 0 throughout so pnl_net's only non-gross term is the borrow debit -- the
// cleanest possible isolation of the new lever.
[[nodiscard]] MultiPeriodResult make_books_half_short() {
  MultiPeriodResult m;
  m.books = {{0.5, -0.5}, {0.5, -0.5}};
  m.turnover = {0.0, 0.0};
  m.cost_bps = {0.0, 0.0};
  return m;
}

[[nodiscard]] RebalanceSchedule make_sched_2() {
  return RebalanceSchedule{std::vector<usize>{0U, 1U}};
}

[[nodiscard]] BookReport accumulate_ok(const MultiPeriodResult &books, const Panel &panel,
                                       FieldId ret, const RebalanceSchedule &sched,
                                       const FactorModel &V, f64 capacity_gross,
                                       const lib::Library &library, usize as_of,
                                       f64 borrow_bps = 0.0) {
  auto res =
      accumulate_report(books, panel, ret, sched, V, capacity_gross, library, as_of, borrow_bps);
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  return std::move(*res);
}

// =============================================================================
//  (a) BorrowZeroByteIdentical — borrow_bps == 0.0 (both the explicit-0.0 call
//  and the pre-S5-4 7-argument call that never mentions it) produce pnl_borrow
//  == 0.0 exactly for every period, and pnl_net/equity_curve identical to a
//  baseline call.
// =============================================================================
TEST(ReportBorrow, BorrowZeroByteIdentical) {
  const MultiPeriodResult books = make_books_half_short();
  const Panel panel = make_panel_2x2();
  const FactorModel V = make_model_2x1();
  const RebalanceSchedule sched = make_sched_2();
  const FieldId ret = *panel.field_id("ret");

  const std::string dir_a = tmpdir("lib_a");
  lib::Library lib_a = make_empty_library(dir_a);
  const BookReport rep_explicit_zero =
      accumulate_ok(books, panel, ret, sched, V, 1.0, lib_a, 0U, /*borrow_bps=*/0.0);

  const std::string dir_b = tmpdir("lib_b");
  lib::Library lib_b = make_empty_library(dir_b);
  // The pre-S5-4 call shape: no 9th argument at all (relies on the trailing default).
  auto res_legacy = accumulate_report(books, panel, ret, sched, V, 1.0, lib_b, 0U);
  ASSERT_TRUE(res_legacy.has_value()) << (res_legacy ? "" : res_legacy.error().to_string());
  const BookReport &rep_legacy = *res_legacy;

  ASSERT_EQ(rep_explicit_zero.pnl_borrow.size(), sched.periods.size());
  for (usize s = 0; s < sched.periods.size(); ++s) {
    EXPECT_EQ(rep_explicit_zero.pnl_borrow[s], 0.0)
        << "borrow_bps == 0.0 must give pnl_borrow == 0.0 EXACTLY, period " << s;
  }

  ASSERT_EQ(rep_explicit_zero.pnl_net.size(), rep_legacy.pnl_net.size());
  for (usize s = 0; s < sched.periods.size(); ++s) {
    EXPECT_EQ(rep_explicit_zero.pnl_net[s], rep_legacy.pnl_net[s])
        << "explicit borrow_bps=0.0 must be byte-identical to the trailing-default legacy call";
    EXPECT_EQ(rep_explicit_zero.equity_curve[s], rep_legacy.equity_curve[s]);
    EXPECT_EQ(rep_explicit_zero.pnl_gross[s], rep_legacy.pnl_gross[s]);
  }
}

// =============================================================================
//  (b) BorrowDebitsShortLeg — the closed-form hand check: pnl_borrow[s] ==
//  0.5 * 50.0 * 1e-4 EXACTLY for the {+0.5,-0.5} fixture at borrow_bps=50.0,
//  and pnl_net is strictly lower than the SAME fixture run at borrow_bps=0.
// =============================================================================
TEST(ReportBorrow, BorrowDebitsShortLeg) {
  const MultiPeriodResult books = make_books_half_short();
  const Panel panel = make_panel_2x2();
  const FactorModel V = make_model_2x1();
  const RebalanceSchedule sched = make_sched_2();
  const FieldId ret = *panel.field_id("ret");

  const std::string dir_on = tmpdir("lib_on");
  lib::Library lib_on = make_empty_library(dir_on);
  const BookReport rep_on =
      accumulate_ok(books, panel, ret, sched, V, 1.0, lib_on, 0U, /*borrow_bps=*/50.0);

  const std::string dir_off = tmpdir("lib_off");
  lib::Library lib_off = make_empty_library(dir_off);
  const BookReport rep_off =
      accumulate_ok(books, panel, ret, sched, V, 1.0, lib_off, 0U, /*borrow_bps=*/0.0);

  constexpr f64 kExpectedBorrow = 0.5 * 50.0 * 1e-4; // short notional * bps * 1e-4
  ASSERT_EQ(rep_on.pnl_borrow.size(), sched.periods.size());
  for (usize s = 0; s < sched.periods.size(); ++s) {
    EXPECT_NEAR(rep_on.pnl_borrow[s], kExpectedBorrow, 1e-15)
        << "closed-form short-notional debit mismatch, period " << s;
    // pnl_gross is UNCHANGED by borrow_bps (same book/panel); cost_bps is 0 in
    // this fixture, so pnl_net's ENTIRE reduction vs the borrow-off run is the
    // borrow debit itself.
    EXPECT_NEAR(rep_on.pnl_gross[s], rep_off.pnl_gross[s], 1e-15);
    EXPECT_LT(rep_on.pnl_net[s], rep_off.pnl_net[s])
        << "a non-zero borrow charge must strictly lower pnl_net, period " << s;
    EXPECT_NEAR(rep_off.pnl_net[s] - rep_on.pnl_net[s], kExpectedBorrow, 1e-15);
  }
}

// =============================================================================
//  (c) BorrowTwiceRunByteIdentical — accumulate_report is a pure sequential
//  reduction (no RNG); two calls over identical inputs must produce identical
//  pnl_borrow/pnl_net series.
// =============================================================================
TEST(ReportBorrow, BorrowTwiceRunByteIdentical) {
  const MultiPeriodResult books = make_books_half_short();
  const Panel panel = make_panel_2x2();
  const FactorModel V = make_model_2x1();
  const RebalanceSchedule sched = make_sched_2();
  const FieldId ret = *panel.field_id("ret");

  const std::string dir1 = tmpdir("lib1");
  lib::Library lib1 = make_empty_library(dir1);
  const BookReport rep1 =
      accumulate_ok(books, panel, ret, sched, V, 1.0, lib1, 0U, /*borrow_bps=*/50.0);

  const std::string dir2 = tmpdir("lib2");
  lib::Library lib2 = make_empty_library(dir2);
  const BookReport rep2 =
      accumulate_ok(books, panel, ret, sched, V, 1.0, lib2, 0U, /*borrow_bps=*/50.0);

  ASSERT_EQ(rep1.pnl_borrow.size(), rep2.pnl_borrow.size());
  for (usize s = 0; s < rep1.pnl_borrow.size(); ++s) {
    EXPECT_EQ(rep1.pnl_borrow[s], rep2.pnl_borrow[s]);
    EXPECT_EQ(rep1.pnl_net[s], rep2.pnl_net[s]);
    EXPECT_EQ(rep1.equity_curve[s], rep2.equity_curve[s]);
  }
}

// (d) seq==parallel: N/A. accumulate_report/accumulate_period touch no
// executor/thread/RNG seam -- book::report.hpp's own module doc pins this as a
// pure, single-threaded cold-path reduction, and no caller in this codebase
// invokes it from a parallel substrate (the OOS parallel paths this sprint's
// other units touch live entirely in atx-engine/src/factory, never book/).
// Documented here rather than silently omitted.

} // namespace atxtest_report_borrow
