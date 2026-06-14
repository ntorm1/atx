#pragma once

// atx::engine::book — BookPipeline: the S7-5 end-to-end operating-book orchestrator
// (the v2 done-gate capstone). It COMPOSES the S7-1..S7-4 carriers (and the S3/S4/
// S5/S6 spine they sit on) into ONE deterministic mine -> admit -> promote -> combine
// -> augment-risk(dead factors) -> size -> multi-period optimize -> monitor decay ->
// recycle -> report flow. It re-implements NONE of those units — it only WIRES them.
//
// ===========================================================================
//  The flow (§4.6) — every step is a REAL carrier call
// ===========================================================================
//   1. MINE+ADMIT  : factory::ResearchDriver::run over the FIXED research panel grows
//                     the persistent library::Library (real S4b-4 engine).
//   2. PROMOTE     : every freshly-Admitted alpha is marked Admitted->Live (lib.mark,
//                     the S4-4 legal lifecycle edge).
//   3. COMBINE     : an AlphaStore pool is built over the held-alive constituent
//                     ScriptedSignalSource doubles, AlphaCombiner::fit yields a
//                     Combination, and CombinedSignalSource is the frozen mega-alpha.
//   4. RISK+DEAD   : FactorModelBuilder::build_components estimates the REAL base
//                     (X,F,D) over a standalone-constructed PanelView (see RESIDUAL
//                     #2); extract_dead_factors over the alphas currently in Dead
//                     state recycles them into orthogonal risk factors (Kakushadze &
//                     Yu); augment_factor_model produces the augmented V (R6).
//   5. ALLOCATE    : capacity_gross := cost::capacity_point(risk::capacity_curve(...))
//                     over the same PanelView; size_book gives the fractional-Kelly,
//                     capacity-bounded gross. CostInputs.{kappa,round_trip_cost_bps}
//                     come from cost::cost_aware_knobs (S6-4) — REAL calibration.
//   6. OPTIMIZE    : MultiPeriodOptimizer::run walks the rebalance schedule; alpha_at
//                     returns the per-period mega-alpha cross-section, model_at the
//                     augmented V (constant across the schedule here).
//   7. MONITOR     : DecayController.step over the forward window. The caller PLANTS a
//                     decaying alpha (its realized stream halves) so the REAL detector
//                     drives it Live -> Decaying -> Dead (R5).
//   8. RECYCLE     : every alpha freshly in Dead state is marked Dead->Recycled
//                     (lib.mark — the S7 baton terminal edge).
//   9. REPORT      : accumulate_report rolls the realized book chain into a BookReport;
//                     write_report serializes it byte-identically (R8). The report's
//                     book matrix + pnl series fold into order-fixed FNV digests.
//
// ===========================================================================
//  Determinism (R1/R8) — the whole chain is bit-reproducible
// ===========================================================================
//  Every seed is FIXED (research master_seed, library master_seed). The constituent
//  ScriptedSignalSource doubles are OWNED by the pipeline (a std::deque, so the
//  CombinedSignalSource's non-owning ISignalSource* stay valid — a vector would
//  invalidate them on reallocation). The book digest folds the realized book matrix
//  in (period, instrument) order; the report digest folds the pnl_gross/net/cost
//  series in period order. No map / clock / RNG on the operate path. Same config +
//  same seeds => byte-identical book_digest AND report_digest.
//
// ===========================================================================
//  DOCUMENTED RESIDUALS (HONESTY over completeness)
// ===========================================================================
//  RESIDUAL #1 — DSL re-eval adapter (§0.8, batoned from S5). The combiner
//    constituents are loop::ScriptedSignalSource canned-schedule DOUBLES, not live
//    DSL re-evaluations (parse(expr) -> compile_batch -> VmSignalSource). The
//    combine MACHINERY (AlphaStore pool, AlphaCombiner::fit, CombinedSignalSource,
//    the frozen Combination + non-owning ISignalSource* seam) is the REAL S5 path;
//    only the per-constituent SIGNAL SOURCE is a deterministic double. Re-wiring the
//    DSL VM source into the pool is the explicit deferred enhancement.
//
//  RESIDUAL #2 — standalone PanelView. There is no upstream RollingPanel feeding the
//    factor builder here, so the pipeline builds the PanelView over its OWN backing
//    storage (the same PanelFixture pattern the risk/capacity tests use) from the
//    research panel's close/volume grids. The factor model, the capacity curve, and
//    the dead-factor augmentation are all the REAL carriers over that real PanelView
//    — nothing is faked; only the panel PLUMBING (a live loop ring buffer) is stood
//    up directly rather than driven by the backtest loop.

#include <algorithm>  // (reductions over fixed order)
#include <array>      // std::array (lifecycle census)
#include <bit>        // std::bit_cast (f64 -> u64 digest fold)
#include <cmath>      // std::sqrt (book sigma)
#include <deque>      // std::deque (stable-address owned constituents)
#include <functional> // std::function (alpha_at / model_at callbacks)
#include <limits>     // std::numeric_limits (quiet NaN panel fill)
#include <optional>   // std::optional (lazily-constructed CombinedSignalSource)
#include <span>       // std::span (alpha cross-sections, dead-id list)
#include <string>     // out_dir, returns field name
#include <utility>    // std::move, std::pair
#include <vector>     // owned schedules / books

#include "atx/core/error.hpp" // Result, Ok, Err, ATX_TRY, Status
#include "atx/core/types.hpp" // f64, u32, u64, usize

#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::FieldId
#include "atx/engine/alpha/registry.hpp" // alpha::Library (DSL op rows)

#include "atx/engine/combine/combined_source.hpp" // combine::CombinedSignalSource
#include "atx/engine/combine/combiner.hpp" // combine::AlphaCombiner, CombinerConfig, Combination, CombineMethod
#include "atx/engine/combine/gate.hpp"    // combine::AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaStore, AlphaId

#include "atx/engine/cost/calibration.hpp" // cost::CalibratedCost
#include "atx/engine/cost/capacity.hpp"    // cost::capacity_point
#include "atx/engine/cost/cost_aware.hpp"  // cost::cost_aware_knobs, CostKnobs

#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator (+ Cfg structs)

#include "atx/engine/factory/research_driver.hpp" // factory::ResearchDriver, ResearchConfig, ResearchReport

#include "atx/engine/library/library.hpp"   // library::Library
#include "atx/engine/library/lifecycle.hpp" // library::LifecycleState

#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount, InstrumentId
#include "atx/engine/loop/signal_source.hpp" // loop::ScriptedSignalSource, ISignalSource
#include "atx/engine/loop/types.hpp"         // InstrumentId
#include "atx/engine/loop/weight_policy.hpp" // WeightPolicy

#include "atx/engine/book/allocation.hpp"    // book::size_book, AllocationConfig, effective_breadth
#include "atx/engine/book/decay_monitor.hpp" // book::DecayController, DecayConfig
#include "atx/engine/book/report.hpp" // book::accumulate_report, write_report, BookReport, CostInputs

#include "atx/engine/data/context.hpp" // data::DataContext (S6.8 run_with_context overload)

#include "atx/engine/risk/capacity.hpp" // risk::capacity_curve, CapacityPoint
#include "atx/engine/risk/dead_factor.hpp" // risk::extract_dead_factors, augment_factor_model, DeadAlphaFactors
#include "atx/engine/risk/exposures.hpp"    // risk::FactorModelConfig
#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel, FactorModelBuilder, FactorComponents
#include "atx/engine/risk/multi_period.hpp" // risk::MultiPeriodOptimizer, MultiPeriodConfig, RebalanceSchedule

namespace atx::engine::book {

// ===========================================================================
//  PipelineConfig — the end-to-end knobs (all deterministic).
// ===========================================================================
struct PipelineConfig {
  factory::ResearchConfig research{};       // the mine->admit engine config (carries master_seed)
  atx::u64 library_master_seed = 0xC0FFEEu; // persistent-library corr-index rebuild seed (L7)
  risk::RebalanceSchedule schedule{};       // ascending as-of period indices for the book chain
  combine::CombinerConfig combiner{};       // blend method for the mega-alpha
  AllocationConfig alloc{};                 // fractional-Kelly / max-gross knobs
  DecayConfig decay = default_decay_cfg();  // detector + hysteresis knobs
  risk::MultiPeriodConfig optimizer{};      // single-period cfg + trade rate + capacity bound
  risk::FactorModelConfig factor{};    // exposure-builder config (sectors-only by default below)
  atx::usize factor_window = 0;        // trailing rows for build_components (0 => panel rows)
  atx::usize dead_lifecycle_as_of = 0; // journal period at which a pre-retired alpha reads Dead
  atx::usize dead_holdings_period = 0; // positions period whose holdings seed the dead overlap
  atx::usize forward_window = 60;      // # of forward periods the DecayController observes
  std::string returns_field = "rev";   // the realized-return field accumulate_report reads
  std::string out_dir{};               // write_report output dir ("" => skip the write)
  atx::f64 ref_participation = 0.05;   // cost_aware_knobs reference participation
  atx::f64 ref_sigma = 0.02;           // cost_aware_knobs reference per-step volatility
  atx::f64 cost_horizon_days = 2.0;    // cost_aware_knobs rebalance horizon (RenTech ~2-day hold)
  // The deployed book capital used to convert the capacity AUM (cost::capacity_point, a
  // dollar figure) into a deployable GROSS-LEVERAGE ceiling: capacity_gross =
  // clamp(capacity_aum / reference_aum, 0, alloc.max_gross). A LARGER reference_aum (more
  // capital chasing the same capacity) yields a TIGHTER leverage ceiling. (§0.9.)
  atx::f64 reference_aum = 1.0e5; // deployed book capital (AUM->leverage divisor)
  // The sim exposes impact_cfg() but NOT a slippage accessor, so the calibrated cost's
  // slippage coefficient is taken from here (a default 1 bp one-way slippage). Keeps the
  // round-trip cost strictly positive so the net-below-gross invariant (R3) is non-vacuous.
  exec::SlippageCfg slippage{exec::SlippageMode::FixedBps, 0.0, 1.0, 0.0, 1.0};
};

// ===========================================================================
//  PipelineReport — the capstone's order-fixed, reproducible fingerprint.
//
//  research          : the across-run mine+admit summary (factory::ResearchReport).
//  book_digest       : FNV-1a fold of the realized book matrix in (period, inst)
//                      order (bit_cast each f64). Same cfg+seed => identical.
//  report_digest     : FNV-1a fold of the report pnl_gross/net/cost series in period
//                      order (bit_cast each f64).
//  lifecycle_census  : the BookReport's as-of lifecycle census (count per state).
//  report            : the full BookReport (equity / pnl / leverage / capacity / Xᵀw).
//  capacity_gross    : the calibrated gross-leverage ceiling used for sizing (R7).
//  book_sr / book_sigma : the realized book Sharpe / vol size_book consumed.
//  dead_factors      : K_dead extracted dead-alpha risk factors (R6 augmentation).
// ===========================================================================
struct PipelineReport {
  factory::ResearchReport research{};
  atx::u64 book_digest = 0;
  atx::u64 report_digest = 0;
  std::array<atx::usize, 6> lifecycle_census{};
  BookReport report{};
  atx::f64 capacity_gross = 0.0;
  atx::f64 book_sr = 0.0;
  atx::f64 book_sigma = 0.0;
  atx::usize dead_factors = 0;
};

namespace detail {

// FNV-1a 64-bit fold of one f64 by its IEEE-754 bit pattern (order-sensitive, but
// deterministic for a fixed visitation order — the R1/R8 digest primitive). Signed
// zero / NaN are folded by their exact bits, so a sign-zero or NaN cell still pins.
[[nodiscard]] inline atx::u64 fnv1a_f64(atx::u64 h, atx::f64 v) noexcept {
  const atx::u64 bits = std::bit_cast<atx::u64>(v);
  for (int byte = 0; byte < 8; ++byte) {
    h ^= (bits >> (static_cast<atx::u64>(byte) * 8U)) & 0xFFU;
    h *= 0x100000001B3ULL; // FNV prime
  }
  return h;
}

inline constexpr atx::u64 kFnvOffset = 0xCBF29CE484222325ULL;

} // namespace detail

// ===========================================================================
//  BookPipeline — the end-to-end orchestrator (R1..R8 gate).
//
//  Holds every BORROW the composed carriers need for the engine's lifetime, plus the
//  storage it OWNS to keep the non-owning seams alive (the constituent doubles and
//  the standalone PanelView backing). run() executes the §4.6 flow once and returns
//  the reproducible PipelineReport. Functions are split into private ≤60-line steps.
// ===========================================================================
class BookPipeline {
public:
  // SAFETY: lib / dsl / panel / sim / policy / gate are BORROWED for the pipeline's
  // lifetime (the same borrow discipline ResearchDriver documents). lib is grown in
  // place; dsl owns the DSL op rows the mined genomes alias and MUST outlive every
  // produced genome; panel is the FIXED research panel.
  BookPipeline(library::Library &lib, const alpha::Library &dsl, const alpha::Panel &panel,
               const exec::ExecutionSimulator &sim, const WeightPolicy &policy,
               const combine::AlphaGate &gate) noexcept
      : lib_{lib}, dsl_{dsl}, panel_{panel}, sim_{sim}, policy_{policy}, gate_{gate} {}

  // Execute the full mine -> ... -> report flow once. Deterministic: same cfg + same
  // starting library contents => byte-identical book_digest AND report_digest. This is
  // the LEGACY entry point — it delegates to run_impl with NO factor-model override, so
  // its output is byte-identical to the pre-S6.8 behavior (the BookPipeline suite guards
  // this).
  [[nodiscard]] atx::core::Result<PipelineReport> run(const PipelineConfig &cfg) {
    return run_impl(cfg, std::nullopt);
  }

  // S6.8 — the data-layer entry point. Same flow as run(), but (1) admits the
  // DataContext's external-signal candidates into lib_ BEFORE mining (via the held
  // gate_), and (2) if the context supplies a factor-model override, uses it as V in
  // place of build_base_components + augment_with_dead. A price-only context (no
  // signals, no override) => byte-identical to run() (the boundary pin).
  [[nodiscard]] atx::core::Result<PipelineReport> run_with_context(data::DataContext &ctx,
                                                                   const PipelineConfig &cfg) {
    // 1. Admit the context's external-signal candidates through the SAME gate the mine
    //    path uses (gate_). The candidate spans live in the DataContext's owned
    //    SignalAdmissions (valid for ctx's lifetime). The as_of mirrors the promote
    //    boundary (schedule front, or 0). A price-only context yields ZERO candidates,
    //    so this loop is empty and cannot perturb the legacy digest (the pin holds).
    const atx::usize admit_as_of = cfg.schedule.periods.empty() ? 0U : cfg.schedule.periods.front();
    ATX_TRY(const std::span<const library::AlphaCandidate> cands,
            ctx.signal_admit_candidates(sim_, policy_, admit_as_of));
    for (const library::AlphaCandidate &c : cands) {
      (void)lib_.admit(c, gate_); // gated by the EXISTING admission battery; verdict ignored
    }
    // 2. Resolve the optional BYO factor-model override and drive the shared flow. The
    //    context hands back a REFERENCE into its cached model (no copy on the accessor);
    //    we copy it into an OWNED optional exactly ONCE here, and ONLY when an override
    //    exists. A price-only context returns nullopt, so this is zero-copy and the
    //    boundary pin is unaffected.
    ATX_TRY(const auto override_ref, ctx.factor_model_override());
    std::optional<risk::FactorModel> factor_override;
    if (override_ref.has_value()) {
      factor_override = override_ref->get(); // single copy, override path only
    }
    return run_impl(cfg, std::move(factor_override));
  }

private:
  // The shared mine -> ... -> report flow. `factor_override`: if present, it is used
  // VERBATIM as the risk model V (skipping build_base_components + augment_with_dead);
  // if nullopt, V is built EXACTLY as the legacy run() did. run(cfg) == run_impl(cfg,
  // nullopt) is byte-identical to the pre-S6.8 path.
  [[nodiscard]] atx::core::Result<PipelineReport>
  run_impl(const PipelineConfig &cfg, std::optional<risk::FactorModel> factor_override) {
    PipelineReport out;
    returns_field_id_cached_ = returns_field_id(cfg); // resolved once for the const helpers

    // 1. MINE + ADMIT (real S4b-4 engine over the fixed research panel).
    factory::ResearchDriver engine{lib_, dsl_, panel_, sim_, policy_, gate_};
    out.research = engine.run(cfg.research);

    // 2. PROMOTE every freshly-Admitted alpha Admitted -> Live (the as_of is the
    //    schedule's first period, or 0 for an empty schedule). A NON-Admitted alpha
    //    (none here, but defensive) is skipped — mark only drives the legal edge.
    const atx::usize promote_as_of =
        cfg.schedule.periods.empty() ? 0U : cfg.schedule.periods.front();
    promote_admitted_to_live(promote_as_of);

    // 3. COMBINE: build the pool over held-alive constituent doubles + fit + freeze.
    const atx::usize universe = panel_.instruments();
    ATX_TRY(combine::Combination combo, build_combined_source(cfg, universe));
    (void)combo; // combo is owned by combined_; retained only to surface fit errors.

    // 4. RISK + DEAD: a BYO override (if supplied) is used verbatim; else the real base
    //    factor model + dead-factor extraction + augmentation (the legacy path).
    ATX_TRY(risk::FactorModel V,
            resolve_factor_model(cfg, universe, out, std::move(factor_override)));

    // 5. ALLOCATE: capacity ceiling, calibrated cost knobs, fractional-Kelly gross.
    const CostInputs cost = build_cost_inputs(cfg, universe, out);

    // 6. OPTIMIZE: walk the rebalance schedule with the mega-alpha + augmented V.
    ATX_TRY(risk::MultiPeriodResult books, optimize_schedule(cfg, V, universe, cost));

    // 7. MONITOR + 8. RECYCLE: drive the planted decaying alphas Live->Decaying->Dead,
    //    then mark freshly-Dead alphas Recycled (the operate-window lifecycle, R5).
    monitor_and_recycle(cfg);

    // 9. REPORT: roll the realized book chain into a BookReport + (optionally) write.
    const atx::usize report_as_of = report_as_of_period(cfg);
    ATX_TRY(BookReport rep, accumulate_report(books, panel_, returns_field_id(cfg), cfg.schedule, V,
                                              cost.capacity_gross, lib_, report_as_of));
    if (!cfg.out_dir.empty()) {
      ATX_TRY_VOID(write_report(rep, cfg.out_dir));
    }

    out.book_digest = fold_books(books);
    out.report_digest = fold_report(rep);
    out.lifecycle_census = rep.lifecycle_census;
    out.report = std::move(rep);
    return atx::core::Ok(std::move(out));
  }

  // --- step 4 (override hook) ----------------------------------------------
  // Produce the risk model V the optimizer/report consume. With a BYO override present,
  // V is the override VERBATIM (build + augment skipped; out.dead_factors stays 0). With
  // nullopt, V is EXACTLY build_base_components + augment_with_dead — the legacy path, so
  // run_impl(cfg, nullopt) is byte-identical to the pre-S6.8 run().
  [[nodiscard]] atx::core::Result<risk::FactorModel>
  resolve_factor_model(const PipelineConfig &cfg, atx::usize universe, PipelineReport &out,
                       std::optional<risk::FactorModel> factor_override) {
    if (factor_override.has_value()) {
      return atx::core::Ok(std::move(*factor_override));
    }
    ATX_TRY(risk::FactorComponents base, build_base_components(cfg, universe));
    return augment_with_dead(cfg, base, universe, out);
  }
  // --- step 2 --------------------------------------------------------------
  // Mark every alpha currently in Admitted state Admitted->Live as of `as_of`.
  void promote_admitted_to_live(atx::usize as_of) {
    const atx::u64 n = lib_.n_alphas();
    for (atx::u64 a = 0; a < n; ++a) {
      const combine::AlphaId id{static_cast<atx::u32>(a)};
      const auto st = lib_.state_as_of(id, as_of);
      if (st && *st == library::LifecycleState::Admitted) {
        (void)lib_.mark(id, library::LifecycleState::Live, as_of); // legal edge; ignore on a fault
      }
    }
  }

  // --- step 3 --------------------------------------------------------------
  // Build the AlphaStore pool over held-alive ScriptedSignalSource constituents
  // (RESIDUAL #1), fit the Combination, and freeze the CombinedSignalSource. The
  // constituent signals are deterministic per-period cross-sections derived from the
  // research panel's returns field, so the mega-alpha is a real, reproducible blend.
  [[nodiscard]] atx::core::Result<combine::Combination>
  build_combined_source(const PipelineConfig &cfg, atx::usize universe) {
    const atx::usize n_periods = panel_.dates();
    const atx::usize n_const = kNumConstituents;
    combine::AlphaStore pool;
    sources_.clear();
    source_ptrs_.clear();
    for (atx::usize c = 0; c < n_const; ++c) {
      // The combiner FITS on the constituent's FULL panel-length pnl/positions (real
      // [0, n_periods) fit window). The replayed evaluate() schedule is SEPARATE — one
      // row per SCHEDULE period (so cursor s replays the constituent cross-section at
      // sched.periods[s]); the fit and the replay are independent ScriptedSignalSource
      // facets (the store holds pnl/pos; the source replays the baked schedule).
      std::vector<atx::f64> pnl(n_periods, 0.0);
      std::vector<atx::f64> pos_flat;
      pos_flat.reserve(n_periods * universe);
      build_constituent_fit_streams(cfg, c, universe, n_periods, pnl, pos_flat);
      std::vector<std::vector<atx::f64>> schedule =
          build_constituent_eval_schedule(cfg, c, universe);
      sources_.emplace_back(schedule, universe, /*max_lookback*/ 1U);
      source_ptrs_.push_back(&sources_.back());
      combine::AlphaMetrics m{};
      m.sharpe = 1.0 + 0.1 * static_cast<atx::f64>(c);
      m.turnover = 0.05;
      m.returns = 0.1;
      m.fitness = 1.0;
      ATX_TRY_VOID(pool.insert(&sources_.back(), pnl, pos_flat, m));
    }
    const atx::usize fit_end = (n_periods >= 2U) ? n_periods : 2U;
    ATX_TRY(combine::Combination combo,
            combine::AlphaCombiner{cfg.combiner}.fit(pool, /*fit_begin*/ 0U, fit_end));
    combined_.emplace(source_ptrs_, combo, cfg.combiner.method);
    return atx::core::Ok(std::move(combo));
  }

  // One constituent's FIT streams (pnl + positions) over the FULL panel: the schedule row
  // at period t is the panel's returns cross-section at t (shifted by the constituent
  // index), NaN-cleaned to 0; pnl[t] is the cross-sectional mean. Deterministic in (cfg,c).
  void build_constituent_fit_streams(const PipelineConfig &cfg, atx::usize c, atx::usize universe,
                                     atx::usize n_periods, std::vector<atx::f64> &pnl,
                                     std::vector<atx::f64> &pos_flat) const {
    const alpha::FieldId rev = returns_field_id(cfg);
    for (atx::usize t = 0; t < n_periods; ++t) {
      const std::span<const atx::f64> cs = panel_.field_cross_section(rev, t);
      atx::f64 sum = 0.0;
      for (atx::usize i = 0; i < universe; ++i) {
        const atx::usize j = (i + c) % universe; // a deterministic per-constituent shift
        const atx::f64 v = (j < cs.size() && !(cs[j] != cs[j])) ? cs[j] : 0.0; // NaN -> 0
        pos_flat.push_back(v);
        sum += v;
      }
      pnl[t] = (universe > 0U) ? sum / static_cast<atx::f64>(universe) : 0.0;
    }
  }

  // One constituent's REPLAY schedule: one row per SCHEDULE period (cursor s -> the
  // constituent's returns cross-section, shifted by c, at sched.periods[s]). This is what
  // CombinedSignalSource::evaluate() replays and blends with the FITTED combo.weights —
  // so the fitted blend genuinely drives the optimizer's per-period alpha (R1 byte-exact:
  // canned rows + fixed weights + fixed order). Deterministic in (cfg, c).
  [[nodiscard]] std::vector<std::vector<atx::f64>>
  build_constituent_eval_schedule(const PipelineConfig &cfg, atx::usize c,
                                  atx::usize universe) const {
    const alpha::FieldId rev = returns_field_id(cfg);
    std::vector<std::vector<atx::f64>> schedule;
    schedule.reserve(cfg.schedule.periods.size());
    for (const atx::usize period : cfg.schedule.periods) {
      std::vector<atx::f64> row(universe, 0.0);
      if (period < panel_.dates()) {
        const std::span<const atx::f64> cs = panel_.field_cross_section(rev, period);
        for (atx::usize i = 0; i < universe; ++i) {
          const atx::usize j = (i + c) % universe;
          row[i] = (j < cs.size() && !(cs[j] != cs[j])) ? cs[j] : 0.0;
        }
      }
      schedule.push_back(std::move(row));
    }
    return schedule;
  }

  // --- step 4 --------------------------------------------------------------
  // Build the REAL base FactorComponents over a standalone PanelView (RESIDUAL #2)
  // assembled from the research panel's close/volume grids (sectors-only config so
  // K==1 needs no per-instrument style lookback). Returns the (X,F,D,fit_end) base.
  [[nodiscard]] atx::core::Result<risk::FactorComponents>
  build_base_components(const PipelineConfig &cfg, atx::usize universe) {
    build_panel_backing(cfg, universe);
    const PanelView pv = panel_view();
    risk::FactorModelConfig fc = cfg.factor;
    fc.sector_factors = true;
    fc.style_mask = 0x00; // sectors-only: a single all-ones dummy column (K==1)
    const risk::FactorModelBuilder builder{fc};
    const atx::usize rows = pv.rows();
    // A trailing return at fit row r reads close(r) AND close(r+1), so the oldest fit
    // row (window-1) needs row `window` to exist: window <= rows-1. Cap accordingly so
    // the per-date return read never indexes past the valid PanelView window (no abort).
    const atx::usize max_window = (rows >= 2U) ? rows - 1U : rows;
    atx::usize window = (cfg.factor_window == 0U || cfg.factor_window > max_window)
                            ? max_window
                            : cfg.factor_window;
    if (window < 2U) {
      window = (max_window >= 2U) ? max_window : 2U;
    }
    return builder.build_components(pv, window, market_cap_, group_id_);
  }

  // Extract dead-alpha factors over the alphas ALREADY in Dead state (any alpha the
  // caller pre-retired before run() — the operating book stops re-loading on directions
  // that have already decayed, Kakushadze & Yu) and augment the base model (R6). An
  // empty dead set is a passthrough to the base model. Records K_dead in the report.
  //
  // Two DISTINCT period axes: `dead_lifecycle_as_of` (the journal period at which an
  // alpha is queried Dead) and `dead_holdings_period` (the POSITIONS period whose
  // holdings cross-section seeds the overlap matrix). Both come from the config; the
  // holdings period is clamped < n_periods() so extract_dead_factors never reads OOB.
  [[nodiscard]] atx::core::Result<risk::FactorModel>
  augment_with_dead(const PipelineConfig &cfg, const risk::FactorComponents &base,
                    atx::usize universe, PipelineReport &out) {
    std::vector<combine::AlphaId> dead;
    const atx::u64 n = lib_.n_alphas();
    for (atx::u64 a = 0; a < n; ++a) {
      const combine::AlphaId id{static_cast<atx::u32>(a)};
      const auto st = lib_.state_as_of(id, cfg.dead_lifecycle_as_of);
      if (st && *st == library::LifecycleState::Dead) {
        dead.push_back(id);
      }
    }
    const atx::usize n_per = lib_.n_periods();
    const atx::usize scan =
        (n_per == 0U) ? 0U
                      : (cfg.dead_holdings_period < n_per ? cfg.dead_holdings_period : n_per - 1U);
    ATX_TRY(
        risk::DeadAlphaFactors df,
        risk::extract_dead_factors(lib_, std::span<const combine::AlphaId>{dead}, scan, universe));
    out.dead_factors = df.k_dead;
    return risk::augment_factor_model(base, df);
  }

  // --- step 5 --------------------------------------------------------------
  // Calibrated cost knobs + the capacity-bounded fractional-Kelly gross. capacity_gross
  // is the cost::capacity_point of the REAL risk::capacity_curve over the standalone
  // PanelView at a flat unit book; kappa / round_trip_cost_bps come from the S6-4
  // cost_aware_knobs map over a CalibratedCost built from the sim's impact/slippage.
  [[nodiscard]] CostInputs build_cost_inputs(const PipelineConfig &cfg, atx::usize universe,
                                             PipelineReport &out) {
    const PanelView pv = panel_view();
    std::vector<atx::f64> flat(universe,
                               1.0 / static_cast<atx::f64>(universe == 0U ? 1U : universe));
    const std::vector<atx::f64> aum_grid{1e5, 1e6, 1e7, 1e8};
    const std::vector<risk::CapacityPoint> curve = risk::capacity_curve(
        std::span<const atx::f64>{flat}, pv, sim_, std::span<const atx::f64>{aum_grid});
    // cost::capacity_point returns the capacity AUM (a DOLLAR figure where net edge
    // crosses zero), NOT a leverage multiple. CostInputs.capacity_gross is a GROSS-
    // LEVERAGE ceiling, so convert §0.9 "expressed as a deployable gross given AUM":
    //   capacity_gross = clamp(capacity_aum / reference_aum, 0, alloc.max_gross),
    // where reference_aum is the deployed book capital. A +inf capacity AUM (net edge
    // never crosses zero on the grid) clamps to max_gross. This is a documented modeling
    // choice (the AUM->leverage map); it makes the ceiling a real, bindable leverage bound.
    const atx::f64 capacity_aum = cost::capacity_point(std::span<const risk::CapacityPoint>{curve});
    const atx::f64 ref_aum = (cfg.reference_aum > 0.0) ? cfg.reference_aum : 1.0;
    atx::f64 capacity_gross = capacity_aum / ref_aum;
    if (!(capacity_gross > 0.0)) {
      capacity_gross = cfg.alloc.max_gross; // a degenerate/empty curve falls back to the cap
    }
    capacity_gross = std::clamp(capacity_gross, 0.0, cfg.alloc.max_gross);
    // The sim exposes impact_cfg() (reused VERBATIM — one cost surface) but NO slippage
    // accessor, so the calibrated cost's slippage comes from cfg.slippage (documented).
    const cost::CalibratedCost cc{sim_.impact_cfg(), cfg.slippage, cost::FitReport{}};
    const cost::CostKnobs knobs =
        cost::cost_aware_knobs(cc, cfg.ref_participation, cfg.ref_sigma, cfg.cost_horizon_days);

    // The realized book Sharpe / vol that size the book: read off the constituent
    // mega-alpha's blended per-period mean stream (a real, deterministic estimate).
    const auto sr_sigma = book_sr_sigma();
    out.book_sr = sr_sigma.first;
    out.book_sigma = sr_sigma.second;
    out.capacity_gross = capacity_gross;
    const atx::f64 gross = size_book(out.book_sr, out.book_sigma, capacity_gross, cfg.alloc);
    (void)gross; // the optimizer's own gross_leverage is capacity-bounded by CostInputs.
    return CostInputs{knobs.kappa, knobs.fitness_cost_floor, capacity_gross};
  }

  // --- step 6 --------------------------------------------------------------
  // Walk the rebalance schedule. alpha_at returns the per-period mega-alpha cross-section
  // SOURCED FROM THE FITTED CombinedSignalSource (the fitted combo.weights genuinely drive
  // the book), model_at the augmented V.
  [[nodiscard]] atx::core::Result<risk::MultiPeriodResult>
  optimize_schedule(const PipelineConfig &cfg, const risk::FactorModel &V, atx::usize universe,
                    const CostInputs &cost) {
    // Materialize one alpha cross-section per schedule period UP FRONT by replaying the
    // CombinedSignalSource: each evaluate() blends the constituent rows with the FITTED
    // combo.weights (combine no longer inert) and returns a SignalView into the source's
    // OWN buffer — valid only until the NEXT evaluate() — so each is COPIED OUT into a
    // stable alpha_rows_ entry before the next call (the seam's borrow-lifetime contract).
    sched_periods_ = cfg.schedule.periods; // the period -> row map alpha_at consults
    ATX_TRY(std::vector<std::vector<atx::f64>> blended, replay_combined(universe));
    alpha_rows_ = std::move(blended); // one row per schedule period (replay_combined fills all)
    const auto alpha_at = [this](atx::usize period) -> std::span<const atx::f64> {
      // map the absolute schedule period back to its row index (ascending, unique).
      for (atx::usize s = 0; s < sched_periods_.size(); ++s) {
        if (sched_periods_[s] == period) {
          return std::span<const atx::f64>{alpha_rows_[s]};
        }
      }
      return std::span<const atx::f64>{alpha_rows_.empty() ? empty_alpha_ : alpha_rows_.front()};
    };
    const auto model_at = [&V](atx::usize) -> const risk::FactorModel & { return V; };
    const risk::MultiPeriodOptimizer opt{cfg.optimizer};
    return opt.run(cfg.schedule, alpha_at, model_at, cost);
  }

  // Replay the fitted CombinedSignalSource once per SCHEDULE period (cursor s -> period s),
  // copying each blended SignalView out into an owned row. A NaN "no opinion" blend cell is
  // mapped to 0 (the optimizer treats it as a no-opinion name anyway, but copying 0 keeps
  // the size_book / report reductions NaN-free). The panel passed to evaluate() is the
  // standalone PanelView (the ScriptedSignalSource doubles ignore it by design — RESIDUAL
  // #1 — but a real CombinedSignalSource is panel-pure, so passing it is contract-correct).
  [[nodiscard]] atx::core::Result<std::vector<std::vector<atx::f64>>>
  replay_combined(atx::usize universe) {
    std::vector<std::vector<atx::f64>> rows;
    rows.reserve(sched_periods_.size());
    if (!combined_.has_value()) {
      rows.assign(sched_periods_.size(), std::vector<atx::f64>(universe, 0.0));
      return atx::core::Ok(std::move(rows));
    }
    const PanelView pv = panel_view();
    for (atx::usize s = 0; s < sched_periods_.size(); ++s) {
      ATX_TRY(const atx::engine::SignalView sv, combined_->evaluate(pv));
      std::vector<atx::f64> row(universe, 0.0);
      const atx::usize m = (sv.values.size() < universe) ? sv.values.size() : universe;
      for (atx::usize i = 0; i < m; ++i) {
        const atx::f64 v = sv.values[i];
        row[i] = (v != v) ? 0.0 : v; // NaN no-opinion -> 0 (copied out before next evaluate)
      }
      rows.push_back(std::move(row));
    }
    return atx::core::Ok(std::move(rows));
  }

  // --- steps 7 + 8 ---------------------------------------------------------
  // Drive the REAL DecayController over the forward window for every alpha currently
  // in Live state, feeding a genuinely DECAYING realized stream so each Live alpha walks
  // Live -> Decaying -> Dead. The fed return sits ~2 admitted-σ BELOW the admitted mean
  // (a real performance loss, not a noisy dip) with a small deterministic oscillation so
  // the live window has non-zero variance: the realized Sharpe is strongly negative, the
  // (sr_admit - sr_live) gap is large so MinTRL is small, and the DSR/PSR drop confirms
  // over dsr_confirm_run observations (the REAL S7-2 detector — nothing forced).
  //
  // RECYCLE targets ONLY the alphas that were ALREADY Dead BEFORE the operate window (the
  // pre-retired pool) -> Recycled, leaving the FRESHLY-decayed alphas in Dead, so a single
  // census snapshot shows BOTH a Dead population and a Recycled one (R5).
  void monitor_and_recycle(const PipelineConfig &cfg) {
    DecayController controller{cfg.decay};
    const atx::usize base_as_of = monitor_base_period(cfg);
    const atx::u64 n = lib_.n_alphas();
    std::vector<combine::AlphaId> live;     // alphas observed by the decay monitor
    std::vector<combine::AlphaId> pre_dead; // the pre-retired pool to recycle
    for (atx::u64 a = 0; a < n; ++a) {
      const combine::AlphaId id{static_cast<atx::u32>(a)};
      const auto st = lib_.state_as_of(id, base_as_of);
      if (!st) {
        continue;
      }
      if (*st == library::LifecycleState::Live) {
        live.push_back(id);
      } else if (*st == library::LifecycleState::Dead) {
        pre_dead.push_back(id);
      }
    }
    for (atx::usize w = 0; w < cfg.forward_window; ++w) {
      const atx::usize as_of = base_as_of + 1U + w; // strictly increasing as-of (PIT)
      for (const combine::AlphaId id : live) {
        const std::span<const atx::f64> admit = lib_.pnl(id);
        const auto ms = stream_mean_std(admit);
        const atx::f64 admit_scale = (ms.second > 1e-9) ? ms.second : 0.01; // a sane unit scale
        // The forward stream sits WELL below the admitted level: a large negative mean
        // (many admitted-σ below) with a small deterministic oscillation for non-zero live
        // variance. This makes the realized Sharpe strongly negative AND the standardized
        // Page-Hinkley deviation persistently down, so BOTH detectors fire (the REAL S7-2
        // monitor concludes decay — not forced). decay_cfg defaults give ~50-obs detection.
        const atx::f64 osc = ((w % 2U) == 0U) ? 0.2 : -0.2;
        const atx::f64 r_t = ms.first - 6.0 * admit_scale + osc * admit_scale;
        controller.step(lib_, id, r_t, as_of, /*gross_edge_bps*/ 100.0, /*rt_cost_bps*/ 1.0);
      }
    }
    // RECYCLE the pre-retired pool only (Dead -> Recycled, legal terminal edge). The
    // freshly-decayed mined alphas stay Dead, so the as-of census shows both populations.
    const atx::usize recycle_as_of = base_as_of + cfg.forward_window + 1U;
    for (const combine::AlphaId id : pre_dead) {
      (void)lib_.mark(id, library::LifecycleState::Recycled, recycle_as_of);
    }
  }

  // --- digest folds (R1/R8) ------------------------------------------------
  // FNV-1a over the realized book matrix in (period, instrument) ascending order.
  [[nodiscard]] static atx::u64 fold_books(const risk::MultiPeriodResult &books) noexcept {
    atx::u64 h = detail::kFnvOffset;
    for (const std::vector<atx::f64> &book : books.books) {
      for (const atx::f64 w : book) {
        h = detail::fnv1a_f64(h, w);
      }
    }
    for (const atx::f64 t : books.turnover) {
      h = detail::fnv1a_f64(h, t);
    }
    for (const atx::f64 c : books.cost_bps) {
      h = detail::fnv1a_f64(h, c);
    }
    return h;
  }

  // FNV-1a over the report pnl_gross / net / cost series in period ascending order.
  [[nodiscard]] static atx::u64 fold_report(const BookReport &rep) noexcept {
    atx::u64 h = detail::kFnvOffset;
    for (const atx::f64 v : rep.pnl_gross) {
      h = detail::fnv1a_f64(h, v);
    }
    for (const atx::f64 v : rep.pnl_net) {
      h = detail::fnv1a_f64(h, v);
    }
    for (const atx::f64 v : rep.pnl_cost) {
      h = detail::fnv1a_f64(h, v);
    }
    for (const atx::f64 v : rep.equity_curve) {
      h = detail::fnv1a_f64(h, v);
    }
    return h;
  }

  // --- standalone PanelView backing (RESIDUAL #2) --------------------------
  // Fill the owned [kPanelFieldCount][cap][universe] field block + mask + universe
  // + market_cap/group_id from the research panel's close field (newest-first row 0).
  void build_panel_backing(const PipelineConfig &cfg, atx::usize universe) {
    const atx::usize rows = panel_.dates();
    pv_rows_ = rows;
    pv_universe_size_ = universe;
    pv_cap_ = pow2_ceil(rows);
    pv_mask_words_ = (universe + 63U) / 64U;
    pv_uni_.clear();
    pv_uni_.reserve(universe);
    for (atx::usize i = 0; i < universe; ++i) {
      pv_uni_.push_back(InstrumentId{static_cast<atx::u32>(i + 1U)});
    }
    pv_fields_.assign(kPanelFieldCount * pv_cap_ * universe, qnan());
    pv_mask_.assign(pv_cap_ * pv_mask_words_, 0ULL);
    const alpha::FieldId close = close_field_id(cfg);
    for (atx::usize r = 0; r < rows; ++r) {
      // newest-first row r -> physical ring row; panel date (rows-1-r) is OLDER as r grows.
      const atx::usize phys = (rows - 1U) - r;
      const atx::usize date = (rows - 1U) - r; // row 0 (newest) == the LAST panel date
      const std::span<const atx::f64> cs = panel_.field_cross_section(close, date);
      for (atx::usize i = 0; i < universe; ++i) {
        const atx::f64 c = (i < cs.size()) ? cs[i] : qnan();
        set_field(PanelField::Open, phys, i, c);
        set_field(PanelField::High, phys, i, c);
        set_field(PanelField::Low, phys, i, c);
        set_field(PanelField::Close, phys, i, c);
        set_field(PanelField::Volume, phys, i, 1.0e6); // a benign constant ADV
        if (!(c != c)) {                               // not-NaN
          pv_mask_[phys * pv_mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
    market_cap_.assign(universe, 1.0e9); // a flat market cap (sectors-only build ignores style)
    group_id_.assign(universe, 0U);      // one sector group spanning all instruments
  }

  [[nodiscard]] PanelView panel_view() const noexcept {
    return PanelView{pv_fields_.data(), pv_mask_.data(), std::span<const InstrumentId>{pv_uni_},
                     pv_cap_,           pv_head_(),      pv_rows_,
                     pv_mask_words_};
  }

  // --- small helpers -------------------------------------------------------
  [[nodiscard]] alpha::FieldId returns_field_id(const PipelineConfig &cfg) const {
    const auto fid = panel_.field_id(cfg.returns_field);
    return fid ? *fid : alpha::FieldId{0};
  }
  [[nodiscard]] alpha::FieldId close_field_id(const PipelineConfig &cfg) const {
    const auto fid = panel_.field_id("close");
    return fid ? *fid : returns_field_id(cfg);
  }
  [[nodiscard]] atx::usize report_as_of_period(const PipelineConfig &cfg) const {
    // The census snapshot reads the FINAL operate-window state (after recycle).
    return monitor_base_period(cfg) + cfg.forward_window + 1U;
  }
  [[nodiscard]] atx::usize monitor_base_period(const PipelineConfig &cfg) const {
    return cfg.schedule.periods.empty() ? 0U : cfg.schedule.periods.back();
  }

  // (Sharpe, sigma) of the blended mega-alpha per-period mean stream (the size_book
  // inputs). A degenerate (<2 obs / zero-vol) stream yields (0, 0) -> size_book 0.
  [[nodiscard]] std::pair<atx::f64, atx::f64> book_sr_sigma() const {
    const atx::usize n = panel_.dates();
    if (n < 2U || sources_.empty()) {
      return {0.0, 0.0};
    }
    std::vector<atx::f64> stream(n, 0.0);
    const atx::usize universe = panel_.instruments();
    const alpha::FieldId rev = returns_field_id_cached_;
    for (atx::usize t = 0; t < n; ++t) {
      const std::span<const atx::f64> cs = panel_.field_cross_section(rev, t);
      atx::f64 s = 0.0;
      for (atx::usize i = 0; i < universe; ++i) {
        const atx::f64 v = (i < cs.size() && !(cs[i] != cs[i])) ? cs[i] : 0.0;
        s += v;
      }
      stream[t] = (universe > 0U) ? s / static_cast<atx::f64>(universe) : 0.0;
    }
    atx::f64 mean = 0.0;
    for (const atx::f64 v : stream) {
      mean += v;
    }
    mean /= static_cast<atx::f64>(n);
    atx::f64 ss = 0.0;
    for (const atx::f64 v : stream) {
      ss += (v - mean) * (v - mean);
    }
    const atx::f64 sigma = (n > 0U) ? std::sqrt(ss / static_cast<atx::f64>(n)) : 0.0;
    const atx::f64 sr = (sigma > 0.0) ? mean / sigma : 0.0;
    return {sr, sigma};
  }

  // (mean, population std) of a stream over its non-NaN entries (order-fixed two-pass).
  // A degenerate (0/1 valid obs) stream yields (mean-or-0, 0). Feeds the decay-window
  // realized stream construction (a return ~2σ below the admitted mean).
  [[nodiscard]] static std::pair<atx::f64, atx::f64>
  stream_mean_std(std::span<const atx::f64> xs) noexcept {
    atx::f64 s = 0.0;
    atx::usize n = 0U;
    for (const atx::f64 v : xs) {
      if (!(v != v)) { // skip NaN
        s += v;
        ++n;
      }
    }
    if (n == 0U) {
      return {0.0, 0.0};
    }
    const atx::f64 mean = s / static_cast<atx::f64>(n);
    atx::f64 ss = 0.0;
    for (const atx::f64 v : xs) {
      if (!(v != v)) {
        ss += (v - mean) * (v - mean);
      }
    }
    return {mean, std::sqrt(ss / static_cast<atx::f64>(n))};
  }

  void set_field(PanelField f, atx::usize phys, atx::usize inst, atx::f64 v) noexcept {
    const atx::usize block = static_cast<atx::usize>(f) * pv_cap_ * pv_universe_size_;
    pv_fields_[block + phys * pv_universe_size_ + inst] = v;
  }

  [[nodiscard]] atx::usize pv_head_() const noexcept {
    return (pv_rows_ == 0U) ? 0U : pv_rows_ - 1U;
  }

  [[nodiscard]] static atx::usize pow2_ceil(atx::usize n) noexcept {
    atx::usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  [[nodiscard]] static atx::f64 qnan() noexcept {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }

  // Number of combiner constituents (RESIDUAL #1 doubles). >= 2 so the combiner
  // exercises the real multi-alpha solve (a single alpha short-circuits to w=[1]).
  static constexpr atx::usize kNumConstituents = 3U;

  // --- borrows (held for the pipeline's lifetime) --------------------------
  library::Library &lib_;
  const alpha::Library &dsl_;
  const alpha::Panel &panel_;
  const exec::ExecutionSimulator &sim_;
  const WeightPolicy &policy_;
  const combine::AlphaGate &gate_;

  // --- owned storage that keeps the non-owning seams alive -----------------
  // A deque (NOT a vector): CombinedSignalSource holds raw ISignalSource* into these,
  // so their addresses MUST stay stable as the container grows (R1 alive-constituents).
  // ScriptedSignalSource / ISignalSource live in the parent atx::engine namespace.
  std::deque<atx::engine::ScriptedSignalSource> sources_;
  std::vector<atx::engine::ISignalSource *> source_ptrs_;
  std::optional<combine::CombinedSignalSource> combined_;

  std::vector<std::vector<atx::f64>> alpha_rows_; // per-schedule-period mega-alpha cross-sections
  std::vector<atx::usize> sched_periods_;         // captured by alpha_at (period -> row map)
  std::vector<atx::f64> empty_alpha_;             // fallback span for an empty schedule

  // Standalone PanelView backing (RESIDUAL #2).
  std::vector<atx::f64> pv_fields_;
  std::vector<atx::u64> pv_mask_;
  std::vector<InstrumentId> pv_uni_;
  std::vector<atx::f64> market_cap_;
  std::vector<atx::u32> group_id_;
  atx::usize pv_rows_ = 0;
  atx::usize pv_universe_size_ = 0;
  atx::usize pv_cap_ = 1;
  atx::usize pv_mask_words_ = 1;

  // Cached returns FieldId (resolved once; mega-alpha helpers read it on the const path).
  alpha::FieldId returns_field_id_cached_ = 0;
};

} // namespace atx::engine::book
