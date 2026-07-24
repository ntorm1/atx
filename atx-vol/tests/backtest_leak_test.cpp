// WS-F F1 (BT-P1-2, BT-P1-3) — the backtest accounting-leak family.
//
// Every gate here drives `run_backtest`'s STRATEGY overload over a corpus with a
// deliberate SURFACE GAP (one date's archive omits one underlier) and observes an
// accounting path that used to succeed silently:
//
//   1. HedgeAtMissingSurfaceFailsClosed        — `spot_of` returned 0.0, so the
//      daily delta hedge FLATTENED residual shares for free (cash unchanged).
//   2. UnmarkedHedgeSharesFailClosed           — the shares-PnL/financing loop
//      `continue`d over a missing base/shifted surface, so shares held across the
//      gap were unmarked and their move vanished from NAV.
//   3. DefaultUnpricedPolicyIsError            — `RunConfig{}` used to default to
//      `ExcludeAndReport`, silently truncating held-lot PnL across a gap.
//   4. NavLiquidationReconciliation            — opt-in per-row reconciliation of
//      NAV against an independently recomputed liquidation value
//      (cash + book MTM + shares MTM + non-cash financing accrual).
//
// Fixtures are synthetic analytic eSSVI surfaces (the backtest_exec_test
// make_surface pattern) so they run everywhere with no fitting and no data.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/strategy.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUidA = 11; // present on every date
constexpr std::uint32_t kUidB = 12; // absent on the gap date

[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, std::int64_t now_ts,
                                         double vol_bump = 0.0) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double term_forward = S * std::exp((kR - 0.02) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = term_forward;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, term_forward, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-btleak-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// A two-name corpus. `gap_step` names the date whose archive contains ONLY the
// "AAA" (kUidA) surface — the surface gap every gate below drives.
struct GapCorpus {
  CorpusManifest manifest;
  std::vector<std::string> dates;
  std::vector<std::string> paths;
};

[[nodiscard]] GapCorpus make_gap_corpus(const fs::path &dir, int n_dates, int gap_step) {
  GapCorpus out;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double drift = 1.0 + 0.006 * static_cast<double>(d);
    const double vb = 0.001 * static_cast<double>(d);
    const PricedSurface a = make_surface(kUidA, 100.0 * drift, now, vb);
    const PricedSurface b = make_surface(kUidB, 150.0 * drift, now, vb);
    std::vector<SurfaceArchiveItem> items;
    items.push_back(SurfaceArchiveItem{"AAA", &a});
    if (d != gap_step) {
      items.push_back(SurfaceArchiveItem{"BBB", &b});
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-09-%02d", d + 1);
    const std::string date = buf;
    const std::string path = (dir / (date + ".atxvsa")).string();
    const Status st = write_surface_archive_v2_file(path, items);
    EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
    out.dates.push_back(date);
    out.paths.push_back(path);
  }
  for (std::size_t i = 0; i < out.dates.size(); ++i) {
    out.manifest.dates.push_back(out.dates[i]);
    CorpusEntry e;
    e.date = out.dates[i];
    e.symbol = "AAA";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = out.paths[i];
    out.manifest.entries.push_back(std::move(e));
  }
  return out;
}

[[nodiscard]] double res_T(std::int64_t expiry, std::int64_t now) noexcept {
  return (static_cast<double>(expiry) - static_cast<double>(now)) / kNsPerYear;
}

// Opens ONE short put on `uid` at step 0 (entry price == the live model mark, so
// the entry books no mark slippage) and optionally closes it at `close_step`.
class GapStrategy final : public IStrategy {
public:
  GapStrategy(std::uint32_t uid, double strike, std::int64_t expiry_ts, HedgeSpec hedge,
              std::size_t close_step) noexcept
      : uid_{uid}, strike_{strike}, expiry_{expiry_ts}, hedge_{hedge}, close_step_{close_step} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0) {
      const SurfaceRef s = base.find(uid_);
      if (s == nullptr) {
        return atx::core::Err(atx::core::ErrorCode::NotFound, "GapStrategy: no entry surface");
      }
      const double T = res_T(expiry_, base.ts_ns());
      const auto fv = s->fair_value(strike_, T, Side::Put);
      if (!fv) {
        return atx::core::Err(fv.error());
      }
      Lot lot;
      lot.id = next_lot_id++;
      lot.contract = OptionContract{uid_, strike_, T, Side::Put};
      lot.qty = -2.0; // short 2 puts => a real positive delta to hedge
      lot.multiplier = 100.0;
      lot.expiry_ts_ns = expiry_;
      lot.cohort = 1;
      lot.entry_price = *fv;
      book.lots.push_back(lot);
      return atx::core::Ok();
    }
    if (close_step_ != 0 && step_index == close_step_) {
      book.lots.clear();
    }
    return atx::core::Ok();
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override { return hedge_; }

private:
  std::uint32_t uid_{0};
  double strike_{0.0};
  std::int64_t expiry_{0};
  HedgeSpec hedge_{};
  std::size_t close_step_{0};
};

} // namespace

// ── F1(a) BT-P1-3: the hedge must never fill at spot 0.0 ────────────────────
//
// A book long/short options on BBB with a DAILY delta-to-zero hedge accumulates
// BBB hedge shares. On the gap date BBB has no surface, so `spot_of` used to
// return 0.0 and the hedge FLATTENED every residual share for free: cash moved by
// `shares * 0.0 == 0`, the ledger went to zero, and the run reported Ok. The
// engine must fail closed exactly as a roll-close with a missing mark does.
TEST(BacktestLeak, HedgeAtMissingSurfaceFailsClosedInsteadOfFreeFlatten) {
  const fs::path dir = fresh_dir("f1a-hedge-gap");
  constexpr int kDates = 5;
  constexpr int kGap = 3;
  const GapCorpus corpus = make_gap_corpus(dir, kDates, kGap);
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 200LL * kDayNs; // far past the run
  GapStrategy strat{kUidB, 150.0, expiry,
                    HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0},
                    /*close_step=*/0};

  RunConfig cfg;
  cfg.unpriced = UnpricedLotPolicy::ExcludeAndReport; // the leak's reachable mode
  const auto r = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(r.has_value()) << "hedge silently flattened residual shares at spot 0.0; "
                              << "nav[last]=" << (r.has_value() ? r->nav.back() : 0.0);
  EXPECT_NE(r.error().to_string().find("hedge"), std::string::npos) << r.error().to_string();
}

// ── F1(b) BT-P1-3 companion: shares held across a gap must not go unmarked ──
//
// AtEntry cadence hedges only on the entry step, so the BBB share ledger survives
// the lot's close and is still held when BBB's surface disappears. The shares
// PnL / financing loop used to `continue` on the missing surface: the share move
// over that step vanished from NAV with no count, no flag and no error.
TEST(BacktestLeak, UnmarkedHedgeSharesFailClosedUnderErrorPolicy) {
  const fs::path dir = fresh_dir("f1b-shares-gap");
  constexpr int kDates = 5;
  constexpr int kGap = 4; // LAST date: only the shares ledger is still live there
  const GapCorpus corpus = make_gap_corpus(dir, kDates, kGap);
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 200LL * kDayNs;
  GapStrategy strat{kUidB, 150.0, expiry,
                    HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::AtEntry, 0.0},
                    /*close_step=*/2};

  RunConfig cfg;
  cfg.unpriced = UnpricedLotPolicy::Error;
  const auto r = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(r.has_value())
      << "shares held across the surface gap were silently unmarked; nav[last]="
      << (r.has_value() ? r->nav.back() : 0.0);
  EXPECT_NE(r.error().to_string().find("share"), std::string::npos) << r.error().to_string();
}

// ── F1(c) BT-P1-2: the DEFAULT policy must fail closed ──────────────────────
//
// No hedge at all: the only leak in play is the held-lot PnL exclusion. A
// default-constructed RunConfig used to run this to completion and report a NAV
// that permanently omitted the gap step's PnL for the unpriced lot.
TEST(BacktestLeak, DefaultUnpricedPolicyIsErrorNotSilentTruncation) {
  const fs::path dir = fresh_dir("f1c-default-policy");
  constexpr int kDates = 5;
  constexpr int kGap = 3;
  const GapCorpus corpus = make_gap_corpus(dir, kDates, kGap);
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 200LL * kDayNs;
  GapStrategy strat{kUidB, 150.0, expiry, HedgeSpec{}, /*close_step=*/0};

  RunConfig cfg; // DEFAULTS ONLY — this is the assertion
  EXPECT_EQ(cfg.unpriced, UnpricedLotPolicy::Error);
  const auto r = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(r.has_value()) << "default policy silently truncated NAV across the surface gap";

  // The old behavior stays reachable, explicitly.
  RunConfig lenient;
  lenient.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  GapStrategy strat2{kUidB, 150.0, expiry, HedgeSpec{}, /*close_step=*/0};
  const auto ok = run_backtest(*clock, strat2, lenient);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  double excluded = 0.0;
  for (const double n : ok->n_unpriced_lots) {
    excluded += n;
  }
  EXPECT_GT(excluded, 0.0) << "fixture did not actually exercise the exclusion path";
}

// ── F1(d): per-row NAV vs independently recomputed liquidation value ────────
//
// NAV is a cumulative flow sum; nothing ever reconciled it against
// cash + book MTM + shares MTM. With `RunConfig::reconcile_nav` the engine
// recomputes that liquidation value on every recorded row and publishes it in
// `BacktestResult::nav_liquidation`; a drift beyond tolerance aborts the run.
TEST(BacktestLeak, NavLiquidationReconciliationHoldsOnACleanHedgedFinancedRun) {
  const fs::path dir = fresh_dir("f1d-recon-clean");
  constexpr int kDates = 6;
  const GapCorpus corpus = make_gap_corpus(dir, kDates, /*gap_step=*/-1); // NO gap
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 200LL * kDayNs;
  GapStrategy strat{kUidB, 150.0, expiry,
                    HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0},
                    /*close_step=*/0};

  RunConfig cfg;
  cfg.reconcile_nav = true;
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  cfg.frictions.half_spread_bps = 25.0;
  cfg.frictions.per_contract_cost = 0.65;
  cfg.frictions.hedge_slippage_bps = 1.0;
  cfg.financing.initial_cash = 1.0e6;
  cfg.financing.finance_premium = true;
  cfg.financing.borrow_rate = 0.005;
  cfg.financing.shares_carry = true;

  const auto r = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->nav_liquidation.size(), r->nav.size());
  for (std::size_t i = 0; i < r->nav.size(); ++i) {
    EXPECT_NEAR(r->nav_liquidation[i], r->nav[i], 1.0e-9)
        << "row " << i << " (" << r->date[i] << ") NAV != liquidation";
  }
}

// The reconciliation must be able to OBSERVE a leak, not merely agree with the
// engine's own arithmetic: the same check on the ExcludeAndReport surface-gap
// fixture (whose NAV legitimately truncates) aborts with a named error.
TEST(BacktestLeak, NavLiquidationReconciliationObservesTheExcludeAndReportDrift) {
  const fs::path dir = fresh_dir("f1d-recon-gap");
  constexpr int kDates = 5;
  constexpr int kGap = 3;
  const GapCorpus corpus = make_gap_corpus(dir, kDates, kGap);
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 200LL * kDayNs;
  GapStrategy strat{kUidB, 150.0, expiry, HedgeSpec{}, /*close_step=*/0};

  RunConfig cfg;
  cfg.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  cfg.reconcile_nav = true;
  const auto r = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(r.has_value()) << "reconciliation did not observe the truncated-NAV drift";
  EXPECT_NE(r.error().to_string().find("reconcil"), std::string::npos) << r.error().to_string();
}

// â”€â”€ F3(b) BT-P1-4: discrete dividends on the hedge-share ledger â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// The option surfaces price the real discrete dividend schedule, but the SHARE
// ledger only ever accrued a continuous `q_eff_at(0.25)` proxy â€” the surface's
// effective carry at a FIXED 3-month tenor, ignoring both the step length and
// where the ex-dates actually fall â€” and only when `shares_carry` was opted
// into (default off). A delta-hedged book crossing an ex-date therefore booked
// no dividend cash at all under the default config.
//
// The fixture: a short-put book on BBB with a daily delta hedge, so a real
// SHORT share position is carried across a step containing an ex-date. Short
// shares PAY the dividend, so the run with a schedule must lose exactly
// `shares * amount` of NAV relative to the run without one â€” and the share
// count is recoverable from the fixture, so the test asserts the AMOUNT, not
// merely a direction.
TEST(BacktestLeak, HedgeSharesBookDiscreteDividendCashOnExDates) {
  const fs::path dir = fresh_dir("f3b-ex-div");
  constexpr int kDates = 5;
  const GapCorpus corpus = make_gap_corpus(dir, kDates, /*gap_step=*/-1); // no gap
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 200LL * kDayNs;
  constexpr double kDivAmount = 0.85; // cash per share

  const auto run = [&](std::vector<ShareDividend> divs) -> Result<BacktestResult> {
    GapStrategy strat{kUidB, 150.0, expiry,
                      HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0},
                      /*close_step=*/0};
    RunConfig cfg;
    cfg.reconcile_nav = true;
    cfg.financing.share_dividends = std::move(divs);
    return run_backtest(*clock, strat, cfg);
  };

  const auto base = run({});
  ASSERT_TRUE(base.has_value()) << base.error().to_string();

  // Ex-date strictly inside the step from date 2 to date 3.
  const std::int64_t ex_ts = kBaseNow + 2LL * kDayNs + kDayNs / 2;
  const auto with_div = run({ShareDividend{kUidB, ex_ts, kDivAmount}});
  ASSERT_TRUE(with_div.has_value()) << with_div.error().to_string();

  ASSERT_EQ(base->size(), static_cast<std::size_t>(kDates));
  ASSERT_EQ(with_div->size(), static_cast<std::size_t>(kDates));

  // Recover the share count carried over step 2 -> 3 from that row's share PnL
  // and the fixture's own spots (make_gap_corpus: S = 150 * (1 + 0.006*d)).
  const double S2 = 150.0 * (1.0 + 0.006 * 2.0);
  const double S3 = 150.0 * (1.0 + 0.006 * 3.0);
  const double shares = base->pnl_shares[3] / (S3 - S2);
  ASSERT_LT(shares, 0.0) << "fixture must carry a SHORT hedge (short 2 puts => positive delta)";

  const double expected = shares * kDivAmount; // negative: short shares pay
  const double actual = with_div->financing[3] - base->financing[3];
  EXPECT_NEAR(actual, expected, 1.0e-9 * std::max(1.0, std::fabs(expected)));
  EXPECT_LT(actual, 0.0);
  std::printf("[F3b] shares=%.6f  dividend cash=%.6f (expected %.6f)\n", shares, actual, expected);

  // The dividend is CASH, not a modelled accrual: the ledger balance moves by
  // the same amount, and NAV still reconciles against liquidation on every row.
  EXPECT_NEAR(with_div->cash[3] - base->cash[3], expected,
              1.0e-9 * std::max(1.0, std::fabs(expected)));
  ASSERT_EQ(with_div->nav_liquidation.size(), with_div->nav.size());
  for (std::size_t i = 0; i < with_div->nav.size(); ++i) {
    EXPECT_NEAR(with_div->nav_liquidation[i], with_div->nav[i], 1.0e-9) << "row " << i;
  }

  // Rows OUTSIDE the ex-date window are untouched, bit-for-bit: a dividend
  // schedule is not a global re-scaling of the financing column.
  for (std::size_t i = 0; i < base->size(); ++i) {
    if (i == 3) {
      continue;
    }
    EXPECT_EQ(with_div->financing[i], base->financing[i]) << "row " << i;
  }

  // An empty schedule is the identity: same run, byte-for-byte NAV.
  const auto again = run({});
  ASSERT_TRUE(again.has_value()) << again.error().to_string();
  for (std::size_t i = 0; i < base->size(); ++i) {
    EXPECT_EQ(again->nav[i], base->nav[i]) << "row " << i;
  }
}

// The discrete schedule REPLACES the q_eff proxy for the names it covers rather
// than stacking on top of it: with `shares_carry` on, a uid carrying a schedule
// accrues no continuous dividend yield (the funding leg is untouched), so the
// dividend is never counted twice.
TEST(BacktestLeak, DiscreteDividendScheduleSupersedesTheContinuousCarryProxy) {
  const fs::path dir = fresh_dir("f3b-proxy-supersede");
  constexpr int kDates = 5;
  const GapCorpus corpus = make_gap_corpus(dir, kDates, /*gap_step=*/-1);
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 200LL * kDayNs;

  const auto run = [&](bool with_schedule) -> Result<BacktestResult> {
    GapStrategy strat{kUidB, 150.0, expiry,
                      HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0},
                      /*close_step=*/0};
    RunConfig cfg;
    cfg.reconcile_nav = true;
    cfg.financing.shares_carry = true;
    if (with_schedule) {
      // A schedule whose only ex-date is far outside the run: it contributes NO
      // cash, so any difference is purely the suppressed yield proxy.
      cfg.financing.share_dividends = {ShareDividend{kUidB, kBaseNow + 400LL * kDayNs, 1.0}};
    }
    return run_backtest(*clock, strat, cfg);
  };

  const auto proxy = run(false);
  ASSERT_TRUE(proxy.has_value()) << proxy.error().to_string();
  const auto scheduled = run(true);
  ASSERT_TRUE(scheduled.has_value()) << scheduled.error().to_string();

  // The proxy was doing real work (the fixture's surfaces carry q_eff = 2%), so
  // suppressing it must move the financing column â€” otherwise this test could
  // not observe a double count.
  bool moved = false;
  for (std::size_t i = 1; i < proxy->size(); ++i) {
    if (scheduled->financing[i] != proxy->financing[i]) {
      moved = true;
    }
  }
  EXPECT_TRUE(moved) << "the q_eff proxy contributed nothing; the test cannot observe suppression";

  // And the suppression is exactly the yield leg: short shares under the proxy
  // PAY q*S*dt, so removing it makes financing strictly larger on every step.
  for (std::size_t i = 1; i < proxy->size(); ++i) {
    EXPECT_GE(scheduled->financing[i], proxy->financing[i]) << "row " << i;
  }
  ASSERT_EQ(scheduled->nav_liquidation.size(), scheduled->nav.size());
  for (std::size_t i = 0; i < scheduled->nav.size(); ++i) {
    EXPECT_NEAR(scheduled->nav_liquidation[i], scheduled->nav[i], 1.0e-9) << "row " << i;
  }
}
