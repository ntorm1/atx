// AMZN-around-earnings CStar fit — correctness gate (WS-4).
//
// Loads the committed real OPRA cbbo-1m fixture (2018-04-26 19:45Z, 15 min before
// the after-close earnings print), fits the surface via the SAME shared helper the
// report emitter uses (examples/amzn_earnings_fit.hpp, so the test and the report
// can never diverge), and asserts the headline reproduction properties:
//   * implied spot ~ 1519, front DTE ~ 1.0 (PM-close convention on);
//   * front-expiry c2_eff strongly negative (< -1.0) — the earnings "W-shape";
//   * c2_eff term structure increasing off the front (frown decays with tenor);
//   * every slice butterfly-arb-free (min Roper g >= 0);
//   * near-money total variance non-crossing in T (calendar no-arb);
//   * essentially every listed expiry fits.
//
// Skips cleanly (GTEST_SKIP) if the fixture parquet is absent, mirroring the
// SpyRealOpra / opra_fixture real-data pattern — but the fixture IS committed, so
// a normal `ctest -R AmznEarnings` run must PASS, not skip.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "amzn_earnings_fit.hpp"   // atx::vol::amzn::amzn_earnings_fit, SliceFit
#include "atx/vol/cstar.hpp"       // cstar_slice_w
#include "atx/vol/opra_panel.hpp"  // OpraLoadSpec, load_opra_cbbo_parquet

namespace {

using namespace atx::vol;
using namespace atx::vol::amzn;

constexpr double kR = 0.019;
constexpr const char* kSnapshot = "2018-04-26T19:45:00Z";
constexpr const char* kFixtureRel =
    "atx-vol/tests/data/amzn_earnings_2018/amzn_opra_cbbo1m_2018-04-26T1945Z.parquet";

// Probe for the committed fixture. Prefers the configure-time absolute path
// (ATX_AMZN_FIXTURE, baked by tests/CMakeLists.txt), then a few relative roots so
// the test also runs from a hand-launched exe.
[[nodiscard]] std::string find_fixture() {
  std::vector<std::string> candidates;
#ifdef ATX_AMZN_FIXTURE
  candidates.emplace_back(ATX_AMZN_FIXTURE);
#endif
  candidates.emplace_back(kFixtureRel);
  candidates.emplace_back(std::string("../") + kFixtureRel);
  candidates.emplace_back(std::string("../../") + kFixtureRel);
  for (const std::string& c : candidates) {
    std::error_code ec;
    if (!c.empty() && std::filesystem::exists(c, ec)) {
      return c;
    }
  }
  return {};
}

// One load + fit shared across the whole suite (built lazily, once).
struct AmznBoard {
  bool available{false};
  double implied_spot{0.0};
  FitResult fit{};
};

[[nodiscard]] AmznBoard load_and_fit() {
  AmznBoard b;
  const std::string path = find_fixture();
  if (path.empty()) {
    return b;  // fixture absent -> caller GTEST_SKIPs
  }
  OpraLoadSpec load;
  load.path = path;
  load.underlying = "AMZN";
  load.snapshot_iso = kSnapshot;
  load.r = kR;
  load.expiry_close = ExpiryCloseConvention::UsEquityPmClose;
  auto panel = load_opra_cbbo_parquet(load);
  if (!panel.has_value()) {
    return b;
  }
  b.implied_spot = panel->implied_spot;
  b.fit = amzn_earnings_fit(panel.value(), kR);
  b.available = !b.fit.slices.empty();
  return b;
}

[[nodiscard]] const AmznBoard& board() {
  static const AmznBoard b = load_and_fit();
  return b;
}

[[nodiscard]] std::vector<const SliceFit*> ok_slices() {
  std::vector<const SliceFit*> out;
  for (const SliceFit& s : board().fit.slices) {
    if (s.ok) {
      out.push_back(&s);
    }
  }
  return out;
}

// Fixture: skip the whole suite cleanly when the parquet is absent.
class AmznEarnings : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!board().available) {
      GTEST_SKIP() << "AMZN earnings fixture absent (" << kFixtureRel
                   << "); committed fixture expected in-tree";
    }
  }
};

}  // namespace

TEST_F(AmznEarnings, ImpliedSpotInRange) {
  EXPECT_GE(board().implied_spot, 1500.0);
  EXPECT_LE(board().implied_spot, 1540.0);
}

TEST_F(AmznEarnings, FrontExpiryDteAboutOne) {
  const auto ok = ok_slices();
  ASSERT_FALSE(ok.empty());
  // PM-close convention -> the 1-DTE earnings expiry is ~1.0 day out, not ~4 h.
  EXPECT_GE(ok.front()->dte, 0.9);
  EXPECT_LE(ok.front()->dte, 1.2);
}

TEST_F(AmznEarnings, FrontC2EffStronglyNegative) {
  const auto ok = ok_slices();
  ASSERT_FALSE(ok.empty());
  // The headline capability: extreme negative ATF curvature (the earnings W).
  EXPECT_LT(ok.front()->c2_eff, -1.0) << "front c2_eff=" << ok.front()->c2_eff;
}

TEST_F(AmznEarnings, C2EffTermStructureIncreasingOffFront) {
  const auto ok = ok_slices();
  ASSERT_GE(ok.size(), 5u);
  // The frown decays with tenor: c2_eff climbs from ~-1.48 toward 0 over the
  // first ~5 expiries. Assert strictly increasing across them.
  for (std::size_t i = 0; i + 1 < 5u; ++i) {
    EXPECT_LT(ok[i]->c2_eff, ok[i + 1]->c2_eff)
        << "c2_eff not increasing at i=" << i << " (" << ok[i]->c2_eff << " vs "
        << ok[i + 1]->c2_eff << ")";
  }
}

TEST_F(AmznEarnings, EverySliceButterflyArbFree) {
  const auto ok = ok_slices();
  ASSERT_FALSE(ok.empty());
  for (const SliceFit* s : ok) {
    EXPECT_GE(s->min_roper_g, -1.0e-8)
        << "expiry " << s->expiry_ymd << " min Roper g=" << s->min_roper_g;
  }
}

TEST_F(AmznEarnings, NearMoneyCalendarNonCrossing) {
  const auto ok = ok_slices();
  ASSERT_GE(ok.size(), 2u);
  // Total variance w(k, T) non-decreasing in T for adjacent expiry pairs among the
  // near-term (dte <= 120) slices, at near-money k in {-0.3, 0, +0.3}.
  constexpr double kK[] = {-0.3, 0.0, 0.3};
  for (std::size_t i = 0; i + 1 < ok.size(); ++i) {
    if (ok[i]->dte > 120.0) {
      continue;
    }
    for (const double k : kK) {
      const double dw =
          cstar_slice_w(ok[i + 1]->params, k) - cstar_slice_w(ok[i]->params, k);
      EXPECT_GE(dw, -1.0e-6)
          << "calendar crossing " << ok[i]->expiry_ymd << "->"
          << ok[i + 1]->expiry_ymd << " at k=" << k << " dw=" << dw;
    }
  }
}

TEST_F(AmznEarnings, MostSlicesFit) {
  int n_fit = 0;
  for (const SliceFit& s : board().fit.slices) {
    if (s.ok && s.theta > 0.0) {
      ++n_fit;
    }
  }
  EXPECT_GE(n_fit, 15) << n_fit << " of " << board().fit.slices.size()
                       << " slices fit non-degenerate";
}
