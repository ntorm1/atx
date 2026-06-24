#pragma once

// atx::engine::factory — Factory: the mine -> gate -> admit capstone (S3-6, plan
// §4.8). This is the FINAL integration unit of Sprint 3: it wires the S3-5
// SearchDriver (the seeded, deflated, pool-aware evolutionary search) into the P4
// AlphaGate admission screen + the S1 deflation bar, and returns the FactoryReport
// the sprint exit criteria + the S1 DSR/PBO accounting read.
//
// ===========================================================================
//  §4.8 — the mine -> gate -> admit loop (authoritative)
// ===========================================================================
//   res := run_search(cfg.search, pool)            // S3-5 SearchDriver
//   for cand in res.ranked_by_deflated_fitness():  // best DEFLATED first
//     cand_pnl := full_oos_pnl(cand)               // computed BEFORE insert (§0.6)
//     metrics  := compute_metrics(cand_pnl, cand_pos, n_inst, book)
//     verdict  := gate.admit(metrics, cand_pnl, pool)   // P4 fitness/turnover/MAX-corr
//     if verdict == Accept and cand.dsr >= cfg.min_dsr: // AND the S1 deflation bar
//       pool.insert(source, cand_pnl, cand_pos, metrics)
//       admitted += 1
//   return { admitted, evaluated = res.trial_count, dedup_pct, cse_pct, trials, seed }
//
// ===========================================================================
//  §0.6 — the DANGLING-SPAN discipline (load-bearing)
// ===========================================================================
//  AlphaStore::pnl() returns a span ALIASING the backing vector; it DANGLES after
//  the next insert() (store.hpp BORROW LIFETIME). BOTH pool_aware_fitness's
//  corr-to-pool AND AlphaGate::admit's max-corr-to-pool read those member spans.
//  So for each candidate we compute EVERYTHING that reads the pool (the deflated
//  fitness `dsr`, the OOS PnL/positions/metrics, the gate verdict) FIRST, and only
//  then — once no live span aliases the pool — insert(). The candidate's own
//  cand_pnl/cand_pos buffers are OWNED std::vectors (copies from extract_streams),
//  so they survive the insert that consumes them.
//
// ===========================================================================
//  RANK-BY-DEFLATED-FITNESS — re-scored against the RUNNING pool
// ===========================================================================
//  The search ranks candidates by RAW fitness (its maximized signal). Admission
//  ranks by DEFLATED fitness (§4.8): we re-run pool_aware_fitness on each distinct
//  scored genome ONCE up front (against the pool as it stands at run start) to
//  obtain its `dsr`/`raw`, sort DESCENDING by (dsr, raw) — the best-deflated
//  candidate is screened first — and then walk that order. Because admission
//  GROWS the pool, the gate's corr-to-pool and each later candidate's
//  diversification are re-evaluated against the CURRENT pool at the moment that
//  candidate is screened (a fresh pool_aware_fitness call inside the loop), so the
//  marginal-contribution thesis (a redundant survivor is rejected once an
//  equivalent one is already admitted) actually bites. The trial count N fed to
//  the deflation is res.trial_count (the search's distinct-candidate count — the
//  multiple-testing N the S1 DSR/PBO accounting expects).
//
// ===========================================================================
//  ISignalSource ownership decision (documented)
// ===========================================================================
//  A mined candidate is a Genome (Ast + Analysis), not an ISignalSource. AlphaStore
//  stores a NON-OWNING ISignalSource* purely so P4-5's CombinedSignalSource can
//  RE-EVALUATE a constituent point-in-time; store.hpp explicitly permits nullptr
//  for a caller that does not exercise re-evaluation. S3-6 mining does NOT
//  re-evaluate — it inserts the already-realized OOS PnL/position streams it just
//  computed — so we insert source = nullptr (the phase4_bench / store unit-test
//  precedent). The only concrete ISignalSource adapter (loop::VmSignalSource) is
//  hardwired to the loop's OHLCV PanelView, not the research alpha::Panel, so
//  fabricating one over a non-OHLCV research panel would be both wrong (field
//  mismatch) and a lifetime hazard. If a later sprint needs a re-evaluable handle,
//  it must build a research-panel ISignalSource adapter; that is out of S3-6 scope.
//
// ===========================================================================
//  cse_pct source (documented, NOT fabricated)
// ===========================================================================
//  §4.8 asks for "mean Program.cache_hit_pct() over generations". SearchResult
//  does NOT surface the per-generation compiled Programs (the driver compiles
//  single-root Programs internally and folds only their eval DIGEST). The reachable
//  CSE telemetry is Program::cache_hit_pct() on the run's distinct scored genomes
//  (res.all_scored): we re-compile each and average its cache_hit_pct(). For the
//  single-root seed grammar this measures intra-expression structural sharing (it
//  is typically small — the cross-alpha CSE lever is exercised by compile_batch,
//  not the per-genome single-root compile the driver uses), but it is a REAL
//  measurement off the as-built Program telemetry, not a fabricated constant. An
//  empty run (no scored genomes / all uncompilable) yields cse_pct = 0.
//
//  Header-only; every function inline. mine() is a COLD path (run once per search),
//  so std::vector / per-candidate allocation is acceptable (the VM hot loop is
//  untouched — F8).

#include <array>  // std::array (reject_histogram, indexed by library::AdmitKind)
#include <limits> // std::numeric_limits (W4a split-sharpe disabling sentinel; R3b oos_pbo NaN default)
#include <string> // std::string (seed-expression / field source)
#include <vector> // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err
#include "atx/core/types.hpp" // atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/panel.hpp"   // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/streams.hpp" // alpha::extract_streams, AlphaStreams

#include "atx/engine/combine/gate.hpp"       // combine::AlphaGate, GateVerdict, GateConfig
#include "atx/engine/combine/store.hpp"      // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/library/library.hpp" // library::Library, AlphaCandidate, AdmitKind, AdmitVerdict

#include "atx/engine/factory/fitness.hpp" // factory::pool_aware_fitness, FitnessCfg
#include "atx/engine/factory/genome.hpp"  // factory::Genome
#include "atx/engine/factory/pool_view.hpp" // factory::LibraryPool, PoolView, pool_aware_fitness overload (S4b-2)
#include "atx/engine/factory/search_driver.hpp" // factory::SearchDriver, SearchConfig, SearchResult

namespace atx::engine::parallel {
class IExecutor; // S7.5d substrate seam (fwd-declared; the .cpp pulls the full header)
} // namespace atx::engine::parallel

namespace atx::engine::factory {

// =========================================================================
//  FactoryConfig — the mine() knobs (§4.8).
//
//  The Factory ctor takes (lib, panel, sim, weight_policy) — the run-wide borrows
//  the SearchDriver needs. The PER-RUN search inputs (the SearchConfig knobs PLUS
//  the seed-expression templates and the field-swap candidate names the
//  SearchDriver ctor requires) live HERE in the cfg passed to mine(), so a single
//  Factory can mine different grammars/budgets without reconstruction. min_dsr is
//  the S1 deflation admission bar (F4): a candidate must clear BOTH the P4 gate AND
//  cand.dsr >= min_dsr to be admitted.
// =========================================================================
struct FactoryConfig {
  SearchConfig search{};                 // the S3-5 search budget + CPCV/deflation geometry
  std::vector<std::string> seed_exprs;   // in-grammar starting templates (SearchDriver ctor)
  std::vector<std::string> panel_fields; // field-swap candidate names (SearchDriver ctor)
  atx::f64 min_dsr = 0.5;                // S1 deflation bar (F4): admit iff dsr >= this
  atx::f64 book_size = 1.0;              // notional divisor for compute_metrics turnover
  // --- W4a split-sample stability floor (OPTIONAL; default DISABLED).
  //  When ACTIVE (a FINITE value), a candidate is admitted only if BOTH halves of
  //  its OOS PnL stream have a per-period Sharpe >= this floor AND both share the
  //  full-sample Sharpe sign (FitnessReport.split_stable) — rejecting a single-
  //  regime artifact (strong H1, dead/negative H2). The DISABLING default is
  //  -infinity (NOT 0.0): a 0.0 floor would reject an exactly-0 H2 Sharpe even on
  //  the default path and break the digest byte-identity. The gate is evaluated
  //  ONLY when std::isfinite(min_split_sharpe); at the -inf default the accept
  //  expression is byte-identical to the pre-W4a screen (kGoldenDigest unchanged).
  atx::f64 min_split_sharpe = -std::numeric_limits<atx::f64>::infinity();
  // --- W4b run-level CSCV-PBO batch verdict (OPTIONAL; default DISABLED = 1.0).
  //  The PROBABILITY OF BACKTEST OVERFITTING (Bailey-López de Prado CSCV) computed
  //  POST-HOC over the SET of alphas this run admitted — a property of the SELECTION
  //  PROCEDURE, not a per-alpha score, so it CANNOT filter individual candidates and is
  //  computed AFTER the admit loop (never alters the admission digest by construction).
  //  PBO ∈ [0, 1] (→0 a persistent edge, →0.5 the IS winner is OOS noise). ACTIVE iff
  //  max_pbo < 1.0: a candidate SET passes iff its run-level PBO <= max_pbo. The
  //  DISABLING default is 1.0 (NOT a finite bar): at 1.0 the whole computation is
  //  SKIPPED, the report's PBO fields stay at their sentinels, and the run is
  //  byte-identical to the pre-W4b path (a 1.0 threshold could never trip strict
  //  pbo > max_pbo anyway, but skipping makes the no-op explicit + free). The verdict
  //  is ADVISORY-but-RECORDED: it is surfaced (rep.pbo_gate_passed) but never un-
  //  persists an alpha or changes admission — recording + a loud warning ARE the gate.
  atx::f64 max_pbo = 1.0;
  // --- P2a out-of-sample (holdout) validation (additive; 0.0 == OFF, default).
  //  When oos_fraction > 0, mine_into SELECTS on a TRAIN window [0, lockbox_begin -
  //  embargo) but CONFIRMS the AlphaGate floors + the DSR bar on the HELD-OUT
  //  terminal window [lockbox_begin, T) the search never optimized on, and persists
  //  the HOLDOUT (admission) metrics. The terminal `oos_fraction` of dates is held
  //  out (eval::reserve_lockbox geometry); 0.0 keeps the legacy path byte-identical.
  atx::f64 oos_fraction = 0.0;           // terminal holdout fraction; 0 == OFF (legacy path)
  // Embargo gap fraction inserted BEFORE the holdout (eval::reserve_lockbox). 0 ⇒
  //  the eval::CpcvConfig default embargo when oos is on. Ignored when oos is off.
  atx::f64 oos_embargo = 0.0;
  // --- W4a robust factor (§0.8): the OPTIONAL weak/holdout sub-universe Panel that
  //  ACTIVATES the robustness re-eval in the search's fitness. Passed to the
  //  SearchDriver ctor; nullptr (the default) keeps robust == 1.0 and raw ==
  //  wq*diversify byte-identical to today (the kGoldenDigest boundary pin). When
  //  non-null each candidate's fitness multiplies in robust =
  //  clamp(wq_on(weak_panel)/wq, 0, 1) — how well the WQ holds on the held-out sub-
  //  universe. BORROWED: the pointee MUST outlive every mine()/mine_into() call (the
  //  caller — e.g. stage_discover — owns the derived weak Panel for the run).
  const alpha::Panel *weak_panel = nullptr;
  // --- R2 walk-forward holdout (additive; 0 == legacy terminal reserve_lockbox path, byte-identical).
  //  When oos_n_windows >= 1, the holdout is the `oos_window`-th of oos_n_windows DISJOINT fixed-length
  //  blocks tiling the terminal region [T - oos_n_windows*w, T), where w = floor(oos_fraction * T):
  //    holdout_begin(k) = T - (oos_n_windows - oos_window) * w ;  holdout = [holdout_begin, holdout_begin + w).
  //  Window (oos_n_windows-1) is the terminal window [T - w, T) — IDENTICAL to today's reserve_lockbox.
  //  PIT-causal: train is always [0, holdout_begin - embargo) (precedes the holdout). 0 keeps the
  //  legacy single-window terminal path byte-for-byte.
  atx::usize oos_n_windows = 0;  // 0 == legacy terminal holdout (reserve_lockbox); >=1 enables walk-forward
  atx::usize oos_window   = 0;   // which window [0, oos_n_windows); the sweep advances this per run
  // --- R1 field-type discipline (opt-in via --typed-fields; both empty == byte-identical default).
  //  When non-empty, SearchDriver's ctor partition is tightened:
  //    numeric_excluded_fields : fields that must NOT appear as numeric leaves (e.g. binary flags,
  //                              low-cardinality categoricals). A field in this list is excluded from
  //                              numeric_field_views_ regardless of is_group_field(). Empty -> unchanged.
  //    extra_group_fields      : fields that SHOULD be routed to the group pool (e.g. a raw `gics`
  //                              integer column that is categorical but not named "sector" / "IndClass.*").
  //                              Added to group_field_views_ iff not already there via is_group_field().
  //  DETERMINISM: both lists empty (the default) -> the partition is IDENTICAL to today ->
  //  BYTE-IDENTICAL research_digest / version_id. A non-empty list may change the digest; that is the
  //  only sanctioned re-baseline.
  std::vector<std::string> numeric_excluded_fields{};  // fields excluded from numeric leaf pool
  std::vector<std::string> extra_group_fields{};       // extra fields routed to group pool
  // --- R2 price-scale admission gate (OPTIONAL; default OFF = 1.0 -> zero overhead).
  //  Rejects a holdout candidate whose book is a trivial 1/price (price-scale) tilt.
  //  The threshold is the MAX allowed |time-averaged cross-sectional Pearson(w, 1/raw_close)|.
  //  INACTIVE (>= 1.0): never enters the gate code, no computation, byte-identical to pre-R2.
  //  INERT (raw_close absent from holdout or no usable dates -> NaN loading): no rejection.
  //  Active when < 1.0. Valid range: (0, 1].
  //  Default 1.0 = OFF (any correlation passes).
  atx::f64 max_price_scale_corr = 1.0; // --reject-price-scale (R2); 1.0 == OFF
  // --- R3 intra-holdout DSR sub-windows gate (OPTIONAL; default OFF = 0 -> zero overhead).
  //  Splits the sealed holdout PnL into K contiguous sub-segments and requires min_dsr on EACH.
  //  This is an ADDITIONAL tightening layered on top of the existing aggregate hold_dsr >= min_dsr gate.
  //  Semantics: 0 (and 1) == OFF. Active when >= 2.
  //  WARNING: large K over a short holdout means short segments — a segment with < 2 real
  //  observations always yields DSR=0.0 and will FAIL the bar. That is user tuning responsibility.
  atx::usize dsr_subwindows = 0; // --dsr-subwindows (R3); 0/absent == OFF, byte-identical
};

// =========================================================================
//  FactoryReport — the §4.8 return value (the sprint exit-criteria fields).
//
//  admitted   : candidates that cleared BOTH the P4 gate AND the dsr bar (inserted).
//  evaluated  : res.trial_count — distinct candidates scored (feeds S1 DSR/PBO N).
//  dedup_pct  : 1 - trial_count/candidates_generated (the F6 dedup lever).
//  cse_pct    : mean Program::cache_hit_pct() over the run's distinct scored genomes
//               (see the header cse_pct note — a real, reachable measurement).
//  trials     : == evaluated (the trial count, surfaced under its §4.8 name too).
//  seed       : res.seed == cfg.search.master_seed (the artifact key).
//  digest     : the F1/F2 byte-identical run fingerprint — the search digest FOLDED
//               with every admission decision (so two runs that mine + admit
//               identically replay to the same digest; F1/F2).
// =========================================================================
// =========================================================================
//  OosReportEntry — per-admitted-alpha IS+OOS metrics for the impl manifest
//  (P2a; additive). Surfaces BOTH the TRAIN (in-sample) metrics and the HOLDOUT
//  (out-of-sample / admission) metrics WITHOUT changing the persistent .alib
//  layout (the library stores ONE AlphaMetrics per alpha — the holdout/admission
//  metrics). Populated ONLY by the oos branch; default-empty on the legacy path.
// =========================================================================
struct OosReportEntry {
  atx::u64 canon_hash{0};            // the F6 dedup key (matches the library record)
  combine::AlphaMetrics is_metrics{};  // TRAIN-window realized metrics (reporting only)
  combine::AlphaMetrics oos_metrics{}; // HOLDOUT-window metrics (what was GATED + persisted)
};

struct FactoryReport {
  atx::usize admitted{0};
  atx::usize evaluated{0};
  atx::f64 dedup_pct{0.0};
  atx::f64 cse_pct{0.0};
  atx::usize trials{0};
  atx::u64 seed{0};
  atx::u64 digest{0};

  // --- S4b-3 mine_into() telemetry (additive; default-init so mine() is untouched).
  // These fields are populated ONLY by mine_into (the persistent-library admit path);
  // the ephemeral-AlphaStore mine() leaves them at their defaults.
  atx::usize duplicates{0};                     // library-wide F6 dedup hits (AdmitKind::Duplicate)
  atx::u64 library_n_alphas_before{0};          // library::n_alphas() at run start
  atx::u64 library_n_alphas_after{0};           // library::n_alphas() at run end
  std::array<atx::usize, 8> reject_histogram{}; // count per library::AdmitKind (0..7)

  // --- P2a OOS telemetry (additive; default-EMPTY so the legacy path is byte-
  //  identical). One entry per admitted alpha when oos_fraction > 0: its IS (train)
  //  and OOS (holdout) metrics. P2b (impl) reads this for the discover manifest.
  std::vector<OosReportEntry> oos_metrics;

  // C2.2 (measurement-only) — REPORT-ONLY: the distinct canonical structures this run
  // SCORED (the all_scored / CanonSet contents; size == evaluated). NOT folded into
  // `digest` (a telemetry vector, exactly like oos_metrics) — adding it leaves the
  // digest, admitted set, and library version_id byte-identical. ResearchDriver folds
  // these across runs to measure cross-run redundant EVALUATION.
  std::vector<atx::u64> scored_canon_hashes;

  // --- W4b run-level CSCV-PBO verdict (additive; default-SENTINEL so the legacy path
  //  is byte-identical — mirrors the oos_metrics precedent). Populated ONLY when the
  //  gate is ACTIVE (FactoryConfig::max_pbo < 1.0) AND the admitted set is feasible
  //  (>= 2 admitted alphas of equal post-drop length, matrix accepted by
  //  pbo_cscv_checked); otherwise every field stays at its sentinel and the report is
  //  indistinguishable from a pre-W4b run.
  //    pbo            : the run-level PBO ∈ [0, 1]. NaN == "not computed" (off /
  //                     infeasible / < 2 admitted) — the legacy-byte-identity sentinel.
  //    pbo_mean_logit : mean per-split logit λ (diagnostic; 0.0 sentinel).
  //    pbo_n_candidates : the admitted-alpha count fed to CSCV (0 sentinel).
  //    pbo_n_splits   : the auto-clamped even split count S actually used (0 sentinel).
  //    pbo_gate_passed: the RECORDED verdict — false iff the gate was active AND
  //                     pbo > max_pbo. FAIL-OPEN: true when off / infeasible / not
  //                     computed (the advisory gate never blocks an otherwise-good run).
  atx::f64 pbo = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 pbo_mean_logit = 0.0;
  atx::usize pbo_n_candidates = 0;
  atx::usize pbo_n_splits = 0;
  bool pbo_gate_passed = true;
  // --- R3b/A3: run-level CSCV PBO (probability of backtest overfitting) over the
  //  admitted alphas' holdout PnL streams for this run. SET-LEVEL statistic: one
  //  value per run (NOT per candidate). Computed AFTER all admission decisions
  //  (pure/post-admission — does NOT change which alphas are admitted, rep.digest,
  //  or the library version_id). NaN when < 2 alphas were admitted on the OOS path,
  //  or when the holdout is too short for any split, or when OOS is off (oos_fraction
  //  == 0). Never emitted on the non-accumulation path (byte-identical to pre-R3).
  //  A3: this is now an ALIAS of `pbo` above — on the two OOS admit paths it is set
  //  from the SINGLE `finalize_run_pbo` computation (called with always_compute=true
  //  so the always-on holdout diagnostic is recorded even when the gate is OFF at the
  //  1.0 default). Its VALUE equals `rep.pbo`. Kept as a distinct field because the
  //  manifest + kvs + factory_oos_test consume `oos_pbo` by name.
  atx::f64 oos_pbo{std::numeric_limits<atx::f64>::quiet_NaN()};
};

namespace detail {

// finalize_run_pbo — the SINGLE SOURCE OF TRUTH for the W4b run-level CSCV-PBO verdict
// over a discovery run's admitted alphas. POST-HOC: called ONCE after the admit loop,
// BEFORE `return rep`, on every Factory mine path. PURE (no RNG; eval::pbo_cscv is
// order-fixed) and it NEVER touches rep.digest or any admission decision — it only
// ADDS to the report's PBO fields. `admitted_pnls[i]` is admitted-alpha i's realized
// OOS PnL stream (the SAME stream that was gated + persisted), in deterministic admit
// order. Rules:
//   * max_pbo >= 1.0 (off, the default) AND !always_compute -> return immediately,
//     fields stay at sentinels (NaN pbo, 0 logit/counts, gate passes) -> byte-identical
//     legacy path. When always_compute is true the statistic is computed + recorded even
//     at the OFF default (the always-on holdout diagnostic, oos_pbo); the gate VERDICT
//     still FAIL-OPENS (pbo_gate_passed = true) when the gate is off — always_compute
//     changes ONLY whether the PBO is recorded, never admission or the digest.
//   * drop index 0 of each row (the §0-F structural zero — the same .subspan(1) /
//     split_floor_ok convention) before forming the candidate-major matrix M[c*T + t].
//   * require >= 2 rows of EQUAL post-drop length T (true by construction — same eval
//     window); a mismatch is treated as infeasible (sentinels, gate passes).
//   * n_splits = the largest EVEN value <= min(8, T) (8 = Bailey's standard CSCV S;
//     auto-clamp for short panels); if < 2 -> infeasible (skip).
//   * call eval::pbo_cscv_checked(M, n_candidates, n_splits); on Err -> infeasible
//     (skip); on Ok -> set rep.pbo / pbo_mean_logit / pbo_n_candidates / pbo_n_splits
//     and rep.pbo_gate_passed = (max_pbo >= 1.0) ? true : !(rep.pbo > max_pbo)
//     (fail-open when the gate is off-but-always_compute: the PBO is recorded but the
//     gate passes — a run is never failed merely for the gate being off).
// Declared so a unit test can verify the verdict on hand-built admitted_pnls (the same
// single-source-of-truth testability the W4a detail::split_half_sharpe helper has).
void finalize_run_pbo(FactoryReport &rep,
                      const std::vector<std::vector<atx::f64>> &admitted_pnls,
                      atx::f64 max_pbo,
                      bool always_compute = false);

} // namespace detail

// =========================================================================
//  Factory — the mine -> gate -> admit capstone (§4.8).
//
//  Borrows the run-wide Library + Panel + ExecutionSimulator + WeightPolicy for its
//  lifetime (the SearchDriver + every fitness/stream eval borrow them). The single
//  Library owns every OpSig row the genomes' Expr::op pointers alias; it MUST
//  outlive the Factory and every produced genome (genome.hpp SAFETY).
// =========================================================================
class Factory {
public:
  // SAFETY: `lib`, `panel`, `sim`, `policy` are BORROWED for the Factory's lifetime
  // (and every mine() call). The single run-wide Library owns the op rows every
  // genome's Expr::op aliases; it must outlive the Factory and all produced genomes.
  // The ctor signature matches the S3-6 verbatim tests: Factory(lib, panel, sim,
  // weight_policy). The SearchDriver's extra ctor inputs (seed_exprs, panel_fields)
  // come from the per-run cfg passed to mine(), not the ctor (see FactoryConfig).
  Factory(const alpha::Library &lib, const alpha::Panel &panel, const exec::ExecutionSimulator &sim,
          const WeightPolicy &policy) noexcept
      : lib_{lib}, panel_{panel}, sim_{sim}, policy_{policy} {}

  // Mine the search space, screen every distinct candidate through the P4 gate +
  // the S1 deflation bar, and insert each survivor into `pool`. `pool` is GROWN
  // in place (the admitted alphas); `gate` is the stateless P4 admission screen.
  // Returns the §4.8 FactoryReport. Deterministic: same cfg + same starting pool
  // contents => byte-identical report.digest (F1/F2).
  [[nodiscard]] FactoryReport mine(const FactoryConfig &cfg, combine::AlphaStore &pool,
                                   const combine::AlphaGate &gate);

  // =======================================================================
  //  mine_into — the REAL admit path: mine + deflate, then admit each
  //  survivor into the PERSISTENT library::Library (S4b-3).
  //
  //  Same seeded SearchDriver path as mine() (shared internals; no fork), but
  //  the admit target is the persistent library — library-wide F6 dedup ->
  //  O(neighbors) corr -> P4 gate floors -> segmented store + PIT lifecycle +
  //  manifest — instead of an ephemeral combine::AlphaStore. The S1 deflation
  //  bar stays FACTORY-side: a candidate must clear cand.dsr >= cfg.min_dsr
  //  BEFORE library::admit is consulted (so a noise candidate the library alone
  //  might pass is still rejected by the multiple-testing deflation). `lib_lib`
  //  is GROWN in place. Deterministic: same cfg + same starting library contents
  //  => byte-identical report.digest (the search digest folded with every
  //  admission decision; F1/F2).
  //
  //  §0.6 DANGLING-SPAN discipline (as in mine()): the AlphaCandidate's pnl /
  //  pos_flat are NON-OWNING spans into the OWNED cand_pnl / cand_pos vectors,
  //  which MUST outlive the admit() call. They are kept alive in the loop body
  //  across the admit, so the spans never dangle.
  // =======================================================================
  //  Returns a Result: on the normal path an Ok(FactoryReport); on a cross-run
  //  --library-dir geometry MISMATCH (a reopened library whose fixed period count
  //  t_ differs from this run's holdout length) an Err(InvalidArgument) propagated
  //  from the guarded library::try_admit seam — so the run HALTS cleanly instead of
  //  aborting (debug ATX_ASSERT) / reading out-of-bounds (release). The same-geometry
  //  / fresh-library path is byte-identical to the pre-guard admit() (try_admit
  //  delegates to admit() VERBATIM when lengths match), so report.digest is unchanged.
  [[nodiscard]] atx::core::Result<FactoryReport>
  mine_into(const FactoryConfig &cfg, library::Library &lib_lib, const combine::AlphaGate &gate,
            SearchProgressSink *sink = nullptr, const SearchResumeState *resume = nullptr);

  // =======================================================================
  //  mine_into (SUBSTRATE-AWARE, S7.5d) — the SAME mine_into admit path with the
  //  PURE expensive per-genome scoring map moved over the IExecutor seam.
  //
  //  run_search stays in the parent (seeded/deterministic, F1/F2). The per-genome
  //  compile+eval+extract_streams + pool-aware-fitness(dsr, raw) map — scored
  //  against the RUN-START library snapshot (a const) — is what crosses the seam:
  //    * InProcess (ThreadExecutor): delegates to the existing mine_into(cfg, lib,
  //      gate) verbatim (the in-process map).
  //    * MultiProcess (ProcessExecutor): serialize {genomes = res.all_scored, the
  //      run-start admitted-pnl snapshot, panel, cfg}; submit(WorkloadId::Mine);
  //      gather per-genome {ok, dsr, raw, streams}; then run the EXISTING
  //      rank_by_deflated_fitness (on the gathered dsr/raw) and the EXISTING
  //      sequential library::admit loop (fed the gathered streams).
  //  Because rank+admit runs in the PARENT identically, report.digest and the
  //  library version_id are byte-identical across every substrate and worker count
  //  BY CONSTRUCTION (the §0.9 sound design). An unknown substrate aborts (ATX_CHECK).
  // =======================================================================
  [[nodiscard]] atx::core::Result<FactoryReport>
  mine_into(const FactoryConfig &cfg, library::Library &lib_lib, const combine::AlphaGate &gate,
            parallel::IExecutor &exec);

private:
  // The as-of period for an admitted alpha's Candidate->Admitted lifecycle transition.
  // A constant (the S4 fixtures use period 1); the realized OOS streams are not keyed
  // to a calendar period in the research panel, so a fixed admit period is sufficient.
  static constexpr atx::usize kAdmitAsOf = 1U;

  // A scored candidate's admission ranking key: its deflated Sharpe (primary) and
  // raw fitness (tiebreak), plus its index into all_scored.
  struct Ranked {
    atx::usize idx{0};
    atx::f64 dsr{0.0};
    atx::f64 raw{0.0};
  };

  // Compile + evaluate a genome over the research panel and extract its per-alpha
  // streams (the candidate is root 0). The single-thread Engine path — exactly
  // pool_aware_fitness's internal eval (S3-4) — so the realized streams match the
  // fitness oracle. Err propagates compile/eval/extract failure.
  [[nodiscard]] atx::core::Result<alpha::AlphaStreams>
  detail_eval_streams(const Genome &cand) const;

  // P2a: compile genome `g`, evaluate over an ARBITRARY sub-panel (train OR
  // holdout), extract_streams with this->policy_ / this->sim_, flatten positions,
  // and return compute_metrics(pnl, pos, n_inst, book_size). The realized single-
  // alpha PnL stream is returned via `pnl_out` so the caller can compute a holdout
  // DSR via the fitness.cpp deflated-Sharpe recipe. Uses the SAME compile/eval path
  // as detail_eval_streams, so metrics_on_panel(g, full_panel, ...) reproduces the
  // in-loop metrics exactly. Err propagates compile/eval/extract failure (or a
  // zero-alpha stream).
  [[nodiscard]] atx::core::Result<combine::AlphaMetrics>
  metrics_on_panel(const Genome &g, const alpha::Panel &sub_panel, atx::f64 book_size,
                   std::vector<atx::f64> &pnl_out) const;

  // P2a: the out-of-sample (holdout) admit path. Dispatched from mine_into when
  // cfg.oos_fraction > 0. SELECTS the search on a TRAIN sub-panel, but CONFIRMS the
  // AlphaGate floors + the DSR bar on a HELD-OUT terminal window the search never
  // optimized on, and persists the HOLDOUT (admission) metrics. Returns both IS and
  // OOS metrics per admitted alpha in FactoryReport::oos_metrics. The legacy
  // mine_into body is left untouched (this is a TOP-of-function additive branch).
  [[nodiscard]] atx::core::Result<FactoryReport>
  mine_into_oos(const FactoryConfig &cfg, library::Library &lib_lib, const combine::AlphaGate &gate,
                SearchProgressSink *sink = nullptr, const SearchResumeState *resume = nullptr);

  // Task 5: the PARALLEL out-of-sample admit path. Dispatched from the
  // substrate-aware mine_into(cfg, lib, gate, exec) when oos_fraction > 0 AND the
  // executor is MultiProcess. Reproduces mine_into_oos BYTE-IDENTICALLY (same digest,
  // admitted count, library version_id, reject histogram, oos_metrics) but moves the
  // two expensive per-candidate VM evals — the TRAIN ranking eval and the HOLDOUT
  // admission eval — over the IExecutor seam via TWO gather_mine_scores submits (one
  // per sub-panel, reusing the existing Mine wire format UNCHANGED). The stateful
  // library::admit fold stays SEQUENTIAL in the parent (it CANNOT be partitioned
  // byte-identically; workload_mine.hpp). The serial mine_into_oos stays the reference
  // (the InProcess path delegates to it); this path must reproduce its result bit-for-bit
  // for every worker count (the binding parallelization invariant). exec MUST be
  // MultiProcess (the caller guarantees it).
  [[nodiscard]] atx::core::Result<FactoryReport>
  mine_into_oos_parallel(const FactoryConfig &cfg, library::Library &lib_lib,
                         const combine::AlphaGate &gate, parallel::IExecutor &exec);

  // Flatten alpha 0's per-period position cross-sections into the period-major,
  // instrument-minor layout AlphaStore::insert / compute_metrics expect
  // (length == n_periods * n_instruments).
  [[nodiscard]] static std::vector<atx::f64> flatten_positions(const alpha::AlphaStreams &strm);

  // Rank the distinct scored genomes by deflated fitness (DESCENDING dsr, then raw,
  // then canon_hash for a deterministic total order — F1). Each genome is scored
  // ONCE against the pool as it stands at run start (the pool grows later, in the
  // admission loop). A genome whose fitness errors sorts last (dsr = raw = 0).
  [[nodiscard]] std::vector<Ranked> rank_by_deflated_fitness(const std::vector<Genome> &scored,
                                                             const FitnessCfg &fit_cfg,
                                                             const combine::AlphaStore &pool) const;

  // The PoolView overload (S4b-3): identical to the AlphaStore overload above, but
  // `pool` is a backing-agnostic PoolView and the inner score call uses the S4b-2
  // pool_aware_fitness(genome, view, ...) overload (the O(neighbors) MAX-|corr| seam).
  // Used by mine_into to rank against the PERSISTENT library. The sort key + total
  // order are identical (DESCENDING dsr, then raw, then canon_hash; F1). A genome
  // whose fitness errors sorts last (dsr = raw = 0). NOTE: Genome.canon_hash is left
  // 0 by the S3 search path, so the canon_hash tiebreak is degenerate here; an
  // `idx` final tiebreak therefore pins a TRUE total order (std::sort is not stable),
  // so the rank — and the digest folded from it — is deterministic regardless of the
  // sort's internal permutation of equal-key elements (all_scored is built in a
  // deterministic order, so equal idx never occurs and the order is reproducible; F1).
  [[nodiscard]] std::vector<Ranked> rank_by_deflated_fitness(const std::vector<Genome> &scored,
                                                             const FitnessCfg &fit_cfg,
                                                             const PoolView &pool) const;

  // Mean Program::cache_hit_pct() over the run's distinct scored genomes (the
  // reachable CSE telemetry — see the header cse_pct note). An uncompilable genome
  // is skipped; an empty / all-uncompilable run yields 0.
  [[nodiscard]] double mean_cse_pct(const SearchResult &res) const;

  // SAFETY: each borrow is held for the Factory's lifetime; the single run-wide
  // Library owns every OpSig the genomes' Expr::op pointers alias and must outlive
  // the Factory and all produced genomes (genome.hpp SAFETY).
  const alpha::Library &lib_;
  const alpha::Panel &panel_;
  const exec::ExecutionSimulator &sim_;
  const WeightPolicy &policy_;
};

} // namespace atx::engine::factory
