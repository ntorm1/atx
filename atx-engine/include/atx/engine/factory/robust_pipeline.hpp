#pragma once

// atx::engine::factory — RobustResearchDriver: the S4.5 END-TO-END robust
// signal-generation pipeline. The deterministic orchestration that fuses the whole
// S4 battery into one run:
//
//   reserve lockbox (S4.4b)  ->  mine + multi-objective gate + robustness gate +
//   library admit (the S4.1-S4.4 ResearchDriver, on SealedPanel.visible())  ->
//   combine (S? AlphaCombiner::fit, ShrinkageMv)  ->  multi-horizon book backtest
//   (the p2 S1 risk::MultiPeriodOptimizer; the p1 S7 multi-period book is the
//   documented fallback).
//
// ===========================================================================
//  Why a WRAPPER (not an extended ResearchDriver)
// ===========================================================================
//  ResearchDriver::run already owns mine -> multi-objective gate (the SearchConfig
//  knobs: objective_mode / novelty_w / fitness.target_aum) -> robustness gate (the
//  S4.4b seam S4.5 filled) -> library admit, and emits the F1 engine fingerprint
//  (digest + manifest_version_id). RobustResearchDriver REUSES it VERBATIM over the
//  visible panel and layers ONLY the two downstream, read-only stages (combine +
//  book) on top. The wrapper takes the LOWER-RISK path the brief asks for:
//
//    * the inner ResearchDriver run is byte-unchanged, so with everything collapsed
//      (ScalarRaw, novelty off, cost off, robustness gate OFF/report-only) the
//      pipeline's run digest EQUALS a plain ResearchDriver::run digest on the SAME
//      visible panel — the pipeline-level boundary pin (an equivalence pin, no
//      golden constant). RobustReport.research IS that inner report verbatim.
//    * the combine + book stages are PURE reads of the grown library (no admit, no
//      RNG, no library mutation), so they cannot perturb the F1 fingerprint.
//
// ===========================================================================
//  The robust subset (combine + book operate on it)
// ===========================================================================
//  When the robustness gate is ON, ResearchDriver screens every admitted survivor
//  with a RobustnessVerdict but does NOT un-admit it (the library lifecycle journal
//  is append-only PIT; un-admitting would be a retroactive relabel). The robust
//  LIBRARY is therefore the SUBSET of admitted alphas that pass the verdict — and
//  the combine + book stages here re-derive that subset by re-screening each
//  admitted alpha's stored OOS PnL with the SAME eval::robustness_verdict over the
//  visible panel's vol-tercile partition. A noise library (0 admits) yields an empty
//  robust subset; combine + book then no-op (the non-vacuity proof). When the gate
//  is OFF the subset is EVERY admitted alpha (no robustness filter — the pin path).
//
// ===========================================================================
//  Determinism (load-bearing)
// ===========================================================================
//  Every stage is deterministic: reserve_lockbox is content-addressed (no RNG);
//  ResearchDriver::run is F1 byte-identical across {1,2,4,8} DetPool workers; the
//  robust-subset re-screen, the combine fit, the K=1 SPD factor model, and the
//  fixed-iteration MultiPeriodOptimizer are all RNG-free and walk the library in
//  ascending AlphaId order before any reduction. So the WHOLE pipeline replays
//  byte-identically — RobustReport.research.{digest,manifest_version_id} is the
//  replay witness, and the book summary (gross / turnover / net) is a pure function
//  of the same inputs. COLD path throughout (one pipeline run); std::vector is fine.

#include <cstddef> // std::size_t
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

#include <Eigen/Core> // Eigen::Index (the K=1 SPD factor model fixture)

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/linalg/linalg.hpp" // MatX, VecX (factor-model assembly)
#include "atx/core/types.hpp"         // atx::f64, atx::u32, atx::u64, atx::usize

#include "atx/engine/alpha/panel.hpp"        // alpha::Panel
#include "atx/engine/alpha/registry.hpp"     // alpha::Library
#include "atx/engine/combine/combiner.hpp"   // combine::AlphaCombiner, Combination, CombinerConfig
#include "atx/engine/combine/gate.hpp"       // combine::AlphaGate
#include "atx/engine/combine/store.hpp"      // combine::AlphaStore
#include "atx/engine/eval/lockbox.hpp"       // eval::reserve_lockbox, SealedPanel
#include "atx/engine/eval/regime_slice.hpp"  // eval::regime_labels, robustness_verdict
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy
#include "atx/engine/risk/factor_model.hpp"  // risk::FactorModel
#include "atx/engine/risk/multi_period.hpp" // risk::MultiPeriodOptimizer, RebalanceSchedule, book::CostInputs

#include "atx/engine/library/library.hpp" // library::Library, AlphaId

#include "atx/engine/factory/research_driver.hpp" // factory::ResearchDriver, ResearchConfig, ResearchReport

namespace atx::engine::factory {

// =========================================================================
//  RobustPipelineConfig — the end-to-end knobs.
//
//  research      : the inner ResearchDriver config (the mine -> gate -> admit
//                  budget + the robustness_gate flag + robustness_cfg). The
//                  multi-objective / novelty / cost levers live in
//                  research.per_run.search; the robustness gate in
//                  research.robustness_gate + research.robustness_cfg.
//  lockbox_frac  : the terminal fraction reserved as the sealed lockbox (S4.4b
//                  reserve_lockbox); mining runs on SealedPanel.visible() only.
//  embargo_len   : the embargo gap (dates) inserted before the lockbox.
//  combiner      : the combine stage config (default ShrinkageMv, combiner.hpp).
//  book          : the book-backtest knobs (the p2 S1 MultiPeriodOptimizer config +
//                  the calibrated cost adapter). gross_leverage / max_iters drive
//                  the fixed-iteration solve; cost.kappa / round_trip_cost_bps /
//                  capacity_gross drive the calibrated net-of-cost book.
// =========================================================================
struct RobustPipelineConfig {
  ResearchConfig research{};
  atx::f64 lockbox_frac = 0.20;
  atx::usize embargo_len = 0;
  combine::CombinerConfig combiner{}; // default == ShrinkageMv (combiner.hpp default)
  risk::MultiPeriodConfig book{};
  book::CostInputs cost{};
};

// =========================================================================
//  RobustBookSummary — the deterministic book-stage roll-up.
//
//  periods       : schedule length the book backtest rolled (== visible periods).
//  mean_gross    : mean Σ|book[s]| over the schedule (realized gross leverage).
//  total_turnover: Σ_s turnover[s] (the realized one-sided L1 trade volume).
//  total_cost_bps: Σ_s cost_bps[s] (the calibrated round-trip cost charged).
//  ran           : true iff the book backtest actually ran (>=1 robust survivor +
//                  a fittable combine); false (all-zero summary) for an empty robust
//                  subset — the noise-library no-op.
// =========================================================================
struct RobustBookSummary {
  atx::usize periods{0};
  atx::f64 mean_gross{0.0};
  atx::f64 total_turnover{0.0};
  atx::f64 total_cost_bps{0.0};
  bool ran{false};
};

// =========================================================================
//  RobustReport — the pipeline result.
//
//  research      : the inner ResearchReport VERBATIM (the F1 replay witness:
//                  digest + manifest_version_id cover the whole mine/gate/admit run;
//                  the collapsed-pipeline equivalence pin compares THIS digest to a
//                  plain ResearchDriver run on the same visible panel).
//  robust_size   : the number of admitted alphas in the ROBUST subset (== admitted
//                  when the gate is off; the verdict-passing subset when on).
//  combined      : the fitted blend over the robust subset (empty weights if the
//                  subset was empty / unfittable).
//  book          : the book-backtest roll-up over the combined book.
//  lockbox_addr  : the content-address of the lockbox reservation (the seal identity;
//                  round-trips byte-identically for the same panel + geometry).
// =========================================================================
struct RobustReport {
  ResearchReport research{};
  atx::usize robust_size{0};
  combine::Combination combined{};
  RobustBookSummary book{};
  atx::u64 lockbox_addr{0};
};

// =========================================================================
//  RobustResearchDriver — the S4.5 end-to-end pipeline wrapper.
//
//  Borrows the persistent library::Library + the run-wide DSL alpha::Library + the
//  FULL research Panel + ExecutionSimulator + WeightPolicy + AlphaGate for its
//  lifetime (same borrow contract as ResearchDriver — see its SAFETY note). The FULL
//  panel is sealed internally; the inner ResearchDriver mines on the visible region.
// =========================================================================
class RobustResearchDriver {
public:
  // SAFETY: every reference is BORROWED for the driver's lifetime (and the run()
  // call). `lib` (the persistent library) is grown in place by the inner mine; `dsl`
  // owns the op rows every mined genome's Expr::op aliases and MUST outlive the
  // driver and all produced genomes; `full_panel` is the FIXED full research panel
  // (sealed internally). The driver stores only references.
  RobustResearchDriver(library::Library &lib, const alpha::Library &dsl,
                       const alpha::Panel &full_panel, const exec::ExecutionSimulator &sim,
                       const WeightPolicy &policy, const combine::AlphaGate &gate) noexcept
      : lib_{lib}, dsl_{dsl}, full_panel_{full_panel}, sim_{sim}, policy_{policy}, gate_{gate} {}

  // Run the full mine -> gate -> admit -> combine -> book pipeline. Deterministic:
  // same cfg + same starting library => byte-identical RobustReport (the inner F1
  // digest + manifest_version_id, AND the combine/book roll-up). Err propagates a
  // lockbox reservation failure (an invalid frac/embargo geometry).
  [[nodiscard]] atx::core::Result<RobustReport> run(const RobustPipelineConfig &cfg) {
    // (1) Reserve + seal the lockbox (S4.4b). Mining runs on visible() ONLY; the
    // sealed terminal region is the S8 held-out judge nothing here may read.
    ATX_TRY(eval::SealedPanel sealed,
            eval::reserve_lockbox(full_panel_, cfg.lockbox_frac, cfg.embargo_len));
    const alpha::Panel &visible = sealed.visible();

    RobustReport out;
    out.lockbox_addr = sealed.reservation().content_address;

    // (2) Mine -> multi-objective gate -> robustness gate -> library admit, VERBATIM
    // ResearchDriver over the visible panel. THIS is the F1 fingerprint the pipeline
    // boundary pin compares to a plain ResearchDriver run on the same visible panel.
    ResearchDriver inner{lib_, dsl_, visible, sim_, policy_, gate_};
    out.research = inner.run(cfg.research);

    // (3) The robust subset: re-screen each admitted alpha's stored OOS PnL with the
    // SAME RobustnessVerdict over the visible vol-tercile partition. Gate OFF => every
    // admit qualifies (no robustness filter — the boundary-pin path). The combine +
    // book stages operate on THIS subset; an empty subset (noise) no-ops them.
    const std::vector<library::AlphaId> robust = robust_subset(visible, cfg);
    out.robust_size = robust.size();

    // (4) Combine (ShrinkageMv) over the robust subset, then (5) the multi-horizon
    // book backtest. Both are pure reads of the grown library (no admit / RNG /
    // mutation), so they never perturb the F1 fingerprint in out.research.
    combine_and_book(visible, robust, cfg, out);
    return atx::core::Ok(std::move(out));
  }

private:
  // The robust subset of admitted alphas (ascending AlphaId order). With the
  // robustness gate OFF every admitted alpha qualifies (the pin path — no filter);
  // ON, only those whose stored OOS PnL passes eval::robustness_verdict over the
  // visible vol-tercile partition. PURE; reads library spans within each iteration
  // (no store growth). Each span is consumed immediately — nothing dangles.
  [[nodiscard]] std::vector<library::AlphaId> robust_subset(const alpha::Panel &visible,
                                                            const RobustPipelineConfig &cfg) const {
    const atx::u64 n = lib_.n_alphas();
    std::vector<library::AlphaId> out;
    out.reserve(static_cast<atx::usize>(n));
    const bool filter = cfg.research.robustness_gate;
    const std::vector<atx::u8> labels =
        filter ? eval::regime_labels(visible, cfg.research.robustness_cfg.vol_window,
                                     eval::kNumRegimes)
               : std::vector<atx::u8>{};
    for (atx::u64 a = 0; a < n; ++a) {
      const library::AlphaId id{static_cast<atx::u32>(a)};
      if (!filter) {
        out.push_back(id);
        continue;
      }
      const std::span<const atx::f64> pnl = lib_.pnl(id);
      if (pnl.size() != labels.size() || labels.empty()) {
        continue; // no usable partition: not robust
      }
      const eval::RobustnessVerdict v = eval::robustness_verdict(
          pnl, std::span<const atx::u8>{labels}, cfg.research.robustness_cfg);
      if (v.is_robust) {
        out.push_back(id);
      }
    }
    return out;
  }

  // (4)+(5): combine the robust subset (AlphaCombiner::fit, ShrinkageMv) and roll the
  // combined book through the p2 S1 MultiPeriodOptimizer. An empty subset / unfittable
  // window leaves out.combined empty and out.book.ran == false (the no-op path).
  void combine_and_book(const alpha::Panel &visible, const std::vector<library::AlphaId> &robust,
                        const RobustPipelineConfig &cfg, RobustReport &out) const {
    if (robust.empty()) {
      return; // noise / empty library: nothing to combine or book
    }
    // Build a fresh AlphaStore over the robust subset (copy each alpha's stored OOS
    // PnL + position cross-sections out of the library BEFORE any growth — the spans
    // alias segment Mappings / the memtable, store.hpp §0.3 dangling-span discipline).
    combine::AlphaStore pool;
    if (!fill_pool(robust, pool)) {
      return; // a shape mismatch / empty store: skip combine + book deterministically
    }
    const combine::AlphaCombiner combiner{cfg.combiner};
    const atx::usize fit_end = pool.n_periods();
    auto fitted = combiner.fit(pool, 0U, fit_end); // ShrinkageMv over the whole window
    if (!fitted.has_value()) {
      return; // a degenerate covariance (e.g. T<2 / single-alpha singularity): no book
    }
    out.combined = std::move(fitted.value());
    book_backtest(visible, pool, out.combined, cfg, out);
  }

  // Copy the robust subset's stored OOS PnL + positions into `pool` (id order). Each
  // library span is consumed immediately into an owned buffer; nothing dangles across
  // the insert that grows the store. Returns false on the first shape mismatch / an
  // empty store (the caller skips combine + book).
  [[nodiscard]] bool fill_pool(const std::vector<library::AlphaId> &robust,
                               combine::AlphaStore &pool) const {
    const atx::usize periods = lib_.n_periods();
    for (const library::AlphaId id : robust) {
      const std::span<const atx::f64> pnl_span = lib_.pnl(id);
      std::vector<atx::f64> pnl{pnl_span.begin(), pnl_span.end()};
      std::vector<atx::f64> pos_flat;
      pos_flat.reserve(periods * full_panel_.instruments());
      for (atx::usize p = 0; p < periods; ++p) {
        const std::span<const atx::f64> cs = lib_.positions(id, p);
        pos_flat.insert(pos_flat.end(), cs.begin(), cs.end());
      }
      const auto ins = pool.insert(nullptr, pnl, pos_flat, lib_.get(id).metrics);
      if (!ins.has_value()) {
        return false;
      }
    }
    return pool.size() != 0U;
  }

  // (5) the multi-horizon book backtest: roll the COMBINED book (the robust subset's
  // last-period target cross-sections blended by out.combined.weights) through the p2
  // S1 risk::MultiPeriodOptimizer over a one-period-per-date schedule, against a K=1
  // SPD factor model + the calibrated cost. Summarize the realized book chain. A solve
  // failure leaves out.book.ran == false (no throw — the pipeline degrades cleanly).
  void book_backtest(const alpha::Panel &visible, const combine::AlphaStore &pool,
                     const combine::Combination &combined, const RobustPipelineConfig &cfg,
                     RobustReport &out) const {
    const atx::usize insts = visible.instruments();
    const atx::usize periods = pool.n_periods();
    if (insts == 0U || periods == 0U) {
      return;
    }
    // The combined per-period alpha cross-section: Σ_a weight[a] · positions(a, s).
    std::vector<std::vector<atx::f64>> combined_cs = combined_cross_sections(pool, combined, insts);
    const risk::FactorModel model = unit_factor_model(insts);

    risk::RebalanceSchedule sched;
    sched.periods.reserve(periods);
    for (atx::usize s = 0; s < periods; ++s) {
      sched.periods.push_back(s);
    }
    const auto alpha_at = [&combined_cs](atx::usize s) -> std::span<const atx::f64> {
      return std::span<const atx::f64>{combined_cs[s]};
    };
    const auto model_at = [&model](atx::usize) -> const risk::FactorModel & { return model; };

    const risk::MultiPeriodOptimizer opt{cfg.book};
    auto books = opt.run(sched, alpha_at, model_at, cfg.cost);
    if (!books.has_value()) {
      return; // a solver/validation reject (e.g. trade_rate domain): no book summary
    }
    summarize_book(books.value(), out.book);
  }

  // Σ_a weight[a]·positions(a, s) for every schedule period s — the combined target
  // cross-section the book optimizer steers toward. pool positions are owned (copied
  // into the store), so these reads never dangle. Ascending (a, s) reductions.
  [[nodiscard]] static std::vector<std::vector<atx::f64>>
  combined_cross_sections(const combine::AlphaStore &pool, const combine::Combination &combined,
                          atx::usize insts) {
    const atx::usize periods = pool.n_periods();
    const atx::usize n = pool.size();
    std::vector<std::vector<atx::f64>> out(periods, std::vector<atx::f64>(insts, 0.0));
    for (atx::usize a = 0; a < n; ++a) {
      const atx::f64 w = (a < combined.weights.size()) ? combined.weights[a] : 0.0;
      for (atx::usize s = 0; s < periods; ++s) {
        const std::span<const atx::f64> cs =
            pool.positions(combine::AlphaId{static_cast<atx::u32>(a)}, s);
        for (atx::usize i = 0; i < insts; ++i) {
          out[s][i] += w * cs[i];
        }
      }
    }
    return out;
  }

  // A K=1 SPD factor model over `insts` instruments (a single unit market factor +
  // a positive specific-variance floor) — the deterministic risk substrate the book
  // optimizer prices wᵀVw against. Mirrors book_bench's make_factor_model SPD form;
  // create() re-checks SPD via Cholesky, so this is a genuine factored covariance.
  [[nodiscard]] static risk::FactorModel unit_factor_model(atx::usize insts) {
    using atx::core::linalg::MatX;
    using atx::core::linalg::VecX;
    const Eigen::Index m = static_cast<Eigen::Index>(insts);
    MatX x(m, 1);
    for (Eigen::Index i = 0; i < m; ++i) {
      x(i, 0) = 1.0; // a single market column
    }
    MatX f(1, 1);
    f(0, 0) = 0.04; // a positive factor variance (SPD 1x1)
    VecX d(m);
    for (Eigen::Index i = 0; i < m; ++i) {
      d[i] = 0.01; // a positive specific-variance floor
    }
    auto fm = risk::FactorModel::create(std::move(x), std::move(f), std::move(d),
                                        /*fit_begin*/ 0U, /*fit_end*/ 1U);
    // The inputs are SPD by construction (positive f + positive d); a create() reject
    // would be a programmer error, so unwrap (the only consumer is this internal book).
    return std::move(fm.value());
  }

  // Fold the realized book chain into the deterministic RobustBookSummary (ascending
  // schedule order). mean_gross = mean_s Σ_i |book[s][i]|.
  static void summarize_book(const risk::MultiPeriodResult &books, RobustBookSummary &sum) {
    sum.periods = books.books.size();
    sum.ran = sum.periods != 0U;
    atx::f64 gross_acc = 0.0;
    for (const std::vector<atx::f64> &book : books.books) {
      atx::f64 g = 0.0;
      for (const atx::f64 w : book) {
        g += (w < 0.0) ? -w : w;
      }
      gross_acc += g;
    }
    sum.mean_gross = (sum.periods == 0U) ? 0.0 : gross_acc / static_cast<atx::f64>(sum.periods);
    for (const atx::f64 t : books.turnover) {
      sum.total_turnover += t;
    }
    for (const atx::f64 c : books.cost_bps) {
      sum.total_cost_bps += c;
    }
  }

  // SAFETY: each borrow is held for the driver's lifetime. lib_ is grown in place by
  // the inner mine; dsl_ owns every OpSig the mined genomes alias and must outlive the
  // driver; full_panel_ is the FIXED full research panel (sealed internally).
  library::Library &lib_;
  const alpha::Library &dsl_;
  const alpha::Panel &full_panel_;
  const exec::ExecutionSimulator &sim_;
  const WeightPolicy &policy_;
  const combine::AlphaGate &gate_;
};

} // namespace atx::engine::factory
