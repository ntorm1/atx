// atx-vol backtest engine (Phase B0) gate tests.
//
// Drives `run_backtest` over synthetic single-underlying corpora built by
// writing hand-fitted eSSVI `PricedSurface`s (the pnl_greeks_consistency
// pattern) to one archive per date. No external data — runs everywhere.
//
// Six gates:
//   1. LoadOnce         — N-date run opens each archive exactly once.
//   2a. AgingSpotOnly   — spot-only step => unexplained tiny vs total.
//   2b. AgingTimeOnly   — time-only step => PnL isolates to theta.
//   3. AttributionCloses— axes + unexplained == pnl_total (non-settlement).
//   4. Determinism      — n_threads 1 vs 4 => BacktestResult bit-identical.
//   5. Granularity      — coarse recorded nav/attribution == fine at samples.
//   6. ExpirySettlement — exact expiry observation settles at intrinsic and drops.

#include "atx/vol/log.hpp"
#include "atx/vol/research/listed_definitions_cache.hpp" // host-integration emitting path

#include "log_sink_probe.hpp" // CapturingSink / ScopedSink / StreamCapture

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic> // plan 5.5 cancellation flag
#include <fstream> // host-integration definitions fixture
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp" // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/detail/counters.hpp"
#include "atx/vol/research/dispersion_backtest.hpp" // DispersionCostModel, dispersion_effective_frictions
#include "atx/vol/portfolio_pricer.hpp"    // OptionContract
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"         // IStrategy
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, Result, Status
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "support/isa_golden_tol.hpp"   // golden_isa_accum_tol (per-ISA FMA band)
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUid = 7;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-backtest-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// A synthetic eSSVI PricedSurface: flat forward `fwd`, genuine American premium
// (q_eff = 0.02), slices spanning T in [0.05, 1.0]. `vol_bump` shifts the whole
// term's ATM variance. Mirrors pnl_greeks_consistency's make_essvi.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts, double vol_bump = 0.0,
                                         AmericanMethod method = AmericanMethod::AndersenLake) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double coherent_fwd = fwd * std::exp((kR - 0.02) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = coherent_fwd;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, coherent_fwd, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = method;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

// Write one surface as this date's archive; return its path (creating `dir`).
[[nodiscard]] std::string write_one(const fs::path &dir, const std::string &date,
                                    const std::string &symbol, const PricedSurface &s,
                                    std::optional<SurfaceProvenance> provenance = std::nullopt) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{symbol, &s, std::move(provenance)};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

[[nodiscard]] SurfaceProvenance make_provenance(SurfacePurpose purpose, SurfaceState state,
                                                std::uint64_t served_generation,
                                                std::uint64_t validation_id = 1u) {
  SurfaceProvenance provenance;
  provenance.purpose = purpose;
  provenance.quality_mode = FitQualityMode::Balanced;
  provenance.state = state;
  provenance.validation.validation_id = validation_id;
  provenance.source_generation = served_generation;
  provenance.served_generation = served_generation;
  switch (state) {
  case SurfaceState::Healthy:
    break;
  case SurfaceState::Degraded:
    provenance.validation.failures = ValidationFailure::CarryGap;
    break;
  case SurfaceState::Stale:
    provenance.validation.failures = ValidationFailure::StaleInput;
    break;
  case SurfaceState::Rejected:
    provenance.validation.failures = ValidationFailure::PriceBounds;
    break;
  }
  return provenance;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows.
[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths,
              const std::string &symbol) {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = symbol;
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

// An N-date corpus: spot drifts +0.4%/day, now advances 1 day, vol drifts up.
[[nodiscard]] CorpusManifest make_evolving_corpus(const fs::path &dir, const std::string &symbol,
                                                  int n_dates) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = 100.0 * (1.0 + 0.004 * static_cast<double>(d));
    const double vbump = 0.001 * static_cast<double>(d);
    const PricedSurface s = make_surface(kUid, S, S, now, vbump);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_one(dir, date, symbol, s));
  }
  return make_manifest(dp, symbol);
}

// A two-lot book (long call, short put) that survives past `expiry`.
[[nodiscard]] PortfolioState survivor_book(std::int64_t expiry) {
  PortfolioState st;
  st.lots.push_back(
      Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Call}, +5.0, 100.0, expiry, 0, 0.0});
  st.lots.push_back(
      Lot{2, OptionContract{kUid, 105.0, 0.0, Side::Put}, -3.0, 100.0, expiry, 0, 0.0});
  return st;
}

class OpenThenCloseStrategy final : public IStrategy {
public:
  explicit OpenThenCloseStrategy(std::int64_t expiry) noexcept : expiry_{expiry} {}

  Status on_step(const MarketSnapshot & /*base*/, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0u) {
      book.lots.push_back(Lot{next_lot_id++, OptionContract{kUid, 95.0, 0.0, Side::Call}, +5.0,
                              100.0, expiry_, 0u, 2.0});
    } else if (step_index == 1u) {
      book.lots.clear();
    }
    return atx::core::Ok();
  }

private:
  std::int64_t expiry_{0};
};

class DuplicateIdOpenThenCloseStrategy final : public IStrategy {
public:
  explicit DuplicateIdOpenThenCloseStrategy(std::int64_t expiry) noexcept : expiry_{expiry} {}

  Status on_step(const MarketSnapshot & /*base*/, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0u) {
      constexpr std::uint64_t duplicate_id = 777u;
      book.lots.push_back(Lot{duplicate_id, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0,
                              100.0, expiry_, 0u, 2.0});
      book.lots.push_back(Lot{duplicate_id, OptionContract{kUid, 105.0, 0.0, Side::Put}, -1.0,
                              100.0, expiry_, 0u, 3.0});
      next_lot_id += 2u;
    } else if (step_index == 1u) {
      book.lots.clear();
    }
    return atx::core::Ok();
  }

private:
  std::int64_t expiry_{0};
};

class MutateExistingLotStrategy final : public IStrategy {
public:
  explicit MutateExistingLotStrategy(std::int64_t expiry) noexcept : expiry_{expiry} {}

  Status on_step(const MarketSnapshot & /*base*/, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0u) {
      book.lots.push_back(Lot{next_lot_id++, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0,
                              100.0, expiry_, 3u, 2.0});
    } else if (step_index == 1u) {
      book.lots.front().qty = 2.0;
    }
    return atx::core::Ok();
  }

private:
  std::int64_t expiry_{0};
};

class ReuseClosedLotIdStrategy final : public IStrategy {
public:
  explicit ReuseClosedLotIdStrategy(std::int64_t expiry) noexcept : expiry_{expiry} {}

  Status on_step(const MarketSnapshot & /*base*/, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0u) {
      reused_id_ = next_lot_id++;
      book.lots.push_back(Lot{reused_id_, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0, 100.0,
                              expiry_, 0u, 2.0});
    } else if (step_index == 1u) {
      book.lots.clear();
    } else if (step_index == 2u) {
      book.lots.push_back(Lot{reused_id_, OptionContract{kUid, 105.0, 0.0, Side::Put}, -1.0, 100.0,
                              expiry_, 1u, 3.0});
    }
    return atx::core::Ok();
  }

private:
  std::int64_t expiry_{0};
  std::uint64_t reused_id_{0};
};

class RollBackNextLotIdStrategy final : public IStrategy {
public:
  Status on_step(const MarketSnapshot & /*base*/, std::size_t /*step_index*/,
                 PortfolioState & /*book*/, std::uint64_t &next_lot_id) override {
    --next_lot_id;
    return atx::core::Ok();
  }
};

class InvalidEntryStrategy final : public IStrategy {
public:
  explicit InvalidEntryStrategy(Lot lot) noexcept : lot_{std::move(lot)} {}

  Status on_step(const MarketSnapshot & /*base*/, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    ++on_step_calls;
    if (step_index == 0u) {
      lot_.id = next_lot_id++;
      book.lots.push_back(lot_);
    }
    return atx::core::Ok();
  }

  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    ++seed_calls;
    return {};
  }

  [[nodiscard]] std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot & /*base*/) const override {
    ++signal_calls;
    return {};
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override {
    ++hedge_calls;
    return {};
  }

  unsigned on_step_calls{0u};
  mutable unsigned seed_calls{0u};
  mutable unsigned signal_calls{0u};
  mutable unsigned hedge_calls{0u};

private:
  Lot lot_{};
};

class InvalidHedgeStrategy final : public IStrategy {
public:
  explicit InvalidHedgeStrategy(HedgeSpec hedge) noexcept : hedge_{hedge} {}

  Status on_step(const MarketSnapshot & /*base*/, std::size_t /*step_index*/,
                 PortfolioState & /*book*/, std::uint64_t & /*next_lot_id*/) override {
    ++on_step_calls;
    return atx::core::Ok();
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override {
    ++hedge_calls;
    return hedge_;
  }

  unsigned on_step_calls{0u};
  mutable unsigned hedge_calls{0u};

private:
  HedgeSpec hedge_{};
};

class MassOpenThenCloseStrategy final : public IStrategy {
public:
  MassOpenThenCloseStrategy(std::int64_t expiry, std::size_t n_lots) noexcept
      : expiry_{expiry}, n_lots_{n_lots} {}

  Status on_step(const MarketSnapshot & /*base*/, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0u) {
      book.lots.reserve(n_lots_);
      for (std::size_t i = 0; i < n_lots_; ++i) {
        book.lots.push_back(Lot{next_lot_id++, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0,
                                100.0, expiry_, 0u, 2.0});
      }
    } else if (step_index == 1u) {
      book.lots.clear();
    }
    return atx::core::Ok();
  }

private:
  std::int64_t expiry_{0};
  std::size_t n_lots_{0};
};

class PriceOptionsSpyStrategy final : public IStrategy {
public:
  Status on_step(const MarketSnapshot & /*base*/, std::size_t /*step_index*/,
                 PortfolioState & /*book*/, std::uint64_t & /*next_lot_id*/) override {
    ++legacy_calls;
    return atx::core::Ok();
  }

  Status on_step(const MarketSnapshot & /*base*/, std::size_t /*step_index*/,
                 PortfolioState & /*book*/, std::uint64_t & /*next_lot_id*/,
                 const PriceOptions &options) override {
    ++priced_calls;
    last_options = options;
    return atx::core::Ok();
  }

  unsigned legacy_calls{0};
  unsigned priced_calls{0};
  PriceOptions last_options{};
};

[[nodiscard]] StrategySpec daily_two_leg_roll_spec() {
  StrategySpec spec;
  spec.name = "daily-two-leg-roll";
  LegSpec call;
  call.uid = kUid;
  call.tenor.target_T = 30.0 / 365.25;
  call.structure.kind = StructureSpec::Kind::Single;
  call.structure.single_side = Side::Call;
  call.strike = {StrikeSelector::Kind::AbsStrike, 95.0};
  call.size = {SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  LegSpec put = call;
  put.structure.single_side = Side::Put;
  put.strike = {StrikeSelector::Kind::AbsStrike, 105.0};
  put.size.sign = -1.0;
  spec.legs = {call, put};
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 29.5 / 365.25;
  return spec;
}

void expect_result_bit_identical(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  const std::vector<std::pair<const std::vector<double> *, const std::vector<double> *>> cols = {
      {&a.pnl_total, &b.pnl_total},
      {&a.pnl_delta, &b.pnl_delta},
      {&a.pnl_gamma, &b.pnl_gamma},
      {&a.pnl_vega, &b.pnl_vega},
      {&a.pnl_vanna, &b.pnl_vanna},
      {&a.pnl_volga, &b.pnl_volga},
      {&a.pnl_theta, &b.pnl_theta},
      {&a.pnl_rho, &b.pnl_rho},
      {&a.pnl_charm, &b.pnl_charm},
      {&a.pnl_unexplained, &b.pnl_unexplained},
      {&a.pnl_settlement, &b.pnl_settlement},
      {&a.nav, &b.nav},
      {&a.gross_delta, &b.gross_delta},
      {&a.gross_gamma, &b.gross_gamma},
      {&a.gross_vega, &b.gross_vega},
      {&a.gross_theta, &b.gross_theta},
      {&a.n_open_lots, &b.n_open_lots},
      {&a.n_unpriced_lots, &b.n_unpriced_lots},
      {&a.n_unpriced_greeks, &b.n_unpriced_greeks}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.date[i], b.date[i]) << i;
    EXPECT_EQ(a.ts_ns[i], b.ts_ns[i]) << i;
    for (const auto &[va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << i;
    }
  }
  ASSERT_EQ(a.step_pnl_total.size(), b.step_pnl_total.size());
  for (std::size_t k = 0; k < a.step_pnl_total.size(); ++k) {
    EXPECT_TRUE(bits_equal(a.step_pnl_total[k], b.step_pnl_total[k])) << k;
  }
}

} // namespace

// ── 0. The RunConfig construction contract (S4-T19, plan item 4.2) ──────────

// RunConfig is a designated-init-only aggregate. The compile-time half of that
// contract is the field-count pin in backtest.hpp; this is the runtime half. It
// asserts the two properties positional init could never give: a named
// initializer lands on the field its name says regardless of declaration order,
// and an OMITTED field takes its own default member initializer rather than a
// neighbour's value. The default assertions double as the determinism gate for
// the three fields this task moved (query_cache_build_policy, step_observer,
// surface_provenance_policy) — a default-constructed RunConfig must still be the
// exact policy bundle it was before the reorder.
TEST(RunConfigContract, DesignatedInitBindsByName) {
  const RunConfig defaults{};
  EXPECT_EQ(defaults.price.n_threads, 0u);
  EXPECT_TRUE(defaults.price.analytic_greeks);
  EXPECT_EQ(defaults.query_pricing_tier, QueryPricingTier::LegacyCompatible);
  EXPECT_EQ(defaults.query_cache_build_policy, QueryCacheBuildPolicy::Eager);
  EXPECT_EQ(defaults.record_every_n, 1u);
  EXPECT_FALSE(static_cast<bool>(defaults.step_observer));
  // Plan 5.5: the default token is the NOT-CANCELLABLE one. This is the
  // determinism assertion for the 15->16 field insertion — a default RunConfig
  // must still describe a run that cannot stop early.
  EXPECT_FALSE(defaults.cancel.stop_possible());
  EXPECT_FALSE(defaults.cancel.stop_requested());
  EXPECT_EQ(defaults.unpriced, UnpricedLotPolicy::Error);
  EXPECT_EQ(defaults.surface_provenance_policy, SurfaceProvenancePolicy::Compatibility);
  EXPECT_EQ(defaults.snapshot_cache, nullptr);
  EXPECT_TRUE(defaults.prefetch_snapshots);
  EXPECT_TRUE(defaults.settlement_mark_memo);
  EXPECT_FALSE(defaults.reconcile_nav);
  EXPECT_FALSE(defaults.book_entry_fill_slippage);
  EXPECT_DOUBLE_EQ(defaults.reconcile_nav_tol, 1.0e-6);

  // Naming the three formerly-appended fields binds them, and only them.
  const RunConfig named{
      .query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly,
      .surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk,
  };
  EXPECT_EQ(named.query_cache_build_policy, QueryCacheBuildPolicy::ReuseOnly);
  EXPECT_EQ(named.surface_provenance_policy, SurfaceProvenancePolicy::RequireAdmittedRisk);
  EXPECT_EQ(named.query_pricing_tier, QueryPricingTier::LegacyCompatible);
  EXPECT_EQ(named.unpriced, UnpricedLotPolicy::Error);
  EXPECT_EQ(named.record_every_n, 1u);
  EXPECT_DOUBLE_EQ(named.reconcile_nav_tol, 1.0e-6);

  // Plan 5.5: `cancel` was INSERTED between step_observer and unpriced rather
  // than appended, so this is the assertion that the insertion did not rebind a
  // neighbour. Naming only `cancel` must set only `cancel` — its two textual
  // neighbours keep their own defaults.
  const std::atomic<bool> flag{false};
  const RunConfig with_cancel{.cancel = CancelToken{flag}};
  EXPECT_TRUE(with_cancel.cancel.stop_possible());
  EXPECT_FALSE(with_cancel.cancel.stop_requested());
  EXPECT_FALSE(static_cast<bool>(with_cancel.step_observer));
  EXPECT_EQ(with_cancel.unpriced, UnpricedLotPolicy::Error);
  EXPECT_EQ(with_cancel.record_every_n, 1u);
}

// ── 0b. Cooperative cancellation (S5-T26, plan item 5.5) ───────────────────
//
// `run_backtest` is the one cancellable entry that touches NO files: it builds a
// BacktestResult in memory and hands it back. So "no corrupted artifacts" here
// is proved differently from the corpus/populate entries — there is nothing on
// disk to inspect, and the property that matters instead is that a cancelled run
// yields NO result at all (never a short one a caller might mistake for a
// complete run), and that cancellation leaves no residue that could perturb a
// later run.

// GATE. A cancelled run must report Cancelled specifically — not Internal, not
// Unavailable, not a generic failure — because a host has to distinguish "you
// asked me to stop" from "something broke" by CODE, never by message text.
TEST(BacktestCancellation, FixedBookRunReturnsCancelledAndNoResult) {
  const fs::path dir = fresh_dir("cancel-fixed");
  const int n = 5;
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", n);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  std::atomic<bool> stop{true}; // already requested: stops before step 1
  RunConfig cfg{};
  cfg.cancel = CancelToken{stop};

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  auto res = run_backtest(*clock, survivor_book(expiry), cfg);

  ASSERT_FALSE(res.has_value()) << "a cancelled run must not return a BacktestResult";
  EXPECT_EQ(res.error().code(), ErrorCode::Cancelled);
  EXPECT_NE(res.error().code(), ErrorCode::Internal);
}

// GATE. The mid-run case, and the one that would catch a check placed after the
// row append: cancel from the step observer at step 2 and assert the run is
// abandoned. The observer also proves the stop is COOPERATIVE — it is honoured at
// the next step boundary, not by tearing down the current step.
TEST(BacktestCancellation, StrategyRunStopsAtTheNextStepBoundary) {
  const fs::path dir = fresh_dir("cancel-strategy");
  const int n = 6;
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", n);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  std::atomic<bool> stop{false};
  std::size_t steps_seen = 0;
  RunConfig cfg{};
  cfg.cancel = CancelToken{stop};
  cfg.step_observer = [&](const StepEvent &) -> Status {
    ++steps_seen;
    if (steps_seen == 2u) {
      stop.store(true, std::memory_order_relaxed);
    }
    return atx::core::Ok();
  };

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  OpenThenCloseStrategy strat{expiry};
  auto res = run_backtest(*clock, strat, cfg);

  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::Cancelled);
  // The stop was requested during step 2 and honoured at the top of a later
  // step, so the run must have ended EARLY — never having observed all n steps.
  EXPECT_LT(steps_seen, static_cast<std::size_t>(n));
  EXPECT_GE(steps_seen, 2u);
}

// GATE, and the determinism half of plan item 5.5. A token that is PRESENT but
// never set must produce a bit-identical run to one with no token at all. This is
// what makes "cancellation adds no timing-dependent state to published artifacts"
// checkable rather than merely asserted: same NAV column, bit for bit.
TEST(BacktestCancellation, AnUntriggeredTokenIsBitIdenticalToNoToken) {
  const fs::path dir = fresh_dir("cancel-noop");
  const int n = 5;
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", n);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 120 * kDayNs;

  auto plain = run_backtest(*clock, survivor_book(expiry), RunConfig{});
  ASSERT_TRUE(plain.has_value()) << plain.error().to_string();

  std::atomic<bool> never{false};
  RunConfig cfg{};
  cfg.cancel = CancelToken{never};
  ASSERT_TRUE(cfg.cancel.stop_possible()); // negative control: the token is live
  auto tokened = run_backtest(*clock, survivor_book(expiry), cfg);
  ASSERT_TRUE(tokened.has_value()) << tokened.error().to_string();

  expect_result_bit_identical(*plain, *tokened);
}

// ── 0c. EXIT CRITERION: a host embedding atx-vol (S5-T26) ──────────────────
//
// The sprint's exit criterion for items 5.4 + 5.5 is a single demonstration that
// a HOST can do both things a host needs: take ownership of every diagnostic the
// library emits, and stop a running backtest. Testing them separately (as the
// suites above do) proves each mechanism; this proves they compose, which is the
// thing an embedder actually depends on.
//
// The host below:
//   1. installs a sink and redirects BOTH process streams, so any library write
//      that escapes the sink is caught rather than silently scrolling past;
//   2. drives a real emitting library path and takes delivery of the record;
//   3. runs a backtest and cancels it from its own step observer;
//   4. asserts the run stopped with Cancelled, and that across the whole episode
//      NOTHING reached stdout or stderr.
TEST(HostIntegration, CapturesAllLibraryOutputAndCancelsARunningBacktest) {
  const fs::path dir = fresh_dir("host-integration");
  const int n = 8;
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", n);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // A definitions TSV whose cache lookup must MISS — a genuine routed library
  // diagnostic for the host to capture.
  const fs::path defs = dir / "definitions.tsv";
  {
    std::ofstream out{defs, std::ios::binary};
    out << "host-integration-probe\n";
  }

  atx::vol::testing::CapturingSink sink;
  std::atomic<bool> stop{false};
  std::size_t steps_seen = 0;
  Result<BacktestResult> run = Err(ErrorCode::Unknown, "not run");

  std::string leaked_stdout;
  std::string leaked_stderr;
  {
    atx::vol::testing::StreamCapture out_capture{
        stdout, atx::vol::testing::sink_scratch_file("host-stdout")};
    atx::vol::testing::StreamCapture err_capture{
        stderr, atx::vol::testing::sink_scratch_file("host-stderr")};
    {
      const atx::vol::testing::ScopedSink installed{sink};

      // (2) a real emitting path
      static_cast<void>(
          read_listed_definitions_cached(defs.string(), (dir / "defcache").string()));

      // (3) a running backtest, cancelled by the host mid-flight
      RunConfig cfg{};
      cfg.cancel = CancelToken{stop};
      cfg.step_observer = [&](const StepEvent &) -> Status {
        ++steps_seen;
        if (steps_seen == 3u) {
          stop.store(true, std::memory_order_relaxed);
        }
        return atx::core::Ok();
      };
      OpenThenCloseStrategy strat{kBaseNow + 120 * kDayNs};
      run = run_backtest(*clock, strat, cfg);
    }
    leaked_stdout = out_capture.release();
    leaked_stderr = err_capture.release();
  }

  // (4a) the backtest stopped, and said so precisely.
  ASSERT_FALSE(run.has_value()) << "the host cancelled the run but got a result back";
  EXPECT_EQ(run.error().code(), ErrorCode::Cancelled);
  EXPECT_LT(steps_seen, static_cast<std::size_t>(n)) << "the run did not stop early";

  // (4b) the host received the library's diagnostics...
  const std::vector<atx::vol::testing::Record> records = sink.snapshot();
  bool saw_definitions_record = false;
  for (const atx::vol::testing::Record &r : records) {
    if (r.message.rfind("listed definitions cache:", 0) == 0) {
      saw_definitions_record = true;
    }
    EXPECT_EQ(r.message.find('\n'), std::string::npos) << "records must be single lines";
  }
  EXPECT_TRUE(saw_definitions_record) << "the host did not receive a known library diagnostic";

  // ...and the process streams stayed completely silent throughout.
  EXPECT_EQ(leaked_stdout, "") << "library output escaped to stdout";
  EXPECT_EQ(leaked_stderr, "") << "library output escaped to stderr";
}

// ── 1. Load-once ────────────────────────────────────────────────────────────
TEST(Backtest, LoadOnce) {
  const fs::path dir = fresh_dir("loadonce");
  const int n = 5;
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", n);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), static_cast<std::size_t>(n));

  const std::int64_t expiry = kBaseNow + 120 * kDayNs; // survives every date
  MarketSnapshot::reset_open_count();
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(MarketSnapshot::open_count(), static_cast<std::uint64_t>(n));
  EXPECT_EQ(res->size(), static_cast<std::size_t>(n)); // inception + (n-1) steps
}

TEST(Backtest, FixedBookDuplicateLotIdsFailBeforeArchiveLoadOrPricing) {
  const fs::path dir = fresh_dir("fixed-book-duplicate-id");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 1);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  PortfolioState book = survivor_book(kBaseNow + 120 * kDayNs);
  book.lots[1].id = book.lots[0].id;
  MarketSnapshot::reset_open_count();

  const auto result = run_backtest(*clock, std::move(book));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("duplicate lot id=1 in initial fixed book"),
            std::string::npos);
  EXPECT_EQ(MarketSnapshot::open_count(), 0u);
}

TEST(Backtest, InvalidRunConfigFailsBeforeArchiveLoadOrPricing) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("invalid-run-config");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 1);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  std::vector<std::pair<std::string, RunConfig>> cases;
  const auto add_case = [&cases](std::string name, const auto &mutate) {
    RunConfig config;
    mutate(config);
    cases.emplace_back(std::move(name), std::move(config));
  };
  add_case("zero record stride", [](RunConfig &c) { c.record_every_n = 0u; });
  add_case("nan price spread", [](RunConfig &c) {
    c.frictions.half_spread_bps = std::numeric_limits<double>::quiet_NaN();
  });
  add_case("negative vol tick", [](RunConfig &c) { c.frictions.vol_tick = -0.01; });
  add_case("infinite contract cost", [](RunConfig &c) {
    c.frictions.per_contract_cost = std::numeric_limits<double>::infinity();
  });
  add_case("negative hedge slippage", [](RunConfig &c) { c.frictions.hedge_slippage_bps = -1.0; });
  add_case("nan initial cash", [](RunConfig &c) {
    c.financing.initial_cash = std::numeric_limits<double>::quiet_NaN();
  });
  add_case("negative borrow rate", [](RunConfig &c) { c.financing.borrow_rate = -0.01; });
  add_case("nan borrow rate", [](RunConfig &c) {
    c.financing.borrow_rate = std::numeric_limits<double>::quiet_NaN();
  });
  add_case("prices-only backtest", [](RunConfig &c) { c.price.prices_only = true; });
  add_case("nan sticky weight", [](RunConfig &c) {
    c.price.sticky.ref_uprc_weight = std::numeric_limits<double>::quiet_NaN();
  });
  add_case("invalid spread kind", [](RunConfig &c) {
    c.frictions.spread_kind = static_cast<FrictionModel::SpreadKind>(255u);
  });
  add_case("invalid query tier",
           [](RunConfig &c) { c.query_pricing_tier = static_cast<QueryPricingTier>(255u); });
  add_case("invalid query execution",
           [](RunConfig &c) { c.price.query_execution = static_cast<QueryExecution>(255u); });

  for (auto &[name, config] : cases) {
    SCOPED_TRACE(name);
    MarketSnapshot::reset_open_count();
    if constexpr (counters_enabled()) {
      atx::vol::counters::reset();
    }
    const auto result = run_backtest(*clock, PortfolioState{}, config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(MarketSnapshot::open_count(), 0u);
    if constexpr (counters_enabled()) {
      EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::BoundarySolves), 0u);
    }

    PriceOptionsSpyStrategy strategy;
    MarketSnapshot::reset_open_count();
    if constexpr (counters_enabled()) {
      atx::vol::counters::reset();
    }
    const auto strategy_result = run_backtest(*clock, strategy, config);
    ASSERT_FALSE(strategy_result.has_value());
    EXPECT_EQ(strategy_result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(strategy.legacy_calls, 0u);
    EXPECT_EQ(strategy.priced_calls, 0u);
    EXPECT_EQ(MarketSnapshot::open_count(), 0u);
    if constexpr (counters_enabled()) {
      EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::BoundarySolves), 0u);
    }
  }
}

TEST(Backtest, InvalidFixedBookEconomicsFailBeforeArchiveLoadOrPricing) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("invalid-fixed-book-economics");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 1);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  std::vector<std::pair<std::string, PortfolioState>> cases;
  const auto add_case = [&cases](std::string name, const auto &mutate) {
    PortfolioState book = survivor_book(kBaseNow + 120 * kDayNs);
    mutate(book.lots.front());
    cases.emplace_back(std::move(name), std::move(book));
  };
  add_case("nan entry price",
           [](Lot &lot) { lot.entry_price = std::numeric_limits<double>::quiet_NaN(); });
  add_case("zero multiplier", [](Lot &lot) { lot.multiplier = 0.0; });
  add_case("nan quantity", [](Lot &lot) { lot.qty = std::numeric_limits<double>::quiet_NaN(); });
  add_case("zero strike", [](Lot &lot) { lot.contract.K = 0.0; });
  add_case("nan model tenor",
           [](Lot &lot) { lot.contract.T = std::numeric_limits<double>::quiet_NaN(); });
  add_case("invalid option side", [](Lot &lot) { lot.contract.side = static_cast<Side>(255u); });

  for (auto &[name, book] : cases) {
    SCOPED_TRACE(name);
    MarketSnapshot::reset_open_count();
    if constexpr (counters_enabled()) {
      atx::vol::counters::reset();
    }
    const auto result = run_backtest(*clock, std::move(book));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(MarketSnapshot::open_count(), 0u);
    if constexpr (counters_enabled()) {
      EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::BoundarySolves), 0u);
    }
  }
}

TEST(Backtest, InitialExpiryMustBeAfterInceptionBeforePricing) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("invalid-fixed-book-expiry");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 1);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  PortfolioState book = survivor_book(kBaseNow);
  MarketSnapshot::reset_open_count();
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }

  const auto result = run_backtest(*clock, std::move(book));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("expiry_ts_ns"), std::string::npos);
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);
  if constexpr (counters_enabled()) {
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::BoundarySolves), 0u);
  }
}

TEST(Backtest, InvalidStrategyEntryFailsBeforeExecutionSideEffects) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("invalid-strategy-entry");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 1);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  Lot invalid{0u, OptionContract{kUid, 95.0, 0.0, Side::Call}, 1.0, 100.0, kBaseNow + 120 * kDayNs,
              0u, std::numeric_limits<double>::quiet_NaN()};
  InvalidEntryStrategy strategy{invalid};
  RunConfig config;
  config.prefetch_snapshots = false;
  MarketSnapshot::reset_open_count();
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("entry_price"), std::string::npos);
  EXPECT_EQ(strategy.on_step_calls, 1u);
  EXPECT_EQ(strategy.hedge_calls, 0u);
  EXPECT_EQ(strategy.seed_calls, 0u);
  EXPECT_EQ(strategy.signal_calls, 0u);
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);
  if constexpr (counters_enabled()) {
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::BoundarySolves), 0u);
  }
}

TEST(Backtest, InvalidHedgeSpecFailsBeforePricingOrExecutionMutation) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("invalid-hedge-spec");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 1);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  HedgeSpec invalid;
  invalid.kind = HedgeSpec::Kind::DeltaToZero;
  invalid.band = std::numeric_limits<double>::quiet_NaN();
  InvalidHedgeStrategy strategy{invalid};
  RunConfig config;
  config.prefetch_snapshots = false;
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("hedge band"), std::string::npos);
  EXPECT_EQ(strategy.on_step_calls, 1u);
  EXPECT_EQ(strategy.hedge_calls, 1u);
  if constexpr (counters_enabled()) {
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::BoundarySolves), 0u);
  }
}

TEST(Backtest, ReusableTargetMarkFrameKeepsWarmStorageAndActivePrefix) {
  ReusableTargetMarkFrame frame;
  frame.prepare(3u);
  TargetMarkView initial = frame.write_view();
  ASSERT_EQ(initial.id.size(), 3u);
  const std::uint64_t *const id_data = initial.id.data();
  const double *const mark_data = initial.price_target.data();
  const double *const vega_data = initial.base_vega_proxy.data();
  const PriceStatus *const status_data = initial.status.data();
  initial.id[0] = 7u;
  initial.price_target[0] = 1.25;
  initial.base_vega_proxy[0] = 0.75;
  initial.status[0] = PriceStatus::Ok;
  initial.id[1] = 8u;
  initial.id[2] = 9u;
  ASSERT_TRUE(frame.seal().has_value());
  const auto match = frame.find_ok(7u);
  ASSERT_TRUE(match.has_value());
  EXPECT_DOUBLE_EQ(match->raw_mark, 1.25);
  EXPECT_DOUBLE_EQ(match->base_vega_proxy, 0.75);

  frame.prepare(1u);
  EXPECT_EQ(frame.size(), 1u);
  EXPECT_EQ(frame.storage_size(), 3u);
  TargetMarkView smaller = frame.write_view();
  EXPECT_EQ(smaller.id.data(), id_data);
  EXPECT_EQ(smaller.price_target.data(), mark_data);
  EXPECT_EQ(smaller.base_vega_proxy.data(), vega_data);
  EXPECT_EQ(smaller.status.data(), status_data);

  frame.prepare(2u);
  EXPECT_EQ(frame.storage_size(), 3u);
  TargetMarkView rewarmed = frame.write_view();
  EXPECT_EQ(rewarmed.id.data(), id_data);
  EXPECT_EQ(rewarmed.price_target.data(), mark_data);
  EXPECT_EQ(rewarmed.base_vega_proxy.data(), vega_data);
  EXPECT_EQ(rewarmed.status.data(), status_data);
}

TEST(Backtest, ReusableTargetMarkFrameSealsOneThousandUnorderedIdsWithoutRegrowing) {
  constexpr std::size_t n = 1'000u;
  ReusableTargetMarkFrame frame;
  frame.prepare(n);
  TargetMarkView rows = frame.write_view();
  for (std::size_t i = 0; i < n; ++i) {
    rows.id[i] = static_cast<std::uint64_t>(n - i);
    rows.price_target[i] = static_cast<double>(i) / 100.0;
    rows.base_vega_proxy[i] = 0.5;
    rows.status[i] = PriceStatus::Ok;
  }
  ASSERT_TRUE(frame.seal().has_value());
  ASSERT_EQ(frame.index_storage_size(), n);
  const auto first = frame.find_ok(1u);
  const auto middle = frame.find_ok(500u);
  const auto last = frame.find_ok(1'000u);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(middle.has_value());
  ASSERT_TRUE(last.has_value());
  EXPECT_DOUBLE_EQ(first->raw_mark, 9.99);
  EXPECT_DOUBLE_EQ(middle->raw_mark, 5.0);
  EXPECT_DOUBLE_EQ(last->raw_mark, 0.0);

  frame.prepare(n / 2u);
  TargetMarkView smaller = frame.write_view();
  for (std::size_t i = 0; i < smaller.id.size(); ++i) {
    smaller.id[i] = static_cast<std::uint64_t>(i + 1u);
    smaller.price_target[i] = 1.0;
    smaller.base_vega_proxy[i] = 0.5;
    smaller.status[i] = PriceStatus::Ok;
  }
  ASSERT_TRUE(frame.seal().has_value());
  EXPECT_EQ(frame.storage_size(), n);
  EXPECT_EQ(frame.index_storage_size(), n);
}

TEST(Backtest, ReusableTargetMarkFrameRejectsDuplicateIdsWhenSealed) {
  ReusableTargetMarkFrame frame;
  frame.prepare(2u);
  TargetMarkView rows = frame.write_view();
  rows.id[0] = 11u;
  rows.id[1] = 11u;
  rows.price_target[0] = 1.0;
  rows.price_target[1] = 2.0;
  rows.base_vega_proxy[0] = 0.5;
  rows.base_vega_proxy[1] = 0.5;
  rows.status[0] = PriceStatus::Ok;
  rows.status[1] = PriceStatus::Ok;

  const Status sealed = frame.seal();
  ASSERT_FALSE(sealed.has_value());
  EXPECT_EQ(sealed.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(sealed.error().message().find("duplicate target-mark lot id=11"), std::string::npos);
  EXPECT_FALSE(frame.find_ok(11u).has_value());
}

TEST(Backtest, ReusableTargetMarkFrameRejectsStaleMissingBadAndMismatchedRows) {
  ReusableTargetMarkFrame frame;
  frame.prepare(1u);
  TargetMarkView row = frame.write_view();
  row.id[0] = 7u;
  row.price_target[0] = 1.25;
  row.base_vega_proxy[0] = 0.75;
  row.status[0] = PriceStatus::Ok;
  ASSERT_TRUE(frame.seal().has_value());
  EXPECT_FALSE(frame.find_ok(8u).has_value()); // id mismatch

  row.status[0] = PriceStatus::NumericError;
  EXPECT_FALSE(frame.find_ok(7u).has_value()); // bad P&L lane
  row.status[0] = PriceStatus::Ok;
  row.price_target[0] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(frame.find_ok(7u).has_value()); // malformed Ok lane
  row.price_target[0] = 1.25;
  row.base_vega_proxy[0] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(frame.find_ok(7u).has_value()); // malformed telemetry proxy

  frame.prepare(0u);
  EXPECT_EQ(frame.storage_size(), 1u);
  EXPECT_FALSE(frame.find_ok(7u).has_value()); // inactive stale storage
}

TEST(Backtest, EngineForwardsRunPriceOptionsToStrategy) {
  const fs::path dir = fresh_dir("strategy-price-options");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 2);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  PriceOptionsSpyStrategy strategy;
  RunConfig config;
  config.price.n_threads = 3u;
  config.price.analytic_greeks = true;
  config.price.query_execution = QueryExecution::ColdReference;
  config.prefetch_snapshots = false;
  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_EQ(strategy.legacy_calls, 0u);
  EXPECT_EQ(strategy.priced_calls, 2u);
  EXPECT_EQ(strategy.last_options.n_threads, config.price.n_threads);
  EXPECT_EQ(strategy.last_options.analytic_greeks, config.price.analytic_greeks);
  EXPECT_EQ(strategy.last_options.query_execution, config.price.query_execution);
}

TEST(Backtest, DeclarativeEntryReusesStrategyFullGreekSeedWithoutDuplicateSolve) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("strategy-entry-seed-reuse");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", path}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.123456789012345;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Call;
  leg.strike = {StrikeSelector::Kind::AbsStrike, 101.0};
  leg.size = {SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  DeclarativeStrategy strategy{spec};

  RunConfig config;
  config.price.n_threads = 1u;
  config.price.analytic_greeks = true;
  config.prefetch_snapshots = false;
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }
  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->n_open_lots.front(), 1.0);
  if constexpr (counters_enabled()) {
    const auto measured = atx::vol::counters::snapshot();
    EXPECT_EQ(measured.get(Counter::FullGreekSeedReuseLanes), 1u);
    EXPECT_EQ(measured.get(Counter::FullGreekSeedRejectedCandidates), 0u);
    EXPECT_EQ(measured.get(Counter::SurfaceFullGreekRoutes), 1u);
    EXPECT_EQ(measured.get(Counter::BoundarySolves), 5u);
  }
}

TEST(Backtest, SnapshotCacheCoalescesPrefetchAndLoads) {
  const fs::path dir = fresh_dir("snapshot-cache");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);

  MarketSnapshot::reset_open_count();
  SnapshotCache cache;
  cache.prefetch(path);
  auto first = cache.load(path);
  auto second = cache.load(path);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(second.has_value()) << second.error().to_string();
  EXPECT_EQ(first->get(), second->get());
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);
  const SnapshotCacheStats stats = cache.stats();
  EXPECT_EQ(stats.loads, 1u);
  EXPECT_EQ(stats.prefetches, 1u);
  EXPECT_GE(stats.hits, 2u);
}

TEST(Backtest, ArchivedSnapshotDefaultsColdAndPreparesEverySurfaceForRequestedTier) {
  const fs::path dir = fresh_dir("snapshot-query-tier");
  const PricedSurface first_surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const PricedSurface second_surface = make_surface(kUid + 1u, 25.0, 25.0, kBaseNow);
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / "2026-08-01.atxvsa").string();
  const std::array<SurfaceArchiveItem, 2> items{
      {{"SPX", &first_surface}, {"VIX", &second_surface}}};
  const Status write_status = write_surface_archive_v2_file(path, items);
  ASSERT_TRUE(write_status.has_value()) << write_status.error().to_string();

  auto legacy = MarketSnapshot::load(path);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  ASSERT_EQ(legacy->n_surfaces(), items.size());
  // WS-ZC1: a cache-free tier is served by BORROWED views, so assert through the
  // backing-agnostic accessor. `query_pricing_route` is a PricedSurface-only
  // introspection hook, so it is checked on whichever backing owns surfaces.
  for (std::size_t i = 0; i < legacy->n_surfaces(); ++i) {
    EXPECT_EQ(legacy->surface_at(i).query_pricing_tier(), QueryPricingTier::LegacyCompatible);
  }
  // WS-ZC1 made this cache-free tier BORROW its surfaces, which left `surfaces()`
  // EMPTY — so the `query_pricing_route` loop that used to stand here iterated zero
  // times and asserted nothing while still reading as protection. "Defaults cold" is
  // asserted through the backing-agnostic handle instead: a cold tier is exactly a
  // surface with no query accelerator, which is what makes its route ColdReference.
  EXPECT_TRUE(legacy->borrows_views());
  for (std::size_t i = 0; i < legacy->n_surfaces(); ++i) {
    EXPECT_EQ(legacy->surface_at(i).query_cache_pair_count(), 0u);
  }

  auto fast = MarketSnapshot::load(path, QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(fast.has_value()) << fast.error().to_string();
  ASSERT_EQ(fast->n_surfaces(), items.size());
  // A fast tier cannot be served by a view (a view carries no accelerator), so it
  // reconstructs OWNED surfaces and `surfaces()` is genuinely populated here. Assert
  // that explicitly: it is what keeps this `PricedSurface`-only loop from silently
  // going vacuous the way the cold one above did.
  EXPECT_FALSE(fast->borrows_views());
  ASSERT_EQ(fast->surfaces().size(), items.size());
  for (const PricedSurface &surface : fast->surfaces()) {
    EXPECT_EQ(surface.query_pricing_tier(), QueryPricingTier::RepresentativeFast);
    EXPECT_GT(surface.query_cache_pair_count(), 0u);
    // Real route introspection, on the owned path that actually exercises it. Query
    // at each surface's OWN spot so the resolved point is inside its certified
    // correction box (the two archived surfaces sit at S=100 and S=25).
    EXPECT_EQ(surface.query_pricing_route(surface.pricing().S, 0.25, Side::Put),
              QueryPricingRoute::RepresentativeFast);
  }
}

TEST(Backtest, MarketSnapshotPreservesSameBlobProvenanceByDirectoryUid) {
  const fs::path dir = fresh_dir("snapshot-provenance-pairing");
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / "2026-08-01.atxvsa").string();
  const PricedSurface zzz = make_surface(42u, 100.0, 100.0, kBaseNow);
  const PricedSurface aaa = make_surface(7u, 25.0, 25.0, kBaseNow);
  SurfaceProvenance zzz_provenance =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Healthy, 42u, 420u);
  SurfaceProvenance aaa_provenance =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Degraded, 7u, 70u);
  const std::array<SurfaceArchiveItem, 2> items{
      SurfaceArchiveItem{"ZZZ", &zzz, zzz_provenance},
      SurfaceArchiveItem{"AAA", &aaa, aaa_provenance},
  };
  const Status written = write_surface_archive_v2_file(path, items);
  ASSERT_TRUE(written.has_value()) << written.error().to_string();

  auto snapshot = MarketSnapshot::load(path);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  ASSERT_EQ(snapshot->n_surfaces(), 2u);
  ASSERT_EQ(snapshot->provenances().size(), snapshot->n_surfaces());
  EXPECT_EQ(snapshot->surface_at(0).uid(), 7u);
  EXPECT_EQ(snapshot->surface_at(1).uid(), 42u);
  EXPECT_EQ(snapshot->provenances()[0].validation.validation_id, 70u);
  EXPECT_EQ(snapshot->provenances()[1].validation.validation_id, 420u);
  const SurfaceProvenance *aaa_got = snapshot->provenance(7u);
  const SurfaceProvenance *zzz_got = snapshot->provenance(42u);
  ASSERT_NE(aaa_got, nullptr);
  ASSERT_NE(zzz_got, nullptr);
  EXPECT_EQ(aaa_got->validation.validation_id, 70u);
  EXPECT_EQ(aaa_got->served_generation, 7u);
  EXPECT_EQ(zzz_got->validation.validation_id, 420u);
  EXPECT_EQ(zzz_got->served_generation, 42u);
  EXPECT_EQ(snapshot->provenance(999u), nullptr);
}

TEST(Backtest, LegacyArchivePassesCompatibilityAndFailsStrictRiskPolicy) {
  const fs::path dir = fresh_dir("legacy-provenance-policy");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", path}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  RunConfig compatibility;
  compatibility.prefetch_snapshots = false;
  const auto accepted = run_backtest(*clock, PortfolioState{}, compatibility);
  ASSERT_TRUE(accepted.has_value()) << accepted.error().to_string();

  RunConfig strict = compatibility;
  strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;
  const auto rejected = run_backtest(*clock, PortfolioState{}, strict);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(rejected.error().message().find("uid=7"), std::string::npos);
  EXPECT_NE(rejected.error().message().find("purpose=MarketMark"), std::string::npos);
  EXPECT_NE(rejected.error().message().find("state=Degraded"), std::string::npos);
  EXPECT_NE(rejected.error().message().find("legacy=true"), std::string::npos);
}

TEST(Backtest, ExplicitHealthyRiskPassesStrictPolicyInBothRunOverloads) {
  const fs::path dir = fresh_dir("healthy-risk-provenance-policy");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const SurfaceProvenance provenance =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Healthy, 3u);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface, provenance);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", path}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  RunConfig strict;
  strict.prefetch_snapshots = false;
  strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;

  const auto fixed = run_backtest(*clock, PortfolioState{}, strict);
  ASSERT_TRUE(fixed.has_value()) << fixed.error().to_string();
  PriceOptionsSpyStrategy strategy;
  const auto declarative = run_backtest(*clock, strategy, strict);
  ASSERT_TRUE(declarative.has_value()) << declarative.error().to_string();
}

TEST(Backtest, StrictPolicyPassesMultipleAlignedAdmittedSurfaces) {
  const fs::path dir = fresh_dir("multiname-provenance-policy");
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / "2026-08-01.atxvsa").string();
  const PricedSurface zzz = make_surface(42u, 100.0, 100.0, kBaseNow);
  const PricedSurface aaa = make_surface(7u, 25.0, 25.0, kBaseNow);
  const SurfaceProvenance healthy =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Healthy, 42u, 420u);
  const SurfaceProvenance degraded =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Degraded, 7u, 70u);
  const std::array<SurfaceArchiveItem, 2> items{
      SurfaceArchiveItem{"ZZZ", &zzz, healthy},
      SurfaceArchiveItem{"AAA", &aaa, degraded},
  };
  const Status written = write_surface_archive_v2_file(path, items);
  ASSERT_TRUE(written.has_value()) << written.error().to_string();
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", path}}, "AAA"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  RunConfig strict;
  strict.prefetch_snapshots = false;
  strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;

  const auto fixed = run_backtest(*clock, PortfolioState{}, strict);
  ASSERT_TRUE(fixed.has_value()) << fixed.error().to_string();
  PriceOptionsSpyStrategy strategy;
  const auto declarative = run_backtest(*clock, strategy, strict);
  ASSERT_TRUE(declarative.has_value()) << declarative.error().to_string();
}

TEST(Backtest, StrictPolicyValidatesEveryShiftedSnapshotInBothRunOverloads) {
  const fs::path dir = fresh_dir("shifted-provenance-policy");
  const PricedSurface base_surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const PricedSurface shifted_surface = make_surface(kUid, 101.0, 101.0, kBaseNow + kDayNs);
  const SurfaceProvenance admitted =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Healthy, 3u);
  const SurfaceProvenance inadmissible =
      make_provenance(SurfacePurpose::MarketMark, SurfaceState::Healthy, 4u);
  const std::string base_path = write_one(dir, "2026-08-01", "SPX", base_surface, admitted);
  const std::string shifted_path =
      write_one(dir, "2026-08-02", "SPX", shifted_surface, inadmissible);
  auto clock = Clock::from_manifest(
      make_manifest({{"2026-08-01", base_path}, {"2026-08-02", shifted_path}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  RunConfig strict;
  strict.prefetch_snapshots = false;
  strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;

  const auto fixed = run_backtest(*clock, PortfolioState{}, strict);
  ASSERT_FALSE(fixed.has_value());
  EXPECT_NE(fixed.error().message().find("purpose=MarketMark"), std::string::npos);
  PriceOptionsSpyStrategy strategy;
  const auto declarative = run_backtest(*clock, strategy, strict);
  ASSERT_FALSE(declarative.has_value());
  EXPECT_NE(declarative.error().message().find("purpose=MarketMark"), std::string::npos);
}

TEST(Backtest, MarketMarkSurfaceFailsStrictRiskPolicy) {
  const fs::path dir = fresh_dir("market-mark-provenance-policy");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const SurfaceProvenance provenance =
      make_provenance(SurfacePurpose::MarketMark, SurfaceState::Healthy, 3u);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface, provenance);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", path}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  RunConfig strict;
  strict.prefetch_snapshots = false;
  strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;

  const auto result = run_backtest(*clock, PortfolioState{}, strict);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("purpose=MarketMark"), std::string::npos);
  EXPECT_NE(result.error().message().find("legacy=false"), std::string::npos);
}

TEST(Backtest, StaleAndRejectedRiskSurfacesFailStrictPolicy) {
  for (const SurfaceState state : {SurfaceState::Stale, SurfaceState::Rejected}) {
    const fs::path dir = fresh_dir(state == SurfaceState::Stale ? "stale-provenance-policy"
                                                                : "rejected-provenance-policy");
    const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
    const SurfaceProvenance provenance = make_provenance(SurfacePurpose::Risk, state, 3u);
    const std::string path = write_one(dir, "2026-08-01", "SPX", surface, provenance);
    auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", path}}, "SPX"));
    ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
    RunConfig strict;
    strict.prefetch_snapshots = false;
    strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;

    const auto result = run_backtest(*clock, PortfolioState{}, strict);
    ASSERT_FALSE(result.has_value());
    const std::string expected_state =
        state == SurfaceState::Stale ? "state=Stale" : "state=Rejected";
    EXPECT_NE(result.error().message().find(expected_state), std::string::npos);
  }
}

TEST(Backtest, DegradedRiskPassesStrictPolicyButZeroServedGenerationFails) {
  const fs::path dir = fresh_dir("degraded-provenance-policy");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const SurfaceProvenance admitted =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Degraded, 3u);
  const std::string admitted_path = write_one(dir, "2026-08-01", "SPX", surface, admitted);
  auto admitted_clock = Clock::from_manifest(make_manifest({{"2026-08-01", admitted_path}}, "SPX"));
  ASSERT_TRUE(admitted_clock.has_value()) << admitted_clock.error().to_string();
  RunConfig strict;
  strict.prefetch_snapshots = false;
  strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;
  const auto accepted = run_backtest(*admitted_clock, PortfolioState{}, strict);
  ASSERT_TRUE(accepted.has_value()) << accepted.error().to_string();

  SurfaceProvenance zero_generation =
      make_provenance(SurfacePurpose::Risk, SurfaceState::Healthy, 0u);
  const std::string zero_path = write_one(dir, "2026-08-02", "SPX", surface, zero_generation);
  auto zero_clock = Clock::from_manifest(make_manifest({{"2026-08-02", zero_path}}, "SPX"));
  ASSERT_TRUE(zero_clock.has_value()) << zero_clock.error().to_string();
  const auto rejected = run_backtest(*zero_clock, PortfolioState{}, strict);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().message().find("served_generation=0"), std::string::npos);
}

TEST(Backtest, StrictPolicyValidatesPreloadedCacheInBothRunOverloads) {
  const fs::path dir = fresh_dir("preloaded-provenance-policy");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", path}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  auto cache = std::make_shared<SnapshotCache>();
  auto preloaded = cache->load(path);
  ASSERT_TRUE(preloaded.has_value()) << preloaded.error().to_string();
  const std::uint64_t load_count = cache->stats().loads;
  RunConfig strict;
  strict.prefetch_snapshots = false;
  strict.snapshot_cache = cache;
  strict.surface_provenance_policy = SurfaceProvenancePolicy::RequireAdmittedRisk;

  const auto fixed = run_backtest(*clock, PortfolioState{}, strict);
  ASSERT_FALSE(fixed.has_value());
  PriceOptionsSpyStrategy strategy;
  const auto declarative = run_backtest(*clock, strategy, strict);
  ASSERT_FALSE(declarative.has_value());
  EXPECT_EQ(cache->stats().loads, load_count);
}

TEST(Backtest, SnapshotCacheIdentityIncludesNormalizedPathAndQueryTier) {
  const fs::path dir = fresh_dir("snapshot-cache-tier-identity");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);
  const std::string alias =
      (fs::path{path}.parent_path() / "." / fs::path{path}.filename()).string();

  MarketSnapshot::reset_open_count();
  SnapshotCache cache;
  auto legacy = cache.load(path);
  auto same_legacy = cache.load(alias);
  auto cold = cache.load(path, QueryPricingTier::ColdReference);
  auto fast = cache.load(path, QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  ASSERT_TRUE(same_legacy.has_value()) << same_legacy.error().to_string();
  ASSERT_TRUE(cold.has_value()) << cold.error().to_string();
  ASSERT_TRUE(fast.has_value()) << fast.error().to_string();
  EXPECT_EQ(legacy->get(), same_legacy->get());
  EXPECT_NE(legacy->get(), cold->get());
  EXPECT_NE(cold->get(), fast->get());
  EXPECT_EQ((*legacy)->surface_at(0).query_pricing_tier(), QueryPricingTier::LegacyCompatible);
  EXPECT_EQ((*cold)->surface_at(0).query_pricing_tier(), QueryPricingTier::ColdReference);
  EXPECT_EQ((*fast)->surface_at(0).query_pricing_tier(), QueryPricingTier::RepresentativeFast);
  EXPECT_EQ(MarketSnapshot::open_count(), 3u);
  const SnapshotCacheStats stats = cache.stats();
  EXPECT_EQ(stats.loads, 3u);
  EXPECT_GE(stats.hits, 1u);
  EXPECT_EQ(stats.retained_entries, 3u);
}

// R-19 (F6): a cache keyed only on (path, tier) served a rewritten archive's
// stale snapshot indefinitely. The cache now keys/evicts on the archive's content
// identity, so rewriting the SAME path with different content (here at the SAME
// byte length and SAME created_ts_ns — only the blob CRC changes) evicts the
// stale entry and reloads the new bytes instead of serving the old snapshot.
TEST(Backtest, SnapshotCacheEvictsStaleEntryWhenArchiveRewrittenSameLength) {
  const fs::path dir = fresh_dir("snapshot-cache-rewrite");
  std::error_code mkdir_ec;
  fs::create_directories(dir, mkdir_ec);
  const std::string path = (dir / "2026-08-01.atxvsa").string();

  // Pin created_ts_ns so the two archives differ ONLY in surface content — the
  // same-byte-length / different-CRC case (§6.5), never a timestamp change. In v2
  // the record's payload_crc32c is mirrored into its directory entry, so this
  // content-only rewrite still changes metadata_crc32c → the content identity.
  ArchiveV2WriteOpts opts;
  opts.created_ts_ns = 1'000;
  const auto write_forward = [&](double forward) {
    const PricedSurface s = make_surface(kUid, 100.0, forward, kBaseNow);
    const SurfaceArchiveItem item{"SPX", &s, std::nullopt};
    const std::span<const SurfaceArchiveItem> items(&item, 1);
    const Status st = write_surface_archive_v2_file(path, items, opts);
    ASSERT_TRUE(st.has_value()) << st.error().to_string();
  };

  write_forward(100.0);
  const auto size_v1 = fs::file_size(path);

  MarketSnapshot::reset_open_count();
  SnapshotCache cache;
  auto first = cache.load(path);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);

  // Rewrite the SAME path with different blob content but identical framing.
  write_forward(101.0);
  ASSERT_EQ(fs::file_size(path), size_v1);  // same byte length: only the CRC moved

  auto second = cache.load(path);
  ASSERT_TRUE(second.has_value()) << second.error().to_string();
  // No stale serve: a fresh snapshot object was reloaded from disk, not the
  // cached one, and the stale entry was evicted.
  EXPECT_NE(first->get(), second->get());
  EXPECT_EQ(MarketSnapshot::open_count(), 2u);
  EXPECT_GE(cache.stats().evictions, 1u);

  // A third load of the now-unchanged archive is a clean cache hit (no re-open,
  // no further eviction).
  auto third = cache.load(path);
  ASSERT_TRUE(third.has_value()) << third.error().to_string();
  EXPECT_EQ(second->get(), third->get());
  EXPECT_EQ(MarketSnapshot::open_count(), 2u);
}

TEST(Backtest, ReuseOnlyFastMissLoadsColdAndLaterReusesEagerFastSnapshot) {
  const fs::path dir = fresh_dir("snapshot-cache-reuse-only");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);
  const std::string alias =
      (fs::path{path}.parent_path() / "." / fs::path{path}.filename()).string();

  MarketSnapshot::reset_open_count();
  SnapshotCache cache;
  auto cold_on_miss =
      cache.load(path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::ReuseOnly);
  ASSERT_TRUE(cold_on_miss.has_value()) << cold_on_miss.error().to_string();
  ASSERT_EQ((*cold_on_miss)->n_surfaces(), 1u);
  EXPECT_EQ((*cold_on_miss)->surface_at(0).query_pricing_tier(),
            QueryPricingTier::ColdReference);
  EXPECT_EQ((*cold_on_miss)->surface_at(0).query_cache_pair_count(), 0u);
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);

  auto same_cold =
      cache.load(alias, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::ReuseOnly);
  ASSERT_TRUE(same_cold.has_value()) << same_cold.error().to_string();
  EXPECT_EQ(same_cold->get(), cold_on_miss->get());
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);

  auto eager_fast =
      cache.load(path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::Eager);
  ASSERT_TRUE(eager_fast.has_value()) << eager_fast.error().to_string();
  ASSERT_EQ((*eager_fast)->n_surfaces(), 1u);
  EXPECT_EQ((*eager_fast)->surface_at(0).query_pricing_tier(),
            QueryPricingTier::RepresentativeFast);
  EXPECT_EQ((*eager_fast)->surface_at(0).query_cache_pair_count(), 1u);
  EXPECT_NE(eager_fast->get(), cold_on_miss->get());
  EXPECT_EQ(MarketSnapshot::open_count(), 2u);

  auto reused_fast =
      cache.load(alias, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::ReuseOnly);
  ASSERT_TRUE(reused_fast.has_value()) << reused_fast.error().to_string();
  EXPECT_EQ(reused_fast->get(), eager_fast->get());
  EXPECT_EQ(MarketSnapshot::open_count(), 2u);
  const SnapshotCacheStats stats = cache.stats();
  EXPECT_EQ(stats.loads, 2u);
  EXPECT_EQ(stats.fast_build_loads, 1u);
  EXPECT_EQ(stats.reuse_only_fast_hits, 1u);
  EXPECT_EQ(stats.reuse_only_cold_resolutions, 2u);
}

TEST(Backtest, ConcurrentReuseOnlyFastMissesCoalesceOnOneColdSnapshot) {
  const fs::path dir = fresh_dir("snapshot-cache-reuse-only-concurrent");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);

  MarketSnapshot::reset_open_count();
  SnapshotCache cache;
  std::vector<std::future<Result<std::shared_ptr<const MarketSnapshot>>>> loads;
  for (int i = 0; i < 8; ++i) {
    loads.push_back(std::async(std::launch::async, [&cache, path] {
      return cache.load(path, QueryPricingTier::RepresentativeFast,
                        QueryCacheBuildPolicy::ReuseOnly);
    }));
  }
  const MarketSnapshot *first = nullptr;
  for (auto &load : loads) {
    auto snapshot = load.get();
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
    first = first == nullptr ? snapshot->get() : first;
    EXPECT_EQ(snapshot->get(), first);
    EXPECT_EQ((*snapshot)->surface_at(0).query_pricing_tier(),
              QueryPricingTier::ColdReference);
  }
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);
  EXPECT_EQ(cache.stats().loads, 1u);
  EXPECT_GE(cache.stats().hits, 7u);
  EXPECT_EQ(cache.stats().fast_build_loads, 0u);
  EXPECT_EQ(cache.stats().reuse_only_fast_hits, 0u);
  EXPECT_EQ(cache.stats().reuse_only_cold_resolutions, 8u);
}

TEST(Backtest, ReuseOnlyFailedFastEntryFallsBackToColdRegardlessOfCacheHistory) {
  const fs::path dir = fresh_dir("snapshot-cache-reuse-only-failed-fast");
  const PricedSurface surface =
      make_surface(kUid, 100.0, 100.0, kBaseNow, 0.0, AmericanMethod::Baw);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);

  MarketSnapshot::reset_open_count();
  SnapshotCache cache;
  const auto eager =
      cache.load(path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::Eager);
  ASSERT_FALSE(eager.has_value());
  EXPECT_EQ(eager.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);

  const auto reuse =
      cache.load(path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::ReuseOnly);
  ASSERT_TRUE(reuse.has_value()) << reuse.error().to_string();
  EXPECT_EQ((*reuse)->surface_at(0).query_pricing_tier(), QueryPricingTier::ColdReference);
  EXPECT_EQ((*reuse)->surface_at(0).query_cache_pair_count(), 0u);
  EXPECT_EQ(MarketSnapshot::open_count(), 2u);

  // Eager retains its original requested-tier error contract even after a cold
  // snapshot has become available through ReuseOnly.
  const auto eager_again =
      cache.load(path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::Eager);
  ASSERT_FALSE(eager_again.has_value());
  EXPECT_EQ(eager_again.error().code(), ErrorCode::InvalidArgument);
  const SnapshotCacheStats stats = cache.stats();
  EXPECT_EQ(stats.loads, 2u);
  EXPECT_EQ(stats.fast_build_loads, 1u);
  EXPECT_EQ(stats.reuse_only_fast_hits, 1u);
  EXPECT_EQ(stats.reuse_only_cold_resolutions, 1u);
}

TEST(Backtest, InvalidQueryCacheBuildPolicyFailsBeforeArchiveOpen) {
  const fs::path dir = fresh_dir("snapshot-cache-invalid-build-policy");
  const PricedSurface surface = make_surface(kUid, 100.0, 100.0, kBaseNow);
  const std::string path = write_one(dir, "2026-08-01", "SPX", surface);
  const auto invalid = static_cast<QueryCacheBuildPolicy>(255u);

  MarketSnapshot::reset_open_count();
  SnapshotCache cache;
  const auto loaded = cache.load(path, QueryPricingTier::RepresentativeFast, invalid);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), ErrorCode::InvalidArgument);
  const Status prefetched = cache.prefetch(path, QueryPricingTier::RepresentativeFast, invalid);
  ASSERT_FALSE(prefetched.has_value());
  EXPECT_EQ(prefetched.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(MarketSnapshot::open_count(), 0u);
  EXPECT_EQ(cache.stats().loads, 0u);
}

TEST(Backtest, RunConfigPropagatesQueryTierThroughPrefetchAndLoad) {
  const fs::path dir = fresh_dir("run-query-tier");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 3);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  RunConfig config;
  config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  config.snapshot_cache = std::make_shared<SnapshotCache>();
  auto result = run_backtest(*clock, PortfolioState{}, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();

  const SnapshotCacheStats after_run = config.snapshot_cache->stats();
  ASSERT_EQ(after_run.loads, static_cast<std::uint64_t>(clock->size()));
  for (const SnapshotRef &ref : clock->refs()) {
    auto snapshot = config.snapshot_cache->load(ref.archive_path, config.query_pricing_tier);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
    EXPECT_EQ((*snapshot)->surface_at(0).query_pricing_tier(),
              QueryPricingTier::RepresentativeFast);
  }
  EXPECT_EQ(config.snapshot_cache->stats().loads, after_run.loads);

  auto legacy = config.snapshot_cache->load(clock->refs().front().archive_path);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  EXPECT_EQ((*legacy)->surface_at(0).query_pricing_tier(), QueryPricingTier::LegacyCompatible);
  EXPECT_EQ(config.snapshot_cache->stats().loads, after_run.loads + 1u);
}

TEST(Backtest, ReuseOnlyRunUsesColdOnMissAndPreparedFastAfterExplicitPreload) {
  const fs::path dir = fresh_dir("run-query-tier-reuse-only");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 3);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 120 * kDayNs;

  RunConfig fresh_config;
  fresh_config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  fresh_config.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
  fresh_config.price.query_execution = QueryExecution::ColdReference;
  fresh_config.snapshot_cache = std::make_shared<SnapshotCache>();
  const auto fresh = run_backtest(*clock, survivor_book(expiry), fresh_config);
  ASSERT_TRUE(fresh.has_value()) << fresh.error().to_string();
  const SnapshotCacheStats fresh_stats = fresh_config.snapshot_cache->stats();
  EXPECT_EQ(fresh_stats.loads, static_cast<std::uint64_t>(clock->size()));
  for (const SnapshotRef &ref : clock->refs()) {
    auto snapshot = fresh_config.snapshot_cache->load(
        ref.archive_path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::ReuseOnly);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
    EXPECT_EQ((*snapshot)->surface_at(0).query_pricing_tier(),
              QueryPricingTier::ColdReference);
    EXPECT_EQ((*snapshot)->surface_at(0).query_cache_pair_count(), 0u);
  }
  EXPECT_EQ(fresh_config.snapshot_cache->stats().loads, fresh_stats.loads);

  RunConfig preloaded_config = fresh_config;
  preloaded_config.snapshot_cache = std::make_shared<SnapshotCache>();
  for (const SnapshotRef &ref : clock->refs()) {
    auto snapshot = preloaded_config.snapshot_cache->load(
        ref.archive_path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::Eager);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  }
  const SnapshotCacheStats preload_stats = preloaded_config.snapshot_cache->stats();
  const auto preloaded = run_backtest(*clock, survivor_book(expiry), preloaded_config);
  ASSERT_TRUE(preloaded.has_value()) << preloaded.error().to_string();
  EXPECT_EQ(preloaded_config.snapshot_cache->stats().loads, preload_stats.loads);
  for (const SnapshotRef &ref : clock->refs()) {
    auto snapshot = preloaded_config.snapshot_cache->load(
        ref.archive_path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::ReuseOnly);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
    EXPECT_EQ((*snapshot)->surface_at(0).query_pricing_tier(),
              QueryPricingTier::RepresentativeFast);
    EXPECT_EQ((*snapshot)->surface_at(0).query_cache_pair_count(), 1u);
  }
  EXPECT_EQ(preloaded_config.snapshot_cache->stats().loads, preload_stats.loads);
  expect_result_bit_identical(*fresh, *preloaded);
}

// WS-ZC regression (the test whose absence let this ship). `run_backtest` used to
// call `snapshot_cache->set_archive_backing(ArchiveBacking::Sealed)` UNCONDITIONALLY,
// including on a cache the CALLER supplied and owns. WS-ZC1 correctly made the
// backing part of the snapshot-cache key, so flipping a live cache to Sealed does not
// re-tune it — it ORPHANS every entry the caller preloaded under the default Mutable
// backing. The run then silently re-loaded all of them, and the follow-up ReuseOnly
// fast request missed and silently resolved down to ColdReference: a quieter pricing
// tier with no error anywhere.
//
// THE INVARIANT: a caller who warms a cache and then runs a backtest keeps BOTH its
// preload (no new loads) and its pricing tier. Note that saving and restoring the
// caller's backing around the run does NOT satisfy this — the run's own loads would
// still be keyed Sealed and would still miss the Mutable preload. Only leaving a
// caller-owned cache alone does. The Sealed win is retained on the cache
// `run_backtest` privately owns, which is asserted separately below.
TEST(Backtest, RunBacktestLeavesCallerOwnedCachePreloadAndBackingIntact) {
  const fs::path dir = fresh_dir("caller-cache-backing-preserved");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 3);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 120 * kDayNs;

  auto cache = std::make_shared<SnapshotCache>();
  ASSERT_EQ(cache->archive_backing(), ArchiveBacking::Mutable);

  // Warm every session at the FAST tier, under the cache's own default backing.
  for (const SnapshotRef &ref : clock->refs()) {
    auto snapshot = cache->load(ref.archive_path, QueryPricingTier::RepresentativeFast,
                                QueryCacheBuildPolicy::Eager);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  }
  const std::uint64_t preload_loads = cache->stats().loads;
  ASSERT_EQ(preload_loads, static_cast<std::uint64_t>(clock->size()));

  RunConfig config;
  config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  config.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
  config.price.query_execution = QueryExecution::ColdReference;
  config.snapshot_cache = cache;

  // BOTH overloads set the backing, so both must leave the caller's cache alone.
  const auto fixed = run_backtest(*clock, survivor_book(expiry), config);
  ASSERT_TRUE(fixed.has_value()) << fixed.error().to_string();
  const auto after_fixed_loads = cache->stats().loads;
  PriceOptionsSpyStrategy strategy;
  const auto declarative = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(declarative.has_value()) << declarative.error().to_string();

  // 1. The runs CONSUMED the preload rather than orphaning it: no new loads at all.
  //    Pre-fix this went 3 -> 6 on the first overload alone.
  EXPECT_EQ(after_fixed_loads, preload_loads);
  EXPECT_EQ(cache->stats().loads, preload_loads);
  // 2. Neither run retargeted the caller's cache.
  EXPECT_EQ(cache->archive_backing(), ArchiveBacking::Mutable);
  // 3. The preloaded FAST tier is still what the cache serves — no silent
  //    RepresentativeFast(02) -> ColdReference(01) downgrade, and the query
  //    accelerator that makes it the fast tier is still attached (pair count 1 -> 0
  //    was the pre-fix symptom).
  for (const SnapshotRef &ref : clock->refs()) {
    auto snapshot = cache->load(ref.archive_path, QueryPricingTier::RepresentativeFast,
                                QueryCacheBuildPolicy::ReuseOnly);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
    EXPECT_EQ((*snapshot)->surface_at(0).query_pricing_tier(),
              QueryPricingTier::RepresentativeFast);
    EXPECT_GT((*snapshot)->surface_at(0).query_cache_pair_count(), 0u);
    // A fast tier reconstructs OWNED surfaces; a Sealed-orphaned reload would have
    // come back as a borrowed cold view instead.
    EXPECT_FALSE((*snapshot)->borrows_views());
  }
  EXPECT_EQ(cache->stats().loads, preload_loads);
}

// The other half of the contract: when NO cache is supplied, `run_backtest` builds a
// private one and DOES seal it — that private replay path is the entire point of
// WS-ZC1 (snapshot_load ~1008 -> ~44 ms). Fixing the caller-owned-cache bug must not
// be done by abandoning the optimization, so pin that the sealed private path still
// produces bit-identical results to an explicitly Mutable caller-supplied cache.
TEST(Backtest, PrivateSnapshotCacheStillSealsAndMatchesMutableCallerCacheBitForBit) {
  const fs::path dir = fresh_dir("private-cache-sealed");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 3);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 120 * kDayNs;

  // No snapshot_cache => private, sealed.
  RunConfig private_config;
  const auto sealed_run = run_backtest(*clock, survivor_book(expiry), private_config);
  ASSERT_TRUE(sealed_run.has_value()) << sealed_run.error().to_string();

  // An explicitly Mutable caller-supplied cache takes the copied backing instead.
  RunConfig mutable_config;
  mutable_config.snapshot_cache = std::make_shared<SnapshotCache>(ArchiveBacking::Mutable);
  const auto mutable_run = run_backtest(*clock, survivor_book(expiry), mutable_config);
  ASSERT_TRUE(mutable_run.has_value()) << mutable_run.error().to_string();
  EXPECT_EQ(mutable_config.snapshot_cache->archive_backing(), ArchiveBacking::Mutable);

  // The backing changes only HOW the borrowed bytes are obtained, never the numbers.
  expect_result_bit_identical(*sealed_run, *mutable_run);
}

TEST(Backtest, ReuseOnlyFastConfiguredEconomicsRejectsAllResidencyBeforeIoInBothOverloads) {
  const fs::path dir = fresh_dir("run-query-tier-reuse-only-route-gate");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 3);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 120 * kDayNs;

  const auto expect_rejected = [&clock](const std::shared_ptr<SnapshotCache> &cache,
                                        QueryPricingTier tier, const char *residency) {
    SCOPED_TRACE(residency);
    RunConfig config;
    config.query_pricing_tier = tier;
    config.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
    config.price.query_execution = QueryExecution::Configured;
    config.snapshot_cache = cache;
    const std::uint64_t loads_before = cache->stats().loads;

    MarketSnapshot::reset_open_count();
    const auto fixed = run_backtest(*clock, survivor_book(expiry), config);
    ASSERT_FALSE(fixed.has_value());
    EXPECT_EQ(fixed.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(fixed.error().message().find("ReuseOnly"), std::string::npos);
    EXPECT_EQ(MarketSnapshot::open_count(), 0u);
    EXPECT_EQ(cache->stats().loads, loads_before);

    MarketSnapshot::reset_open_count();
    PriceOptionsSpyStrategy strategy;
    const auto declarative = run_backtest(*clock, strategy, config);
    ASSERT_FALSE(declarative.has_value());
    EXPECT_EQ(declarative.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(declarative.error().message().find("ReuseOnly"), std::string::npos);
    EXPECT_EQ(MarketSnapshot::open_count(), 0u);
    EXPECT_EQ(cache->stats().loads, loads_before);
  };

  expect_rejected(std::make_shared<SnapshotCache>(), QueryPricingTier::RepresentativeFast,
                  "fresh-representative-fast");
  expect_rejected(std::make_shared<SnapshotCache>(), QueryPricingTier::CarryBank,
                  "fresh-carry-bank");

  auto full = std::make_shared<SnapshotCache>();
  for (const SnapshotRef &ref : clock->refs()) {
    auto loaded = full->load(ref.archive_path, QueryPricingTier::RepresentativeFast,
                             QueryCacheBuildPolicy::Eager);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  }
  expect_rejected(full, QueryPricingTier::RepresentativeFast, "full-fast-residency");

  auto partial = std::make_shared<SnapshotCache>();
  auto loaded = partial->load(clock->refs().front().archive_path,
                              QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::Eager);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  expect_rejected(partial, QueryPricingTier::RepresentativeFast, "partial-fast-residency");
}

TEST(Backtest, ReuseOnlyConfiguredEconomicsAllowsColdAndLegacyRequestedTiers) {
  const fs::path dir = fresh_dir("run-query-tier-reuse-only-cold-compatible");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 2);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  for (const QueryPricingTier tier :
       {QueryPricingTier::ColdReference, QueryPricingTier::LegacyCompatible}) {
    SCOPED_TRACE(static_cast<unsigned>(tier));
    RunConfig config;
    config.query_pricing_tier = tier;
    config.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
    config.price.query_execution = QueryExecution::Configured;
    config.snapshot_cache = std::make_shared<SnapshotCache>();
    const auto fixed = run_backtest(*clock, PortfolioState{}, config);
    ASSERT_TRUE(fixed.has_value()) << fixed.error().to_string();
    PriceOptionsSpyStrategy strategy;
    const auto declarative = run_backtest(*clock, strategy, config);
    ASSERT_TRUE(declarative.has_value()) << declarative.error().to_string();
  }
}

TEST(Backtest, AdaptiveStrategyRejectsFastEconomicsUnlessColdIsExplicit) {
  const fs::path dir = fresh_dir("adaptive-economic-route");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 2);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.resolution.fast_screen_cold_confirm = true;
  DeclarativeStrategy strategy{spec};
  RunConfig config;
  config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  config.prefetch_snapshots = false;

  const auto rejected = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(rejected.error().message().find("requires ColdReference economics"), std::string::npos);

  config.price.query_execution = QueryExecution::ColdReference;
  const auto accepted = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(accepted.has_value()) << accepted.error().to_string();

  config.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
  config.snapshot_cache = std::make_shared<SnapshotCache>();
  const auto reuse_only = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(reuse_only.has_value()) << reuse_only.error().to_string();
  auto actual =
      config.snapshot_cache->load(clock->refs().front().archive_path, config.query_pricing_tier,
                                  QueryCacheBuildPolicy::ReuseOnly);
  ASSERT_TRUE(actual.has_value()) << actual.error().to_string();
  EXPECT_EQ((*actual)->surface_at(0).query_pricing_tier(), QueryPricingTier::ColdReference);
  EXPECT_EQ((*actual)->surface_at(0).query_cache_pair_count(), 0u);
}

TEST(Backtest, AdaptiveStrategyHandlesFreshFullAndPartialFastResidency) {
  const fs::path dir = fresh_dir("adaptive-cache-residency");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 3);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.name = "adaptive-residency";
  spec.resolution.fast_screen_cold_confirm = true;
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.50;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::FixedContracts, 1.0, -1.0};
  spec.legs.push_back(leg);

  const auto make_config = [](std::shared_ptr<SnapshotCache> cache) {
    RunConfig config;
    config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
    config.query_cache_build_policy = QueryCacheBuildPolicy::ReuseOnly;
    config.price.query_execution = QueryExecution::ColdReference;
    config.snapshot_cache = std::move(cache);
    return config;
  };

  RunConfig fresh_config = make_config(std::make_shared<SnapshotCache>());
  DeclarativeStrategy fresh_strategy{spec};
  const auto fresh = run_backtest(*clock, fresh_strategy, fresh_config);
  ASSERT_TRUE(fresh.has_value()) << fresh.error().to_string();
  const SnapshotCacheStats fresh_stats = fresh_config.snapshot_cache->stats();
  EXPECT_EQ(fresh_stats.fast_build_loads, 0u);
  EXPECT_EQ(fresh_stats.reuse_only_fast_hits, 0u);
  EXPECT_GT(fresh_stats.reuse_only_cold_resolutions, 0u);

  RunConfig full_config = make_config(std::make_shared<SnapshotCache>());
  for (const SnapshotRef &ref : clock->refs()) {
    auto snapshot = full_config.snapshot_cache->load(
        ref.archive_path, QueryPricingTier::RepresentativeFast, QueryCacheBuildPolicy::Eager);
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  }
  const std::uint64_t full_preload_count = full_config.snapshot_cache->stats().loads;
  DeclarativeStrategy full_strategy{spec};
  const auto full = run_backtest(*clock, full_strategy, full_config);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();
  const SnapshotCacheStats full_stats = full_config.snapshot_cache->stats();
  EXPECT_EQ(full_stats.loads, full_preload_count);
  EXPECT_EQ(full_stats.fast_build_loads, static_cast<std::uint64_t>(clock->size()));
  EXPECT_GT(full_stats.reuse_only_fast_hits, 0u);
  EXPECT_EQ(full_stats.reuse_only_cold_resolutions, 0u);

  RunConfig partial_config = make_config(std::make_shared<SnapshotCache>());
  auto first_fast = partial_config.snapshot_cache->load(clock->refs().front().archive_path,
                                                        QueryPricingTier::RepresentativeFast,
                                                        QueryCacheBuildPolicy::Eager);
  ASSERT_TRUE(first_fast.has_value()) << first_fast.error().to_string();
  DeclarativeStrategy partial_strategy{spec};
  const auto partial = run_backtest(*clock, partial_strategy, partial_config);
  ASSERT_TRUE(partial.has_value()) << partial.error().to_string();
  const SnapshotCacheStats partial_stats = partial_config.snapshot_cache->stats();
  EXPECT_EQ(partial_stats.fast_build_loads, 1u);
  EXPECT_GT(partial_stats.reuse_only_fast_hits, 0u);
  EXPECT_GT(partial_stats.reuse_only_cold_resolutions, 0u);

  ASSERT_EQ(fresh->size(), full->size());
  ASSERT_EQ(fresh->size(), partial->size());
  for (std::size_t i = 0; i < fresh->size(); ++i) {
    EXPECT_EQ(fresh->n_open_lots[i], full->n_open_lots[i]) << i;
    EXPECT_EQ(fresh->n_open_lots[i], partial->n_open_lots[i]) << i;
    EXPECT_NEAR(fresh->pnl_total[i], full->pnl_total[i], 1.0) << i;
    EXPECT_NEAR(fresh->pnl_total[i], partial->pnl_total[i], 1.0) << i;
    EXPECT_NEAR(fresh->nav[i], full->nav[i], 2.0) << i;
    EXPECT_NEAR(fresh->nav[i], partial->nav[i], 2.0) << i;
  }

  const auto validate_cold_residuals = [&spec, &clock](const RunConfig &config) {
    for (const SnapshotRef &ref : clock->refs()) {
      auto snapshot = config.snapshot_cache->load(ref.archive_path, config.query_pricing_tier,
                                                  QueryCacheBuildPolicy::ReuseOnly);
      ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
      const auto resolved = resolve_spec(**snapshot, spec);
      ASSERT_TRUE(resolved.has_value()) << resolved.error().to_string();
      for (const SizedLeg &resolved_leg : *resolved) {
        const SurfaceRef surface = (*snapshot)->find(resolved_leg.leg.uid);
        ASSERT_NE(surface, nullptr);
        const auto delta = surface->delta(resolved_leg.leg.K, resolved_leg.leg.T,
                                          resolved_leg.leg.side, QueryExecution::ColdReference);
        ASSERT_TRUE(delta.has_value()) << delta.error().to_string();
        EXPECT_LE(std::fabs(std::fabs(*delta) - 0.40), spec.resolution.cold_delta_tolerance);
      }
    }
  };
  validate_cold_residuals(fresh_config);
  validate_cold_residuals(full_config);
  validate_cold_residuals(partial_config);
}

TEST(Backtest, BoundedSnapshotCacheCoalescesAndRetainsOnlyWorkingSet) {
  constexpr int kDates = 12;
  constexpr std::size_t kCapacity = 3u;
  const fs::path dir = fresh_dir("snapshot-cache-bounded");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", kDates);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::span<const SnapshotRef> refs = clock->refs();

  SnapshotCache cache{kCapacity};
  MarketSnapshot::reset_open_count();

  // Exercise duplicate synchronous callers against the same prefetched future.
  cache.prefetch(refs[0].archive_path);
  std::vector<std::future<Result<std::shared_ptr<const MarketSnapshot>>>> duplicate_loads;
  for (int i = 0; i < 8; ++i) {
    duplicate_loads.push_back(std::async(
        std::launch::async, [&cache, path = refs[0].archive_path] { return cache.load(path); }));
  }
  const MarketSnapshot *first = nullptr;
  for (auto &pending : duplicate_loads) {
    auto loaded = pending.get();
    ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
    first = first == nullptr ? loaded->get() : first;
    EXPECT_EQ(loaded->get(), first);
  }

  // A long one-pass traversal must not retain the entire corpus.
  for (std::size_t i = 1; i < refs.size(); ++i) {
    auto loaded = cache.load(refs[i].archive_path);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  }
  const SnapshotCacheStats stats = cache.stats();
  EXPECT_EQ(MarketSnapshot::open_count(), static_cast<std::uint64_t>(kDates));
  EXPECT_EQ(stats.loads, static_cast<std::uint64_t>(kDates));
  EXPECT_GE(stats.hits, 8u);
  EXPECT_EQ(stats.retained_entries, kCapacity);
  EXPECT_EQ(stats.evictions, static_cast<std::uint64_t>(kDates) - kCapacity);
}

TEST(Backtest, PrivateBoundedCachePreservesUnboundedSweepOutput) {
  constexpr int kDates = 10;
  const fs::path dir = fresh_dir("snapshot-cache-private-bounded");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", kDates);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 120 * kDayNs;

  auto private_bounded = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(private_bounded.has_value()) << private_bounded.error().to_string();

  RunConfig reusable_config;
  reusable_config.snapshot_cache = std::make_shared<SnapshotCache>();
  auto reusable_unbounded = run_backtest(*clock, survivor_book(expiry), reusable_config);
  ASSERT_TRUE(reusable_unbounded.has_value()) << reusable_unbounded.error().to_string();
  expect_result_bit_identical(*private_bounded, *reusable_unbounded);

  const SnapshotCacheStats stats = reusable_config.snapshot_cache->stats();
  EXPECT_EQ(stats.retained_entries, static_cast<std::uint64_t>(kDates));
  EXPECT_EQ(stats.evictions, 0u);
}

// ── 2a. Aging: spot-only step reconstructs to a tiny residual ───────────────
TEST(Backtest, AgingSpotOnly) {
  const fs::path dir = fresh_dir("spot");
  const std::int64_t now = kBaseNow; // identical valuation time both dates
  const double S0 = 100.0;
  const double ratio = 1.004; // bump spot AND forward by the same ratio
  const PricedSurface d0 = make_surface(kUid, S0, S0, now);
  const PricedSurface d1 = make_surface(kUid, S0 * ratio, S0 * ratio, now);
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-08-02", "SPX", d1);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", p0}, {"2026-08-02", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), 2u);

  const double total = res->pnl_total[1];
  const double unexpl = res->pnl_unexplained[1];
  EXPECT_EQ(res->pnl_settlement[1], 0.0);
  EXPECT_GT(std::fabs(total), 0.0);
  EXPECT_LE(std::fabs(unexpl), 1e-3 * std::fabs(total) + 1e-6)
      << "unexplained=" << unexpl << " total=" << total;
}

// ── 2b. Aging: time-only step isolates to theta ─────────────────────────────
TEST(Backtest, AgingTimeOnly) {
  const fs::path dir = fresh_dir("time");
  const double S0 = 100.0;
  const PricedSurface d0 = make_surface(kUid, S0, S0, kBaseNow);
  const PricedSurface d1 = make_surface(kUid, S0, S0, kBaseNow + kDayNs); // +1 day only
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-08-02", "SPX", d1);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", p0}, {"2026-08-02", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), 2u);

  // Pure time move: only theta lights up (dS = dvol = dr = 0 exactly).
  EXPECT_EQ(res->pnl_delta[1], 0.0);
  EXPECT_EQ(res->pnl_gamma[1], 0.0);
  EXPECT_EQ(res->pnl_vega[1], 0.0);
  EXPECT_EQ(res->pnl_vanna[1], 0.0);
  EXPECT_EQ(res->pnl_volga[1], 0.0);
  EXPECT_EQ(res->pnl_rho[1], 0.0);
  EXPECT_EQ(res->pnl_charm[1], 0.0);
  EXPECT_NE(res->pnl_theta[1], 0.0);
}

// ── 3. Attribution closes each step ─────────────────────────────────────────
TEST(Backtest, AttributionCloses) {
  const fs::path dir = fresh_dir("attrib");
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", 4);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  auto res = run_backtest(*clock, survivor_book(expiry));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult &r = *res;
  ASSERT_GT(r.size(), 1u);

  for (std::size_t i = 0; i < r.size(); ++i) {
    const double axes = r.pnl_delta[i] + r.pnl_gamma[i] + r.pnl_vega[i] + r.pnl_vanna[i] +
                        r.pnl_volga[i] + r.pnl_theta[i] + r.pnl_rho[i] + r.pnl_charm[i] +
                        r.pnl_unexplained[i];
    const double nonsettle = r.pnl_total[i] - r.pnl_settlement[i];
    EXPECT_NEAR(axes, nonsettle, 1e-9 * (std::fabs(nonsettle) + 1.0)) << "row " << i;
  }
}

// ── 4. Determinism across thread counts ─────────────────────────────────────
TEST(Backtest, Determinism) {
  const fs::path dir = fresh_dir("determinism");
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", 4);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  RunConfig cfg1;
  cfg1.price.n_threads = 1;
  RunConfig cfg4;
  cfg4.price.n_threads = 4;

  auto r1 = run_backtest(*clock, survivor_book(expiry), cfg1);
  auto r4 = run_backtest(*clock, survivor_book(expiry), cfg4);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();
  expect_result_bit_identical(*r1, *r4);
}

// ── 5. Storage granularity ──────────────────────────────────────────────────
TEST(Backtest, Granularity) {
  const fs::path dir = fresh_dir("granularity");
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", 7);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs;
  RunConfig fine;
  fine.record_every_n = 1;
  RunConfig coarse;
  coarse.record_every_n = 2;

  auto rf = run_backtest(*clock, survivor_book(expiry), fine);
  auto rc = run_backtest(*clock, survivor_book(expiry), coarse);
  ASSERT_TRUE(rf.has_value()) << rf.error().to_string();
  ASSERT_TRUE(rc.has_value()) << rc.error().to_string();
  EXPECT_LT(rc->size(), rf->size()); // coarse genuinely downsampled

  // The FULL-RESOLUTION per-step series is retained at any stride and is bit-for-bit
  // identical between the fine and coarse runs — this is what makes the downsampled
  // risk metrics stride-invariant.
  ASSERT_EQ(rc->step_pnl_total.size(), rf->step_pnl_total.size());
  for (std::size_t k = 0; k < rf->step_pnl_total.size(); ++k) {
    EXPECT_TRUE(bits_equal(rc->step_pnl_total[k], rf->step_pnl_total[k])) << k;
  }

  const auto col_sum = [](const std::vector<double> &v) {
    double s = 0.0;
    for (const double x : v) {
      s += x;
    }
    return s;
  };
  // Column TOTALS survive downsampling: block-summing the skipped steps into the
  // recorded rows keeps Σ column == the fine run's Σ (attribution totals exact).
  const double tol = 1e-9;
  EXPECT_NEAR(col_sum(rc->pnl_total), col_sum(rf->pnl_total),
              tol * (std::fabs(col_sum(rf->pnl_total)) + 1.0));
  EXPECT_NEAR(col_sum(rc->pnl_delta), col_sum(rf->pnl_delta),
              tol * (std::fabs(col_sum(rf->pnl_delta)) + 1.0));
  EXPECT_NEAR(col_sum(rc->pnl_theta), col_sum(rf->pnl_theta),
              tol * (std::fabs(col_sum(rf->pnl_theta)) + 1.0));
  EXPECT_NEAR(col_sum(rc->pnl_settlement), col_sum(rf->pnl_settlement),
              tol * (std::fabs(col_sum(rf->pnl_settlement)) + 1.0));
  // Σ recorded pnl_total reconstructs the final NAV at either stride.
  EXPECT_NEAR(col_sum(rc->pnl_total), rf->nav.back(),
              tol * (std::fabs(rf->nav.back()) + 1.0));

  // Every coarse row lands on a fine timestamp; its STATE columns match that fine
  // row bit-for-bit, and its block-summed pnl_total equals the fine NAV increment
  // over the block since the previous recorded row.
  std::size_t prev_fi = 0;
  for (std::size_t j = 0; j < rc->size(); ++j) {
    std::size_t fi = rf->size();
    for (std::size_t k = 0; k < rf->size(); ++k) {
      if (rf->ts_ns[k] == rc->ts_ns[j]) {
        fi = k;
        break;
      }
    }
    ASSERT_LT(fi, rf->size()) << "no fine row at coarse ts row " << j;
    EXPECT_TRUE(bits_equal(rc->nav[j], rf->nav[fi])) << j;              // cumulative state
    EXPECT_TRUE(bits_equal(rc->gross_vega[j], rf->gross_vega[fi])) << j; // recorded-row state
    if (j > 0) {
      const double block = rf->nav[fi] - rf->nav[prev_fi];
      EXPECT_NEAR(rc->pnl_total[j], block, tol * (std::fabs(block) + 1.0)) << j;
    }
    prev_fi = fi;
  }
}

// ── 6. Expiry settlement ────────────────────────────────────────────────────
TEST(Backtest, ExpirySettlement) {
  const fs::path dir = fresh_dir("expiry");
  const std::int64_t now0 = kBaseNow;
  const std::int64_t now1 = kBaseNow + 30 * kDayNs;
  const std::int64_t exp_expiring = now1;                    // exact settlement observation
  const std::int64_t exp_survivor = kBaseNow + 200 * kDayNs; // survives

  const PricedSurface d0 = make_surface(kUid, 100.0, 100.0, now0);
  const PricedSurface d1 = make_surface(kUid, 103.0, 103.0, now1); // spot up to 103
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-09-15", "SPX", d1);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", p0}, {"2026-09-15", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  PortfolioState st;
  // Expiring long call K=95 (settles ITM at S=103) + surviving short put K=100.
  st.lots.push_back(
      Lot{1, OptionContract{kUid, 95.0, 0.0, Side::Call}, +5.0, 100.0, exp_expiring, 0, 0.0});
  st.lots.push_back(
      Lot{2, OptionContract{kUid, 100.0, 0.0, Side::Put}, -2.0, 100.0, exp_survivor, 0, 0.0});

  auto res = run_backtest(*clock, std::move(st));
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult &r = *res;
  ASSERT_EQ(r.size(), 2u);

  // Hand-compute the settlement: intrinsic(S_shifted) - base_mark on d0.
  const double T_base =
      (static_cast<double>(exp_expiring) - static_cast<double>(now0)) / kNsPerYear;
  auto mark = d0.fair_value(95.0, T_base, Side::Call);
  ASSERT_TRUE(mark.has_value()) << mark.error().to_string();
  const double intrinsic = std::max(0.0, 103.0 - 95.0);
  const double expected_settle = 5.0 * 100.0 * (intrinsic - *mark);

  EXPECT_NE(r.pnl_settlement[1], 0.0);
  EXPECT_NEAR(r.pnl_settlement[1], expected_settle, 1e-6 * (std::fabs(expected_settle) + 1.0));
  EXPECT_EQ(r.n_open_lots[0], 2.0); // inception: both lots open
  EXPECT_EQ(r.n_open_lots[1], 1.0); // after settlement: survivor only

  std::printf("[backtest] settlement=%.4f (intrinsic=%.2f base_mark=%.4f) survivors=%.0f\n",
              r.pnl_settlement[1], intrinsic, *mark, r.n_open_lots[1]);
}

TEST(Backtest, ForcedColdEconomicsOnFastSnapshotsMatchColdPreparedSettlementRun) {
  const fs::path dir = fresh_dir("forced-cold-settlement");
  const std::int64_t now0 = kBaseNow;
  const std::int64_t now1 = kBaseNow + 30 * kDayNs;
  const std::int64_t exp_expiring = now1;
  const std::int64_t exp_survivor = kBaseNow + 200 * kDayNs;
  const PricedSurface d0 = make_surface(kUid, 100.0, 100.0, now0);
  const PricedSurface d1 = make_surface(kUid, 103.0, 103.0, now1);
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-09-15", "SPX", d1);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", p0}, {"2026-09-15", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const auto initial = [=] {
    PortfolioState state;
    state.lots.push_back(
        Lot{1, OptionContract{kUid, 95.0, 0.0, Side::Call}, +5.0, 100.0, exp_expiring, 0, 0.0});
    state.lots.push_back(
        Lot{2, OptionContract{kUid, 100.0, 0.0, Side::Put}, -2.0, 100.0, exp_survivor, 0, 0.0});
    return state;
  };

  RunConfig fast_config;
  fast_config.price.n_threads = 1u;
  fast_config.price.query_execution = QueryExecution::ColdReference;
  fast_config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  fast_config.prefetch_snapshots = false;
  RunConfig cold_config;
  cold_config.price.n_threads = 1u;
  cold_config.price.query_execution = QueryExecution::Configured;
  cold_config.query_pricing_tier = QueryPricingTier::ColdReference;
  cold_config.prefetch_snapshots = false;

  const auto fast_result = run_backtest(*clock, initial(), fast_config);
  const auto cold_result = run_backtest(*clock, initial(), cold_config);
  ASSERT_TRUE(fast_result.has_value()) << fast_result.error().to_string();
  ASSERT_TRUE(cold_result.has_value()) << cold_result.error().to_string();
  ASSERT_EQ(fast_result->size(), 2u);
  ASSERT_NE(fast_result->pnl_settlement[1], 0.0);
  expect_result_bit_identical(*fast_result, *cold_result);
}

TEST(Backtest, ForcedColdEconomicsOnFastSnapshotsMatchColdPreparedRollClose) {
  const fs::path dir = fresh_dir("forced-cold-roll-close");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 2);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 200 * kDayNs;

  RunConfig fast_config;
  fast_config.price.n_threads = 1u;
  fast_config.price.query_execution = QueryExecution::ColdReference;
  fast_config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  fast_config.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  fast_config.frictions.half_spread_bps = 100.0;
  fast_config.prefetch_snapshots = false;
  RunConfig cold_config = fast_config;
  cold_config.price.query_execution = QueryExecution::Configured;
  cold_config.query_pricing_tier = QueryPricingTier::ColdReference;

  OpenThenCloseStrategy fast_strategy{expiry};
  OpenThenCloseStrategy cold_strategy{expiry};
  const auto fast_result = run_backtest(*clock, fast_strategy, fast_config);
  const auto cold_result = run_backtest(*clock, cold_strategy, cold_config);
  ASSERT_TRUE(fast_result.has_value()) << fast_result.error().to_string();
  ASSERT_TRUE(cold_result.has_value()) << cold_result.error().to_string();
  ASSERT_EQ(fast_result->size(), 2u);
  ASSERT_EQ(cold_result->size(), fast_result->size());
  expect_result_bit_identical(*fast_result, *cold_result);
  for (std::size_t i = 0u; i < fast_result->size(); ++i) {
    EXPECT_TRUE(bits_equal(fast_result->cost[i], cold_result->cost[i])) << i;
    EXPECT_TRUE(bits_equal(fast_result->cash[i], cold_result->cash[i])) << i;
    EXPECT_TRUE(bits_equal(fast_result->turnover_notional[i], cold_result->turnover_notional[i]))
        << i;
    EXPECT_TRUE(bits_equal(fast_result->turnover_vega[i], cold_result->turnover_vega[i])) << i;
  }
  EXPECT_GT(fast_result->cost[1], 0.0) << "step 1 must exercise the roll-close mark";
  EXPECT_EQ(fast_result->n_open_lots[1], 0.0);
}

TEST(Backtest, DailyTwoLegRollReusesExactPnlTargetMarksWithoutChangingEconomics) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("daily-roll-target-marks");
  const CorpusManifest manifest = make_evolving_corpus(dir, "SPX", 4);
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  DeclarativeStrategy strategy{daily_two_leg_roll_spec()};
  RunConfig config;
  config.price.n_threads = 1u;
  config.price.analytic_greeks = true;
  config.prefetch_snapshots = false;
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_EQ(result->size(), 4u);
  EXPECT_EQ(result->n_open_lots, (std::vector<double>{2.0, 2.0, 2.0, 2.0}));
  // Bit pins captured before the target-mark handoff. Close cash and notional
  // remain numerically identical while their redundant Greek solves disappear.
  // infra / test-tolerance: the running `cash` accumulator sums sub-ULP drift over the
  // run's surface solves. Gate is golden_isa_accum_tol — one unconditional 1e-11
  // relative band (WS-P1a re-band; no __FMA__ branch; see support/isa_golden_tol.hpp).
  // [WS-M M1] These pins are MAIN's (pricing-greeks SOTA) captured values: the merged
  // American greeks kernel is main's rewrite (incl. the A1 BAW critical-price seed-sign
  // fix), so the marks reproduce main's numbers, not feat/disp-hotpath's pre-rewrite
  // pins (which miss by ~3.6e-6). Observed merged-engine drift vs pin ~2.3e-13 rel
  // (cost bit-identical), ~40x inside the band. (M2 owns any formal quiet-window re-pin.)
  constexpr std::array<double, 4> expected_cash{-3.0734592640139908, -3.5489832955311158,
                                                -4.0009154520034826, -4.4307759090513628};
  constexpr std::array<double, 4> expected_turnover{2200.5417344553625, 4444.318791516017,
                                                    4493.5146508026201, 4543.6617284600488};
  for (std::size_t i = 0; i < result->size(); ++i) {
    EXPECT_NEAR(result->cash[i], expected_cash[i],
                atx::vol::test::golden_isa_accum_tol(expected_cash[i]))
        << i;
    EXPECT_NEAR(result->turnover_notional[i], expected_turnover[i],
                atx::vol::test::golden_isa_accum_tol(expected_turnover[i]))
        << i;
  }
  EXPECT_TRUE(std::isfinite(result->turnover_vega[1]));
  EXPECT_GT(result->turnover_vega[1], 0.0);
  if constexpr (counters_enabled()) {
    const auto measured = atx::vol::counters::snapshot();
    // Inception resolves two entry bundles (10 solves). Each later date pays
    // two target marks in P&L plus two new entry bundles: 12, never the old 22.
    EXPECT_EQ(measured.get(Counter::BoundarySolves), 10u + 3u * 12u);
    EXPECT_EQ(measured.get(Counter::SurfaceFullGreekRoutes), 4u * 2u);
  }
}

TEST(Backtest, PriceBpsRollCloseReusesPnlMarkWithoutASecondSurfaceSolve) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("price-bps-target-marks");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 2));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  DeclarativeStrategy strategy{daily_two_leg_roll_spec()};
  RunConfig config;
  config.price.n_threads = 1u;
  config.price.analytic_greeks = true;
  config.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  config.frictions.half_spread_bps = 100.0;
  config.prefetch_snapshots = false;
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_EQ(result->size(), 2u);
  // infra / test-tolerance: same unconditional 1e-11 relative band as the sibling
  // above. [WS-M M1] MAIN's captured values (merged greeks kernel = main's rewrite):
  // cost[1] and turnover_notional[1] reproduce bit-identically, cash[1] ~2e-13 rel.
  EXPECT_NEAR(result->cost[1], 44.443187915160173,
              atx::vol::test::golden_isa_accum_tol(44.443187915160173));
  EXPECT_NEAR(result->cash[1], -69.997588555245017,
              atx::vol::test::golden_isa_accum_tol(-69.997588555245017));
  EXPECT_NEAR(result->turnover_notional[1], 4444.318791516017,
              atx::vol::test::golden_isa_accum_tol(4444.318791516017));
  EXPECT_TRUE(std::isfinite(result->turnover_vega[1]));
  EXPECT_GT(result->turnover_vega[1], 0.0);
  if constexpr (counters_enabled()) {
    const auto measured = atx::vol::counters::snapshot();
    EXPECT_EQ(measured.get(Counter::BoundarySolves), 10u + 12u);
    EXPECT_EQ(measured.get(Counter::SurfaceFullGreekRoutes), 4u);
  }
}

TEST(Backtest, VolTicksRollCloseUsesConfiguredFdOrAnalyticGreekRoute) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("vol-ticks-target-marks");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 2));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  auto cache = std::make_shared<SnapshotCache>();
  for (const SnapshotRef &ref : clock->refs()) {
    const auto loaded = cache->load(ref.archive_path, QueryPricingTier::RepresentativeFast,
                                    QueryCacheBuildPolicy::Eager);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  }
  const auto run = [&](bool analytic) {
    DeclarativeStrategy strategy{daily_two_leg_roll_spec()};
    RunConfig config;
    config.price.n_threads = 1u;
    config.price.analytic_greeks = analytic;
    config.price.query_execution = QueryExecution::ColdReference;
    config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
    config.snapshot_cache = cache;
    config.frictions.spread_kind = FrictionModel::SpreadKind::VolTicks;
    config.frictions.vol_tick = 0.01;
    config.prefetch_snapshots = false;
    if constexpr (counters_enabled()) {
      atx::vol::counters::reset();
    }
    auto result = run_backtest(*clock, strategy, config);
    const auto measured = atx::vol::counters::snapshot();
    return std::pair{std::move(result), measured};
  };

  auto [fd, fd_counts] = run(false);
  auto [analytic, analytic_counts] = run(true);
  ASSERT_TRUE(fd.has_value()) << fd.error().to_string();
  ASSERT_TRUE(analytic.has_value()) << analytic.error().to_string();
  // Route parity, not a value pin: the configured greek route must not change the
  // economics. Since WS-P1a the ANALYTIC route runs the laned AVX2 greeks kernel
  // while the FD route stays scalar, so the two now agree to the documented
  // economic band rather than to the bit. Measured here (dev preset): cost and
  // turnover_notional are exactly equal, cash differs by 6.3664629124104977e-12
  // on -69.36 (9.18e-14 relative). See support/isa_golden_tol.hpp.
  using atx::vol::test::laned_greeks_close;
  EXPECT_TRUE(laned_greeks_close(fd->cost[1], analytic->cost[1]));
  EXPECT_TRUE(laned_greeks_close(fd->cash[1], analytic->cash[1]));
  EXPECT_TRUE(laned_greeks_close(fd->turnover_notional[1], analytic->turnover_notional[1]));
  EXPECT_TRUE(laned_greeks_close(fd->turnover_vega[1], analytic->turnover_vega[1]));
  if constexpr (counters_enabled()) {
    // FD: 14 inception + (2 target + 14 close + 14 new-entry) = 44.
    EXPECT_EQ(fd_counts.get(Counter::BoundarySolves), 44u);
    // Analytic: 10 inception + (2 target + 10 close + 10 new-entry) = 32.
    EXPECT_EQ(analytic_counts.get(Counter::BoundarySolves), 32u);
    EXPECT_EQ(fd_counts.get(Counter::SurfaceFullGreekRoutes), 4u);
    EXPECT_EQ(analytic_counts.get(Counter::SurfaceFullGreekRoutes), 4u);
  }
}

// REVIEW C-4 — a configured vol-tick spread must SURVIVE an active impact model.
//
// The contract this pins is the one the headers already state:
//   * dispersion_backtest.hpp:40-45 — "signed half-spread PLUS square-root impact,
//     both per share", i.e. `mid + direction * (half_spread + impact)`;
//   * dispersion_run.hpp:250-254 — FrictionedWithImpact is "THE ABOVE PLUS impact".
//
// The oracle is ADDITIVITY OF THE CHARGED COST, not the shape of the folded
// `FrictionModel`: every friction here is charged on the same |qty| * multiplier,
// and all three runs take the identical VolTicks execution path, so
//
//     cost(spread + impact) == cost(spread only) + cost(impact only)
//
// exactly, up to the reassociation of a single add. That makes this an
// independent oracle — it never repeats the production expression.
//
// Before the fix `dispersion_effective_frictions` rewrote ANY active-impact
// configuration to `SpreadKind::PriceBps`, so the third run charged impact ONLY
// and this failed by exactly `spread_only->cost[1]` (the whole vol-tick spread).
TEST(Backtest, C4_ImpactIsChargedOnTopOfAVolTickSpreadNotInsteadOfIt) {
  const fs::path dir = fresh_dir("c4-vol-ticks-plus-impact");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 2));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionCostModel costs;
  costs.k = 0.02;
  costs.beta = 0.6;
  costs.adv_fraction = 0.04;
  ASSERT_TRUE(costs.active());
  const DispersionCostModel inert; // k == 0 => inactive, the identity fold

  const auto run_with = [&](double vol_tick, const DispersionCostModel &model) {
    DeclarativeStrategy strategy{daily_two_leg_roll_spec()};
    RunConfig config;
    config.price.n_threads = 1u;
    config.price.analytic_greeks = true;
    config.prefetch_snapshots = false;
    FrictionModel base;
    base.spread_kind = FrictionModel::SpreadKind::VolTicks;
    base.vol_tick = vol_tick;
    config.frictions = dispersion_effective_frictions(base, model);
    return run_backtest(*clock, strategy, config);
  };

  // vol_tick == 0 isolates the impact term while keeping the VolTicks lane (and
  // therefore the identical marks/vegas) selected in all three runs.
  const auto spread_only = run_with(0.01, inert);
  const auto impact_only = run_with(0.0, costs);
  const auto both = run_with(0.01, costs);
  ASSERT_TRUE(spread_only.has_value()) << spread_only.error().to_string();
  ASSERT_TRUE(impact_only.has_value()) << impact_only.error().to_string();
  ASSERT_TRUE(both.has_value()) << both.error().to_string();
  ASSERT_EQ(both->size(), 2u);

  // Both components must actually bite, or the additive identity is vacuous.
  ASSERT_GT(spread_only->cost[1], 0.0);
  ASSERT_GT(impact_only->cost[1], 0.0);

  const double expected = spread_only->cost[1] + impact_only->cost[1];
  EXPECT_NEAR(both->cost[1], expected, 1.0e-9 * expected)
      << "vol-tick spread " << spread_only->cost[1] << " + impact " << impact_only->cost[1]
      << " != charged " << both->cost[1];
  // The composed run is strictly more expensive than either component alone —
  // the economic statement C-4 is about (a spec charging impact only overstates NAV).
  EXPECT_GT(both->cost[1], impact_only->cost[1]);
  EXPECT_GT(both->cost[1], spread_only->cost[1]);
}

TEST(Backtest, DuplicateOpenLotIdsFailClosedBeforePricingOrPartialClose) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;

  const fs::path dir = fresh_dir("ambiguous-target-mark-fallback");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 2));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  DuplicateIdOpenThenCloseStrategy strategy{kBaseNow + 200 * kDayNs};
  RunConfig config;
  config.price.n_threads = 1u;
  config.prefetch_snapshots = false;
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("duplicate lot id=777"), std::string::npos);
  if constexpr (counters_enabled()) {
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::BoundarySolves), 0u);
  }
}

TEST(Backtest, StrategyCannotMutateEconomicFieldsOfASurvivingLot) {
  const fs::path dir = fresh_dir("mutated-surviving-lot");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 2));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  MutateExistingLotStrategy strategy{kBaseNow + 200 * kDayNs};
  RunConfig config;
  config.price.n_threads = 1u;
  config.prefetch_snapshots = false;

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("mutated surviving lot id=1"), std::string::npos);
}

TEST(Backtest, StrategyCannotReuseAClosedLotId) {
  const fs::path dir = fresh_dir("reused-closed-lot-id");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 3));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ReuseClosedLotIdStrategy strategy{kBaseNow + 200 * kDayNs};
  RunConfig config;
  config.price.n_threads = 1u;
  config.prefetch_snapshots = false;

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("reused or non-monotonic lot id=1"), std::string::npos);
}

TEST(Backtest, StrategyCannotRollBackNextLotId) {
  const fs::path dir = fresh_dir("next-lot-id-rollback");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 1));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  RollBackNextLotIdStrategy strategy;
  RunConfig config;
  config.prefetch_snapshots = false;

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("next_lot_id non-monotonic"), std::string::npos);
}

TEST(Backtest, ThousandLotMassRollUsesUniqueIndexedIdentityAndPreservesAccounting) {
  constexpr std::size_t n_lots = 1'000u;
  const fs::path dir = fresh_dir("thousand-lot-mass-roll");
  auto clock = Clock::from_manifest(make_evolving_corpus(dir, "SPX", 2));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  MassOpenThenCloseStrategy strategy{kBaseNow + 200 * kDayNs, n_lots};
  RunConfig config;
  config.price.n_threads = 4u;
  config.prefetch_snapshots = false;

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ(result->n_open_lots[0], static_cast<double>(n_lots));
  EXPECT_EQ(result->n_open_lots[1], 0.0);
  EXPECT_TRUE(std::isfinite(result->nav[1]));
}

TEST(Backtest, BadTargetMarkWithoutFallbackSurfaceNeverBooksAZeroClose) {
  const fs::path dir = fresh_dir("bad-target-mark-no-zero-close");
  const PricedSurface day0 = make_surface(kUid, 100.0, 100.0, kBaseNow);
  constexpr std::uint32_t other_uid = 8u;
  const PricedSurface day1 = make_surface(other_uid, 101.0, 101.0, kBaseNow + kDayNs);
  const std::string path0 = write_one(dir, "2026-08-01", "SPX", day0);
  const std::string path1 = write_one(dir, "2026-08-02", "OTHER", day1);
  auto clock =
      Clock::from_manifest(make_manifest({{"2026-08-01", path0}, {"2026-08-02", path1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  OpenThenCloseStrategy strategy{kBaseNow + 200 * kDayNs};
  RunConfig config;
  config.price.n_threads = 1u;
  config.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  config.frictions.half_spread_bps = 100.0;
  config.prefetch_snapshots = false;
  // WS-F F1(c): the default is now UnpricedLotPolicy::Error, whose step-level
  // held-lot guard would fire FIRST on this corpus (the SPX board is absent on
  // date 2) and mask the guarantee under test. The subject here is the EXECUTOR's
  // roll-close guard — it must fail closed regardless of the held-valuation
  // policy — so pin the lenient policy to let the close path be reached.
  config.unpriced = UnpricedLotPolicy::ExcludeAndReport;

  const auto result = run_backtest(*clock, strategy, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
  EXPECT_NE(result.error().message().find("roll-close lot id=1"), std::string::npos);
  EXPECT_NE(result.error().message().find("uid=7"), std::string::npos);
}

TEST(Backtest, MissingExactExpiryObservationFailsClosed) {
  const fs::path dir = fresh_dir("expiry-observation-gap");
  const std::int64_t now0 = kBaseNow;
  const std::int64_t expiry = kBaseNow + 30 * kDayNs;
  const std::int64_t now1 = kBaseNow + 45 * kDayNs;

  const PricedSurface d0 = make_surface(kUid, 100.0, 100.0, now0);
  const PricedSurface d1 = make_surface(kUid, 103.0, 103.0, now1);
  const std::string p0 = write_one(dir, "2026-08-01", "SPX", d0);
  const std::string p1 = write_one(dir, "2026-09-15", "SPX", d1);
  auto clock = Clock::from_manifest(make_manifest({{"2026-08-01", p0}, {"2026-09-15", p1}}, "SPX"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  PortfolioState st;
  st.lots.push_back(
      Lot{7, OptionContract{kUid, 95.0, 0.0, Side::Call}, +5.0, 100.0, expiry, 0, 0.0});

  auto res = run_backtest(*clock, std::move(st));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
  EXPECT_NE(res.error().message().find("exact expiry observation"), std::string::npos);
  EXPECT_NE(res.error().message().find("lot id=7"), std::string::npos);
}

// ── S1-3b: the fixed-book overload counts unpriced lots and stays bit-identical ─
//
// The fixed-book step loop was inlined body-for-body from `compute_step`; S1-3b
// routes it through the shared implementation and adds the `n_unpriced_lots`
// column. On a clean corpus (surface present every date) nothing is unpriced and
// EVERY column is bit-identical to the pre-change (d54c191) run — the pins below
// were captured on d54c191 before the routing change.
TEST(Backtest, DefaultPolicyIsBitIdenticalToBaseline) {
  const fs::path dir = fresh_dir("s1-3b-default");
  const CorpusManifest man = make_evolving_corpus(dir, "SPX", 5);
  auto clock = Clock::from_manifest(man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry = kBaseNow + 120 * kDayNs;    // survives every date
  // WS-F F1(c) flipped the default to Error; this corpus never loses a surface,
  // so the default and the explicit lenient policy must still agree bit-for-bit.
  auto res = run_backtest(*clock, survivor_book(expiry)); // default: Error (post-F1c)
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), 5u);

  ASSERT_EQ(res->n_unpriced_lots.size(), 5u);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(res->n_unpriced_lots[i], 0.0) << "row " << i; // nothing ever missing
  }

  RunConfig explicit_default;
  explicit_default.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  auto explicit_res = run_backtest(*clock, survivor_book(expiry), explicit_default);
  ASSERT_TRUE(explicit_res.has_value()) << explicit_res.error().to_string();
  expect_result_bit_identical(*res, *explicit_res);

  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(res->pnl_settlement[i], 0.0) << "settle row " << i;
    EXPECT_EQ(res->n_open_lots[i], 2.0) << "nlots row " << i;
  }
  std::printf("[s1-3b] default policy equals explicit ExcludeAndReport over 5 rows\n");
}

// ── B1. Subset-deserialize: the loader deserializes only referenced uids ──────
//
// MarketSnapshot::load(..., referenced_uids) reconstructs ONLY the archive
// directory entries the book references, dropping the whole-board
// reconstruct_all_with_provenance (bottleneck #1). uid_of still resolves every
// archived name, and a subset-reconstructed surface prices bit-identically to the
// whole-board one (v2 reconstruct is bit-exact to the source surface, S4 seam).
TEST(Backtest, SubsetDeserializeLoadsOnlyReferencedUids) {
  const fs::path dir = fresh_dir("b1-subset");
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / "2026-08-01.atxvsa").string();

  std::vector<PricedSurface> surfaces;
  surfaces.reserve(4);
  for (std::uint32_t i = 0; i < 4; ++i) {
    const double S = 100.0 + 5.0 * static_cast<double>(i);
    surfaces.push_back(make_surface(kUid + i, S, S, kBaseNow, 0.002 * static_cast<double>(i)));
  }
  std::vector<SurfaceArchiveItem> items;
  for (std::uint32_t i = 0; i < 4; ++i) {
    static const char* kNames[] = {"AAA", "BBB", "CCC", "DDD"};
    items.push_back(SurfaceArchiveItem{kNames[i], &surfaces[i]});
  }
  ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());

  auto whole = MarketSnapshot::load(path);
  ASSERT_TRUE(whole.has_value()) << whole.error().to_string();
  EXPECT_EQ(whole->n_surfaces(), 4u);

  const std::uint32_t subset_uids[] = {kUid, kUid + 2};
  auto subset = MarketSnapshot::load(path, QueryPricingTier::LegacyCompatible,
                                     std::span<const std::uint32_t>{subset_uids});
  ASSERT_TRUE(subset.has_value()) << subset.error().to_string();
  EXPECT_EQ(subset->n_surfaces(), 2u) << "whole-board reconstruct was not dropped";
  EXPECT_NE(subset->find(kUid), nullptr);
  EXPECT_NE(subset->find(kUid + 2), nullptr);
  EXPECT_EQ(subset->find(kUid + 1), nullptr) << "unreferenced uid must not be deserialized";
  EXPECT_EQ(subset->find(kUid + 3), nullptr) << "unreferenced uid must not be deserialized";

  // uid_of resolves every archived name even under the subset load.
  auto u1 = subset->uid_of("BBB");
  auto u3 = subset->uid_of("DDD");
  ASSERT_TRUE(u1.has_value());
  ASSERT_TRUE(u3.has_value());
  EXPECT_EQ(*u1, kUid + 1);
  EXPECT_EQ(*u3, kUid + 3);

  // A subset-reconstructed surface prices bit-identically to the whole-board one.
  const SurfaceRef ws = whole->find(kUid);
  const SurfaceRef ss = subset->find(kUid);
  ASSERT_NE(ws, nullptr);
  ASSERT_NE(ss, nullptr);
  auto wv = ws->fair_value(100.0, 0.25, Side::Call);
  auto sv = ss->fair_value(100.0, 0.25, Side::Call);
  ASSERT_TRUE(wv.has_value());
  ASSERT_TRUE(sv.has_value());
  EXPECT_TRUE(bits_equal(*wv, *sv)) << "subset reconstruct must match whole-board bit-for-bit";
  std::printf("[b1] subset-deser: 2 of 4 surfaces loaded; uid_of resolves all; marks bit-identical\n");
}

// P-9: a requested subset with no matching uid is a normal missing-name date,
// not a reason to materialize every unrelated surface. The snapshot must retain
// timestamp and symbol-directory metadata while its pricing set remains empty.
TEST(Backtest, MissingSubsetRetainsMetadataWithoutLoadingFullBoard) {
  const fs::path dir = fresh_dir("p9-missing-subset");
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / "2026-08-02.atxvsa").string();

  std::vector<PricedSurface> surfaces;
  surfaces.reserve(4);
  for (std::uint32_t i = 0; i < 4; ++i) {
    const double S = 100.0 + 5.0 * static_cast<double>(i);
    surfaces.push_back(make_surface(kUid + i, S, S, kBaseNow, 0.002 * static_cast<double>(i)));
  }
  static const char *kNames[] = {"AAA", "BBB", "CCC", "DDD"};
  std::vector<SurfaceArchiveItem> items;
  for (std::uint32_t i = 0; i < 4; ++i) {
    items.push_back(SurfaceArchiveItem{kNames[i], &surfaces[i]});
  }
  ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());

  const std::uint32_t missing_uid = kUid + 1000u;
  MarketSnapshot::reset_deserialized_bytes();
  auto missing = MarketSnapshot::load(path, QueryPricingTier::LegacyCompatible,
                                      std::span<const std::uint32_t>{&missing_uid, 1u});
  ASSERT_TRUE(missing.has_value()) << missing.error().to_string();
  EXPECT_EQ(missing->n_surfaces(), 0u);
  EXPECT_EQ(missing->ts_ns(), kBaseNow);
  EXPECT_EQ(missing->find(missing_uid), nullptr);
  EXPECT_EQ(MarketSnapshot::deserialized_bytes(), 0u)
      << "a subset miss must not materialize unrelated record bodies";

  auto archived_uid = missing->uid_of("ccc");
  ASSERT_TRUE(archived_uid.has_value());
  EXPECT_EQ(*archived_uid, kUid + 2u)
      << "metadata-only snapshots still expose the complete symbol directory";
}

// The fixed-book overload's private cache subset-deserializes the book's uids; the
// run is bit-identical to the whole-board path (a supplied shared cache), proving
// the subset load is economically exact while touching fewer surfaces.
TEST(Backtest, SubsetDeserializeFixedBookParity) {
  const fs::path dir = fresh_dir("b1-parity");
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < 3; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(4);
    for (std::uint32_t i = 0; i < 4; ++i) {
      const double S =
          (100.0 + 5.0 * static_cast<double>(i)) * (1.0 + 0.003 * static_cast<double>(d));
      surfaces.push_back(make_surface(kUid + i, S, S, now,
                                      0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(i)));
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string path = (dir / (std::string(buf) + ".atxvsa")).string();
    std::vector<SurfaceArchiveItem> items;
    for (std::uint32_t i = 0; i < 4; ++i) {
      static const char* kNames[] = {"AAA", "BBB", "CCC", "DDD"};
    items.push_back(SurfaceArchiveItem{kNames[i], &surfaces[i]});
    }
    ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    dp.emplace_back(buf, path);
  }
  auto clock = Clock::from_manifest(make_manifest(dp, "AAA"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 120 * kDayNs; // held past the clock
  const auto make_book = [&]() {
    PortfolioState b;
    b.lots.push_back(
        Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Call}, +2.0, 100.0, expiry, 0, 0.0});
    b.lots.push_back(
        Lot{2, OptionContract{kUid + 2, 110.0, 0.0, Side::Put}, -1.0, 100.0, expiry, 0, 0.0});
    return b;
  };

  // Default cfg => private cache => subset-deserialize (2 of the 4 archived names).
  auto subset_res = run_backtest(*clock, make_book());
  ASSERT_TRUE(subset_res.has_value()) << subset_res.error().to_string();

  // Supplied (shared) cache => whole-board load. Must be bit-identical.
  RunConfig whole_cfg;
  whole_cfg.snapshot_cache = std::make_shared<SnapshotCache>();
  auto whole_res = run_backtest(*clock, make_book(), whole_cfg);
  ASSERT_TRUE(whole_res.has_value()) << whole_res.error().to_string();

  expect_result_bit_identical(*subset_res, *whole_res);
  std::printf("[b1] fixed-book subset-deser bit-identical to whole-board over %zu rows\n",
              subset_res->size());
}

// ── BacktestResult column-shape invariant (S4-T22 / plan item 4.6) ──────────
//
// `BacktestResult` is ~30 parallel public columns that nothing checked. A
// column one row short of `date` used to index OUT OF RANGE inside the
// tearsheet fold and both serializers instead of reporting a shape error;
// benchmark_stats_test.cpp's fixture comment documented exactly that hazard.
// `validate()` is the enforcement point, and these tests are its contract.

namespace {

// An n-row result with every row-parallel column populated — the shape the
// engine emits.
[[nodiscard]] BacktestResult make_shape_fixture(std::size_t n) {
  BacktestResult r;
  for (std::size_t i = 0; i < n; ++i) {
    r.date.push_back("d" + std::to_string(i));
    r.ts_ns.push_back(static_cast<std::int64_t>(i));
  }
  const std::vector<double> col(n, 1.0);
  for (std::vector<double> *c :
       {&r.pnl_total, &r.pnl_delta, &r.pnl_gamma, &r.pnl_vega, &r.pnl_vanna, &r.pnl_volga,
        &r.pnl_theta, &r.pnl_rho, &r.pnl_charm, &r.pnl_unexplained, &r.pnl_settlement,
        &r.pnl_shares, &r.financing, &r.cost, &r.nav, &r.cash, &r.gross_delta, &r.gross_gamma,
        &r.gross_vega, &r.gross_theta, &r.gross_vega_abs, &r.turnover_notional, &r.turnover_vega,
        &r.n_open_lots, &r.n_unpriced_lots, &r.n_unpriced_greeks, &r.nav_liquidation}) {
    *c = col;
  }
  r.signals.emplace_back("sig", col);
  return r;
}

} // namespace

// THE red test: a result skewed by ONE row in ONE column is caught, and the
// error names the column and both lengths so the report is actionable.
TEST(BacktestResultShape, SkewedColumnIsRejected) {
  BacktestResult r = make_shape_fixture(3);
  ASSERT_TRUE(r.validate().has_value()) << r.validate().error().to_string();

  r.nav.pop_back(); // 2 rows of nav against 3 rows of date
  const Status skewed = r.validate();
  ASSERT_FALSE(skewed.has_value()) << "a skewed column must not validate";
  EXPECT_EQ(skewed.error().code(), ErrorCode::InvalidArgument);
  const std::string msg = skewed.error().message();
  EXPECT_NE(msg.find("nav"), std::string::npos) << msg;
  EXPECT_NE(msg.find('2'), std::string::npos) << msg;
  EXPECT_NE(msg.find('3'), std::string::npos) << msg;

  // A column LONGER than `date` is the same defect from the other side.
  BacktestResult longer = make_shape_fixture(3);
  longer.cost.push_back(0.0);
  const Status over = longer.validate();
  ASSERT_FALSE(over.has_value());
  EXPECT_NE(over.error().message().find("cost"), std::string::npos) << over.error().message();

  // The int64 column is checked like every other one.
  BacktestResult ts = make_shape_fixture(3);
  ts.ts_ns.pop_back();
  const Status ts_bad = ts.validate();
  ASSERT_FALSE(ts_bad.has_value());
  EXPECT_NE(ts_bad.error().message().find("ts_ns"), std::string::npos)
      << ts_bad.error().message();
}

// EMPTY-or-row-parallel, not all-or-nothing: a fixture that fills only the
// columns a fold reads is a legal partial result, and an empty result is legal.
TEST(BacktestResultShape, EmptyColumnsAreLegalPartialResults) {
  BacktestResult sparse;
  for (std::size_t i = 0; i < 4; ++i) {
    sparse.date.push_back("d" + std::to_string(i));
    sparse.ts_ns.push_back(static_cast<std::int64_t>(i));
  }
  sparse.pnl_total = {0.0, 1.0, 2.0, 3.0};
  sparse.nav = {0.0, 1.0, 3.0, 6.0};
  EXPECT_TRUE(sparse.validate().has_value()) << sparse.validate().error().to_string();

  const BacktestResult empty;
  EXPECT_TRUE(empty.validate().has_value()) << empty.validate().error().to_string();

  // ... but a non-empty column of the WRONG length is still rejected, which is
  // the whole point of admitting empties.
  sparse.cost = {0.0, 1.0};
  EXPECT_FALSE(sparse.validate().has_value());
}

// `step_pnl_total` is exempt by contract (full-resolution, length == refs-1,
// deliberately not parallel to the downsampled `date`); signals are not.
TEST(BacktestResultShape, StepSeriesIsExemptAndSignalsAreChecked) {
  BacktestResult stride = make_shape_fixture(3);
  stride.step_pnl_total = std::vector<double>(11, 0.5); // stride-4 run over 12 refs
  EXPECT_TRUE(stride.validate().has_value()) << stride.validate().error().to_string();

  BacktestResult skewed_signal = make_shape_fixture(3);
  skewed_signal.signals.emplace_back("short", std::vector<double>(2, 0.0));
  const Status sig = skewed_signal.validate();
  ASSERT_FALSE(sig.has_value()) << "a skewed signal series must not validate";
  EXPECT_EQ(sig.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(sig.error().message().find("short"), std::string::npos) << sig.error().message();

  // Both writers append one dynamic column per signal, so a duplicate name
  // emits an ambiguous header — reject it here rather than on the wire.
  BacktestResult dup = make_shape_fixture(3);
  dup.signals.emplace_back("sig", std::vector<double>(3, 0.0));
  const Status dup_st = dup.validate();
  ASSERT_FALSE(dup_st.has_value());
  EXPECT_NE(dup_st.error().message().find("sig"), std::string::npos) << dup_st.error().message();

  BacktestResult unnamed = make_shape_fixture(3);
  unnamed.signals.emplace_back("", std::vector<double>(3, 0.0));
  EXPECT_FALSE(unnamed.validate().has_value());
}
