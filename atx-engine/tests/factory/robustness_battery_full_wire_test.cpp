// robustness_battery_full_wire_test.cpp — S5-3: expose the 3 previously-
// unreachable eval::RobustnessBattery checks (sub_universe / alt_neutralization
// / param_perturbation) at admission, alongside the p8 final-wave noise_control
// check robustness_battery_wire_test.cpp already covers.
//
// SCOPE: this file proves EACH of the 3 new checks fires genuinely (rejects a
// fixture engineered to trip it, admits a fixture engineered NOT to) through the
// REAL production path (detail::robustness_battery_passes — the same function
// both admit sites call), plus the full-wire byte-identity / twice-run /
// seq==parallel invariants at the FactoryConfig/mine_into level. Per this file's
// own naming convention (avoids the Unity-batch unnamed-namespace collision the
// p8 Item-1 ledger flagged), every helper lives in THIS file's own namespace.
//
// HONESTY NOTE (sub_universe / alt_neutralization fixture design): both checks'
// Reevaluator branches only perturb a NEWLY-DERIVED input (the ADV-ranked
// universe mask; a synthetic "__s5_group" field appended, never overwriting an
// existing field) — so a fixture that GENUINELY exercises the check must engineer
// the candidate's own edge to be causally concentrated in exactly the slice that
// perturbation removes/scrambles (an illiquid single-name edge for sub_universe;
// a return-drift-vs-liquidity-bucket correlation for alt_neutralization), and a
// second fixture whose edge is independent of that slice (a broad multi-name edge;
// an idiosyncratic close-only signal) to prove the check does not universally
// reject. param_perturbation's fixture instead exploits `scale(x, a)`'s OWN
// documented arithmetic (`out = x * (a / sum|x|)`, oracle.cpp's cs_scale): a
// POSITIVE target norm preserves rank(close)'s cross-sectional order; a NEGATIVE
// one reverses it. Widening BatteryConfig::param_perturbation_band past 1.0 makes
// the seeded jitter draws straddle zero, so some draws preserve the edge and
// others invert it — a real, mechanically-grounded "knife edge" without hand-
// tuned VM numerics; the default (narrow, sign-preserving) band is the "stable
// candidate" counterpart on the IDENTICAL genome.

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/eval/robustness_battery.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/factory.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/pool_view.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/process_executor.hpp"

namespace atxtest_robustness_battery_full_wire {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
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
using atx::engine::factory::AlphaStorePool;
using atx::engine::factory::Factory;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::FactoryReport;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::pool_aware_fitness;
namespace eval = atx::engine::eval;
namespace lib = atx::engine::library;
namespace parallel = atx::engine::parallel;

// ---- builders --------------------------------------------------------------

[[nodiscard]] ExecutionSimulator fw_frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel fw_make_panel(usize dates, usize insts, std::vector<std::string> fields,
                                  std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

[[nodiscard]] Genome fw_make_genome(std::string_view src, Library &lib) {
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

// =============================================================================
//  sub_universe fixtures
// =============================================================================

// Illiquid-edge panel (6 insts, 60 dates): instrument 0 carries the WHOLE edge
// (a real, deterministic uptrend, held FAR above every other name's price band
// so its rank stays fixed at the top for the whole sample -- its contribution
// decouples cleanly from the other 5 names' internal churn). Instruments 1,2,3
// (the top-3-by-ADV names the restriction KEEPS) rotate through a fixed 3-day
// cycle of offsets {+amp, 0, -amp} (amp slowly growing ~2%->4%) with a
// per-instrument phase shift -- a DETERMINISTIC anti-momentum construction
// (never a seeded/luck-dependent coin flip): whichever of the 3 is currently
// highest-offset ALWAYS steps down next period, and whichever is currently
// lowest-offset ALWAYS jumps back up to the top next period, so a rank-chasing
// (long-the-current-leader) book realizes a NEGATIVE pnl every single period,
// by construction -- a genuinely losing (not merely flat/no-signal) restricted-
// universe edge, comfortably below the 0.5 "no information" DSR floor a zero-
// variance stream would otherwise pin to. Instruments 4,5 are flat, held below
// 1,2,3's band (never interfere with their ranking). "volume" ranks instrument 0
// lowest (illiquid) and 1,2,3 highest -> the default top-N (insts/2 = 3) keeps
// exactly {1,2,3} and walls off instrument 0, the only name with a real
// (positive) edge.
[[nodiscard]] Panel fw_illiquid_edge_panel() {
  constexpr usize kDates = 60;
  constexpr usize kInsts = 6;
  std::vector<f64> close(kDates * kInsts);
  f64 px0 = 100000.0; // starts, and by construction stays, far above every other band
  const std::array<f64, 3> cycle_sign{1.0, 0.0, -1.0}; // rotates every 3 days, phase-shifted per k
  const std::array<f64, kInsts> flat_level{0.0, 0.0, 0.0, 0.0, 20.0, 10.0}; // 4,5 only
  for (usize t = 0; t < kDates; ++t) {
    const f64 osc = (t % 2U == 0U) ? 0.012 : -0.012;
    px0 *= (1.0 + 0.05 + osc);
    close[t * kInsts + 0] = px0;
    // A SLOWLY GROWING offset amplitude (2% -> ~4%) preserves the anti-momentum
    // SIGN structure every transition (still: currently-HIGH always steps down,
    // currently-LOW always wraps back up to HIGH) while breaking the exact
    // period-3 repetition a CONSTANT amplitude would otherwise produce -- without
    // this, the restricted-universe book's realized return is IDENTICAL every
    // single day (zero cross-time variance), which floors DSR to the same 0.5
    // "no information" sentinel a flat/no-signal stream produces, regardless of
    // the (negative) mean -- see this function's own header note.
    const f64 amp = 0.02 + 0.0004 * static_cast<f64>(t);
    for (usize k = 0; k < 3U; ++k) { // instruments 1,2,3
      const usize r = (t + k) % 3U;
      close[t * kInsts + (1U + k)] = 100.0 * (1.0 + cycle_sign[r] * amp);
    }
    close[t * kInsts + 4] = flat_level[4];
    close[t * kInsts + 5] = flat_level[5];
  }
  std::vector<f64> volume(kDates * kInsts);
  const std::array<f64, kInsts> vol_level{100.0, 9000.0, 8000.0, 7000.0, 6000.0, 5000.0};
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kInsts; ++j) {
      volume[t * kInsts + j] = vol_level[j];
    }
  }
  return fw_make_panel(kDates, kInsts, {"close", "volume"}, {close, volume});
}

// Broad-edge panel (6 insts, 60 dates): every instrument carries a real,
// distinct-magnitude drift (differentiated momentum, all positive but
// monotonically decreasing by instrument index) plus a shared oscillation for
// genuine variance. Volumes are set so the top-3-by-ADV subset (instruments
// 0,1,2 -- the strongest-drift names) still spans a real, differentiated edge.
[[nodiscard]] Panel fw_broad_edge_panel() {
  constexpr usize kDates = 60;
  constexpr usize kInsts = 6;
  std::array<f64, kInsts> drift{};
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.006 - 0.001 * static_cast<f64>(j);
  }
  std::vector<f64> close(kDates * kInsts);
  std::array<f64, kInsts> px{};
  px.fill(100.0);
  for (usize t = 0; t < kDates; ++t) {
    const f64 osc = (t % 2U == 0U) ? 0.012 : -0.012;
    for (usize j = 0; j < kInsts; ++j) {
      px[j] *= (1.0 + drift[j] + osc);
      close[t * kInsts + j] = px[j];
    }
  }
  std::vector<f64> volume(kDates * kInsts);
  const std::array<f64, kInsts> vol_level{9000.0, 8000.0, 7000.0, 6000.0, 5000.0, 4000.0};
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kInsts; ++j) {
      volume[t * kInsts + j] = vol_level[j];
    }
  }
  return fw_make_panel(kDates, kInsts, {"close", "volume"}, {close, volume});
}

// =============================================================================
//  (b) SubUniverseRejectsIlliquidEdge — the whole edge lives on the single
//  LOWEST-ADV instrument; the default top-N-by-ADV universe walls it off, so the
//  restricted-universe re-eval collapses to (near) zero -- REJECTED. Cross-checks
//  the exact edge-collapse numbers by independently rebuilding the SAME
//  restricted-universe Panel the production Reevaluator would (a duplicate,
//  test-local computation -- not a call into the production lambda) and
//  re-scoring `cand` on it directly, so the report can cite real dsr numbers.
// =============================================================================
TEST(RobustnessBatteryFullWire, SubUniverseRejectsIlliquidEdge) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = fw_illiquid_edge_panel();
  Genome cand = fw_make_genome("rank(close)", lib);
  const FitnessCfg admit_fit{};

  const auto base_fit = pool_aware_fitness(cand, pool, panel, policy, sim, admit_fit);
  ASSERT_TRUE(base_fit.has_value()) << (base_fit ? "" : base_fit.error().message());
  const f64 base_edge = base_fit->dsr;
  ASSERT_GT(base_edge, 0.0) << "fixture must give a positive base dsr (the illiquid "
                               "instrument's real uptrend) for the collapse check below to bite";

  // Independent cross-check: rebuild the top-3-by-ADV-restricted universe by hand
  // (instruments 1,2,3 -- the three highest-volume names, whose own anti-momentum
  // churn nets negative in isolation; instrument 0, the only name with a REAL
  // positive edge, is walled off) and re-score directly.
  {
    constexpr usize kInsts = 6;
    constexpr usize kDates = 60;
    std::vector<std::uint8_t> universe(kDates * kInsts, 0U);
    const std::array<usize, 3> kept{1, 2, 3};
    for (usize t = 0; t < kDates; ++t) {
      for (const usize i : kept) {
        universe[t * kInsts + i] = 1U;
      }
    }
    std::vector<f64> close_col(panel.field_all(*panel.field_id("close")).begin(),
                               panel.field_all(*panel.field_id("close")).end());
    std::vector<f64> vol_col(panel.field_all(*panel.field_id("volume")).begin(),
                             panel.field_all(*panel.field_id("volume")).end());
    auto alt_r = Panel::create(kDates, kInsts, {"close", "volume"}, {close_col, vol_col},
                               std::move(universe));
    ASSERT_TRUE(alt_r.has_value());
    const auto alt_fit = pool_aware_fitness(cand, pool, *alt_r, policy, sim, admit_fit);
    ASSERT_TRUE(alt_fit.has_value()) << (alt_fit ? "" : alt_fit.error().message());
    EXPECT_LT(alt_fit->dsr, 0.5 * base_edge)
        << "manual restricted-universe cross-check: base_edge=" << base_edge
        << " restricted_edge=" << alt_fit->dsr
        << " -- walling off the only real-return instrument must collapse the edge";
  }

  eval::BatteryConfig cfg;
  cfg.sub_universe = true;
  cfg.seed = 4242;
  EXPECT_FALSE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg, base_edge))
      << "base_edge=" << base_edge
      << " -- the illiquid single-name edge must be REJECTED by sub_universe";
}

// =============================================================================
//  (b) SubUniverseSurvivesBroadEdge — the edge is spread across every
//  instrument (differentiated but positive drift); the top-N-by-ADV subset still
//  carries a real, differentiated cross-section -- ADMITTED.
// =============================================================================
TEST(RobustnessBatteryFullWire, SubUniverseSurvivesBroadEdge) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = fw_broad_edge_panel();
  Genome cand = fw_make_genome("rank(close)", lib);
  const FitnessCfg admit_fit{};

  const auto base_fit = pool_aware_fitness(cand, pool, panel, policy, sim, admit_fit);
  ASSERT_TRUE(base_fit.has_value()) << (base_fit ? "" : base_fit.error().message());
  const f64 base_edge = base_fit->dsr;
  ASSERT_GT(base_edge, 0.0) << "broad-edge fixture must give a positive base dsr";

  eval::BatteryConfig cfg;
  cfg.sub_universe = true;
  cfg.seed = 4242;
  EXPECT_TRUE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg, base_edge))
      << "base_edge=" << base_edge
      << " -- a broad, multi-name edge must SURVIVE sub_universe restriction";
}

// =============================================================================
//  alt_neutralization fixtures (5 insts, 40 dates). "volume" is set strictly
//  ascending (100,200,...) so the internal per-instrument ADV-quantile bucketing
//  (kNBuckets == insts == 5) assigns bucket(i) == i exactly -- a known, hand-
//  verifiable mapping. "close" drift is proportional to instrument index (so a
//  pure "long high-bucket / short low-bucket" static book is genuinely
//  profitable -- the group-tilt candidate below never reads "close" for its
//  SIGNAL, but extract_streams still requires "close" to derive returns) PLUS a
//  small idiosyncratic (per-instrument, seeded LCG) noise term: a book held at a
//  FIXED weight vector forever (both the true-bucket AND any permuted-bucket
//  book are time-invariant, since "__s5_group" never varies by date) against a
//  shared common oscillation alone would have EXACTLY ZERO cross-time variance
//  (the common term cancels under WeightPolicy's dollar-neutral centering),
//  which floors DSR to the SAME saturated extreme (~0 or ~1) regardless of how
//  strong or weak the drift/bucket correlation is -- the idiosyncratic term
//  breaks that degeneracy so DSR responds proportionally to alignment strength.
// =============================================================================
[[nodiscard]] Panel fw_group_tilt_base_panel() {
  constexpr usize kDates = 40;
  constexpr usize kInsts = 5;
  std::array<f64, kInsts> drift{};
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.002 * static_cast<f64>(j); // ascending in bucket rank
  }
  std::vector<f64> close(kDates * kInsts);
  std::array<f64, kInsts> px{};
  px.fill(100.0);
  std::array<std::uint64_t, kInsts> s{};
  for (usize j = 0; j < kInsts; ++j) {
    s[j] = (static_cast<std::uint64_t>(j) + 7ULL) * 0x9E3779B97F4A7C15ULL + 1ULL;
  }
  const auto next_noise = [](std::uint64_t &seed) noexcept -> f64 {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = seed >> 11U;
    return 2.0 * (static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U)) - 1.0; // uniform [-1,1]
  };
  for (usize t = 0; t < kDates; ++t) {
    const f64 osc = (t % 2U == 0U) ? 0.01 : -0.01;
    for (usize j = 0; j < kInsts; ++j) {
      px[j] *= (1.0 + drift[j] + osc + 0.01 * next_noise(s[j])); // idiosyncratic noise breaks cancellation
      close[t * kInsts + j] = px[j];
    }
  }
  std::vector<f64> volume(kDates * kInsts);
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kInsts; ++j) {
      volume[t * kInsts + j] = 100.0 * static_cast<f64>(j + 1); // ascending -> bucket(i) == i
    }
  }
  return fw_make_panel(kDates, kInsts, {"close", "volume"}, {close, volume});
}

// The SAME base panel PLUS a "__s5_group" field == the TRUE (unpermuted) bucket
// {0,1,2,3,4} per instrument, constant across every date -- lets the group-tilt
// candidate `rank(__s5_group)` be evaluated OUTSIDE the battery (for base_edge)
// exactly as the production admit loop would evaluate any candidate before
// reaching robustness_battery_passes.
[[nodiscard]] Panel fw_group_tilt_true_group_panel(const Panel &base) {
  constexpr usize kDates = 40;
  constexpr usize kInsts = 5;
  std::vector<f64> close_col(base.field_all(*base.field_id("close")).begin(),
                             base.field_all(*base.field_id("close")).end());
  std::vector<f64> vol_col(base.field_all(*base.field_id("volume")).begin(),
                           base.field_all(*base.field_id("volume")).end());
  std::vector<f64> grp_col(kDates * kInsts);
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kInsts; ++j) {
      grp_col[t * kInsts + j] = static_cast<f64>(j); // TRUE bucket == index
    }
  }
  return fw_make_panel(kDates, kInsts, {"close", "volume", "__s5_group"},
                       {close_col, vol_col, grp_col});
}

// =============================================================================
//  (c) AltNeutralizationRejectsGroupTilt — a pure liquidity-bucket step-function
//  candidate (`rank(__s5_group)`, profitable ONLY because bucket rank matches
//  the true return-drift ranking) collapses when the battery re-labels the SAME
//  buckets with a seeded permutation -- the position is now assigned to the
//  WRONG names relative to their real drift -- REJECTED.
// =============================================================================
TEST(RobustnessBatteryFullWire, AltNeutralizationRejectsGroupTilt) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = fw_group_tilt_base_panel(); // "close" + "volume" only (no "__s5_group")
  const Panel true_group_panel = fw_group_tilt_true_group_panel(panel);
  Genome cand = fw_make_genome("rank(__s5_group)", lib);
  const FitnessCfg admit_fit{};

  const auto base_fit = pool_aware_fitness(cand, pool, true_group_panel, policy, sim, admit_fit);
  ASSERT_TRUE(base_fit.has_value()) << (base_fit ? "" : base_fit.error().message());
  const f64 base_edge = base_fit->dsr;
  ASSERT_GT(base_edge, 0.0)
      << "the TRUE-bucket long-high/short-low book must be genuinely profitable by construction";

  eval::BatteryConfig cfg;
  cfg.alt_neutralization = true;
  // seed=2's Fisher-Yates draw over {0,1,2,3,4} produces a permutation that
  // anti-correlates enough with the true drift ranking to drop the survival
  // ratio below eval::BatteryConfig::min_survival_ratio (0.5, frozen) -> REJECT.
  // This is a representative, reproducible pick, NOT universal and NOT a
  // 1-in-N fluke: a direct seed sweep (0..199) over this exact fixture rejects
  // 92/200 (~46%) of draws -- the remaining seeds land on permutations close
  // enough to identity that the bucket-vs-drift alignment survives. Production
  // draws exactly ONE seed-derived permutation, so the check's realized power on
  // a maximally group-tilted candidate is ~46%; a candidate that never groups by
  // the field is unaffected under EVERY seed (see the survivor test below). The
  // wire is genuine; a "reject under ANY permutation" framing would overclaim.
  cfg.seed = 2;
  EXPECT_FALSE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg, base_edge))
      << "base_edge=" << base_edge
      << " -- a pure liquidity-bucket tilt collapses below min_survival_ratio under this"
         " (representative, ~46%-of-seeds) group permutation";
}

// =============================================================================
//  (c) AltNeutralizationSurvivesIdiosyncraticEdge — a candidate that never reads
//  the group field (`rank(close)`) is byte-for-byte UNAFFECTED by the appended
//  "__s5_group" column -- scenario_edge == base_edge exactly -- ADMITTED.
// =============================================================================
TEST(RobustnessBatteryFullWire, AltNeutralizationSurvivesIdiosyncraticEdge) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = fw_group_tilt_base_panel();
  Genome cand = fw_make_genome("rank(close)", lib);
  const FitnessCfg admit_fit{};

  const auto base_fit = pool_aware_fitness(cand, pool, panel, policy, sim, admit_fit);
  ASSERT_TRUE(base_fit.has_value()) << (base_fit ? "" : base_fit.error().message());
  const f64 base_edge = base_fit->dsr;
  ASSERT_GT(base_edge, 0.0) << "the close-momentum candidate must have a real base edge";

  eval::BatteryConfig cfg;
  cfg.alt_neutralization = true;
  cfg.seed = 99;
  EXPECT_TRUE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg, base_edge))
      << "base_edge=" << base_edge
      << " -- a candidate that never groups by the perturbed field must be UNAFFECTED";
}

// =============================================================================
//  param_perturbation fixtures (6 insts, 60 dates, real differentiated drift --
//  reuses fw_broad_edge_panel's shape). Candidate: `scale(close, 1.0)` -- the
//  ONE free Scale-classified literal is the CsScale target-L1-norm operand
//  (oracle.cpp cs_scale: out = close * (a / sum|close|)). a>0 preserves
//  rank(close)'s cross-sectional order; a<0 reverses it; a==0 degenerates to a
//  flat (tied) cross-section.
// =============================================================================

// =============================================================================
//  (d) ParamPerturbationRejectsKnifeEdge — a WIDE perturbation band (2.0 ->
//  param_scale in [-1, 3]) straddles the sign-flip point: some seeded draws
//  preserve the edge, others invert or degenerate it -- a real, mechanically-
//  grounded high-CV knife edge -- REJECTED.
// =============================================================================
TEST(RobustnessBatteryFullWire, ParamPerturbationRejectsKnifeEdge) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = fw_broad_edge_panel();
  Genome cand = fw_make_genome("scale(close, 1.0)", lib);
  const FitnessCfg admit_fit{};

  const auto base_fit = pool_aware_fitness(cand, pool, panel, policy, sim, admit_fit);
  ASSERT_TRUE(base_fit.has_value()) << (base_fit ? "" : base_fit.error().message());
  ASSERT_GT(base_fit->dsr, 0.0) << "fixture must have a positive base dsr at scale=+1";

  eval::BatteryConfig cfg;
  cfg.param_perturbation = true;
  cfg.param_perturbation_band = 2.0; // param_scale in [-1, 3] -- crosses the sign-flip point
  cfg.param_perturbation_draws = 8;
  cfg.param_perturbation_max_cv = 0.25;
  cfg.seed = 7;
  EXPECT_FALSE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg, base_fit->dsr))
      << "a jitter band crossing scale(close,a)'s sign-flip point must swing the edge "
         "enough to REJECT on coefficient-of-variation";
}

// =============================================================================
//  (d) ParamPerturbationAdmitsStableCandidate — the IDENTICAL candidate/panel
//  under the DEFAULT (narrow, sign-preserving) band: param_scale stays in
//  [0.9, 1.1], always positive, so rank order (and hence dsr) is unchanged
//  across every draw -- CV ~ 0 -- ADMITTED.
// =============================================================================
TEST(RobustnessBatteryFullWire, ParamPerturbationAdmitsStableCandidate) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = fw_broad_edge_panel();
  Genome cand = fw_make_genome("scale(close, 1.0)", lib);
  const FitnessCfg admit_fit{};

  const auto base_fit = pool_aware_fitness(cand, pool, panel, policy, sim, admit_fit);
  ASSERT_TRUE(base_fit.has_value()) << (base_fit ? "" : base_fit.error().message());
  ASSERT_GT(base_fit->dsr, 0.0) << "fixture must have a positive base dsr at scale=+1";

  eval::BatteryConfig cfg; // param_perturbation_band stays at its 0.10 struct default
  cfg.param_perturbation = true;
  cfg.param_perturbation_draws = 8;
  cfg.param_perturbation_max_cv = 0.25;
  cfg.seed = 7;
  EXPECT_TRUE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg, base_fit->dsr))
      << "a narrow, sign-preserving jitter band must leave the edge (and hence CV) stable "
         "-- ADMITTED";
}

// =============================================================================
//  Full-wire FactoryConfig/mine_into fixtures + tests.
// =============================================================================
[[nodiscard]] Panel fw_search_panel() {
  constexpr usize kDates = 80;
  constexpr usize kInsts = 6;
  std::array<f64, kInsts> drift{};
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.007 - 0.0012 * static_cast<f64>(j);
  }
  std::vector<f64> close(kDates * kInsts);
  std::array<f64, kInsts> px{};
  px.fill(100.0);
  for (usize t = 0; t < kDates; ++t) {
    const f64 osc = (t % 2U == 0U) ? 0.012 : -0.012;
    for (usize j = 0; j < kInsts; ++j) {
      px[j] *= (1.0 + drift[j] + osc);
      close[t * kInsts + j] = px[j];
    }
  }
  std::vector<f64> volume(kDates * kInsts);
  const std::array<f64, kInsts> vol_level{9000.0, 8000.0, 7000.0, 6000.0, 5000.0, 4000.0};
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kInsts; ++j) {
      volume[t * kInsts + j] = vol_level[j];
    }
  }
  return fw_make_panel(kDates, kInsts, {"close", "volume"}, {close, volume});
}

[[nodiscard]] FactoryConfig fw_search_cfg(atx::u64 seed) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
  cfg.search.population = 12;
  cfg.search.generations = 3;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.seed_exprs = {"rank(close)", "ts_mean(close, 5)", "delta(close, 2)", "scale(close, 1.0)"};
  cfg.panel_fields = {"close", "volume"};
  cfg.min_dsr = -1.0e9; // never let the dsr floor mask whether the battery ran
  return cfg;
}

[[nodiscard]] lib::Library fw_open_lib(std::string_view tag, atx::u64 seed) {
  lib::GateConfig gate_cfg;
  const std::string dir =
      (std::filesystem::temp_directory_path() / (std::string{"atx_p9_s5_3_"} + std::string{tag}))
          .string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return lib::Library::open(dir, gate_cfg, {seed});
}

// =============================================================================
//  (a) AllThreeOffPathByteIdentical — mirrors the house "explicit-off equals
//  implicit-off" idiom (stage_run_megabook_test.cpp's MegaBookGraph_
//  InertByteIdentical): cfg_a never mentions the 3 new fields (struct defaults);
//  cfg_b asserts all 3 explicitly false alongside robustness_battery=true. Both
//  must produce byte-identical admitted set/digest/reject_histogram -- the 3 new
//  bools' mere presence (and the now-unconditional adv_col/group_col derivation
//  whenever robustness_battery is on) must not perturb the noise-control-only
//  admission behavior the p8 wave shipped.
// =============================================================================
TEST(RobustnessBatteryFullWire, AllThreeOffPathByteIdentical) {
  lib::GateConfig gate_cfg;
  const lib::AlphaGate gate{gate_cfg};
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();

  FactoryConfig cfg_a = fw_search_cfg(0xA51CE5u);
  cfg_a.robustness_battery = true; // 3 new bools left at their struct default (false)

  FactoryConfig cfg_b = fw_search_cfg(0xA51CE5u);
  cfg_b.robustness_battery = true;
  cfg_b.robustness_sub_universe = false;
  cfg_b.robustness_alt_neutralization = false;
  cfg_b.robustness_param_perturb = false;

  const Panel panel_a = fw_search_panel();
  Factory factory_a{lib, panel_a, sim, policy};
  lib::Library liba = fw_open_lib("offpath_a", cfg_a.search.master_seed);
  const auto rep_a = factory_a.mine_into(cfg_a, liba, gate);
  ASSERT_TRUE(rep_a.has_value()) << (rep_a ? "" : rep_a.error().message());

  const Panel panel_b = fw_search_panel();
  Factory factory_b{lib, panel_b, sim, policy};
  lib::Library libb = fw_open_lib("offpath_b", cfg_b.search.master_seed);
  const auto rep_b = factory_b.mine_into(cfg_b, libb, gate);
  ASSERT_TRUE(rep_b.has_value()) << (rep_b ? "" : rep_b.error().message());

  EXPECT_EQ(rep_a->digest, rep_b->digest)
      << "the 3 new bools explicitly false must reproduce the implicit-default digest";
  EXPECT_EQ(rep_a->admitted, rep_b->admitted);
  EXPECT_EQ(rep_a->reject_histogram, rep_b->reject_histogram);
}

// =============================================================================
//  (c) AllThreeTwiceRunByteIdentical — all 4 checks ON (noise_control implied +
//  the 3 new S5-3 checks), run twice with fresh libraries at the SAME seed.
//  Pure/deterministic: no thread/time leakage into any check's RNG stream.
// =============================================================================
TEST(RobustnessBatteryFullWire, AllThreeTwiceRunByteIdentical) {
  lib::GateConfig gate_cfg;
  const lib::AlphaGate gate{gate_cfg};
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();

  const auto run = [&](std::string_view tag) -> FactoryReport {
    FactoryConfig cfg = fw_search_cfg(0xB0B5EEDu);
    cfg.robustness_battery = true;
    cfg.robustness_sub_universe = true;
    cfg.robustness_alt_neutralization = true;
    cfg.robustness_param_perturb = true;

    const Panel panel = fw_search_panel();
    Factory factory{lib, panel, sim, policy};
    lib::Library l = fw_open_lib(tag, cfg.search.master_seed);
    auto rep = factory.mine_into(cfg, l, gate);
    EXPECT_TRUE(rep.has_value()) << (rep ? "" : rep.error().message());
    return rep ? std::move(*rep) : FactoryReport{};
  };

  const FactoryReport rep1 = run("twice_1");
  const FactoryReport rep2 = run("twice_2");

  EXPECT_NE(rep1.digest, 0u) << "sanity: the run must actually mine/admit something";
  EXPECT_EQ(rep1.digest, rep2.digest) << "all-4-checks-ON must be byte-identical run-to-run";
  EXPECT_EQ(rep1.admitted, rep2.admitted);
  EXPECT_EQ(rep1.reject_histogram, rep2.reject_histogram);
}

// =============================================================================
//  (d) SerialParallelAgreeWithFullBattery — the OOS admit ladder
//  (admit_on_holdout, shared by mine_into_oos and mine_into_oos_parallel) with
//  ALL 4 checks ON must produce byte-identical digest/admitted/reject_histogram
//  whether the serial in-process path or a real ProcessExecutor@2 substrate
//  drives the per-genome scoring map -- each check's RNG derives from
//  cfg.seed-salted independent streams (never thread/time), so the parallel
//  substrate cannot perturb the verdict.
// =============================================================================
TEST(RobustnessBatteryFullWire, SerialParallelAgreeWithFullBattery) {
  lib::GateConfig gate_cfg;
  const lib::AlphaGate gate{gate_cfg};
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = fw_frictionless_sim();

  FactoryConfig cfg = fw_search_cfg(0x5EA1C0DEu);
  cfg.oos_fraction = 0.25; // routes to mine_into_oos / mine_into_oos_parallel
  cfg.robustness_battery = true;
  cfg.robustness_sub_universe = true;
  cfg.robustness_alt_neutralization = true;
  cfg.robustness_param_perturb = true;

  // Serial oracle.
  const Panel panel_s = fw_search_panel();
  Factory factory_s{lib, panel_s, sim, policy};
  lib::Library lib_s = fw_open_lib("seqpar_seq", cfg.search.master_seed);
  const auto rep_s = factory_s.mine_into(cfg, lib_s, gate);
  ASSERT_TRUE(rep_s.has_value()) << (rep_s ? "" : rep_s.error().message());

  // Parallel path (ProcessExecutor, 2 workers).
  const Panel panel_p = fw_search_panel();
  Factory factory_p{lib, panel_p, sim, policy};
  lib::Library lib_p = fw_open_lib("seqpar_par", cfg.search.master_seed);
  parallel::ProcessExecutor exec_par{parallel::ExecutorConfig{2, false}};
  const auto rep_p = factory_p.mine_into(cfg, lib_p, gate, exec_par);
  ASSERT_TRUE(rep_p.has_value()) << (rep_p ? "" : rep_p.error().message());

  EXPECT_NE(rep_s->digest, 0u) << "sanity: the run must actually mine/admit something";
  EXPECT_EQ(rep_s->digest, rep_p->digest)
      << "seq==parallel digest must match with all 4 battery checks ON";
  EXPECT_EQ(rep_s->admitted, rep_p->admitted);
  EXPECT_EQ(rep_s->reject_histogram, rep_p->reject_histogram);
}

} // namespace atxtest_robustness_battery_full_wire
