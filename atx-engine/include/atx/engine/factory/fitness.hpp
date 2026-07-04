#pragma once

// atx::engine::factory — pool-aware fitness: the WorldQuant marginal-contribution
// score (S3-4, plan §4.6 + §0.6 / §0.7 / §0.8).
//
// ===========================================================================
//  What this unit is — the WQ thesis, made into a number
// ===========================================================================
//  The factory does NOT reward a candidate's standalone Sharpe. It rewards the
//  candidate's MARGINAL contribution to an already-diversified live pool, judged
//  OUT-OF-SAMPLE and DEFLATED by the running search trial count. A strong alpha
//  that merely duplicates a pool member adds nothing; a weaker alpha that is
//  uncorrelated with the pool genuinely expands the frontier. `raw` encodes that:
//
//      raw = wq * diversify * robust
//
//  where wq is the OOS WorldQuant fitness (combine::compute_metrics().fitness,
//  reused VERBATIM — no second convention), diversify = 1 − mean|corr-to-pool|
//  (F7), and robust is the sub-universe-stability ratio (§0.8). Admission (S3-6)
//  then gates on the DEFLATED Sharpe `dsr` (F4), which a higher trial count N
//  drives toward 0 — the anti-snooping lever.
//
// ===========================================================================
//  §0.6 — the missing corr-to-pool helper, and the DANGLING-SPAN hazard
// ===========================================================================
//  combine/ exposes only `pairwise_complete_corr(a, b) -> f64`; there is no
//  reusable corr-to-pool. This unit writes the public `corr_to_pool(candidate,
//  pool, Reduce)` over it: Reduce::Max (the gate-consistent max|corr| screen) and
//  Reduce::Mean (the diversification discount). CRITICAL: AlphaStore::pnl()
//  returns a span that ALIASES the backing vector and DANGLES after the next
//  insert() (store.hpp BORROW LIFETIME). corr_to_pool therefore reads the pool
//  WITHOUT mutating it; the caller (S3-6) must compute corr-to-pool BEFORE
//  inserting the candidate. See the SAFETY note on corr_to_pool.
//
// ===========================================================================
//  §0.7 — the as-built S1 signatures consumed here
// ===========================================================================
//  * combine::compute_metrics(pnl, positions_flat, n_instruments, book) -> the
//    AlphaMetrics POD whose `.fitness` IS the WQ term (turnover floor 0.125).
//  * eval::deflated_sharpe(sr, T, skew, exkurt, N, std::optional<var>) -> DsrResult
//    where N is the TRIAL COUNT. Moments: eval::skewness / eval::excess_kurtosis.
//  * eval::cpcv_folds(spans, cfg) -> per-fold {train_idx, test_idx}; fitness is
//    computed on the TEST partitions only (F3 OOS-only).
//
// ===========================================================================
//  §0.8 — sub-universe robustness is a RE-EVAL, not a re-score
// ===========================================================================
//  No scoring layer takes a universe argument. Robustness re-runs the candidate
//  through extract_streams against an ALTERNATE Panel built with a weaker
//  universe (a different UniversePolicy / mask). pool_aware_fitness takes the weak
//  panel as an optional borrow; when none is configured, robust = 1.0 (a clean,
//  documented degenerate — robustness neither rewards nor penalizes).
//
//  Header-only, every function inline; the fitness path is COLD (one call per
//  distinct candidate, never on the VM hot loop), so std::vector is fine.

#include <array>   // std::array (the multi-objective vector, S4.1)
#include <cstring> // std::memcpy (CpcvCache: embed embargo f64 as bit pattern for map key)
#include <limits>  // std::numeric_limits (FitnessCfg::max_turnover_target default +inf)
#include <map>     // std::map (CpcvCache: key-ordered, no custom hash needed)
#include <mutex>   // std::mutex, std::lock_guard (CpcvCache thread-safety)
#include <span>    // std::span
#include <tuple>   // std::tuple (CpcvCache key)
#include <vector>  // std::vector (fold-sliced streams)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::usize

#include "atx/engine/alpha/panel.hpp"        // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/streams.hpp"      // alpha::AlphaStreams (cost book aggregate input)
#include "atx/engine/combine/store.hpp"      // combine::AlphaStore, AlphaId
#include "atx/engine/cost/calibration.hpp"   // cost::CalibratedCost (the calibrated coeffs, S4.3)
#include "atx/engine/cost/cost_selection_config.hpp" // cost::CostSelectionConfig (B7 selection-cost wire)
#include "atx/engine/eval/cpcv.hpp"          // eval::cpcv_folds, CpcvFold, LabelSpan
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/factory/genome.hpp"     // factory::Genome
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

// Forward declaration — an `alpha::Engine*` parameter needs only a forward decl;
// pulling in vm.hpp here would add it to every fitness consumer's translation unit.
// (alpha::SignalSet is already a complete type via panel.hpp above, so it needs none.)
namespace atx::engine::alpha {
class Engine;
}

namespace atx::engine::factory {

// =========================================================================
//  CpcvCache — a thread-safe cache of pre-built CPCV label spans + folds,
//  keyed on (n_periods, n_groups, n_test_groups, embargo).
//
//  WHY: detail::fitness_core rebuilds point_label_spans(n_periods) and
//  eval::cpcv_folds(spans, cfg.cpcv) for EVERY genome even though these
//  depend ONLY on (n_periods, cfg.cpcv).  In a typical search (pop=60 ×
//  gen=15) the same (n_periods, cpcv) pair is rebuilt 900+ times; this cache
//  reduces that to a SINGLE build with O(1) cache-hit allocations thereafter.
//
//  THREAD SAFETY: the internal mutex serialises the rare cold-path insert.
//  The hot-path lookup acquires the same lock, which is cheap (the lock is
//  uncontested almost always once the cache is warm — typically just one entry
//  for the main panel and at most one more for the weak panel).
//
//  BYTE IDENTITY: the cached spans and folds are bit-identical to recomputing
//  them (they are pure deterministic functions of their inputs).  No value or
//  RNG stream changes — purely a performance optimisation.
//
//  OWNERSHIP: pass a pointer to a CpcvCache that outlives every concurrent
//  call that touches it.  The cache is NOT a member of SearchDriver; instead
//  it is created per evaluate_generation call and passed into fitness_core via
//  pool_aware_fitness so the cache's lifetime is always tighter than the
//  objects it references.
// =========================================================================
struct CpcvCache {
  // ---- key -----------------------------------------------------------------
  // (n_periods, n_groups, n_test_groups, embargo_bits)
  // embargo is stored as its IEEE-754 bit pattern to avoid float-equality UB.
  using Key = std::tuple<atx::usize, atx::usize, atx::usize, atx::u64>;

  struct Entry {
    std::vector<eval::LabelSpan>  spans;
    std::vector<eval::CpcvFold>   folds;
  };

  // ---- get_or_build --------------------------------------------------------
  // Returns a CONST REFERENCE to the cached spans+folds for (n_periods, cpcv).
  // On the first call for a given key the spans and folds are built and stored;
  // subsequent calls return the stored result without recomputing.  Thread-safe.
  [[nodiscard]] const Entry &get_or_build(atx::usize n_periods, const eval::CpcvConfig &cpcv) {
    // Build the key: embed embargo as its bit pattern for a reliable map key.
    atx::u64 embargo_bits{};
    static_assert(sizeof(embargo_bits) == sizeof(cpcv.embargo), "f64 size mismatch");
    std::memcpy(&embargo_bits, &cpcv.embargo, sizeof(embargo_bits));
    const Key key{n_periods, cpcv.n_groups, cpcv.n_test_groups, embargo_bits};

    {
      std::lock_guard<std::mutex> g{mu_};
      const auto it = map_.find(key);
      if (it != map_.end()) {
        return it->second; // cache hit — no recompute
      }
    }

    // Cache miss: build outside the lock so other workers can proceed
    // concurrently on their (different) keys.  The work is deterministic and
    // idempotent, so a racing double-build is harmless (the second insert is
    // a no-op via try_emplace).
    Entry entry;
    entry.spans.reserve(n_periods);
    for (atx::usize t = 0U; t < n_periods; ++t) {
      entry.spans.push_back(eval::LabelSpan{t, t + 1U});
    }
    entry.folds = eval::cpcv_folds(std::span<const eval::LabelSpan>{entry.spans}, cpcv);

    std::lock_guard<std::mutex> g{mu_};
    // try_emplace: if another thread raced and already inserted, keep theirs
    // (deterministic same value; this build is discarded).
    const auto [it, inserted] = map_.try_emplace(key, std::move(entry));
    return it->second;
  }

private:
  mutable std::mutex            mu_;
  std::map<Key, Entry>          map_;
};

// =========================================================================
//  kMaxObjectives — the fixed capacity of a candidate's objective vector (S4.1).
//
//  S4.1 ships three objectives {wq, diversify, robust} (a re-projection of the
//  existing FitnessReport fields — NO new fitness math). S4 (p9) grows the capacity
//  7->9 to APPEND kObjCapacity (slot 7) + kObjTurnover (slot 8) as two new OPT-IN
//  columns; slots 0-6 are UNCHANGED (frozen-prefix pin). The
//  std::array<f64, kMaxObjectives> is a fixed-size, allocation-free inline buffer
//  — it carries through Scored (search_driver.hpp) into the NSGA-II ObjMatrix.
//
//  OFF-PATH SAFETY: NSGA-II builds its ObjMatrix over the first k = max(n_objectives)
//  columns (search_driver.cpp assign_pareto_ranks), NOT kMaxObjectives. Inactive
//  slots 6/7/8 stay at their zero default and are NEVER read unless their own gate
//  flag is on — so growing 7->9 is byte-identical on the off-path (the width NSGA-II
//  uses is n_objectives, never kMaxObjectives). res.digest folds signal_set_digest
//  (NOT the objectives array), so no golden hashes this width.
// =========================================================================
inline constexpr atx::usize kMaxObjectives = 9; // S4: 7->9 (+capacity +turnover)
// Objective-slot indices (NSGA-II maximizes every column; inactive columns MUST be
// uniform across genomes -> inert). 0 wq, 1 diversify, 2 robust, 3 novelty,
// 4 -cost_bps, 5 -node_count (parsimony), 6 dsr (deflated-Sharpe, R4 opt-in).
inline constexpr atx::usize kObjParsimony  = 5;
inline constexpr atx::usize kObjDeflation  = 6; // R4: deflated-Sharpe selection objective
// S4: capacity/turnover — APPEND-ONLY (frozen-prefix pin: slots 0-6 unchanged).
inline constexpr atx::usize kObjCapacity   = 7; // S4-1: sqrt-law impact capacity score
inline constexpr atx::usize kObjTurnover   = 8; // S4-2: signal first-order autocorrelation
static_assert(kObjCapacity == 7, "S4 frozen-prefix: kObjCapacity is APPEND-ONLY slot 7");
static_assert(kObjTurnover == 8, "S4 frozen-prefix: kObjTurnover is APPEND-ONLY slot 8");
static_assert(kMaxObjectives == 9, "S4: kMaxObjectives must grow 7->9 for the two new slots");

// =========================================================================
//  Reduce — how corr_to_pool folds the per-member |corr| over the pool.
//
//  Max  : max_j |corr(candidate, member_j)| — the gate-consistent screen
//         (AlphaGate rejects on this exact MAX, gate.hpp).
//  Mean : mean_j |corr(candidate, member_j)| — the diversification discount
//         (a candidate redundant with the WHOLE pool, not just its nearest
//         neighbour, is penalized in proportion).
// =========================================================================
enum class Reduce : atx::u8 { Max, Mean };

// =========================================================================
//  corr_to_pool — the marginal-correlation helper (§0.6), over the shared
//  pairwise-complete Pearson convention.
//
//  Returns max|corr| (Reduce::Max) or mean|corr| (Reduce::Mean) of the
//  candidate's PnL against every pool member's PnL. An EMPTY pool -> 0 (a
//  candidate is maximally diversifying against nothing — diversify = 1).
//
//  SAFETY: this function only READS the pool — it never inserts. AlphaStore::pnl()
//  returns a span aliasing the backing vector that DANGLES after the next
//  insert()/ingest_streams() (store.hpp §0.6). The caller (S3-6 admission) MUST
//  compute corr-to-pool on the candidate BEFORE inserting it into the pool; doing
//  it after would read freed memory. Each member span is consumed immediately
//  inside the loop (no span outlives one iteration), so the read is sound here.
// =========================================================================
[[nodiscard]] atx::f64 corr_to_pool(std::span<const atx::f64> candidate_pnl,
                                    const combine::AlphaStore &pool, Reduce reduce) noexcept;

// =========================================================================
//  book_cost_bps — the S4.3 book-aggregate round-trip impact cost (bps).
//
//  For the candidate's LAST-period target weights (strm.positions(0, last) — the
//  capacity_for_alpha convention: the most recent rebalance is what is sized to
//  `target_aum`), prices each name at `target_aum` and aggregates the per-name
//  round-trip cost into a single |w|-weighted book figure:
//
//    price_i   = close(last_date, i)                     (the current mark)
//    adv_i     = mean_{last kAdvWindow rows} close*volume (dollar ADV)
//    part_i    = (target_aum·|w_i|/price_i) / adv_i       (participation)
//    sigma_i   = popstd_{last kVolWindow returns} ret_i   (return volatility)
//    cost_bps  = Σ_i |w_i| · cost::round_trip_cost_bps(cost, part_i, sigma_i)
//
//  REUSES the ONE cost model (cost::round_trip_cost_bps) verbatim and the SAME
//  participation / ADV / σ sizing arithmetic risk::capacity_curve uses — no second
//  formula. A dead/NaN weight, non-positive price, zero ADV, zero participation,
//  or zero σ makes a name contribute nothing (no NaN/Inf/div-by-zero leak). The
//  panel needs "close" (mark + returns) and "volume" (ADV); a panel WITHOUT a
//  volume field yields 0 ADV everywhere ⇒ cost 0 (a documented degenerate). PURE
//  given (strm, panel, cost, target_aum); NO RNG; bit-deterministic. A non-positive
//  target_aum returns 0 (the caller also guards this — the cost objective is off).
// =========================================================================
[[nodiscard]] atx::f64 book_cost_bps(const alpha::AlphaStreams &strm, const alpha::Panel &panel,
                                     const cost::CalibratedCost &cost,
                                     atx::f64 target_aum) noexcept;

// =========================================================================
//  FitnessReport — one candidate's scored result (plan §4.6 step 5).
//
//  wq            : OOS WorldQuant fitness, mean over CPCV TEST folds.
//  redundancy    : mean|corr-to-pool| of the OOS PnL (the diversification input).
//  diversify     : clamp(1 − redundancy, 0, 1) — the F7 marginal-contribution weight.
//  robust        : clamp(wq_on(weak_universe) / max(wq, eps), 0, 1); 1.0 if no
//                  weak universe is configured (documented degenerate, §0.8).
//  raw           : wq * diversify * robust — the signal the search maximizes.
//  dsr           : deflated Sharpe (F4) at the running trial count N — the
//                  admission statistic; higher N -> lower dsr.
//  haircut_sharpe: max(0, sharpe − SR*_N) — the selection-adjusted point estimate.
//  cost_bps      : the S4.3 book-aggregate ROUND-TRIP impact cost (bps) at the
//                  recorded FitnessCfg.target_aum — a |w|-weighted mean of
//                  cost::round_trip_cost_bps over the candidate's last-period
//                  target weights. EXACTLY 0 when target_aum == 0 (cost off, the
//                  boundary-pin no-op). A pure function of (genome, panel, cfg) ⇒
//                  canon-cacheable. objectives[4] carries its NEGATION.
//  objectives    : the S4.1 multi-objective vector {wq, diversify, robust, ...},
//                  a RE-PROJECTION of the fields above (no new fitness math). The
//                  search's MultiObjective mode ranks genomes by NSGA-II over the
//                  first `n_objectives` entries; ScalarRaw collapses to `raw`.
//                  Index 4 = -cost_bps (NEGATED so pareto.hpp's pure-max dominance
//                  treats a CHEAPER alpha as better); set only when target_aum > 0.
//  n_objectives  : how many leading entries of `objectives` are live (3 in S4.1;
//                  grows to +novelty (S4.2, slot 3) / +cost (S4.3, slot 4) without
//                  a layout change). Fixed-slot scheme: 0,1,2 always; 3=novelty;
//                  4=cost. n_objectives covers the highest ACTIVE slot, an inactive
//                  intermediate slot left at its uniform default (inert in NSGA).
//  descriptor    : the candidate's realized OOS PnL profile (== detail::FitnessCore
//                  .oos_pnl). S4.2 BEHAVIORAL descriptor — the phenotype the
//                  population-relative novelty objective is computed from. A pure
//                  function of (genome, panel), so it is canon-cacheable (the
//                  SearchDriver copies it into CachedScore); the novelty itself is
//                  population-relative and is NOT cached. Empty on an eval failure.
//
//  Rule of Zero (the `descriptor` vector self-manages). Matches the fwd.hpp
//  forward declaration.
// =========================================================================
struct FitnessReport {
  atx::f64 wq;
  atx::f64 redundancy;
  atx::f64 diversify;
  atx::f64 robust;
  atx::f64 raw;
  atx::f64 dsr;
  atx::f64 haircut_sharpe;
  atx::f64 cost_bps{};                               // S4.3 book round-trip cost (0 if cost off)
  std::array<atx::f64, kMaxObjectives> objectives{}; // {wq, diversify, robust, novelty, -cost_bps, -node_count}
  atx::u8 n_objectives{0};                           // live leading entries
  std::vector<atx::f64> descriptor{};                // S4.2 OOS PnL profile (phenotype)
  // W4a split-sample stability (always COMPUTED; reporting + the optional, default-
  // disabled split-Sharpe admission floor). sharpe_h1 / sharpe_h2 are the per-period
  // Sharpe of the first / second half of the OOS PnL stream (index-0 zero dropped,
  // floor midpoint); split_stable == both halves share the full-sample Sharpe sign.
  // These do NOT enter `raw`, the objective vector, or the determinism digest — so
  // adding them is byte-identical on every existing path. A pure function of the OOS
  // PnL (no RNG). default-init (0/0/false) for an eval-failure / empty stream.
  atx::f64 sharpe_h1{0.0};
  atx::f64 sharpe_h2{0.0};
  bool split_stable{false};
  // S3-0 OOS mean turnover (combine::AlphaMetrics::turnover averaged over the CPCV
  // TEST folds — the value the opt-in turnover penalty reads). A pure projection of
  // core.turnover; it does NOT enter `raw`, the objective vector, or the determinism
  // digest, so surfacing it is byte-identical on every existing path (same pattern
  // as sharpe_h1/sharpe_h2). default-init 0.0 for an eval-failure / empty stream.
  atx::f64 turnover{0.0};
  // S4: bounded, finite projections of the two new NSGA columns -- ALSO the exact
  // value written into objectives[kObjCapacity]/[kObjTurnover] when the gate is on.
  // 0.0 (the default) when off -- the boundary-pin no-op, same convention as
  // cost_bps/turnover above. Do NOT enter `raw`; pure reporting + the objective copy.
  atx::f64 capacity_score{0.0};    // S4-1: bounded [0,1) sqrt-law capacity score
  atx::f64 turnover_autocorr{0.0}; // S4-2: |w|-weighted mean AR(1) coefficient
};

// =========================================================================
//  FitnessCfg — the knobs the search feeds the fitness call.
//
//  trial_count            : N for eval::deflated_sharpe (F4). Every distinct
//                           candidate the search scores increments it; a higher
//                           N lowers dsr.
//  cpcv                   : the CPCV fold geometry (TEST folds are the OOS
//                           partitions).
//  book_size              : the notional divisor for turnover (1.0 when weights
//                           are already gross-normalized fractions, the
//                           extract_streams convention).
//  target_aum             : the recorded artifact AUM at which the S4.3 cost
//                           objective is priced. 0 (the default) ⇒ the cost
//                           objective is OFF: no cost compute, no 5th objective,
//                           no eval-path change — a PURE no-op that keeps the
//                           boundary pin (and every existing digest) byte-
//                           identical. > 0 ⇒ the book round-trip cost is computed
//                           at this AUM and pushed (negated) into objectives[4]
//                           (n_objectives → 5).
//  cost                   : the calibrated cost coefficients
//                           (cost::round_trip_cost_bps reads its impact Y/δ/γ +
//                           slippage). Only consulted when target_aum > 0.
//  turnover_penalty_slope : S3-0 opt-in net-of-cost turnover penalty slope.
//                           Default 0.0 ⇒ the penalty branch is NEVER entered
//                           and the result is byte-identical to the pre-S3-0
//                           path (no NaN/inf risk, no digest drift). When > 0
//                           AND max_turnover_target is finite, excess turnover
//                           above the target is penalised as a multiplicative
//                           discount on `raw` (see finish_report). The penalty
//                           uses the OOS mean turnover already computed by
//                           aggregate_oos — no second eval pass.
//  max_turnover_target    : S3-0 the target turnover threshold (per-period,
//                           same units as combine::AlphaMetrics::turnover).
//                           Default +inf ⇒ no turnover is considered "excess"
//                           even when slope > 0 (the formula reduces to mult=1
//                           because excess==0 and slack==+inf). Set a finite
//                           positive value to activate the bite.
//  cost_selection         : B7 (S4-4/S5 wire) — charges the sqrt-law impact
//                           cost in the SEARCH SELECTION scalar (ScalarRaw
//                           `raw`), not only the target_aum-gated NSGA cost
//                           objective above. Inert default (impact_in_selection
//                           = false, selection_aum = 0) ⇒ finish_report's
//                           apply_selection_cost call is a documented no-op —
//                           byte-identical to pre-B7. See
//                           cost/cost_selection_config.hpp and
//                           factory/fitness_cost_selection.hpp.
// =========================================================================
struct FitnessCfg {
  atx::usize trial_count = 1;
  eval::CpcvConfig cpcv{};
  atx::f64 book_size = 1.0;
  atx::f64 target_aum = 0.0;                                    // S4.3: 0 ⇒ cost objective off
  cost::CalibratedCost cost{};                                   // S4.3: calibrated impact/slippage
  atx::f64 turnover_penalty_slope = 0.0;                        // S3-0: 0 ⇒ no penalty (default)
  atx::f64 max_turnover_target =                                 // S3-0: +inf ⇒ no excess ever
      std::numeric_limits<atx::f64>::infinity();
  cost::CostSelectionConfig cost_selection{};                    // B7: off by default (see above)
  // S4: capacity/turnover NSGA objective gates. SearchDriver::evaluate_generation
  // copies the SAME-NAMED SearchConfig-level flag into the per-generation FitnessCfg
  // (gen_fit) because only fitness_core/finish_report (which hold strm/panel) can
  // compute the columns. Both default false: FitnessCfg{} is the pre-S4 struct plus
  // two inert bools appended at the end -- no aggregate-init break, no digest drift.
  bool capacity_objective = false; // S4-1: gates the kObjCapacity compute
  bool turnover_objective = false; // S4-2: gates the kObjTurnover compute
};

namespace detail {

// =========================================================================
//  Tradeable-profile turnover defaults (S4-4) — named, not magic numbers.
//
//  kTradeableMaxTurnover = 0.20: at a 10 bps round-trip and 0.20/period turnover
//    the transaction-cost drag is ~2 bps/period — still below the minimum viable
//    gross edge the tradeable profile targets. At 0.30+/period (the mean-reversion
//    regime's natural maximum) costs exceed that floor, so 0.20 is the per-period
//    turnover budget the search should treat as the soft ceiling.
//  kTradeableTurnoverSlope = 2.0: an excess of 0.10 above the 0.20 target (i.e.
//    turnover 0.30) gives mult = clamp(1 - 0.10/(0.20*2), 0, 1) = 0.75 — a 25%
//    raw-fitness haircut. Meaningful but not catastrophic: it leaves the search
//    latitude while discouraging chronically high turnover. (See finish_report for
//    the multiplier formula these feed.)
// =========================================================================
inline constexpr atx::f64 kTradeableMaxTurnover = 0.20;  // per-period turnover budget
inline constexpr atx::f64 kTradeableTurnoverSlope = 2.0; // penalty slope at the budget

// =========================================================================
//  SplitHalf — the W4a split-sample stability result over an OOS PnL stream.
//
//  sharpe_h1 / sharpe_h2 : the PER-PERIOD Sharpe (mean_std_pop's ms.mean/ms.std,
//                          std==0 ⇒ 0 — the PBO/subset_sharpe convention) of the
//                          first / second half of the stream, split at the FLOOR
//                          midpoint (H1 = first floor(T/2) periods, H2 = the rest).
//  stable                : both half-Sharpes share the SIGN of the full-sample
//                          per-period Sharpe — a single-regime artifact (strong H1,
//                          dead/negative H2) is NOT stable. The full-sample sign is
//                          supplied by the caller (it already has the de-annualized
//                          per-period Sharpe); 0 (flat) requires both halves == 0.
// =========================================================================
struct SplitHalf {
  atx::f64 sharpe_h1{0.0};
  atx::f64 sharpe_h2{0.0};
  bool stable{false};
};

// split_half_sharpe — slice `oos_moments` (the OOS PnL stream with the structural
// index-0 zero ALREADY dropped, the deflation-moment span) at the floor midpoint and
// form each half's per-period Sharpe; `stable` iff both share `full_sign`'s sign.
// A PURE function (no RNG, no eval) — computing it perturbs no fitness value/digest.
// Declared so a unit test can verify the rule on a hand-built stream (single source
// of truth: fitness_core calls this).
[[nodiscard]] SplitHalf split_half_sharpe(std::span<const atx::f64> oos_moments,
                                          atx::f64 full_sign) noexcept;

// =========================================================================
//  FitnessCore — every POOL-INDEPENDENT term of a candidate's fitness.
//
//  Holds the candidate's full realized OOS PnL stream plus wq / robust / dsr /
//  haircut_sharpe. The ONLY thing missing from a FitnessReport is the
//  redundancy/diversify pair (the pool-dependent term), which each
//  pool_aware_fitness overload computes from a DIFFERENT backing (the legacy
//  AlphaStore Mean scan, or the PoolView Max scan) and folds in via
//  finish_report(). Extracting this guarantees the wq/robust/dsr/haircut math is
//  written ONCE — the legacy overload's result is byte-unchanged (it feeds the
//  same oos_pnl into the same corr_to_pool(..., Mean) it always did).
// =========================================================================
struct FitnessCore {
  std::vector<atx::f64> oos_pnl; // full realized stream (the corr-to-pool input)
  atx::f64 wq;
  atx::f64 robust;
  atx::f64 dsr;
  atx::f64 haircut_sharpe;
  // S4.3 book round-trip cost (bps) at FitnessCfg.target_aum. EXACTLY 0 when the
  // cost objective is off (target_aum == 0) — the boundary-pin no-op. Computed in
  // fitness_core from the candidate's own streams (positions) + panel while they
  // are still live, so finish_report can project it into objectives[4] = -cost_bps.
  atx::f64 cost_bps{0.0};
  // S3-0 OOS mean turnover (combine::AlphaMetrics::turnover averaged over CPCV TEST
  // folds — the same averaging that produces `wq`). Computed by aggregate_oos and
  // threaded here so finish_report can apply the opt-in turnover penalty WITHOUT a
  // second eval pass. 0.0 when no TEST fold produced a valid turnover (degenerate
  // stream; the penalty evaluates to mult=1 by the max(0, turnover-target) formula).
  atx::f64 turnover{0.0};
  // W4a split-sample stability: each half's PER-PERIOD Sharpe (ms.mean/ms.std,
  // std==0 ⇒ 0) over the OOS PnL stream with the structural index-0 zero dropped,
  // split at the FLOOR midpoint (H1 = first floor(T/2) periods, H2 = the rest).
  // A PURE function of oos_pnl (no RNG, no eval) — computing them perturbs no
  // existing value or RNG draw. split_stable == both halves SAME SIGN as the full-
  // sample Sharpe (a single-regime artifact — strong H1, dead/negative H2 — is
  // NOT stable). Carried through finish_report into the FitnessReport (reporting +
  // the optional, default-disabled split-Sharpe admission floor).
  atx::f64 sharpe_h1{0.0};
  atx::f64 sharpe_h2{0.0};
  bool split_stable{false};
  // B7 (S4-4/S5 wire): the SELECTION-scalar impact cost (bps) — book_cost_bps
  // priced at cfg.cost_selection.selection_aum (which may differ from
  // cfg.target_aum's cost-OBJECTIVE figure above). Computed in fitness_core
  // ONLY when cfg.cost_selection.impact_in_selection &&
  // cfg.cost_selection.selection_aum > 0 (else stays the default 0.0 — the
  // inert-default no-op); finish_report feeds it through
  // factory::apply_selection_cost to net `raw`. Pre-computed HERE (not in
  // finish_report) because finish_report has no strm/panel in scope — see
  // fitness_cost_selection.hpp's documented SEAM.
  atx::f64 selection_cost_bps{0.0};
  // S4: the two new NSGA columns, computed in fitness_core (which holds strm/panel)
  // ONLY when their gate is set, else the inert default 0.0. finish_report projects
  // them into objectives[kObjCapacity]/[kObjTurnover] and bumps n_objectives. 0.0 on
  // the off-path -> the objective slots stay at their uniform default (inert in NSGA).
  atx::f64 capacity_score{0.0};    // S4-1: sqrt-law capacity headroom, bounded [0,1)
  atx::f64 turnover_autocorr{0.0}; // S4-2: |w|-weighted mean AR(1) coefficient
};

// Compute every pool-independent fitness term (steps 1, 3, 5 of the §4.6 score:
// the OOS WQ aggregate, the sub-universe robustness re-eval, and the deflation).
// IDENTICAL control flow + values to the original pool_aware_fitness body for
// those steps — the legacy overload below now simply layers the Mean-based
// redundancy on top, so its output is provably unchanged. Err propagates a
// candidate compile/eval/extract failure (full or weak panel).
//
// S3-1 PERF: `cpcv_cache` (optional, default nullptr) is a thread-safe cache of
// pre-built label spans + CPCV folds keyed on (n_periods, cpcv).  When supplied,
// the first call for a given key builds and stores the result; every subsequent
// call for the same key is O(1) and allocates nothing.  nullptr -> recompute on
// every call (the legacy behaviour, byte-identical).
[[nodiscard]] atx::core::Result<FitnessCore>
fitness_core(const Genome &cand, const alpha::Panel &panel, const WeightPolicy &policy,
             const exec::ExecutionSimulator &sim, const FitnessCfg &cfg,
             const alpha::Panel *weak_panel, alpha::Engine *engine = nullptr,
             const alpha::SignalSet *signals = nullptr,
             CpcvCache *cpcv_cache = nullptr);

// Fold a pool-dependent redundancy into a FitnessCore -> the final FitnessReport.
// `redundancy` is the (Mean for the legacy AlphaStore path, Max for the PoolView
// path) |corr-to-pool| of core.oos_pnl; diversify = clamp(1−redundancy, 0, 1) and
// raw = wq * diversify * robust — identical to the original assembly. S4.3: when
// `cost_active` (FitnessCfg.target_aum > 0) the report's cost_bps = core.cost_bps
// is projected into objectives[4] = -cost_bps and n_objectives bumped to 5 (slot 3
// left at its inert default 0); when inactive the objective vector is the exact
// S4.1 {wq,diversify,robust} with n_objectives 3 and cost_bps 0 (boundary-pin
// no-op). `cost_active` (not core.cost_bps != 0) is the gate, so a genuinely-zero
// cost at a positive target_aum still registers the (uniform, inert) 5th objective.
//
// S3-0: when cfg.turnover_penalty_slope > 0.0, a multiplicative discount is applied
// to `raw` AFTER the wq*diversify*robust product, based on core.turnover vs
// cfg.max_turnover_target.  Default slope == 0.0 -> the if-branch is never entered
// -> byte-identical to pre-S3-0 (the boundary-pin holds).  The cfg reference is
// borrowed for the duration of the call only (no ownership).
//
// B7 (S4-4/S5 wire): immediately after `raw` is assembled (wq*diversify*robust,
// BEFORE the S3-0 turnover-penalty multiplier), `raw` is netted of
// `core.selection_cost_bps` via `apply_selection_cost(raw, core.selection_cost_bps,
// cfg.cost_selection)`. That function's OWN inert-default guard
// (impact_in_selection==false or selection_aum<=0 -> return raw unchanged) is the
// sole gate — cfg.cost_selection{} default keeps this call byte-identical to
// pre-B7 on every existing path (core.selection_cost_bps is also 0.0 by default,
// so even without the guard the subtraction would be a no-op).
[[nodiscard]] FitnessReport finish_report(const FitnessCore &core, atx::f64 redundancy,
                                          bool cost_active, const FitnessCfg &cfg);

} // namespace detail

// =========================================================================
//  Tradeable-profile turnover helpers (S4-4) — OPT-IN, header-only inline.
//
//  These make the existing (default-inert) turnover-penalty mechanism actively
//  useful for the tradeable profile WITHOUT changing any default: FitnessCfg{} is
//  untouched, so the no-flag search path stays byte-identical. S7 calls
//  tradeable_fitness_cfg() when the user passes --tradeable-profile and feeds a
//  gate's cost_max_turnover through turnover_target_from_gate(); S4 only ships and
//  proves the helpers.
// =========================================================================

/// A FitnessCfg with the recommended tradeable-profile turnover defaults
/// (turnover_penalty_slope = kTradeableTurnoverSlope, max_turnover_target =
/// kTradeableMaxTurnover). EVERY other field is the inert FitnessCfg{} default; the
/// caller may override any field after the call. OPT-IN: FitnessCfg{} (the default)
/// is unchanged, so no existing golden digest moves. Pure / thread-safe.
[[nodiscard]] inline FitnessCfg tradeable_fitness_cfg() noexcept {
  FitnessCfg cfg{}; // start from the inert defaults
  cfg.turnover_penalty_slope = detail::kTradeableTurnoverSlope;
  cfg.max_turnover_target = detail::kTradeableMaxTurnover;
  return cfg;
}

/// Derive a max_turnover_target from a gate's cost_max_turnover threshold so the
/// search penalty is coherent with admission: when the gate bars alphas with
/// turnover > L, the search should be penalised for exceeding L rather than
/// discovering them and failing admission. Returns L for L > 0, and +inf for
/// L <= 0 (no gate -> no target -> the inert no-penalty value). The caller sets the
/// result into FitnessCfg::max_turnover_target. Pure / thread-safe.
[[nodiscard]] inline atx::f64 turnover_target_from_gate(atx::f64 gate_turnover_limit) noexcept {
  return (gate_turnover_limit > 0.0) ? gate_turnover_limit
                                     : std::numeric_limits<atx::f64>::infinity();
}

// Re-export the tradeable turnover constants into the factory namespace so callers
// and tests reference them without reaching into `detail` (the values live once in
// detail; these are aliases, not duplicates).
inline constexpr atx::f64 kTradeableMaxTurnover = detail::kTradeableMaxTurnover;
inline constexpr atx::f64 kTradeableTurnoverSlope = detail::kTradeableTurnoverSlope;

// =========================================================================
//  pool_aware_fitness — the §4.6 marginal-contribution score (OOS + deflated).
//
//  (1) Eval the candidate ONCE over the full `panel` (causal VM, no look-ahead),
//      extract its PnL/position streams, and average combine::compute_metrics()
//      .fitness/.sharpe over the CPCV TEST folds -> OOS `wq`/`sharpe` (F3).
//  (2) redundancy = corr_to_pool(OOS PnL, pool, Mean); diversify = clamp(1−red,0,1) (F7).
//  (3) robust = clamp(wq_on(weak_panel)/max(wq,eps),0,1) when a weak universe Panel
//      is supplied; else 1.0 (§0.8 degenerate).
//  (4) raw = wq * diversify * robust.
//  (5) dsr = eval::deflated_sharpe(sharpe, T, skew, kurt, N=trial_count, nullopt) (F4).
//
//  `weak_panel` (optional borrow): the §0.8 alternate-universe Panel for the
//  robustness re-eval. nullptr/nullopt -> robust = 1.0. Borrows `panel`,
//  `policy`, `sim`, `pool` for the duration of the call (no ownership taken).
//  Returns Err only if the candidate fails to compile/evaluate/extract.
// =========================================================================
// S3-1 PERF: `cpcv_cache` (optional, default nullptr) is forwarded directly into
// detail::fitness_core. When supplied, spans+folds are built once per unique
// (n_periods, cpcv) and reused for every subsequent genome — O(1) per call.
[[nodiscard]] atx::core::Result<FitnessReport>
pool_aware_fitness(const Genome &cand, const combine::AlphaStore &pool, const alpha::Panel &panel,
                   const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                   const FitnessCfg &cfg, const alpha::Panel *weak_panel = nullptr,
                   alpha::Engine *engine = nullptr,
                   const alpha::SignalSet *signals = nullptr,
                   CpcvCache *cpcv_cache = nullptr);

} // namespace atx::engine::factory
