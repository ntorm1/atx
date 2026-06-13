// book_report_test.cpp — S7-4: reproducible operating-book reporting (§4.5).
//
// accumulate_report walks the rebalance schedule and rolls the realized book
// chain into a BookReport: net-of-cost equity, per-period pnl attribution
// (gross/net/cost), gross-leverage / net-exposure / turnover, per-period book
// factor exposure Xᵀw, capacity utilization, and the lifecycle-state census.
// write_report serializes the report to deterministic per-series files.
//
// The two LOAD-BEARING proofs:
//   * R7 — capacity_utilization == gross / capacity is <= 1 when the book is
//     sized at/under the capacity ceiling (CapacityUtilizationBounded).
//   * R8 — write_report is BYTE-IDENTICAL across two runs (fixed locale-free
//     float formatting, binary writes, no clock / map / absolute paths).
//
// Plus: net == gross − cost (AccumulatesNetOfCostEquityCurve), the factor-
// exposure matrix carries every (dead-augmented) factor column
// (FactorExposureIncludesDeadColumns), and a NaN realized-return cell contributes
// ZERO to pnl_gross (NoSurvivorship, R4).

#include <algorithm>    // std::sort
#include <array>        // std::array (lifecycle census)
#include <cmath>        // std::isnan, std::fabs
#include <filesystem>   // per-test tmpdir + byte-identity read-back
#include <fstream>      // raw file byte read (R8 proof) + std::ios
#include <iterator>     // std::istreambuf_iterator
#include <limits>       // std::numeric_limits (quiet_NaN)
#include <span>
#include <string>
#include <system_error> // std::error_code
#include <utility>      // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Core> // Eigen::Index

#include "atx/core/error.hpp"         // Result, Status
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, u64, usize

#include "atx/engine/alpha/panel.hpp"          // alpha::Panel, FieldId
#include "atx/engine/combine/gate.hpp"         // AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp"      // AlphaMetrics
#include "atx/engine/combine/store.hpp"        // AlphaId
#include "atx/engine/library/library.hpp"      // library::Library
#include "atx/engine/library/lifecycle.hpp"    // LifecycleState
#include "atx/engine/library/record.hpp"       // Provenance
#include "atx/engine/risk/factor_model.hpp"    // risk::FactorModel
#include "atx/engine/risk/multi_period.hpp"    // risk::MultiPeriodResult, RebalanceSchedule

#include "atx/engine/book/report.hpp" // the unit under test

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;
using atx::engine::book::accumulate_report;
using atx::engine::book::BookReport;
using atx::engine::book::write_report;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::GateConfig;
using atx::engine::library::LifecycleState;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MultiPeriodResult;
using atx::engine::risk::RebalanceSchedule;

namespace lib = atx::engine::library;

// Per-test unique temp directory (mirrors the S4 tests' tmpdir helper).
[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S7") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s7_book" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// Read every regular file under `dir` (ascending by name) into one byte blob,
// tagging each with its filename so the comparison covers BOTH names and bytes.
[[nodiscard]] std::vector<std::string> read_all_files(const std::string &dir) {
  std::vector<std::filesystem::path> paths;
  for (const auto &e : std::filesystem::directory_iterator(dir)) {
    if (e.is_regular_file()) {
      paths.push_back(e.path());
    }
  }
  std::sort(paths.begin(), paths.end()); // ascending name order (deterministic)
  std::vector<std::string> out;
  for (const auto &p : paths) {
    std::ifstream in(p, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // A genuine NUL byte separates name from contents: "x" + "\x00" (a one-char
    // C-string would terminate at the NUL and emit nothing — std::string("\x00",1)
    // forces the byte to be part of the element so name/content can't bleed across.
    out.push_back(p.filename().string() + std::string("\x00", 1) + bytes);
  }
  return out;
}

constexpr usize kInst = 3;   // instruments per cross-section (== M)
constexpr usize kDates = 3;  // panel dates
constexpr usize kT = 6;      // library pnl stream length

// A K=2, M=3 FactorModel via create (X 3×2, F 2×2 SPD, D length 3).
[[nodiscard]] FactorModel make_model() {
  MatX x(3, 2);
  x << 1.0, 0.0, // exposures: factor 0 ~ market, factor 1 ~ a style tilt
       0.5, 1.0,
       -1.0, 0.5;
  MatX f(2, 2);
  f << 0.04, 0.00,
       0.00, 0.02;
  VecX d(3);
  d << 0.10, 0.20, 0.05;
  auto r = FactorModel::create(x, f, d, /*fit_begin=*/0U, /*fit_end=*/3U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// A returns Panel: dates×instruments, single field "ret", date-major. A known
// realized cross-section per date; `nan_cell` optionally poisons (date,inst).
[[nodiscard]] Panel make_panel(bool poison = false) {
  // date-major column: [d0:i0,i1,i2, d1:..., d2:...].
  std::vector<f64> ret = {
      0.01, -0.02, 0.03,  // date 0
      0.00, 0.04, -0.01,  // date 1
      0.02, 0.01, 0.00};  // date 2
  if (poison) {
    ret[1] = std::numeric_limits<f64>::quiet_NaN(); // (date0, inst1) is NaN
  }
  std::vector<std::vector<f64>> cols = {ret};
  std::vector<std::string> names = {"ret"};
  auto r = Panel::create(kDates, kInst, names, cols, /*universe=*/{});
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// A 3-period realized book chain (books[s][i]) + matching turnover / cost_bps.
[[nodiscard]] MultiPeriodResult make_books() {
  MultiPeriodResult m;
  m.books = {
      {0.20, -0.10, 0.10}, // period 0 (gross 0.40)
      {0.15, 0.05, -0.20}, // period 1 (gross 0.40)
      {0.30, -0.05, 0.05}, // period 2 (gross 0.40)
  };
  m.turnover = {0.40, 0.30, 0.25};
  m.cost_bps = {0.40 * 5.0, 0.30 * 5.0, 0.25 * 5.0}; // turnover × 5bps round-trip
  return m;
}

[[nodiscard]] RebalanceSchedule make_sched() {
  return RebalanceSchedule{std::vector<usize>{0U, 1U, 2U}};
}

// A tiny library with two admitted alphas; one marked Live. as_of returns a
// census of {Admitted:1, Live:1} (the rest zero). Built by admit (Candidate->
// Admitted) then mark (Admitted->Live). Returns the dir-rooted Library.
[[nodiscard]] lib::Library make_library(const std::string &dir) {
  GateConfig cfg; // permissive defaults are fine; our metrics clear the floors
  lib::Library library = lib::Library::open(dir, cfg, std::vector<u64>{42ULL});
  AlphaGate gate{cfg};

  // Two structurally-distinct pnl streams (orthogonal enough to clear corr gate).
  std::vector<f64> pnl_a(kT), pnl_b(kT), pos(kT * 1, 0.0);
  for (usize t = 0; t < kT; ++t) {
    pnl_a[t] = (t % 2 == 0) ? 0.5 : -0.3; // alternating
    pnl_b[t] = static_cast<f64>(t) - 2.5; // ramp
  }
  AlphaMetrics good{/*sharpe*/ 2.0,   /*turnover*/ 0.10, /*returns*/ 0.5,
                    /*drawdown*/ 0.1, /*margin*/ 5.0,    /*fitness*/ 2.0,
                    /*holding_days*/ 10.0};

  lib::AlphaCandidate ca{};
  ca.canon_hash = 0xAAAA;
  ca.pnl = pnl_a;
  ca.pos_flat = pos;
  ca.metrics = good;
  ca.prov = lib::Provenance{"alpha_a", {}, 0, 1};
  ca.as_of = 0U;
  const auto va = library.admit(ca, gate);
  EXPECT_EQ(static_cast<int>(va.kind), static_cast<int>(lib::AdmitKind::Accept));

  lib::AlphaCandidate cb{};
  cb.canon_hash = 0xBBBB;
  cb.pnl = pnl_b;
  cb.pos_flat = pos;
  cb.metrics = good;
  cb.prov = lib::Provenance{"alpha_b", {}, 0, 2};
  cb.as_of = 0U;
  const auto vb = library.admit(cb, gate);
  EXPECT_EQ(static_cast<int>(vb.kind), static_cast<int>(lib::AdmitKind::Accept));

  // Promote alpha_a to Live as of period 1 (Admitted->Live is a legal edge).
  if (vb.kind == lib::AdmitKind::Accept) {
    const auto st = library.mark(va.id, LifecycleState::Live, 1U);
    EXPECT_TRUE(st.has_value());
  }
  return library;
}

// Unwrap accumulate_report (now Result<BookReport>): assert success and return the
// report so each happy-path test stays compact.
[[nodiscard]] BookReport accumulate_ok(const MultiPeriodResult &books, const Panel &panel,
                                       FieldId ret, const RebalanceSchedule &sched,
                                       const FactorModel &V, f64 capacity_gross,
                                       const lib::Library &library, usize as_of) {
  auto res = accumulate_report(books, panel, ret, sched, V, capacity_gross, library, as_of);
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  return std::move(*res);
}

// ===========================================================================
//  Net-of-cost equity + pnl attribution
// ===========================================================================
TEST(BookReport, AccumulatesNetOfCostEquityCurve) {
  const MultiPeriodResult books = make_books();
  const Panel panel = make_panel();
  const FactorModel V = make_model();
  const RebalanceSchedule sched = make_sched();
  const std::string dir = tmpdir("lib");
  lib::Library library = make_library(dir);
  const FieldId ret = *panel.field_id("ret");

  const BookReport rep =
      accumulate_ok(books, panel, ret, sched, V, /*capacity_gross=*/1.0, library, /*as_of=*/2U);

  ASSERT_EQ(rep.equity_curve.size(), sched.periods.size());
  ASSERT_EQ(rep.pnl_net.size(), sched.periods.size());
  for (usize s = 0; s < sched.periods.size(); ++s) {
    EXPECT_NEAR(rep.pnl_net[s], rep.pnl_gross[s] - rep.pnl_cost[s], 1e-12);
  }
  // Equity is the running cumulative sum of net pnl.
  f64 acc = 0.0;
  for (usize s = 0; s < sched.periods.size(); ++s) {
    acc += rep.pnl_net[s];
    EXPECT_NEAR(rep.equity_curve[s], acc, 1e-12);
  }
}

// ===========================================================================
//  Capacity utilization bounded (R7)
// ===========================================================================
TEST(BookReport, CapacityUtilizationBounded) {
  const MultiPeriodResult books = make_books(); // gross 0.40 each period
  const Panel panel = make_panel();
  const FactorModel V = make_model();
  const RebalanceSchedule sched = make_sched();
  const std::string dir = tmpdir("lib");
  lib::Library library = make_library(dir);
  const FieldId ret = *panel.field_id("ret");

  // capacity_gross = 0.5 >= every period's gross (0.40) ⇒ utilization <= 1.
  const BookReport rep =
      accumulate_ok(books, panel, ret, sched, V, /*capacity_gross=*/0.5, library, 2U);

  ASSERT_EQ(rep.capacity_utilization.size(), sched.periods.size());
  for (usize s = 0; s < sched.periods.size(); ++s) {
    EXPECT_LE(rep.capacity_utilization[s], 1.0 + 1e-9);
    EXPECT_NEAR(rep.capacity_utilization[s], rep.gross_leverage[s] / 0.5, 1e-12);
  }
}

// ===========================================================================
//  Byte-identical write (R8, LOAD-BEARING)
// ===========================================================================
TEST(BookReport, WriteIsByteIdenticalAcrossRuns) {
  const MultiPeriodResult books = make_books();
  const Panel panel = make_panel();
  const FactorModel V = make_model();
  const RebalanceSchedule sched = make_sched();
  const std::string libdir = tmpdir("lib");
  lib::Library library = make_library(libdir);
  const FieldId ret = *panel.field_id("ret");

  const BookReport rep =
      accumulate_ok(books, panel, ret, sched, V, /*capacity_gross=*/1.0, library, 2U);

  const std::string dirA = tmpdir("A");
  const std::string dirB = tmpdir("B");
  const auto stA = write_report(rep, dirA);
  const auto stB = write_report(rep, dirB);
  ASSERT_TRUE(stA.has_value()) << (stA ? "" : stA.error().to_string());
  ASSERT_TRUE(stB.has_value()) << (stB ? "" : stB.error().to_string());

  const std::vector<std::string> bytesA = read_all_files(dirA);
  const std::vector<std::string> bytesB = read_all_files(dirB);
  ASSERT_FALSE(bytesA.empty()); // wrote at least one file
  EXPECT_EQ(bytesA, bytesB);    // byte-for-byte identical (names + contents)
}

// ===========================================================================
//  Factor exposure carries every factor column
// ===========================================================================
TEST(BookReport, FactorExposureIncludesDeadColumns) {
  const MultiPeriodResult books = make_books();
  const Panel panel = make_panel();
  const FactorModel V = make_model(); // K == 2 factor columns
  const RebalanceSchedule sched = make_sched();
  const std::string dir = tmpdir("lib");
  lib::Library library = make_library(dir);
  const FieldId ret = *panel.field_id("ret");

  const BookReport rep =
      accumulate_ok(books, panel, ret, sched, V, /*capacity_gross=*/1.0, library, 2U);

  EXPECT_EQ(rep.factor_exposures.cols(), static_cast<Eigen::Index>(V.n_factors()));
  EXPECT_EQ(rep.factor_exposures.rows(), static_cast<Eigen::Index>(sched.periods.size()));

  // Cross-check one row against a hand-computed Xᵀw (period 0).
  const VecX w0 = (VecX(3) << 0.20, -0.10, 0.10).finished();
  const VecX expected = V.exposures().transpose() * w0;
  for (Eigen::Index k = 0; k < expected.size(); ++k) {
    EXPECT_NEAR(rep.factor_exposures(0, k), expected[k], 1e-12);
  }
}

// ===========================================================================
//  No-survivorship (R4): a NaN realized return contributes 0 to pnl_gross
// ===========================================================================
TEST(BookReport, NoSurvivorship) {
  const MultiPeriodResult books = make_books();
  const Panel clean = make_panel(/*poison=*/false);
  const Panel poisoned = make_panel(/*poison=*/true); // (date0, inst1) ret == NaN
  const FactorModel V = make_model();
  const RebalanceSchedule sched = make_sched();
  const std::string dir = tmpdir("lib");
  lib::Library library = make_library(dir);
  const FieldId ret = *clean.field_id("ret");

  const BookReport rc = accumulate_ok(books, clean, ret, sched, V, 1.0, library, 2U);
  const BookReport rp = accumulate_ok(books, poisoned, ret, sched, V, 1.0, library, 2U);

  // Period-0 book[1] = -0.10, clean ret[1] = -0.02 contributes -0.10·-0.02 = 0.002.
  // Poisoning that cell to NaN drops its contribution to 0, so the poisoned
  // pnl_gross[0] equals the clean value MINUS that one term.
  const f64 dropped = books.books[0][1] * (-0.02);
  EXPECT_NEAR(rp.pnl_gross[0], rc.pnl_gross[0] - dropped, 1e-12);
  EXPECT_FALSE(std::isnan(rp.pnl_gross[0])); // NaN never poisons the reduction
}

// ===========================================================================
//  Lifecycle census reflects the journal at as_of
// ===========================================================================
TEST(BookReport, LifecycleCensusCountsStates) {
  const MultiPeriodResult books = make_books();
  const Panel panel = make_panel();
  const FactorModel V = make_model();
  const RebalanceSchedule sched = make_sched();
  const std::string dir = tmpdir("lib");
  lib::Library library = make_library(dir); // alpha_a Live@1, alpha_b Admitted
  const FieldId ret = *panel.field_id("ret");

  // as_of = 2 (>= the Live transition at period 1): alpha_a is Live, alpha_b is
  // Admitted ⇒ census[Admitted]==1, census[Live]==1.
  const BookReport rep = accumulate_ok(books, panel, ret, sched, V, 1.0, library, /*as_of=*/2U);
  EXPECT_EQ(rep.lifecycle_census[static_cast<usize>(LifecycleState::Admitted)], 1U);
  EXPECT_EQ(rep.lifecycle_census[static_cast<usize>(LifecycleState::Live)], 1U);
  EXPECT_EQ(rep.lifecycle_census[static_cast<usize>(LifecycleState::Candidate)], 0U);
}

// ===========================================================================
//  Front-door guards (fix #1): OOB schedule date + dimension mismatch → Err
// ===========================================================================
TEST(BookReport, RejectsOutOfRangeScheduleDate) {
  const MultiPeriodResult books = make_books();
  const Panel panel = make_panel(); // kDates == 3 dates
  const FactorModel V = make_model();
  const std::string dir = tmpdir("lib");
  lib::Library library = make_library(dir);
  const FieldId ret = *panel.field_id("ret");

  // Schedule references date index 3, but the panel only has dates 0..2 ⇒ Err
  // (previously an ATX_ASSERT-only path → release OOB read).
  const RebalanceSchedule bad{std::vector<usize>{0U, 1U, 3U}};
  const auto res = accumulate_report(books, panel, ret, bad, V, 1.0, library, 2U);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(BookReport, RejectsDimMismatch) {
  const Panel panel = make_panel();
  const FactorModel V = make_model(); // n_instruments() == 3
  const RebalanceSchedule sched = make_sched();
  const std::string dir = tmpdir("lib");
  lib::Library library = make_library(dir);
  const FieldId ret = *panel.field_id("ret");

  // (a) MultiPeriodResult vectors shorter than the schedule (2 books vs 3 periods).
  {
    MultiPeriodResult short_books = make_books();
    short_books.books.pop_back();    // now 2 books, but 3 schedule periods
    short_books.turnover.pop_back();
    short_books.cost_bps.pop_back();
    const auto res = accumulate_report(short_books, panel, ret, sched, V, 1.0, library, 2U);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  // (b) a book whose length != V.n_instruments() (would UB the Xᵀw Map + pnl loop).
  {
    MultiPeriodResult bad_books = make_books();
    bad_books.books[1] = std::vector<f64>{0.1, 0.2}; // length 2, model wants 3
    const auto res = accumulate_report(bad_books, panel, ret, sched, V, 1.0, library, 2U);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

} // namespace
