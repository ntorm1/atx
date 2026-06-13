#pragma once

// atx::engine::book — alpha-decay monitor + DecayController (Sprint S7-2).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  An alpha-decay monitor that detects a statistically-significant DOWNWARD
//  shift in a LIVE alpha's realized performance versus its admitted backtest
//  baseline, at a controlled false-alarm rate, then demotes the alpha through
//  the S4 lifecycle (library::Library::mark).
//
//  Two complementary detectors plus a discriminator:
//   * a fast streaming Page-Hinkley DOWN test on the standardized realized
//     return, gated by a MinTRL (Bailey-López de Prado) significance floor so a
//     handful of bad observations cannot conclude decay;
//   * a realized-DSR/PSR drop below the admitted baseline (a slower, moment-aware
//     cross-check), reusing eval::probabilistic_sharpe / eval::deflated_sharpe;
//   * a cost-flooding discriminator: an alpha whose NET return decayed only
//     because its trading cost rose (its GROSS edge no longer clears cost) is
//     SIZED DOWN by the allocator, NOT retired — so cost-flooding SUPPRESSES the
//     decay flag (reuses cost::should_trade);
//   * a DecayController that maps verdicts to library::Library::mark with an
//     asymmetric "retire fast / restore slow" hysteresis.
//
// ===========================================================================
//  Reuse discipline (no second implementation)
// ===========================================================================
//  This unit re-implements NONE of the DSR, the higher moments, or the journal:
//   * eval::probabilistic_sharpe / eval::deflated_sharpe — PSR/DSR (S1-2);
//   * eval::skewness / eval::excess_kurtosis / eval::mean_std_pop — the ONE
//     population-moment convention (S1-1);
//   * eval::norm_ppf — the inverse-normal quantile (S1-1);
//   * cost::should_trade — the RenTech edge-vs-cost throttle (S6-2);
//   * library::Library::mark / state_as_of — the PIT lifecycle journal (S4-4).
//
//  dsr == psr note (D6): eval::deflated_sharpe sets dsr == psr as-built, so the
//  `dsr < dsr_admit` cross-check below is a PSR comparison; documented as such.
//
// ===========================================================================
//  Point-in-time (PIT) discipline
// ===========================================================================
//  observe() reads ONLY the frozen AdmittedBaseline + the caller's live_window
//  (realized returns up to and including t) — never any future value. The
//  DecayController freezes each alpha's baseline from lib.pnl(id) on the FIRST
//  step and COPIES the span out immediately (the store span aliases mmap/segment
//  memory that a later store growth could invalidate). Every mark() is keyed to
//  the caller's as_of period, so the append-only journal keeps the no-retroactive-
//  relabel guarantee (S4-4).
//
//  DecayController holds a per-alpha bookkeeping map keyed by AlphaId.value. The
//  map is BOOKKEEPING, not a numeric reduction: each alpha's verdict depends only
//  on its own frozen baseline + its own live window, so iteration order is
//  irrelevant and determinism (L7) is preserved.

#include <algorithm>     // std::max
#include <cmath>         // std::ceil, std::isfinite
#include <limits>        // std::numeric_limits (MinTRL sentinel)
#include <optional>      // std::optional (lazily-frozen baseline)
#include <span>          // std::span (non-owning live window)
#include <unordered_map> // per-alpha controller bookkeeping
#include <vector>        // owned per-alpha live window copy

#include "atx/core/macro.hpp" // ATX_CHECK (assert legal-path mark success)
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/combine/store.hpp"  // combine::AlphaId
#include "atx/engine/cost/temp_perm.hpp" // cost::should_trade
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::probabilistic_sharpe, deflated_sharpe
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, excess_kurtosis, mean_std_pop, norm_ppf
#include "atx/engine/library/library.hpp"      // library::Library
#include "atx/engine/library/lifecycle.hpp"    // library::LifecycleState

namespace atx::engine::book {

// ===========================================================================
//  AdmittedBaseline — the alpha's admitted backtest reference, frozen PIT.
//
//  Computed ONCE from the admitted pnl stream (lib.pnl(id)) at the first live
//  observation and never re-derived. Trivial aggregate (Rule of Zero).
// ===========================================================================
struct AdmittedBaseline {
  atx::f64 sr_admit;    // per-observation admitted Sharpe (mean/sd, the DSR convention)
  atx::f64 skew;        // admitted population skewness (γ3)
  atx::f64 exkurt;      // admitted population excess kurtosis
  atx::usize t_admit;   // admitted backtest length (observation count)
  atx::f64 dsr_admit;   // admitted DSR (== PSR as-built, D6) against n_trials selection
  atx::usize n_trials;  // the admission deflation trial count N
  atx::f64 mean_admit;  // admitted population mean (standardizes the live stream)
  atx::f64 sd_admit;    // admitted population std (standardizes the live stream)
};

// ===========================================================================
//  PageHinkleyState — one-sided LOWER (DOWN) cumulative change detector (§4.2).
//
//  `mean` is the running mean of the standardized observation, `cum` the
//  cumulative deviation below (mean + delta), `max` its running maximum. A
//  persistent negative LEVEL shift drives cum down, so (max − cum) grows past
//  lambda and the detector trips. Exactly the §4.2 quadruple {mean,cum,max,n};
//  it owns no other responsibility. Trivial aggregate (Rule of Zero).
// ===========================================================================
struct PageHinkleyState {
  atx::f64 mean = 0.0;
  atx::f64 cum = 0.0;
  atx::f64 max = 0.0;
  atx::usize n = 0;
};

// ===========================================================================
//  DecayState — the per-alpha CROSS-PERIOD streaming state observe() threads.
//
//  Bundles the Page-Hinkley detector state with the DSR-drop confirmation run so
//  each struct keeps a single responsibility: PageHinkleyState stays the pure
//  §4.2 detector quadruple, while `dsr_low_run` (the count of CONSECUTIVE
//  observations for which the realized DSR/PSR drop condition held, reset to 0 on
//  any clean observation) lives here. The DSR-drop only flags after a sustained
//  run — what separates a genuine decay (PSR stays near 0 indefinitely) from a
//  stable stream's transient sample dip (a short run that recovers).
// ===========================================================================
struct DecayState {
  PageHinkleyState ph;
  atx::usize dsr_low_run = 0; // consecutive DSR-drop observations (confirmation streak)
};

// ===========================================================================
//  DecayConfig — detector + hysteresis knobs.
//
//  Defaults are tuned (see default_decay_cfg) for a separation between a genuine
//  mean-halving decay (detected within ~50 obs) and a stable stream on the S7-2
//  fixtures — verified across the asserted 12-seed sweep in both directions
//  (book_decay_monitor_test). The residual stable false-alarm rate is consistent
//  with the by-design psr_alpha one-sided gate: suppressed (but not eliminated)
//  by the effect-size floor + confirmation run.
// ===========================================================================
struct DecayConfig {
  atx::f64 ph_delta = 0.005;      // Page-Hinkley slack (tolerated downward drift / step)
  atx::f64 ph_lambda = 50.0;      // Page-Hinkley trip threshold on (max − cum)
  atx::usize ph_min_obs = 30;     // min live obs before either detector may flag
  atx::f64 psr_alpha = 0.05;      // PSR significance / MinTRL false-alarm rate (one-sided)
  atx::f64 decay_effect_frac = 0.75; // realized Sharpe must fall below this fraction of admitted
                                     // (an EFFECT-SIZE floor: a real decay, not a noisy dip)
  atx::usize dsr_confirm_run = 10;   // consecutive DSR-drop obs before the drop flags (persistence)
  atx::usize confirm_periods = 5;  // consecutive flags in Decaying -> Dead (retire fast)
  atx::usize recover_periods = 60; // consecutive clean periods Decaying -> Live (restore slow)
  atx::f64 cost_flood_safety = 1.0; // should_trade safety multiplier on the cost hurdle
  atx::usize default_n_trials = 1;  // admission deflation N when none is available (D6: N=1 => DSR==PSR(0))
};

// The default decay config (the calibrated separation point — see DecayConfig).
[[nodiscard]] inline DecayConfig default_decay_cfg() noexcept { return DecayConfig{}; }

// ===========================================================================
//  DecayVerdict — the per-observation decay decision.
//
//  `recommend` is Live (unchanged) or Decaying (flag raised). realized_psr /
//  realized_dsr are the live moments' PSR/DSR vs the admitted baseline; min_trl
//  is the MinTRL significance floor (a large sentinel when decay cannot yet be
//  concluded). cost_flooded records whether the decay is cost-driven (then the
//  flag is SUPPRESSED — the alpha is sized down, not retired).
// ===========================================================================
struct DecayVerdict {
  bool flag;
  library::LifecycleState recommend;
  atx::f64 realized_psr;
  atx::f64 realized_dsr;
  atx::usize min_trl;
  bool cost_flooded;
};

// ===========================================================================
//  make_baseline — freeze an AdmittedBaseline from an admitted pnl stream (PIT).
//
//  Computes the population mean/std/skew/exkurt of `pnl` (the ONE eval moment
//  convention), the per-observation admitted Sharpe (mean/sd, the convention
//  deflated_sharpe consumes), and the admitted DSR against `n_trials` selection.
//  A degenerate (zero-vol) stream yields sr_admit = 0 (no edge information).
// ===========================================================================
[[nodiscard]] inline AdmittedBaseline make_baseline(std::span<const atx::f64> pnl,
                                                    atx::usize n_trials) noexcept {
  const eval::MeanStd ms = eval::mean_std_pop(pnl);
  const atx::f64 sr_admit = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
  const atx::f64 skew = eval::skewness(pnl);
  const atx::f64 exkurt = eval::excess_kurtosis(pnl);
  const atx::usize t_admit = pnl.size();
  const eval::DsrResult d = eval::deflated_sharpe(sr_admit, t_admit, skew, exkurt, n_trials, std::nullopt);
  return AdmittedBaseline{sr_admit, skew, exkurt, t_admit, d.dsr, n_trials, ms.mean, ms.std};
}

// ===========================================================================
//  min_track_record_length — MinTRL (Bailey-López de Prado), as a pure function.
//
//  The track-record length at which a realized PSR of sr_live against the
//  admitted sr_admit would be significant at psr_alpha:
//      MinTRL = 1 + var_term · (Φ⁻¹(1 − α) / (sr_admit − sr_live))²,
//      var_term = 1 − γ3·sr_live + ((κ+2)/4)·sr_live²   (the as-built PSR var-term).
//  den = sr_admit − sr_live is positive when decaying. A NON-POSITIVE gap (live
//  not below admit) means decay cannot be concluded -> the SIZE_MAX sentinel; a
//  degenerate (≤0) var_term or a non-finite result also returns the sentinel.
//  Monotone: a SMALLER positive gap yields a LARGER MinTRL (1/den²). Free
//  function so it is unit-testable against its closed form (S7-2 review).
// ===========================================================================
[[nodiscard]] inline atx::usize min_track_record_length(atx::f64 sr_admit, atx::f64 sr_live,
                                                        atx::f64 skew, atx::f64 exkurt,
                                                        atx::f64 psr_alpha) noexcept {
  const atx::f64 den = sr_admit - sr_live;
  if (!(den > 0.0)) {
    return std::numeric_limits<atx::usize>::max(); // cannot conclude decay yet
  }
  const atx::f64 var_term = 1.0 - skew * sr_live + ((exkurt + 2.0) / 4.0) * sr_live * sr_live;
  if (!(var_term > 0.0)) {
    return std::numeric_limits<atx::usize>::max(); // degenerate moments -> no conclusion
  }
  const atx::f64 ratio = eval::norm_ppf(1.0 - psr_alpha) / den;
  const atx::f64 trl = 1.0 + var_term * ratio * ratio;
  if (!std::isfinite(trl) || trl <= 0.0) {
    return std::numeric_limits<atx::usize>::max();
  }
  return static_cast<atx::usize>(std::ceil(trl));
}

// ===========================================================================
//  DecayMonitor — stateless per-observation decay evaluator.
//
//  Holds only its config. observe() is const + [[nodiscard]] and mutates only the
//  caller's PageHinkleyState (the explicit online accumulator), never internal
//  state — so the monitor is trivially reusable across alphas and threads.
// ===========================================================================
class DecayMonitor {
public:
  DecayMonitor() = default;
  explicit DecayMonitor(DecayConfig c) noexcept : cfg{c} {}

  DecayConfig cfg{};

  /// Evaluate ONE live period. `live_window` is the realized return stream since
  /// admission (OOS continuation, length >= 1); its back() is the newest return
  /// r_t. `gross_edge_bps`/`round_trip_cost_bps` drive the cost-flooding
  /// discriminator. `st` is the caller's persistent cross-period streaming state
  /// (Page-Hinkley + DSR-drop run), updated in place. Reads nothing in the future
  /// (PIT).
  [[nodiscard]] DecayVerdict observe(const AdmittedBaseline &base,
                                     std::span<const atx::f64> live_window,
                                     atx::f64 gross_edge_bps, atx::f64 round_trip_cost_bps,
                                     DecayState &st) const {
    // 1. Page-Hinkley DOWN on the standardized newest realized return.
    const bool ph_trip = page_hinkley_down(base, live_window, st.ph);

    // 2. Realized moments of the live window (order-fixed; the eval convention).
    const eval::MeanStd ms = eval::mean_std_pop(live_window);
    const atx::f64 sr_live = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
    const atx::f64 skew = eval::skewness(live_window);
    const atx::f64 exkurt = eval::excess_kurtosis(live_window);
    const atx::usize t_live = live_window.size();
    const atx::f64 psr = eval::probabilistic_sharpe(sr_live, base.sr_admit, t_live, skew, exkurt);
    const eval::DsrResult dsr = eval::deflated_sharpe(sr_live, t_live, skew, exkurt, base.n_trials, std::nullopt);

    // 3. MinTRL significance gate + the DSR/PSR drop cross-check (the SLOW, robust
    //    confirmer). A single noisy window is NOT enough: the per-observation Sharpe
    //    convention makes MinTRL tiny and a stable stream's cumulative sample Sharpe
    //    can transiently dip below the (high) admitted bar, so a one-shot PSR test
    //    false-alarms. The drop must therefore clear THREE conditions for a run of
    //    dsr_confirm_run CONSECUTIVE observations:
    //      (a) enough observations: t_live >= max(MinTRL, ph_min_obs);
    //      (b) statistical significance: psr < psr_alpha AND dsr < dsr_admit;
    //      (c) an EFFECT-SIZE floor: sr_live < decay_effect_frac * sr_admit — a REAL
    //          performance loss, not a noisy dip. A stable stream's transient dip is a
    //          SHORT run that recovers; a genuine decay's PSR stays near 0 forever, so
    //          the run reaches the confirmation length. This SUPPRESSES (does not
    //          eliminate) the by-design psr_alpha false-alarm rate — clean across the
    //          asserted 12-seed both-directions sweep (book_decay_monitor_test).
    const atx::usize min_trl = min_track_record_length(base.sr_admit, sr_live, skew, exkurt, cfg.psr_alpha);
    const atx::usize trl_floor = std::max(min_trl, cfg.ph_min_obs);
    const bool drop_obs = (t_live >= trl_floor) && (dsr.dsr < base.dsr_admit) &&
                          (psr < cfg.psr_alpha) && (sr_live < cfg.decay_effect_frac * base.sr_admit);
    if (drop_obs) {
      st.dsr_low_run += 1U;
    } else {
      st.dsr_low_run = 0U;
    }
    const bool dsr_drop = st.dsr_low_run >= cfg.dsr_confirm_run;

    // 4. Cost-flooding discriminator (a cost-driven NET decay is sized down).
    const bool cost_flooded = !cost::should_trade(gross_edge_bps, round_trip_cost_bps, cfg.cost_flood_safety);

    // 5. Verdict: a TRUE alpha decay (not cost-driven) recommends Decaying.
    DecayVerdict v{false, library::LifecycleState::Live, psr, dsr.dsr, min_trl, cost_flooded};
    if ((ph_trip || dsr_drop) && !cost_flooded) {
      v.flag = true;
      v.recommend = library::LifecycleState::Decaying;
    }
    return v;
  }

private:
  // Page-Hinkley LOWER detector on z = (r_t − mean_admit)/sd_admit. Updates ph in
  // place; returns true once enough observations show a persistent downward drift.
  [[nodiscard]] bool page_hinkley_down(const AdmittedBaseline &base,
                                       std::span<const atx::f64> live_window,
                                       PageHinkleyState &ph) const noexcept {
    const atx::f64 sd = (base.sd_admit > 1e-12) ? base.sd_admit : 1e-12;
    const atx::f64 z = (live_window.back() - base.mean_admit) / sd;
    ph.n += 1U;
    ph.mean += (z - ph.mean) / static_cast<atx::f64>(ph.n);
    ph.cum += (z - ph.mean - cfg.ph_delta);
    ph.max = std::max(ph.max, ph.cum);
    return (ph.n >= cfg.ph_min_obs) && (ph.max - ph.cum > cfg.ph_lambda);
  }
};

// ===========================================================================
//  DecayController — drives the S4 lifecycle from streaming verdicts (R5).
//
//  Holds per-alpha bookkeeping (the frozen baseline, the Page-Hinkley state, the
//  growing live window, and the asymmetric hysteresis counters). step() is called
//  once per live period per alpha; the FIRST step for an id freezes the baseline
//  from lib.pnl(id). The map is per-alpha bookkeeping, not a numeric reduction —
//  iteration order does not affect any verdict, so determinism (L7) holds.
// ===========================================================================
class DecayController {
public:
  DecayController() = default;
  explicit DecayController(DecayConfig c) : cfg_{c}, monitor_{c} {}

  /// Advance alpha `id` by one live period with realized return `r_t` at `as_of`.
  /// Freezes the baseline from lib.pnl(id) on the first step (PIT, span copied out
  /// before any store growth). Applies the asymmetric retire-fast / restore-slow
  /// hysteresis to lib.mark(). `gross_edge_bps`/`rt_cost_bps` feed the cost-flood
  /// discriminator.
  void step(library::Library &lib, combine::AlphaId id, atx::f64 r_t, atx::usize as_of,
            atx::f64 gross_edge_bps = 0.0, atx::f64 rt_cost_bps = 0.0) {
    PerAlpha &pa = state_[id.value];
    if (!pa.base.has_value()) {
      // Freeze the admitted baseline. COPY the pnl span out NOW: it aliases the
      // store's segment memory and a later store growth could invalidate it.
      const std::span<const atx::f64> admit = lib.pnl(id);
      const std::vector<atx::f64> admit_copy(admit.begin(), admit.end());
      pa.base = make_baseline(admit_copy, cfg_.default_n_trials);
    }
    pa.live_window.push_back(r_t);
    const DecayVerdict v =
        monitor_.observe(*pa.base, pa.live_window, gross_edge_bps, rt_cost_bps, pa.st);
    apply_hysteresis(lib, id, as_of, v, pa);
  }

  [[nodiscard]] const DecayConfig &cfg() const noexcept { return cfg_; }

private:
  // Per-alpha bookkeeping. Independent across alphas (no cross-alpha coupling).
  struct PerAlpha {
    std::optional<AdmittedBaseline> base;
    DecayState st;
    std::vector<atx::f64> live_window;
    atx::usize consec_flags = 0; // consecutive Decaying-state flags (toward Dead)
    atx::usize clean_periods = 0; // consecutive clean periods (toward recovery)
  };

  // Map a verdict to a lifecycle mark with asymmetric hysteresis. Reads the
  // current PIT state; only the legal Live<->Decaying<->Dead edges are driven.
  void apply_hysteresis(library::Library &lib, combine::AlphaId id, atx::usize as_of,
                        const DecayVerdict &v, PerAlpha &pa) {
    const auto cur = lib.state_as_of(id, as_of);
    if (!cur.has_value()) {
      return; // a journal fault: leave the state untouched (no guess)
    }
    const library::LifecycleState state = *cur;
    if (state == library::LifecycleState::Live) {
      if (v.flag) {
        ATX_CHECK(lib.mark(id, library::LifecycleState::Decaying, as_of).has_value());
        pa.consec_flags = 1;
        pa.clean_periods = 0;
      }
      return;
    }
    if (state == library::LifecycleState::Decaying) {
      if (v.flag) { // retire FAST after confirm_periods consecutive flags
        pa.consec_flags += 1;
        pa.clean_periods = 0;
        if (pa.consec_flags >= cfg_.confirm_periods) {
          ATX_CHECK(lib.mark(id, library::LifecycleState::Dead, as_of).has_value());
        }
      } else { // restore SLOW after recover_periods consecutive clean periods
        pa.clean_periods += 1;
        pa.consec_flags = 0;
        if (pa.clean_periods >= cfg_.recover_periods) {
          ATX_CHECK(lib.mark(id, library::LifecycleState::Live, as_of).has_value());
        }
      }
      return;
    }
    // Dead / Admitted / Candidate / Recycled: no decay action.
  }

  DecayConfig cfg_{};
  DecayMonitor monitor_{};
  std::unordered_map<atx::u32, PerAlpha> state_;
};

} // namespace atx::engine::book
