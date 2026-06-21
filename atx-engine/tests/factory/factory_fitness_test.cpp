// atx::engine::factory — pool-aware fitness tests (S3-4, suite FactoryFitness).
//
// Covers the plan's verbatim Task-S3-4 list (§4.6 + §0.6 / §0.7 / §0.8):
//   * CorrToPoolMeanAndMax                       — the marginal-corr helper (§0.6)
//   * PrefersDiversifyingWeakAlphaOverRedundantStrong — THE WQ thesis (F7 / spec exit)
//   * FitnessIsOosOnly                           — F3: no look-ahead (CPCV TEST only)
//   * DeflationShrinksWithTrialCount             — F4: higher N -> lower deflated dsr
//
// The fixtures build streams + a pool + panels DIRECTLY (public members), the S2
// precedent — the test controls the data so the WQ thesis holds by construction.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/validation/bias_audit.hpp"

namespace atxtest_factory_fitness_test {

using atx::f64;
using atx::usize;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::corr_to_pool;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::FitnessReport;
using atx::engine::factory::Genome;
using atx::engine::factory::pool_aware_fitness;
using atx::engine::factory::Reduce;
using atx::engine::factory::detail::split_half_sharpe;
using atx::engine::factory::detail::SplitHalf;
using atx::engine::WeightPolicy;

// ---- builders ---------------------------------------------------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Genome make_genome(std::string_view src, Library &lib) {
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// Build an owned Panel from a date-major {field -> column} set. Each column is
// dates*instruments cells. Empty universe == all-in-universe.
[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols,
                               std::vector<std::uint8_t> universe = {}) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), std::move(universe));
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// A pool with one member whose PnL stream is `pnl` (positions are a flat
// dollar-neutral cross-section so compute_metrics is well-formed; we only ever
// read the member PnL for corr-to-pool, so the positions content is immaterial).
[[nodiscard]] AlphaStore make_pool_from(const std::vector<std::vector<f64>> &pnls,
                                        usize n_instruments) {
  AlphaStore pool;
  for (const std::vector<f64> &pnl : pnls) {
    const std::vector<f64> pos(pnl.size() * n_instruments, 0.0);
    const auto id = pool.insert(nullptr, pnl, pos, AlphaMetrics{});
    EXPECT_TRUE(id.has_value());
  }
  return pool;
}

// Extract a candidate's full causal PnL stream over `panel` (alpha 0).
[[nodiscard]] std::vector<f64> full_pnl_of(const Genome &g, const Panel &panel,
                                           const WeightPolicy &policy,
                                           const ExecutionSimulator &sim) {
  auto prog = atx::engine::alpha::compile(g.ast, g.analysis);
  EXPECT_TRUE(prog.has_value());
  atx::engine::alpha::Engine engine{panel};
  auto ss = engine.evaluate(*prog);
  EXPECT_TRUE(ss.has_value());
  auto strm = atx::engine::alpha::extract_streams(*ss, policy, panel, sim);
  EXPECT_TRUE(strm.has_value());
  const std::span<const f64> p = strm->pnl(0);
  return std::vector<f64>(p.begin(), p.end());
}

constexpr f64 kEps = 1e-9;

// A tiny deterministic LCG -> uniform(-1, 1). Used to give the synthetic price
// paths REALISTIC idiosyncratic noise so the realized alpha PnL has genuine
// variance (a noiseless monotone path yields a near-zero-variance PnL whose
// Sharpe diverges — not a meaningful fixture). Pure / reproducible / no RNG dep.
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U; // 53 high bits
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U); // [0,1)
    return 2.0 * u - 1.0;                                               // (-1,1)
  }
};

// Build a noisy close matrix [dates*insts]: each instrument follows a random
// walk with a small PER-INSTRUMENT persistent drift `drift[j]`, so rank(close)
// has a genuine (but noisy) momentum edge — a realistic, finite-Sharpe alpha.
[[nodiscard]] std::vector<f64> noisy_close(usize dates, usize insts,
                                           const std::vector<f64> &drift, std::uint64_t seed,
                                           f64 noise_amp) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + noise_amp * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// =============================================================================
//  CorrToPoolMeanAndMax — the marginal-corr helper (§0.6).
// =============================================================================
TEST(FactoryFitness, CorrToPoolMeanAndMax) {
  // Two known streams: a is the candidate; b is anti-/weakly-related.
  const std::vector<f64> pnl_a{0.0, 0.10, -0.05, 0.08, -0.02, 0.06, 0.01, -0.03};
  const std::vector<f64> pnl_b{0.0, -0.02, 0.07, -0.06, 0.05, -0.01, -0.04, 0.03};
  AlphaStore pool = make_pool_from({pnl_a, pnl_b}, /*n_instruments*/ 2);

  // a is identical to pool member 0 -> max |corr| == 1.
  EXPECT_NEAR(corr_to_pool(std::span<const f64>{pnl_a}, pool, Reduce::Max), 1.0, kEps);
  // Max >= Mean always (the gate-consistent screen dominates the mean discount).
  EXPECT_GE(corr_to_pool(std::span<const f64>{pnl_a}, pool, Reduce::Max),
            corr_to_pool(std::span<const f64>{pnl_a}, pool, Reduce::Mean));

  // Empty pool -> 0 (maximally diversifying against nothing).
  const AlphaStore empty;
  EXPECT_NEAR(corr_to_pool(std::span<const f64>{pnl_a}, empty, Reduce::Max), 0.0, kEps);
  EXPECT_NEAR(corr_to_pool(std::span<const f64>{pnl_a}, empty, Reduce::Mean), 0.0, kEps);
}

// =============================================================================
//  PrefersDiversifyingWeakAlphaOverRedundantStrong — THE WQ thesis (F7).
// =============================================================================
//
// Construction: a 2-field, 2-instrument panel over many dates.
//   * field "close" drives returns AND candidate A (`rank(close)`): A's weights
//     long the up-mover -> a STRONG, consistent standalone PnL.
//   * field "sig" drives candidate B (`rank(sig)`): sig is engineered so B's
//     realized PnL is WEAKER standalone but ~uncorrelated with A's stream.
// The pool holds a member equal to A's own OOS stream, so corr(A, pool) ~ 1 and
// corr(B, pool) ~ 0. The diversification discount must flip the ranking:
// EXPECT_GT(fb.raw, fa.raw) — the factory prefers the diversifier.
TEST(FactoryFitness, PrefersDiversifyingWeakAlphaOverRedundantStrong) {
  constexpr usize kDates = 96;
  constexpr usize kInsts = 6;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();

  // Two orthogonal alpha sources baked into ONE close path so BOTH candidates are
  // profitable yet LOW-correlated (a momentum book and a 1-day-reversal book over
  // the same universe are classically near-uncorrelated):
  //   * a PERSISTENT per-instrument drift gradient (a momentum edge) — captured
  //     by candidate A = rank(close): A is the STRONG standalone alpha.
  //   * a TRANSIENT mean-reverting (alternating-sign) wiggle per instrument — a
  //     reversal edge captured by candidate B = rank(rev), rev[t] = −ret[t]
  //     (long yesterday's loser). B is WEAKER standalone but uncorrelated with A.
  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.008 - 0.0032 * static_cast<f64>(j); // +0.8% .. -0.8% momentum gradient (strong)
  }
  std::vector<f64> close(kDates * kInsts);
  std::vector<f64> rev(kDates * kInsts, 0.0); // rev[t] = -(ret over [t-1,t]); 0 at t=0
  std::vector<f64> px(kInsts, 100.0);
  std::vector<f64> wiggle(kInsts, 0.0); // current mean-reverting displacement
  Lcg rng{0x5EEDu};
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kInsts; ++j) {
      // Mean-reverting transient: a fresh (small) shock minus a strong pull back
      // to 0 (so the wiggle alternates sign period-to-period -> a real but WEAK
      // reversal edge, kept smaller than the momentum drift above).
      const f64 shock = 0.006 * rng.next();
      const f64 new_wiggle = shock - 0.9 * wiggle[j];
      const f64 ret = drift[j] + new_wiggle; // momentum drift + reversal wiggle
      wiggle[j] = new_wiggle;
      const f64 prev = px[j];
      px[j] = prev * (1.0 + ret);
      close[t * kInsts + j] = px[j];
      if (t > 0) {
        rev[t * kInsts + j] = -(px[j] / prev - 1.0); // long the recent loser
      }
    }
  }
  const Panel panel = make_panel(kDates, kInsts, {"close", "rev"}, {close, rev});

  Genome a = make_genome("rank(close)", lib); // strong (momentum), redundant
  Genome b = make_genome("rank(rev)", lib);   // weaker (reversal), diversifying

  // Build a pool whose single member IS A's own full causal PnL stream, so
  // corr(A, pool) ~ 1 (redundant) and corr(B, pool) ~ 0 (diversifying).
  AlphaStore pool = make_pool_from({full_pnl_of(a, panel, policy, sim)}, kInsts);

  FitnessCfg cfg;
  const auto fa = pool_aware_fitness(a, pool, panel, policy, sim, cfg);
  const auto fb = pool_aware_fitness(b, pool, panel, policy, sim, cfg);
  ASSERT_TRUE(fa.has_value()) << (fa ? "" : fa.error().message());
  ASSERT_TRUE(fb.has_value()) << (fb ? "" : fb.error().message());

  // A is strong but redundant (corr-to-pool ~ 1 -> diversify ~ 0); B is weaker
  // standalone but uncorrelated with the pool (diversify ~ 1). The discount must
  // flip the ranking: the factory prefers the diversifier.
  EXPECT_GT(fb->diversify, fa->diversify) << "B must diversify the pool more than A";
  EXPECT_GT(fb->raw, fa->raw) << "the factory prefers the diversifier (WQ thesis)";
}

// =============================================================================
//  FitnessIsOosOnly — F3: no look-ahead (perturbing the LAST date does not
//  change an earlier OOS fold's fitness).
// =============================================================================
TEST(FactoryFitness, FitnessIsOosOnly) {
  constexpr usize kDates = 48;
  constexpr usize kInsts = 6;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();

  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.004 - 0.0016 * static_cast<f64>(j);
  }

  // recompute(n): the candidate's realized PnL stream seen over the FIRST n dates
  // ONLY. The fitness's load-bearing causal object is this stream — every fold's
  // OOS metric is computed by SLICING it, so if the stream is truncation-invariant
  // (an earlier period's PnL is identical whether the VM saw n or fewer later
  // dates), no later date can leak into an earlier OOS fold's fitness (F3). A
  // look-ahead leak would make recompute(full)[i] != recompute(cut)[i] for some
  // i < cut, which check_no_lookahead catches with an EXACT bit-for-bit compare.
  // The noisy close is generated date-by-date from a stable seed, so the FIRST n
  // rows are identical across truncations (a causal generator), isolating the
  // VM/streams causality as the property under test.
  const std::vector<f64> close_full = noisy_close(kDates, kInsts, drift, /*seed*/ 0xBEEFu, 0.010);
  auto recompute = [&](usize n) -> std::vector<f64> {
    std::vector<f64> close(close_full.begin(),
                           close_full.begin() + static_cast<std::ptrdiff_t>(n * kInsts));
    const Panel panel = make_panel(n, kInsts, {"close"}, {close});
    Genome g = make_genome("rank(close)", lib);
    return full_pnl_of(g, panel, policy, sim);
  };

  EXPECT_TRUE(atx::engine::validation::check_no_lookahead(kDates, kDates - 1, recompute));
}

// =============================================================================
//  DeflationShrinksWithTrialCount — F4: more trials -> lower deflated dsr.
// =============================================================================
TEST(FactoryFitness, DeflationShrinksWithTrialCount) {
  constexpr usize kDates = 64;
  constexpr usize kInsts = 6;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();
  const AlphaStore empty;

  // A noisy momentum panel -> the candidate has a MODERATE, finite OOS Sharpe
  // (the deflation lever only bites on a finite estimate; a degenerate near-zero-
  // variance PnL would diverge the Sharpe and poison the moments).
  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.004 - 0.0016 * static_cast<f64>(j);
  }
  const std::vector<f64> close = noisy_close(kDates, kInsts, drift, /*seed*/ 0xC0DEu, 0.010);
  const Panel panel = make_panel(kDates, kInsts, {"close"}, {close});
  Genome cand = make_genome("rank(close)", lib);

  FitnessCfg cfg1;
  cfg1.trial_count = 1;
  FitnessCfg cfg1k;
  cfg1k.trial_count = 1000;

  const auto d1 = pool_aware_fitness(cand, empty, panel, policy, sim, cfg1);
  const auto d1k = pool_aware_fitness(cand, empty, panel, policy, sim, cfg1k);
  ASSERT_TRUE(d1.has_value());
  ASSERT_TRUE(d1k.has_value());
  // Same alpha, more trials -> the selection benchmark SR*_N rises -> deflated
  // dsr falls (F4: the anti-snooping lever). N=1 is "no selection" (SR* = 0).
  EXPECT_GT(d1->dsr, d1k->dsr) << "same alpha, more trials -> lower deflated fitness (F4)";
}


// =============================================================================
//  W4a RobustFactorActivatedByWeakPanel — the §0.8 robustness re-eval (Deliverable 2).
// =============================================================================
//
// With NO weak panel (the default), robust == 1.0 and raw == wq*diversify EXACTLY as
// today. With a weak SUB-UNIVERSE panel (the full close path, a restricted universe
// mask), the robustness re-eval activates: robust = clamp(wq_on(weak)/wq, 0, 1) is in
// [0, 1] and raw == wq*diversify*robust holds. Same inputs -> identical robust (a
// pure, deterministic re-eval). The weak universe MASKS OUT a subset of instruments
// so the candidate's WQ genuinely differs across universes (robust strictly < 1 when
// the masked universe is weaker for the candidate).
TEST(FactoryFitness, RobustFactorActivatedByWeakPanel) {
  constexpr usize kDates = 96;
  constexpr usize kInsts = 8;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();
  const AlphaStore empty;

  // A momentum close path with a per-instrument drift gradient -> rank(close) has a
  // genuine, finite-Sharpe edge over the FULL universe.
  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.010 - 0.0026 * static_cast<f64>(j);
  }
  const std::vector<f64> close = noisy_close(kDates, kInsts, drift, /*seed*/ 0x1234u, 0.008);
  const Panel full = make_panel(kDates, kInsts, {"close"}, {close});

  // Weak sub-universe: the SAME close columns, but only EVEN-indexed instruments are
  // in-universe (a deterministic ~50% instrument sub-sample) — a held-out universe
  // the candidate is re-scored on. NaN is not needed: the universe mask alone
  // restricts the cross-section the streams/WQ see.
  std::vector<std::uint8_t> weak_univ(kDates * kInsts, 0u);
  for (usize d = 0; d < kDates; ++d) {
    for (usize i = 0; i < kInsts; ++i) {
      weak_univ[d * kInsts + i] = (i % 2u == 0u) ? 1u : 0u; // even instruments only
    }
  }
  const Panel weak = make_panel(kDates, kInsts, {"close"}, {close}, weak_univ);

  Genome cand = make_genome("rank(close)", lib);

  // (1) Default (nullptr weak panel): robust == 1.0, raw == wq*diversify exactly.
  const auto f_off = pool_aware_fitness(cand, empty, full, policy, sim, FitnessCfg{});
  ASSERT_TRUE(f_off.has_value()) << (f_off ? "" : f_off.error().message());
  EXPECT_EQ(f_off->robust, 1.0) << "no weak panel -> robust is the inert constant 1.0";
  EXPECT_NEAR(f_off->raw, f_off->wq * f_off->diversify, kEps) << "raw == wq*diversify when robust==1";

  // (2) Weak panel supplied: robust is the §0.8 ratio in [0, 1]; raw folds it in.
  const auto f_on =
      pool_aware_fitness(cand, empty, full, policy, sim, FitnessCfg{}, /*weak_panel=*/&weak);
  ASSERT_TRUE(f_on.has_value()) << (f_on ? "" : f_on.error().message());
  EXPECT_GE(f_on->robust, 0.0);
  EXPECT_LE(f_on->robust, 1.0);
  EXPECT_NE(f_on->robust, 1.0) << "the candidate's WQ differs across universes -> robust != 1";
  EXPECT_NEAR(f_on->raw, f_on->wq * f_on->diversify * f_on->robust, kEps)
      << "raw == wq*diversify*robust with the weak factor active";

  // (3) Determinism: same seed + same weak config -> identical robust (pure re-eval).
  const auto f_on2 =
      pool_aware_fitness(cand, empty, full, policy, sim, FitnessCfg{}, /*weak_panel=*/&weak);
  ASSERT_TRUE(f_on2.has_value());
  EXPECT_EQ(f_on->robust, f_on2->robust) << "same inputs -> byte-identical robust (F1)";
  EXPECT_EQ(f_on->raw, f_on2->raw);
}

// =============================================================================
//  W4a SplitHalfSharpeStrongH1NegativeH2 — the split-sample stability rule (unit).
// =============================================================================
//
// A HAND-BUILT moment stream (the OOS PnL with the structural index-0 zero already
// dropped — split_half_sharpe receives the deflation-moment span directly) with a
// STRONG positive H1 and a mirror-image NEGATIVE H2. Each half has the same |mean|
// and the same population std, so the per-period Sharpes are exact negatives of one
// another. With a positive full-sample sign, H1 matches (+) but H2 does NOT (−) ⇒
// split_stable == false (the single-regime artifact the gate is designed to catch).
TEST(FactoryFitness, SplitHalfSharpeStrongH1NegativeH2) {
  // H1 = {0.08, 0.12, 0.10, 0.10}: mean 0.10, popstd sqrt(2e-4) = 0.0141421356...
  //   per-period Sharpe = 0.10 / 0.0141421356 = 7.0710678...
  // H2 = the exact negation -> Sharpe = -7.0710678...
  const std::vector<f64> moments{0.08, 0.12, 0.10, 0.10, -0.08, -0.12, -0.10, -0.10};
  const f64 expect_h1 = 0.10 / std::sqrt(2.0e-4);

  // full_sign = +1 (a positive full-sample Sharpe): H1 (+) matches, H2 (−) does not.
  const SplitHalf s = split_half_sharpe(std::span<const f64>{moments}, /*full_sign=*/1.0);
  EXPECT_NEAR(s.sharpe_h1, expect_h1, 1e-9) << "H1 per-period Sharpe must match hand calc";
  EXPECT_NEAR(s.sharpe_h2, -expect_h1, 1e-9) << "H2 per-period Sharpe must match hand calc";
  EXPECT_FALSE(s.stable) << "strong-H1 / negative-H2 is a single-regime artifact (not stable)";

  // Odd length -> FLOOR midpoint: T=7, mid=3 -> H1 has 3 periods, H2 has 4.
  // EXACTLY-representable constants (mean is exact -> every deviation is exactly 0
  // -> popstd is exactly 0 -> Sharpe 0 by the subset_sharpe std==0 convention).
  const std::vector<f64> odd{1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0}; // H1 const, H2 const
  const SplitHalf so = split_half_sharpe(std::span<const f64>{odd}, /*full_sign=*/1.0);
  EXPECT_EQ(so.sharpe_h1, 0.0) << "constant H1 -> std 0 -> Sharpe 0 (subset_sharpe convention)";
  EXPECT_EQ(so.sharpe_h2, 0.0) << "constant H2 -> std 0 -> Sharpe 0";
  EXPECT_FALSE(so.stable) << "both halves 0 cannot match a positive full-sample sign";

  // Stable case: both halves positive with variance, positive full sign -> stable.
  const std::vector<f64> stable{0.08, 0.12, 0.09, 0.11};
  const SplitHalf ss = split_half_sharpe(std::span<const f64>{stable}, /*full_sign=*/1.0);
  EXPECT_GT(ss.sharpe_h1, 0.0);
  EXPECT_GT(ss.sharpe_h2, 0.0);
  EXPECT_TRUE(ss.stable) << "both halves share the positive full-sample sign -> stable";
}

}  // namespace atxtest_factory_fitness_test
