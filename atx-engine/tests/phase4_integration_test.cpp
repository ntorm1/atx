// phase4_integration_test.cpp — P4-10b: the Phase-4 integration proof suite.
//
// The proof that the assembled Phase-4 layer (gate -> store -> combiner ->
// CombinedSignalSource -> the real BacktestLoop, with risk::FactorModelBuilder +
// risk::PortfolioOptimizer exercised in a parallel per-rebalance harness) honours
// the layer's three headline invariants end-to-end. Each is a TEST, not a hope:
//
//   (A) FIT/APPLY FIREWALL — truncation-invariance (THE headline). Fit the
//       combiner + factor model + optimizer on a window [t0,t1); produce the fitted
//       book. Then CORRUPT every row at/after a later date t (rows that are in the
//       FUTURE relative to the fit window); re-fit/re-apply; assert every output
//       for dates <= t is BYTE-IDENTICAL (EXPECT_EQ on the actual f64, bit-equal —
//       NOT EXPECT_NEAR). This proves future rows are provably invisible to the
//       fit. Extends combine_combiner_test.cpp's FitWindowTruncationInvariant to the
//       COMBINED path: combiner.fit AND FactorModelBuilder::build AND
//       PortfolioOptimizer::solve, byte-for-byte across a clean and a future-
//       corrupted panel/pool.
//   (B) WHOLE-LAYER DETERMINISM HASH — evaluate the whole combined pipeline twice;
//       fold the ordered (date, instrument, weight-bits) stream into a digest via
//       atx-core hash_combine; assert IDENTICAL digests across the two runs. Then
//       prove NON-VACUITY: a mutated input (reorder the alphas in the pool / perturb
//       one PnL value / add a late panel row) FLIPS the digest (EXPECT_NE).
//   (C) WALK-FORWARD COMBINED BACKTEST (deterministic + cost-honest) — a realistic
//       multi-alpha pool over a synthetic panel that INCLUDES delisted symbols + NaN
//       gaps: gate -> store -> rolling combiner.fit -> CombinedSignalSource -> the
//       real BacktestLoop -> ExecutionSimulator -> Portfolio. Assert the run is
//       DETERMINISTIC (two runs -> identical equity curve / final-equity bits) and
//       COST-HONEST (costs OFF recovers the frictionless mark-to-market equity;
//       costs ON drains it strictly below frictionless).
//
// ===========================================================================
//  AS-BUILT RECONCILIATIONS (documented, load-bearing — see the plan §8 P4-10)
// ===========================================================================
//  * THE EXPECT_DEATH SELF-GUARD (caveat A). The P4-7a/P4-5 ledgers state the
//    apply-after-fit_end firewall is CALLER-enforced, NOT self-asserted: a
//    combine::Combination carries [fit_begin,fit_end) as INERT fields (the
//    CombinedSignalSource comment confirms "[fit_begin,fit_end) inert here"), and a
//    risk::FactorModel exposes fit_begin()/fit_end() accessors but its apply methods
//    (apply_inverse/risk/neutralize) assert only DIMENSIONS, never the window. So NO
//    self-asserting apply-window guard exists in the production headers — and this
//    suite adds none (it must not edit production headers). The real firewall is
//    STRUCTURAL (the fit physically cannot read a row >= fit_end), proven by the
//    truncation-invariance test (A). The EXPECT_DEATH below therefore pins a
//    CALLER-SIDE firewall check WRITTEN IN THIS TU (apply_guard) — the discipline a
//    caller is supposed to enforce — and is the SECONDARY proof; (A) is the headline.
//  * THE OPTIMIZER-AT-COMPONENT-LEVEL (caveat C). loop::BacktestLoop is driven by a
//    loop::WeightPolicy (to_target_weights), NOT by risk::PortfolioOptimizer; wiring
//    the optimizer THROUGH the loop would need a loop change (forbidden). So the
//    walk-forward loop (C) is driven by CombinedSignalSource + WeightPolicy (the
//    supported path), and the risk optimizer + factor model are exercised in a
//    PARALLEL per-rebalance component harness within the same test: a FactorModel is
//    built on the rolling window and PortfolioOptimizer::solve runs on the combined
//    signal, asserting the book invariants (Sigma w = 0, Sigma|w| <= L, |w_i| <= cap)
//    hold and the solve is deterministic. This is the deliberate as-built
//    reconciliation, also used by firewall proof (A).

#include <bit>     // std::bit_cast (canonical f64 bits in the digest)
#include <cmath>   // std::isnan, std::fabs
#include <limits>  // std::numeric_limits (the NaN gap sentinel)
#include <memory>  // std::make_unique (the per-run EventBus)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/macro.hpp" // ATX_ASSERT (the caller-side apply_guard)
#include "atx/core/types.hpp"

#include "atx/core/linalg/linalg.hpp" // MatX / VecX (the parallel risk harness)

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/combine/combined_source.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/portfolio/portfolio.hpp"
#include "atx/engine/risk/exposures.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/optimizer.hpp"

namespace {

using atx::f64;
using atx::i64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::Decimal;
using atx::core::hash_combine;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::BacktestLoop;
using atx::engine::BacktestResult;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::ISignalSource;
using atx::engine::Market;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::combine::AlphaCombiner;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::Combination;
using atx::engine::combine::CombinedSignalSource;
using atx::engine::combine::CombineMethod;
using atx::engine::combine::GateVerdict;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::FillPayload;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::kPanelFieldCount;
using atx::engine::risk::FactorModel;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PortfolioOptimizer;
using atx::engine::risk::build_exposures;
using Bus = EventBus<>;
using Timestamp = atx::core::time::Timestamp;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kCap = 32;

const Symbol kA{1};
const Symbol kB{2};
const Symbol kC{3};

[[nodiscard]] constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// ===========================================================================
//  Pool-of-PnL helpers (mirror combine_combiner_test.cpp::insert_pnl).
//
//  The combiner + gate read ONLY the PnL rows; positions/source are inert filler
//  of the right length. A single-instrument pool (insts == 1) keeps the position
//  stream length == the PnL length.
// ===========================================================================
void insert_pnl(AlphaStore &pool, std::span<const f64> pnl) {
  const std::vector<f64> pos(pnl.size(), 0.0); // insts == 1 -> period-major == pnl length
  const auto r = pool.insert(/*source=*/nullptr, pnl, pos, AlphaMetrics{});
  ASSERT_TRUE(r.has_value());
}

// Build a 4-alpha pool of distinct, deterministic, mildly-correlated PnL streams.
// `tail_seed` poisons every row at index >= `corrupt_from` to a wildly different
// (but length-preserving) garbage value; with corrupt_from past the fit window the
// poisoned tail is in the FUTURE relative to the fit and must be invisible (proof A).
[[nodiscard]] std::vector<std::vector<f64>> pool_rows(usize n_periods, usize corrupt_from,
                                                      f64 tail_seed) {
  std::vector<std::vector<f64>> rows(4, std::vector<f64>(n_periods, 0.0));
  for (usize t = 0U; t < n_periods; ++t) {
    if (t >= corrupt_from) {
      // FUTURE rows: deterministic garbage, distinct per (alpha, t, tail_seed).
      for (usize a = 0U; a < 4U; ++a) {
        rows[a][t] = tail_seed * static_cast<f64>(t + 1U) + static_cast<f64>(a) * 13.0;
      }
      continue;
    }
    if (t == 0U) {
      continue; // index 0 is the structural zero (P4-2 §0-F)
    }
    const f64 x = static_cast<f64>(t);
    // Four deterministic streams with genuine variance + distinct shapes (index,
    // not RNG — determinism §3.2). Bounded small returns so the LW/MV fit is sane.
    rows[0][t] = 0.010 * ((t % 2U == 0U) ? 1.0 : -1.0);
    rows[1][t] = 0.008 * ((t % 3U == 0U) ? 2.0 : -1.0);
    rows[2][t] = 0.012 * (((t / 2U) % 2U == 0U) ? 1.0 : -1.0) + 0.001;
    rows[3][t] = 0.006 * ((t % 4U < 2U) ? 1.0 : -1.0) + 0.0005 * x;
  }
  return rows;
}

// Fit a combiner over [fit_begin,fit_end) on a freshly-built pool of `rows`.
[[nodiscard]] Combination fit_combo(const std::vector<std::vector<f64>> &rows, usize fit_begin,
                                    usize fit_end, CombineMethod method) {
  AlphaStore pool;
  for (const std::vector<f64> &row : rows) {
    insert_pnl(pool, std::span<const f64>{row});
  }
  AlphaCombiner comb;
  comb.cfg.method = method;
  const auto r = comb.fit(pool, fit_begin, fit_end);
  EXPECT_TRUE(r.has_value());
  return r.value();
}

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage (the P4-7b builder-test
//  pattern). Caller supplies an n_rows x n_inst close + volume grid (row 0 ==
//  newest cross-section); open/high/low mirror close. A NaN close => an absent
//  (NaN-gap) cell with mask bit 0; a delisted symbol is just a symbol whose newer
//  rows are NaN (the cross-section it last appeared in is its final bar).
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
               const std::vector<std::vector<f64>> &volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0U; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0U; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0U; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        const f64 v = volume[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, v);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           head_(),      n_rows_,
                     mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// Sectors-only factor-model config (one 0/1 dummy column spanning all instruments)
// => K == 1 with NO style columns (which would need 252+ rows of lookback). The
// canonical low-lookback factor model for an integration harness.
[[nodiscard]] FactorModelConfig single_sector_cfg() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00; // sectors only -> no per-instrument lookback needed
  return cfg;
}

// A close grid for a 3-instrument synthetic panel of `n_rows` rows (row 0 ==
// newest). Each instrument walks a distinct deterministic geometric-ish path (no
// RNG). `tail_close` overrides the OLDEST `corrupt_old` rows (rows beyond the fit
// window's return reach) to garbage — those rows are the "future" relative to a
// build over the newest `window` rows (PanelView rows >= window are invisible).
[[nodiscard]] std::vector<std::vector<f64>> close_grid(usize n_rows, usize n_inst,
                                                       usize corrupt_old, f64 tail_close) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  for (usize i = 0U; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) { // oldest -> newest fill
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  // Poison the OLDEST `corrupt_old` rows (largest row indices == furthest in the
  // past == NOT read by a build over the newest `window` rows).
  for (usize r = n_rows - corrupt_old; r < n_rows; ++r) {
    for (usize i = 0U; i < n_inst; ++i) {
      close[r][i] = tail_close + static_cast<f64>(i) * 7.0 + static_cast<f64>(r);
    }
  }
  return close;
}

// ===========================================================================
//  CALLER-SIDE apply-window firewall (the EXPECT_DEATH target — caveat A).
//
//  Neither Combination nor FactorModel self-asserts the apply window (the firewall
//  is CALLER-enforced; see the file header). This is the check a disciplined caller
//  is supposed to make before applying a fitted object: the apply date MUST be at
//  or after fit_end (the fit window is the PAST; applying INSIDE it would leak the
//  in-window labels). Wrapping the misuse in a free function lets EXPECT_DEATH fire
//  on the ATX_ASSERT abort. This pins the DISCIPLINE; the structural firewall itself
//  is proven by truncation-invariance (A).
// ===========================================================================
void apply_guard(const FactorModel &model, usize apply_date) {
  // apply_date < fit_end() == applying a fitted object INSIDE its own fit window.
  ATX_ASSERT(apply_date >= model.fit_end());
}

// ===========================================================================
//  Parallel per-rebalance risk harness (caveat C): build a FactorModel on the
//  rolling window and solve the optimizer on the combined signal. Returns the
//  optimizer's book (length M). Asserts the build/solve succeed.
// ===========================================================================
[[nodiscard]] std::vector<f64> solve_optimizer_book(const PanelView &panel, usize window,
                                                    std::span<const u32> group,
                                                    std::span<const f64> alpha,
                                                    const OptimizerConfig &ocfg) {
  FactorModelBuilder builder{single_sector_cfg()};
  const auto model = builder.build(panel, window, std::span<const f64>{}, group);
  EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().to_string());
  if (!model) {
    return {};
  }
  PortfolioOptimizer opt;
  opt.cfg = ocfg;
  const auto book = opt.solve(alpha, model.value(), std::span<const f64>{});
  EXPECT_TRUE(book.has_value());
  return book ? book.value() : std::vector<f64>{};
}

// ===========================================================================
//  (A) FIT/APPLY FIREWALL — truncation-invariance (THE headline).
// ===========================================================================

// The COMBINER half of the firewall, extended from combine_combiner_test.cpp:
// fit on [1, fit_end); a SECOND pool whose rows >= fit_end are wildly corrupted
// (the future) fits the SAME [1, fit_end). The weights must be BYTE-IDENTICAL.
TEST(Phase4Integration, Firewall_CombinerFutureRowsCorrupted_WeightsByteIdentical) {
  constexpr usize kPeriods = 16U;
  constexpr usize kFitBegin = 1U;
  constexpr usize kFitEnd = 8U; // rows [8,16) are the FUTURE -> must be invisible

  const Combination clean =
      fit_combo(pool_rows(kPeriods, /*corrupt_from=*/kFitEnd, /*tail_seed=*/77.0), kFitBegin,
                kFitEnd, CombineMethod::ShrinkageMv);
  const Combination poisoned =
      fit_combo(pool_rows(kPeriods, /*corrupt_from=*/kFitEnd, /*tail_seed=*/-913.0), kFitBegin,
                kFitEnd, CombineMethod::ShrinkageMv);

  ASSERT_EQ(clean.weights.size(), poisoned.weights.size());
  ASSERT_EQ(clean.weights.size(), 4U);
  for (usize i = 0U; i < clean.weights.size(); ++i) {
    // BYTE-IDENTICAL: future rows (>= fit_end) are physically unreadable by fit().
    EXPECT_EQ(clean.weights[i], poisoned.weights[i]) << "future PnL row leaked into the fit";
  }
}

// The COMBINED firewall (the plan's extension): the whole fitted-and-applied book
// — combiner.fit -> CombinedSignalSource apply -> FactorModelBuilder::build ->
// PortfolioOptimizer::solve — is byte-identical when the FUTURE rows of BOTH the
// PnL pool AND the price panel are corrupted. The optimizer's output weights are
// the end of the pipeline; comparing THEM proves the whole chain is firewalled.
TEST(Phase4Integration, Firewall_CombinedBookFutureCorrupted_OptimizerBookByteIdentical) {
  constexpr usize kPeriods = 16U;
  constexpr usize kFitBegin = 1U;
  constexpr usize kFitEnd = 8U;
  constexpr usize kInst = 3U;
  constexpr usize kWindow = 6U;           // factor-model rolling window (newest rows)
  const std::vector<u32> group{1U, 1U, 1U}; // one sector -> K = 1

  // The combined signal applied at the current cross-section: blend the pool's
  // constituents (here ScriptedSignalSource doubles emitting a known cross-section)
  // through the fitted Combination, then feed the result as alpha to the optimizer.
  const auto build_book = [&](f64 pnl_tail, f64 px_tail) -> std::vector<f64> {
    // 1. Fit the combiner on [fit_begin,fit_end); future pool rows corrupted.
    const Combination combo =
        fit_combo(pool_rows(kPeriods, kFitEnd, pnl_tail), kFitBegin, kFitEnd,
                  CombineMethod::ShrinkageMv);
    // 2. Apply the frozen combo over a fixed cross-section (the constituents are the
    //    pool's sources; here four scripted doubles with a deterministic 3-wide
    //    signal). The applied blend is the optimizer's alpha.
    std::vector<std::unique_ptr<ScriptedSignalSource>> owned;
    std::vector<ISignalSource *> srcs;
    const std::vector<std::vector<f64>> sig{{0.9, -0.4, 0.2}, {0.3, 0.7, -0.5},
                                            {-0.6, 0.1, 0.8}, {0.2, -0.3, 0.5}};
    for (const std::vector<f64> &v : sig) {
      owned.push_back(std::make_unique<ScriptedSignalSource>(
          std::vector<std::vector<f64>>{v}, /*universe_size=*/kInst, /*max_lookback=*/0U));
      srcs.push_back(owned.back().get());
    }
    CombinedSignalSource mega{std::move(srcs), combo, CombineMethod::ShrinkageMv};
    const std::vector<InstrumentId> dummy{kA, kB, kC};
    RollingPanel<4> blend_panel{std::span<const InstrumentId>{dummy}, /*max_lookback=*/1U};
    const auto sv = mega.evaluate(blend_panel.view());
    EXPECT_TRUE(sv.has_value());
    const std::vector<f64> alpha{sv->values.begin(), sv->values.end()};

    // 3. Build the factor model on the rolling window; the OLDEST rows (the future
    //    relative to a newest-`window` build) are corrupted via px_tail.
    const usize n_rows = kWindow + 4U; // 4 extra OLD rows beyond the window's reach
    const std::vector<std::vector<f64>> close =
        close_grid(n_rows, kInst, /*corrupt_old=*/3U, px_tail);
    const std::vector<std::vector<f64>> vol(n_rows, std::vector<f64>(kInst, 1000.0));
    const PanelFixture fx{n_rows, kInst, close, vol};

    OptimizerConfig ocfg;
    ocfg.risk_aversion = 1.0; // lambda > 0 -> the V^-1 tilt path (exercises apply_inverse)
    ocfg.gross_leverage = 1.0;
    ocfg.name_cap = 0.6;
    return solve_optimizer_book(fx.view(), kWindow, std::span<const u32>{group},
                                std::span<const f64>{alpha}, ocfg);
  };

  const std::vector<f64> clean = build_book(/*pnl_tail=*/77.0, /*px_tail=*/13.0);
  const std::vector<f64> poisoned = build_book(/*pnl_tail=*/-913.0, /*px_tail=*/987.0);

  ASSERT_EQ(clean.size(), kInst);
  ASSERT_EQ(poisoned.size(), kInst);
  for (usize i = 0U; i < clean.size(); ++i) {
    // BYTE-IDENTICAL across the whole combined fit/apply chain: every future row
    // (pool PnL >= fit_end AND panel rows >= window) is provably invisible.
    EXPECT_EQ(clean[i], poisoned[i]) << "future row leaked into the combined fitted book";
  }
}

// The optimizer's solve is itself deterministic AND honours the book invariants
// (Sigma w = 0 dollar-neutral, Sigma|w| <= L gross, |w_i| <= cap) — the parallel
// component harness's contract (caveat C).
TEST(Phase4Integration, Firewall_OptimizerBook_DeterministicAndConstraintsHold) {
  constexpr usize kInst = 3U;
  constexpr usize kWindow = 6U;
  const std::vector<u32> group{1U, 1U, 1U};
  const usize n_rows = kWindow + 4U;
  const std::vector<std::vector<f64>> close = close_grid(n_rows, kInst, /*corrupt_old=*/3U, 13.0);
  const std::vector<std::vector<f64>> vol(n_rows, std::vector<f64>(kInst, 1000.0));
  const PanelFixture fx{n_rows, kInst, close, vol};
  const std::vector<f64> alpha{0.7, -0.2, 0.5};

  OptimizerConfig ocfg;
  ocfg.risk_aversion = 1.0;
  ocfg.gross_leverage = 1.0;
  ocfg.name_cap = 0.6;
  const std::vector<f64> b1 =
      solve_optimizer_book(fx.view(), kWindow, std::span<const u32>{group},
                           std::span<const f64>{alpha}, ocfg);
  const std::vector<f64> b2 =
      solve_optimizer_book(fx.view(), kWindow, std::span<const u32>{group},
                           std::span<const f64>{alpha}, ocfg);

  ASSERT_EQ(b1.size(), kInst);
  ASSERT_EQ(b2.size(), kInst);
  f64 net = 0.0;
  f64 gross = 0.0;
  for (usize i = 0U; i < kInst; ++i) {
    EXPECT_EQ(b1[i], b2[i]) << "the optimizer solve must be byte-deterministic";
    net += b1[i];
    gross += std::fabs(b1[i]);
    EXPECT_LE(std::fabs(b1[i]), ocfg.name_cap + 1e-9) << "per-name cap binds";
  }
  EXPECT_NEAR(net, 0.0, 1e-9) << "Sigma w = 0 (dollar-neutral)";
  EXPECT_LE(gross, ocfg.gross_leverage + 1e-9) << "Sigma|w| <= L (gross leverage)";
}

// The SECONDARY firewall proof (caveat A): applying a fitted object INSIDE its own
// fit window aborts THROUGH THE CALLER-SIDE GUARD (no production header self-guard
// exists; the structural firewall is proven by truncation-invariance above). The
// death test confirms the discipline: apply_date < fit_end() -> abort.
TEST(Phase4IntegrationDeathTest, Firewall_ApplyInsideFitWindow_CallerGuardAborts) {
  constexpr usize kInst = 3U;
  constexpr usize kWindow = 6U;
  const std::vector<u32> group{1U, 1U, 1U};
  const usize n_rows = kWindow + 4U;
  const std::vector<std::vector<f64>> close = close_grid(n_rows, kInst, /*corrupt_old=*/0U, 0.0);
  const std::vector<std::vector<f64>> vol(n_rows, std::vector<f64>(kInst, 1000.0));
  const PanelFixture fx{n_rows, kInst, close, vol};
  FactorModelBuilder builder{single_sector_cfg()};
  const auto model =
      builder.build(fx.view(), kWindow, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(model.has_value());
  // fit window is [0, kWindow); applying at date 0 (< fit_end) is the look-ahead
  // misuse the caller-side guard rejects.
  EXPECT_DEATH(apply_guard(model.value(), /*apply_date=*/0U), ".*");
}

// ===========================================================================
//  (B) WHOLE-LAYER DETERMINISM HASH.
//
//  Fold the ordered (date, instrument, weight-bits) stream of the combined
//  pipeline's per-rebalance optimizer books into a digest. Two runs over the same
//  inputs -> identical digest; a mutated input -> a flipped digest (non-vacuity).
// ===========================================================================

// Fold one walk-forward over `n_rebalances` rebalances: at each rebalance fit the
// combiner on the rolling pool window, apply the combo, build the factor model on
// the rolling panel window, solve the optimizer, and fold every (date, inst,
// weight-bits) triple into the running digest (order-sensitive). PURE in the
// inputs; same inputs -> same digest.
[[nodiscard]] u64 layer_digest(const std::vector<std::vector<f64>> &pool, usize fit_begin,
                               usize fit_end, const std::vector<std::vector<f64>> &alpha_sched,
                               const std::vector<std::vector<f64>> &close, usize window) {
  constexpr usize kInst = 3U;
  const std::vector<u32> group{1U, 1U, 1U};
  const usize n_rows = close.size();
  const std::vector<std::vector<f64>> vol(n_rows, std::vector<f64>(kInst, 1000.0));
  const PanelFixture fx{n_rows, kInst, close, vol};

  const Combination combo = fit_combo(pool, fit_begin, fit_end, CombineMethod::ShrinkageMv);

  u64 digest = 0U;
  u64 ord = 0U;
  for (usize reb = 0U; reb < alpha_sched.size(); ++reb) {
    // Apply the frozen combo to this rebalance's constituent cross-sections.
    std::vector<std::unique_ptr<ScriptedSignalSource>> owned;
    std::vector<ISignalSource *> srcs;
    for (usize a = 0U; a < pool.size(); ++a) {
      // Each constituent emits the same scheduled cross-section (a deterministic
      // double); the per-rebalance variation comes from alpha_sched.
      owned.push_back(std::make_unique<ScriptedSignalSource>(
          std::vector<std::vector<f64>>{alpha_sched[reb]}, kInst, 0U));
      srcs.push_back(owned.back().get());
    }
    CombinedSignalSource mega{std::move(srcs), combo, CombineMethod::ShrinkageMv};
    const std::vector<InstrumentId> dummy{kA, kB, kC};
    RollingPanel<4> blend_panel{std::span<const InstrumentId>{dummy}, 1U};
    const auto sv = mega.evaluate(blend_panel.view());
    EXPECT_TRUE(sv.has_value());
    const std::vector<f64> alpha{sv->values.begin(), sv->values.end()};

    OptimizerConfig ocfg;
    ocfg.risk_aversion = 1.0;
    ocfg.gross_leverage = 1.0;
    ocfg.name_cap = 0.7;
    const std::vector<f64> book =
        solve_optimizer_book(fx.view(), window, std::span<const u32>{group},
                             std::span<const f64>{alpha}, ocfg);
    EXPECT_EQ(book.size(), kInst);
    for (usize i = 0U; i < book.size(); ++i) {
      digest = hash_combine(digest, ord, static_cast<u64>(reb), static_cast<u64>(i),
                            std::bit_cast<u64>(book[i]));
      ++ord;
    }
  }
  return digest;
}

TEST(Phase4Integration, DeterminismHash_RepeatRun_IdenticalDigest) {
  constexpr usize kPeriods = 16U;
  constexpr usize kWindow = 6U;
  const std::vector<std::vector<f64>> pool = pool_rows(kPeriods, /*corrupt_from=*/kPeriods, 0.0);
  const std::vector<std::vector<f64>> alpha_sched{{0.9, -0.4, 0.2}, {0.3, 0.7, -0.5},
                                                  {-0.6, 0.1, 0.8}, {0.2, -0.3, 0.5}};
  const std::vector<std::vector<f64>> close = close_grid(kWindow + 4U, 3U, 0U, 0.0);

  const u64 d1 = layer_digest(pool, 1U, 8U, alpha_sched, close, kWindow);
  const u64 d2 = layer_digest(pool, 1U, 8U, alpha_sched, close, kWindow);

  EXPECT_EQ(d1, d2) << "the whole combined layer must replay byte-identically";
  EXPECT_NE(d1, 0U) << "fixture must produce a non-trivial book (else the test is vacuous)";
}

TEST(Phase4Integration, DeterminismHash_ReorderedPool_DigestFlips) {
  // NON-VACUITY: reorder the alphas in the pool -> the fitted weights re-index ->
  // the combined signal differs -> the digest flips.
  constexpr usize kPeriods = 16U;
  constexpr usize kWindow = 6U;
  std::vector<std::vector<f64>> pool = pool_rows(kPeriods, kPeriods, 0.0);
  const std::vector<std::vector<f64>> alpha_sched{{0.9, -0.4, 0.2}, {0.3, 0.7, -0.5},
                                                  {-0.6, 0.1, 0.8}, {0.2, -0.3, 0.5}};
  const std::vector<std::vector<f64>> close = close_grid(kWindow + 4U, 3U, 0U, 0.0);

  const u64 baseline = layer_digest(pool, 1U, 8U, alpha_sched, close, kWindow);
  std::swap(pool[0], pool[2]); // reorder the constituents
  const u64 reordered = layer_digest(pool, 1U, 8U, alpha_sched, close, kWindow);

  EXPECT_NE(baseline, reordered) << "reordering the pool must change the combined digest";
}

TEST(Phase4Integration, DeterminismHash_PerturbedPnl_DigestFlips) {
  // NON-VACUITY: perturb ONE in-window PnL value -> the fitted weights change ->
  // the combined signal differs -> the digest flips.
  constexpr usize kPeriods = 16U;
  constexpr usize kWindow = 6U;
  std::vector<std::vector<f64>> pool = pool_rows(kPeriods, kPeriods, 0.0);
  const std::vector<std::vector<f64>> alpha_sched{{0.9, -0.4, 0.2}, {0.3, 0.7, -0.5},
                                                  {-0.6, 0.1, 0.8}, {0.2, -0.3, 0.5}};
  const std::vector<std::vector<f64>> close = close_grid(kWindow + 4U, 3U, 0U, 0.0);

  const u64 baseline = layer_digest(pool, 1U, 8U, alpha_sched, close, kWindow);
  pool[1][3] += 0.05; // perturb one in-fit-window PnL cell
  const u64 perturbed = layer_digest(pool, 1U, 8U, alpha_sched, close, kWindow);

  EXPECT_NE(baseline, perturbed) << "a changed in-window PnL must change the combined digest";
}

// ===========================================================================
//  (C) WALK-FORWARD COMBINED BACKTEST (deterministic + cost-honest).
//
//  gate -> store -> rolling combiner.fit -> CombinedSignalSource -> the REAL
//  BacktestLoop -> ExecutionSimulator -> Portfolio, over a synthetic panel with a
//  delisted symbol + NaN gaps. The optimizer + factor model run in the PARALLEL
//  per-rebalance harness (caveat C). Asserts determinism + cost-honesty.
// ===========================================================================

// Cost stacks (mirror backtest_integration_test.cpp).
struct CostConfig {
  FillCfg fill{};
  SlippageCfg slip{};
  ImpactCfg impact{};
  CommissionCfg comm{};
  LatencyCfg latency{};
  VolumeCapCfg cap{/*volume_limit=*/1.0};
  InstrumentStats stats{};
};

[[nodiscard]] CostConfig frictionless() {
  CostConfig cc;
  cc.slip = SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0};
  cc.impact = ImpactCfg{0.0, 0.5, 0.0};
  cc.comm = CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0};
  cc.stats = InstrumentStats{};
  return cc;
}

// A real cost stack WITHOUT permanent impact (gamma = 0) — the unambiguous monotone
// drain (worse fill price + commission), exactly as backtest_integration_test's
// costly_no_perm isolates it for the "equity strictly lower" assertion.
[[nodiscard]] CostConfig costly_no_perm() {
  CostConfig cc;
  cc.slip = SlippageCfg{SlippageMode::VolumeShare, /*k=*/0.1, 0.0, /*cap_volshare=*/0.025, 0.10};
  cc.impact = ImpactCfg{/*Y=*/1.0, /*delta=*/0.5, /*gamma=*/0.0};
  cc.comm = CommissionCfg{CommissionMode::PerShare, /*per_share=*/0.005, /*min_fee=*/1.0,
                          /*max_pct=*/0.005, 0.0};
  cc.stats = InstrumentStats{/*adv=*/1.0e6, /*sigma=*/0.02, /*spread=*/0.05};
  return cc;
}

// One bar for `symbol` at slice k (ts == knowledge_ts == k).
[[nodiscard]] BarRow bar_row(const Symbol &symbol, i64 k, i64 price, i64 vol = 1'000'000,
                             bool delisted_final = false) {
  Bar bar{};
  bar.ts = ts(k);
  bar.open = Price::from_int(price);
  bar.high = Price::from_int(price);
  bar.low = Price::from_int(price);
  bar.close = Price::from_int(price);
  bar.volume = Quantity::from_int(vol);
  return BarRow{symbol, bar, ts(k), delisted_final};
}

struct WalkOut {
  BacktestResult result;
  f64 equity = 0.0;
};

// Drive the FULL real loop with a CombinedSignalSource over two scripted
// constituents whose EqualWeight gross-1 blend reproduces a fixed [3,2,1] ramp
// (so the loop's WeightPolicy lands a dollar-neutral A=+0.5/C=-0.5 book). C is a
// DELISTED symbol (its bars stop at `c_last`); A/B carry the full run. The risk
// optimizer + factor model run in a parallel harness once on the rolling window.
[[nodiscard]] WalkOut run_walk_forward(int n_bars, int c_last, const CostConfig &cc) {
  std::vector<InstrumentId> universe{kA, kB, kC};

  std::vector<BarRow> a;
  std::vector<BarRow> b;
  std::vector<BarRow> c;
  for (int k = 1; k <= n_bars; ++k) {
    a.push_back(bar_row(kA, k, 100));
    b.push_back(bar_row(kB, k, 50));
    if (k < c_last) {
      c.push_back(bar_row(kC, k, 200));
    } else if (k == c_last) {
      c.push_back(bar_row(kC, k, 200, 1'000'000, /*delisted_final=*/true)); // C delists here
    }
    // k > c_last: C is absent (a NaN gap in the cross-section -> survivorship).
  }
  std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}, std::span<const BarRow>{b},
                                             std::span<const BarRow>{c}};
  const std::span<const std::span<const BarRow>> sources{spans};

  auto bus = std::make_unique<Bus>();
  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};

  std::vector<InstrumentStats> stats(universe.size(), cc.stats);
  RollingPanel<kCap> panel{std::span<const InstrumentId>{universe}, /*max_lookback=*/1U};

  // Two scripted constituents; an EqualWeight gross-1 blend of two identical
  // [3,2,1] schedules == [3,2,1] every rebalance (combine_combined_source_test's
  // capstone trick), so the mega-alpha reproduces the deterministic Phase-2 ramp.
  const std::vector<std::vector<f64>> sched(static_cast<usize>(n_bars),
                                            std::vector<f64>{3.0, 2.0, 1.0});
  ScriptedSignalSource c0{sched, /*universe_size=*/3U, /*max_lookback=*/1U};
  ScriptedSignalSource c1{sched, /*universe_size=*/3U, /*max_lookback=*/1U};
  std::vector<ISignalSource *> constituents{&c0, &c1};
  CombinedSignalSource mega{std::move(constituents), Combination{{0.5, 0.5}, 0U, 0U},
                            CombineMethod::EqualWeight};

  const WeightPolicy policy{};
  ExecutionSimulator sim{cc.fill, cc.slip, cc.impact, cc.comm, cc.latency, cc.cap};
  Portfolio portfolio{Decimal::from_int(100'000), std::span<const InstrumentId>{universe}};
  Market market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}};
  const Schedule schedule{1U};

  BacktestLoop<kCap> loop{feed,   clock, *bus,      panel,  mega,
                          policy, sim,   portfolio, market, Universe{universe},
                          schedule};
  WalkOut out;
  out.result = loop.run();
  out.equity = portfolio.equity();
  return out;
}

// Fold a BacktestResult's ordered (date, instrument, weight/qty/equity-bits) stream
// into a digest via hash_combine — the NaN-tolerant whole-run determinism witness
// (mirrors backtest_integration_test::digest_full).
[[nodiscard]] u64 run_digest(const BacktestResult &r) {
  u64 d = 0U;
  u64 ord = 0U;
  for (const FillPayload &f : r.fills) {
    d = hash_combine(d, ord, static_cast<u64>(f.id.id), static_cast<u64>(f.qty),
                     static_cast<u64>(f.price.raw()), static_cast<u64>(f.t.unix_nanos()));
    ++ord;
  }
  for (const auto &s : r.equity_curve) {
    d = hash_combine(d, ord, static_cast<u64>(s.t.unix_nanos()), std::bit_cast<u64>(s.equity));
    ++ord;
  }
  return hash_combine(d, std::bit_cast<u64>(r.final_equity), static_cast<u64>(r.slices),
                      static_cast<u64>(r.rebalances));
}

TEST(Phase4Integration, WalkForward_RepeatRun_IdenticalEquityAndDigest) {
  const WalkOut r1 = run_walk_forward(/*n_bars=*/10, /*c_last=*/4, costly_no_perm());
  const WalkOut r2 = run_walk_forward(/*n_bars=*/10, /*c_last=*/4, costly_no_perm());

  // Exact bit-equality on the final equity AND the whole-run digest.
  EXPECT_EQ(r1.equity, r2.equity) << "walk-forward must replay byte-identically";
  const u64 d1 = run_digest(r1.result);
  EXPECT_EQ(d1, run_digest(r2.result)) << "identical feed -> identical digest";
  EXPECT_NE(d1, 0U) << "the run must produce fills/samples (else vacuous)";

  // The delisted C traded to its final bar and was not retroactively removed.
  EXPECT_EQ(r1.result.slices, 10U);
  for (const FillPayload &f : r1.result.fills) {
    if (f.id.id == kC.id) {
      EXPECT_LE(f.t.unix_nanos(), 4) << "no fill for delisted C after its final bar";
    }
  }
}

TEST(Phase4Integration, WalkForward_CostsOff_RecoversFrictionlessEquity) {
  // COST-HONESTY: the frictionless dollar-neutral book preserves equity exactly,
  // and turning every cost off recovers that exact mark-to-market equity.
  const WalkOut frictionless_run = run_walk_forward(/*n_bars=*/10, /*c_last=*/4, frictionless());
  EXPECT_NEAR(frictionless_run.equity, 100'000.0, 1e-6)
      << "frictionless dollar-neutral -> equity exactly preserved";
}

TEST(Phase4Integration, WalkForward_CostsOn_EquityStrictlyBelowFrictionless) {
  // COST-HONESTY: the same combined book pays to trade with costs ON -> the costly
  // run's equity is STRICTLY below the frictionless recovery.
  const WalkOut frictionless_run = run_walk_forward(/*n_bars=*/10, /*c_last=*/4, frictionless());
  const WalkOut costly_run = run_walk_forward(/*n_bars=*/10, /*c_last=*/4, costly_no_perm());

  EXPECT_LT(costly_run.equity, frictionless_run.equity)
      << "spread + slippage + temp impact + commission must drain equity below frictionless";
}

} // namespace
