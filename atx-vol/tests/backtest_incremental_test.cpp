#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
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

constexpr double kRate = 0.043;
constexpr std::int64_t kBaseNow = 1'700'000'000'000'000'000LL;
constexpr std::int64_t kDayNs = 86'400LL * 1'000'000'000LL;
constexpr std::uint32_t kUid = 7u;

[[nodiscard]] PricedSurface make_surface(double spot, std::int64_t now_ts, double vol_bump) {
  CurveSurface curves;
  std::vector<SliceContext> contexts;
  constexpr double tenors[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  std::uint16_t expiry_id = 0u;
  for (const double tenor : tenors) {
    const double forward = spot * std::exp((kRate - 0.02) * tenor);
    EssviParams params{};
    params.theta = 0.04 + 0.005 * static_cast<double>(expiry_id) + vol_bump;
    params.phi = 1.5 - 0.05 * static_cast<double>(expiry_id);
    params.rho = -0.4 + 0.02 * static_cast<double>(expiry_id);
    params.psi = 0.5;
    params.p = 0.5;
    params.lambda = 0.5;
    params.T = tenor;
    params.F = forward;
    params.expiry_id = expiry_id;
    curves.push(std::make_unique<EssviCurve>(params, std::exp(-kRate * tenor)));
    contexts.push_back(SliceContext{tenor, forward, 0.0, 0.02, 250, 7});
    ++expiry_id;
  }

  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = now_ts;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = kUid;
  auto surface = PricedSurface::create(std::move(curves), std::move(contexts), pricing);
  EXPECT_TRUE(surface.has_value())
      << (surface.has_value() ? std::string{} : surface.error().to_string());
  return std::move(*surface);
}

struct TestCorpus {
  std::vector<std::pair<std::string, std::string>> refs;
};

[[nodiscard]] TestCorpus make_corpus(const char *tag, std::size_t count) {
  const fs::path dir = fs::temp_directory_path() / (std::string{"atx-incremental-"} + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  TestCorpus corpus;
  for (std::size_t i = 0; i < count; ++i) {
    const auto day = static_cast<std::int64_t>(i);
    const PricedSurface surface =
        make_surface(100.0 + 0.7 * static_cast<double>(i), kBaseNow + day * kDayNs,
                     0.001 * static_cast<double>(i));
    char date_buffer[16];
    std::snprintf(date_buffer, sizeof date_buffer, "2026-08-%02u", static_cast<unsigned>(i + 1u));
    const std::string date{date_buffer};
    const std::string path = (dir / (date + ".atxvsa")).string();
    const SurfaceArchiveItem item{"SPY", &surface};
    const Status write_status =
        write_surface_archive_v2_file(path, std::span<const SurfaceArchiveItem>{&item, 1u});
    EXPECT_TRUE(write_status.has_value())
        << (write_status.has_value() ? std::string{} : write_status.error().to_string());
    corpus.refs.emplace_back(date, path);
  }
  return corpus;
}

[[nodiscard]] Result<Clock> make_clock(std::span<const std::pair<std::string, std::string>> refs) {
  CorpusManifest manifest;
  for (const auto &[date, path] : refs) {
    manifest.dates.push_back(date);
    CorpusEntry entry;
    entry.date = date;
    entry.symbol = "SPY";
    entry.status = CorpusFitStatus::Ok;
    entry.archive_path = path;
    manifest.entries.push_back(std::move(entry));
  }
  return Clock::from_manifest(manifest);
}

class DailyHedgedCall final : public IStrategy {
public:
  explicit DailyHedgedCall(std::int64_t expiry) noexcept : expiry_{expiry} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    seen_steps.push_back(step_index);
    if (book.lots.empty()) {
      const SurfaceRef surface = base.find(kUid);
      if (surface == nullptr) {
        return atx::core::Err(ErrorCode::NotFound, "test strategy surface missing");
      }
      const double tenor = static_cast<double>(expiry_ - base.ts_ns()) / kNsPerYear;
      const Result<double> mark = surface->fair_value(100.0, tenor, Side::Call);
      if (!mark) {
        return atx::core::Err(mark.error());
      }
      book.lots.push_back(Lot{next_lot_id++, OptionContract{kUid, 100.0, 0.0, Side::Call}, 3.0,
                              100.0, expiry_, 0u, *mark});
    }
    return atx::core::Ok();
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override {
    HedgeSpec hedge;
    hedge.kind = HedgeSpec::Kind::DeltaToZero;
    hedge.cadence = HedgeSpec::Cadence::Daily;
    return hedge;
  }

  std::vector<std::size_t> seen_steps;

private:
  std::int64_t expiry_{0};
};

template <typename T> void append(std::vector<T> &destination, const std::vector<T> &source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

void append_result(BacktestResult &destination, const BacktestResult &source) {
  append(destination.date, source.date);
  append(destination.ts_ns, source.ts_ns);
  append(destination.pnl_total, source.pnl_total);
  append(destination.pnl_delta, source.pnl_delta);
  append(destination.pnl_gamma, source.pnl_gamma);
  append(destination.pnl_vega, source.pnl_vega);
  append(destination.pnl_vanna, source.pnl_vanna);
  append(destination.pnl_volga, source.pnl_volga);
  append(destination.pnl_theta, source.pnl_theta);
  append(destination.pnl_rho, source.pnl_rho);
  append(destination.pnl_charm, source.pnl_charm);
  append(destination.pnl_unexplained, source.pnl_unexplained);
  append(destination.pnl_settlement, source.pnl_settlement);
  append(destination.pnl_shares, source.pnl_shares);
  append(destination.financing, source.financing);
  append(destination.cost, source.cost);
  append(destination.nav, source.nav);
  append(destination.cash, source.cash);
  append(destination.gross_delta, source.gross_delta);
  append(destination.gross_gamma, source.gross_gamma);
  append(destination.gross_vega, source.gross_vega);
  append(destination.gross_vega_abs, source.gross_vega_abs);
  append(destination.gross_theta, source.gross_theta);
  append(destination.turnover_notional, source.turnover_notional);
  append(destination.turnover_vega, source.turnover_vega);
  append(destination.n_open_lots, source.n_open_lots);
  append(destination.n_unpriced_lots, source.n_unpriced_lots);
  append(destination.n_unpriced_greeks, source.n_unpriced_greeks);
  append(destination.step_pnl_total, source.step_pnl_total);
  append(destination.nav_liquidation, source.nav_liquidation);
}

void expect_same_result(const BacktestResult &actual, const BacktestResult &expected) {
  EXPECT_EQ(actual.date, expected.date);
  EXPECT_EQ(actual.ts_ns, expected.ts_ns);
  EXPECT_EQ(actual.pnl_total, expected.pnl_total);
  EXPECT_EQ(actual.pnl_delta, expected.pnl_delta);
  EXPECT_EQ(actual.pnl_gamma, expected.pnl_gamma);
  EXPECT_EQ(actual.pnl_vega, expected.pnl_vega);
  EXPECT_EQ(actual.pnl_vanna, expected.pnl_vanna);
  EXPECT_EQ(actual.pnl_volga, expected.pnl_volga);
  EXPECT_EQ(actual.pnl_theta, expected.pnl_theta);
  EXPECT_EQ(actual.pnl_rho, expected.pnl_rho);
  EXPECT_EQ(actual.pnl_charm, expected.pnl_charm);
  EXPECT_EQ(actual.pnl_unexplained, expected.pnl_unexplained);
  EXPECT_EQ(actual.pnl_settlement, expected.pnl_settlement);
  EXPECT_EQ(actual.pnl_shares, expected.pnl_shares);
  EXPECT_EQ(actual.financing, expected.financing);
  EXPECT_EQ(actual.cost, expected.cost);
  EXPECT_EQ(actual.nav, expected.nav);
  EXPECT_EQ(actual.cash, expected.cash);
  EXPECT_EQ(actual.gross_delta, expected.gross_delta);
  EXPECT_EQ(actual.gross_gamma, expected.gross_gamma);
  EXPECT_EQ(actual.gross_vega, expected.gross_vega);
  EXPECT_EQ(actual.gross_vega_abs, expected.gross_vega_abs);
  EXPECT_EQ(actual.gross_theta, expected.gross_theta);
  EXPECT_EQ(actual.turnover_notional, expected.turnover_notional);
  EXPECT_EQ(actual.turnover_vega, expected.turnover_vega);
  EXPECT_EQ(actual.n_open_lots, expected.n_open_lots);
  EXPECT_EQ(actual.n_unpriced_lots, expected.n_unpriced_lots);
  EXPECT_EQ(actual.n_unpriced_greeks, expected.n_unpriced_greeks);
  EXPECT_EQ(actual.step_pnl_total, expected.step_pnl_total);
  EXPECT_EQ(actual.nav_liquidation, expected.nav_liquidation);
  EXPECT_EQ(actual.signals, expected.signals);
}

[[nodiscard]] RunConfig run_config() {
  RunConfig cfg;
  cfg.price.n_threads = 1u;
  cfg.frictions.hedge_slippage_bps = 1.5;
  cfg.financing.initial_cash = 250'000.0;
  cfg.financing.finance_premium = true;
  cfg.financing.borrow_rate = 0.01;
  cfg.financing.shares_carry = true;
  cfg.reconcile_nav = true;
  return cfg;
}

} // namespace

TEST(BacktestIncremental, SplitResumeIsBitIdenticalToOneShotWithDailyHedgeAndCash) {
  const TestCorpus corpus = make_corpus("parity", 6u);
  auto full_clock = make_clock(corpus.refs);
  ASSERT_TRUE(full_clock.has_value()) << full_clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 90 * kDayNs;

  DailyHedgedCall one_shot_strategy{expiry};
  auto one_shot = run_backtest(*full_clock, one_shot_strategy, run_config());
  ASSERT_TRUE(one_shot.has_value()) << one_shot.error().to_string();
  DailyHedgedCall full_incremental_strategy{expiry};
  auto full_incremental =
      run_backtest_incremental(*full_clock, full_incremental_strategy, run_config(), nullptr);
  ASSERT_TRUE(full_incremental.has_value()) << full_incremental.error().to_string();
  expect_same_result(full_incremental->rows, *one_shot);

  auto first_clock = make_clock(std::span{corpus.refs}.first(3u));
  auto resume_clock = make_clock(std::span{corpus.refs}.subspan(2u));
  ASSERT_TRUE(first_clock.has_value()) << first_clock.error().to_string();
  ASSERT_TRUE(resume_clock.has_value()) << resume_clock.error().to_string();

  DailyHedgedCall incremental_strategy{expiry};
  auto first = run_backtest_incremental(*first_clock, incremental_strategy, run_config(), nullptr);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  auto resumed = run_backtest_incremental(*resume_clock, incremental_strategy, run_config(),
                                          &first->checkpoint);
  ASSERT_TRUE(resumed.has_value()) << resumed.error().to_string();

  BacktestResult stitched = first->rows;
  append_result(stitched, resumed->rows);
  expect_same_result(stitched, *one_shot);
  EXPECT_EQ(incremental_strategy.seen_steps, (std::vector<std::size_t>{0u, 1u, 2u, 3u, 4u, 5u}));
  ASSERT_EQ(resumed->checkpoint.hedge_shares.size(), 1u);
  EXPECT_EQ(resumed->checkpoint.hedge_shares[0].uid, kUid);
  EXPECT_TRUE(std::isfinite(resumed->checkpoint.hedge_shares[0].shares));
  EXPECT_EQ(resumed->checkpoint.cash, one_shot->cash.back());
  EXPECT_EQ(resumed->checkpoint.nav, one_shot->nav.back());
  EXPECT_EQ(resumed->checkpoint.completed_step_index, 5u);
  EXPECT_EQ(resumed->checkpoint.next_lot_id, 2u);
  ASSERT_EQ(resumed->checkpoint.portfolio.lots.size(), 1u);
  EXPECT_EQ(resumed->checkpoint, full_incremental->checkpoint);
}

TEST(BacktestIncremental, ResumeSkipsAnchorAndReturnsOnlyNewRows) {
  const TestCorpus corpus = make_corpus("anchor", 3u);
  auto first_clock = make_clock(std::span{corpus.refs}.first(2u));
  ASSERT_TRUE(first_clock.has_value()) << first_clock.error().to_string();
  DailyHedgedCall strategy{kBaseNow + 90 * kDayNs};
  auto first = run_backtest_incremental(*first_clock, strategy, run_config(), nullptr);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();

  auto anchor_only = make_clock(std::span{corpus.refs}.subspan(1u, 1u));
  ASSERT_TRUE(anchor_only.has_value()) << anchor_only.error().to_string();
  const std::size_t calls_before_resume = strategy.seen_steps.size();
  auto resumed = run_backtest_incremental(*anchor_only, strategy, run_config(), &first->checkpoint);
  ASSERT_TRUE(resumed.has_value()) << resumed.error().to_string();
  EXPECT_EQ(resumed->rows.size(), 0u);
  EXPECT_TRUE(resumed->rows.step_pnl_total.empty());
  EXPECT_EQ(strategy.seen_steps.size(), calls_before_resume);
  EXPECT_EQ(resumed->checkpoint.completed_step_index, first->checkpoint.completed_step_index);
}

TEST(BacktestIncremental, RejectsUnsupportedStrideAnchorMismatchAndCorruptState) {
  const TestCorpus corpus = make_corpus("invalid", 3u);
  auto first_clock = make_clock(std::span{corpus.refs}.first(2u));
  auto wrong_anchor_clock = make_clock(std::span{corpus.refs}.subspan(2u));
  ASSERT_TRUE(first_clock.has_value()) << first_clock.error().to_string();
  ASSERT_TRUE(wrong_anchor_clock.has_value()) << wrong_anchor_clock.error().to_string();
  DailyHedgedCall strategy{kBaseNow + 90 * kDayNs};

  RunConfig bad_stride = run_config();
  bad_stride.record_every_n = 2u;
  auto stride = run_backtest_incremental(*first_clock, strategy, bad_stride, nullptr);
  ASSERT_FALSE(stride.has_value());
  EXPECT_EQ(stride.error().code(), ErrorCode::InvalidArgument);

  auto first = run_backtest_incremental(*first_clock, strategy, run_config(), nullptr);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  auto wrong_anchor =
      run_backtest_incremental(*wrong_anchor_clock, strategy, run_config(), &first->checkpoint);
  ASSERT_FALSE(wrong_anchor.has_value());
  EXPECT_EQ(wrong_anchor.error().code(), ErrorCode::InvalidArgument);

  BacktestCheckpoint nonfinite = first->checkpoint;
  nonfinite.cash = std::numeric_limits<double>::quiet_NaN();
  auto bad_cash = run_backtest_incremental(*first_clock, strategy, run_config(), &nonfinite);
  ASSERT_FALSE(bad_cash.has_value());
  EXPECT_EQ(bad_cash.error().code(), ErrorCode::InvalidArgument);

  BacktestCheckpoint duplicate_share = first->checkpoint;
  ASSERT_FALSE(duplicate_share.hedge_shares.empty());
  duplicate_share.hedge_shares.push_back(duplicate_share.hedge_shares.front());
  auto bad_share = run_backtest_incremental(*first_clock, strategy, run_config(), &duplicate_share);
  ASSERT_FALSE(bad_share.has_value());
  EXPECT_EQ(bad_share.error().code(), ErrorCode::InvalidArgument);

  BacktestCheckpoint reused_id = first->checkpoint;
  ASSERT_FALSE(reused_id.portfolio.lots.empty());
  reused_id.next_lot_id = reused_id.portfolio.lots.front().id;
  auto bad_id = run_backtest_incremental(*first_clock, strategy, run_config(), &reused_id);
  ASSERT_FALSE(bad_id.has_value());
  EXPECT_EQ(bad_id.error().code(), ErrorCode::InvalidArgument);
}
