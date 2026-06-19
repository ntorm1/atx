#pragma once

// atx::engine::eval — cluster validation + the GICS-grouping head-to-head
// benchmark (Sprint S11-6, the HONEST-EVIDENCE unit).
//
// ===========================================================================
//  What this header is — and what it deliberately is NOT
// ===========================================================================
//  The S11 clustering spine produces a data-driven `IndClass.cluster` labeling
//  that DEPARTS from the GICS `IndClass.sector` taxonomy. The open question is
//  whether that departure is (a) STABLE enough to trade and (b) actually BETTER
//  than sectors when used to neutralize an alpha. The research is explicit that
//  the second claim is UNPROVEN — every ">10% return / Sharpe>1 over GICS"
//  assertion was refuted (clustering-module-sprint-plan §8–§9). So this unit is
//  built to MEASURE, not to assume. It provides three honest instruments:
//
//    1. cluster_stability  — temporal label-churn between consecutive rolling
//       snapshots, plus a per-cluster best-match Jaccard recovery (a Hennig
//       `clusterboot` analogue): how well each cluster at snapshot i survives
//       into snapshot i+1. Clusters whose recovery falls below cfg.jaccard_floor
//       are FLAGGED as unstable. The estimate is point-in-time: scoring snapshot
//       i reads only snapshots <= i (no look-ahead into future re-estimations).
//
//    2. agreement_vs_gics  — per-date Adjusted Rand Index (ARI) and Normalized
//       Mutual Information (NMI) between the cluster labeling and the sector
//       labeling: how far the data has moved away from GICS. ARI == 1 iff the
//       two partitions are identical (up to relabeling); ARI ~ 0 for independent
//       labelings. NMI is the information-theoretic companion, 1 iff identical.
//
//    3. run_head_to_head  — the fair contest. Given ONE Panel that carries BOTH
//       classifiers (built via append_cluster_field) plus a signal, it runs the
//       SAME alpha neutralized two ways — group_neutralize(sig, IndClass.sector)
//       vs group_neutralize(sig, IndClass.cluster) — through the EXISTING eval
//       path (extract_streams -> compute_return_metrics -> deflated_sharpe) and
//       emits two Scorecards plus their delta. render_head_to_head prints the
//       research §9 caveat INLINE so a reader cannot mistake the delta for a
//       proven win. The harness is callable for the deferred real-universe run.
//
// ===========================================================================
//  Reuse, not reinvention
// ===========================================================================
//  This header adds NO new performance/metric definitions and NO new DSL ops.
//   * The alpha is evaluated through the same parse -> analyze -> compile ->
//     Engine::evaluate VM path the rest of the engine uses; the two arms differ
//     ONLY in the group field name passed to group_neutralize.
//   * PnL/positions come from alpha::extract_streams (no look-ahead: w[t-1]
//     earns ret[t]); the cost coefficient is the same ExecutionSimulator field.
//   * Sharpe/DSR come from eval::compute_return_metrics + eval::deflated_sharpe
//     (one Sharpe convention across the engine). Turnover and the %ADV capacity
//     proxy are derived from the extracted positions with the same definition
//     extract_streams uses for its turnover cost.
//
// ===========================================================================
//  Determinism (mandatory, tested)
// ===========================================================================
//  Every metric reduction walks its inputs in ascending index. The stability
//  windows are point-in-time (a past score never reads a future snapshot). No
//  RNG anywhere on the result path — the Jaccard recovery is a purely
//  deterministic temporal best-match, not a bootstrap. Two runs over identical
//  inputs are byte-identical (two-runs-equal, asserted in the suite).
//
// Header-only; every free function is `inline`. These are COLD research/report
// paths (run once per evaluation), so std::vector allocation is fine and the
// metric machinery is reused rather than hand-fused.

#include <algorithm>   // std::max, std::min
#include <cmath>       // std::isnan, std::isfinite, std::log
#include <limits>      // std::numeric_limits (quiet_NaN, infinity)
#include <map>         // std::map (order-fixed label -> index)
#include <optional>    // std::nullopt (single-stream DSR variance)
#include <span>        // std::span
#include <string>      // std::string
#include <utility>     // std::move, std::pair
#include <vector>      // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/types.hpp" // f64, i64, usize

#include "atx/engine/alpha/bytecode.hpp"      // alpha::compile, alpha::Program
#include "atx/engine/alpha/cluster_panel.hpp" // alpha::ClusterPanel (snapshot stream)
#include "atx/engine/alpha/panel.hpp"         // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp"        // alpha::parse_expr
#include "atx/engine/alpha/registry.hpp"      // alpha::Library
#include "atx/engine/alpha/streams.hpp"       // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/typecheck.hpp"     // alpha::analyze
#include "atx/engine/alpha/vm.hpp"            // alpha::Engine

#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/perf_metrics.hpp"    // eval::compute_return_metrics, ReturnMetrics
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, eval::excess_kurtosis

namespace atx::engine::eval {

// The research §9 caveat printed inline by every head-to-head report. Kept as a
// named constant so the test can assert its presence verbatim: the whole point
// of S11-6 is that the cluster-vs-GICS delta is the TEST, never an assumption.
inline constexpr const char *kGicsCaveat =
    "CAVEAT (research §9): profitability over GICS is UNPROVEN -- all "
    "\">10% return / Sharpe>1 over GICS\" claims were refuted. This delta is the "
    "test, not a result; read it as evidence to weigh, not a clustering win.";

// ===========================================================================
//  1. Temporal cluster stability (clusterboot analogue)
// ===========================================================================

// Tuning for cluster_stability. Aggregate so callers brace-init only overrides.
struct StabilityCfg {
  // A cluster whose best-match Jaccard recovery into the next snapshot is below
  // this floor is flagged unstable. 0.6 is the conventional Hennig clusterboot
  // "moderately stable" threshold (a cluster recovered < 0.6 of the time is not
  // a dependable structure to trade).
  atx::f64 jaccard_floor = 0.6;
};

// Per consecutive-snapshot transition (i -> i+1): the label churn and the
// per-cluster best-match Jaccard recovery of every cluster present at snapshot i.
struct StabilityStep {
  atx::usize from_date = 0; // snapshot i's date index
  atx::usize to_date = 0;   // snapshot i+1's date index

  // Fraction of jointly-clustered instruments whose (best-matched) cluster
  // membership changed from snapshot i to i+1, in [0, 1]. 0 == perfectly stable.
  atx::f64 churn = 0.0;

  // best_jaccard[c] is the Jaccard overlap of cluster `c` at snapshot i with its
  // BEST-matching cluster at snapshot i+1: |A_c ∩ B_best| / |A_c ∪ B_best|. One
  // entry per cluster label present at snapshot i, indexed by that label value.
  // A label absent at snapshot i has recovery NaN (it is not scored).
  std::vector<atx::f64> best_jaccard;
};

// The full temporal-stability picture over a snapshot stream.
struct StabilityResult {
  std::vector<StabilityStep> steps; // one per consecutive snapshot pair

  // (snapshot_index, cluster_label) pairs whose best Jaccard recovery fell below
  // cfg.jaccard_floor. The snapshot_index is the index of the FROM snapshot.
  std::vector<std::pair<atx::usize, int>> flagged;

  // Mean churn across all transitions (0 when there is < 1 transition). A single
  // order-fixed summary scalar for the report line.
  atx::f64 mean_churn = 0.0;
};

namespace detail {

// Best-match Jaccard recovery of every cluster in `a` against the clusters in
// `b`, over the instruments jointly clustered in BOTH snapshots (an instrument
// kUnclustered in either snapshot carries no cluster membership and is skipped).
// Returns a vector indexed by the label value in `a` (length == max label + 1),
// with NaN for any label not present. Plus the churn fraction (1 - mean best
// Jaccard weighted by cluster size is not used; churn here is the fraction of
// joint instruments NOT in their cluster's best-matched target — see below).
//
// Pure, order-fixed (ascending instrument, ascending label). No RNG.
struct PairStability {
  std::vector<atx::f64> best_jaccard; // indexed by label in `a`
  atx::f64 churn = 0.0;
};

[[nodiscard]] inline PairStability
pair_stability(const std::vector<int> &a, const std::vector<int> &b) {
  const atx::usize n = std::min(a.size(), b.size());

  // Label inventories (ascending) over the JOINTLY-clustered instruments only.
  int max_a = -1;
  int max_b = -1;
  for (atx::usize i = 0; i < n; ++i) {
    if (a[i] >= 0 && b[i] >= 0) {
      max_a = std::max(max_a, a[i]);
      max_b = std::max(max_b, b[i]);
    }
  }
  PairStability out;
  if (max_a < 0) {
    return out; // no jointly-clustered instrument -> nothing to score
  }

  // Cluster sizes in `a`, pair-counts |A_c ∩ B_d|, and sizes in `b`, all over
  // the joint set. Dense 2D contingency table (rows = a-labels, cols = b-labels).
  const atx::usize na = static_cast<atx::usize>(max_a) + 1U;
  const atx::usize nb = static_cast<atx::usize>(max_b) + 1U;
  std::vector<atx::usize> size_a(na, 0U);
  std::vector<atx::usize> size_b(nb, 0U);
  std::vector<atx::usize> table(na * nb, 0U);
  atx::usize joint = 0U;
  for (atx::usize i = 0; i < n; ++i) {
    if (a[i] >= 0 && b[i] >= 0) {
      const atx::usize ca = static_cast<atx::usize>(a[i]);
      const atx::usize cb = static_cast<atx::usize>(b[i]);
      ++size_a[ca];
      ++size_b[cb];
      ++table[ca * nb + cb];
      ++joint;
    }
  }

  // Best-match Jaccard per a-label, and the churn fraction. For each cluster c in
  // `a`, find the b-cluster d maximizing |A_c ∩ B_d| / |A_c ∪ B_d|. The instrument
  // count NOT in (A_c ∩ B_best) over c accumulates the churn numerator.
  out.best_jaccard.assign(na, std::numeric_limits<atx::f64>::quiet_NaN());
  atx::usize stayed = 0U; // instruments in their a-cluster's best-matched b-cluster
  for (atx::usize ca = 0; ca < na; ++ca) {
    if (size_a[ca] == 0U) {
      continue; // label not present in `a` over the joint set
    }
    atx::f64 best = 0.0;
    atx::usize best_inter = 0U;
    for (atx::usize cb = 0; cb < nb; ++cb) {
      const atx::usize inter = table[ca * nb + cb];
      if (inter == 0U) {
        continue;
      }
      const atx::usize uni = size_a[ca] + size_b[cb] - inter;
      const atx::f64 j = static_cast<atx::f64>(inter) / static_cast<atx::f64>(uni);
      if (j > best) {
        best = j;
        best_inter = inter;
      }
    }
    out.best_jaccard[ca] = best;
    stayed += best_inter;
  }
  // churn = fraction of joint instruments NOT retained in their cluster's best
  // match. joint > 0 here (max_a >= 0 implies at least one joint instrument).
  out.churn = 1.0 - static_cast<atx::f64>(stayed) / static_cast<atx::f64>(joint);
  return out;
}

} // namespace detail

// Temporal stability over a ClusterPanel's snapshot stream. For each consecutive
// pair (i, i+1) it computes the label churn and the per-cluster best-match
// Jaccard recovery, flagging any cluster below cfg.jaccard_floor. POINT-IN-TIME:
// the score for transition i depends only on snapshots i and i+1 — truncating
// the stream after snapshot i+1 leaves every score at and before i unchanged
// (asserted in the suite). Pure; order-fixed; no RNG.
[[nodiscard]] inline StabilityResult
cluster_stability(const std::vector<alpha::ClusterPanel::Snapshot> &snapshots,
                  const StabilityCfg &cfg = {}) {
  StabilityResult out;
  if (snapshots.size() < 2U) {
    return out; // fewer than two re-estimations -> no transition to score
  }

  atx::f64 churn_sum = 0.0;
  for (atx::usize i = 0; i + 1U < snapshots.size(); ++i) {
    const detail::PairStability ps =
        detail::pair_stability(snapshots[i].cluster_id, snapshots[i + 1U].cluster_id);

    StabilityStep step;
    step.from_date = snapshots[i].date;
    step.to_date = snapshots[i + 1U].date;
    step.churn = ps.churn;
    step.best_jaccard = ps.best_jaccard;
    churn_sum += ps.churn;

    // Flag clusters below the floor (ascending label order is preserved by the
    // dense index walk, so the flagged list is deterministic).
    for (atx::usize c = 0; c < ps.best_jaccard.size(); ++c) {
      const atx::f64 jac = ps.best_jaccard[c];
      if (!std::isnan(jac) && jac < cfg.jaccard_floor) {
        out.flagged.emplace_back(i, static_cast<int>(c));
      }
    }
    out.steps.push_back(std::move(step));
  }
  out.mean_churn = churn_sum / static_cast<atx::f64>(out.steps.size());
  return out;
}

// ===========================================================================
//  2. Agreement vs GICS — Adjusted Rand Index + Normalized Mutual Information
// ===========================================================================

struct AgreementResult {
  atx::f64 ari = 0.0; // Adjusted Rand Index: 1 == identical partitions, ~0 == independent
  atx::f64 nmi = 0.0; // Normalized Mutual Information: 1 == identical, 0 == independent
};

namespace detail {

// Dense contingency table over the instruments labeled (>= 0) in BOTH `x` and
// `y`. Returns (table row-major [nx*ny], row sums, col sums, N). An instrument
// unlabeled (NaN/negative) in either side is excluded — the standard treatment
// of a missing classifier cell (matches the cs_ops / GICS missing-sector intent).
// Labels are remapped to dense [0, k) via an order-fixed std::map so the indices
// are deterministic regardless of the raw label magnitudes.
struct Contingency {
  std::vector<atx::usize> table; // row-major [nx * ny]
  std::vector<atx::usize> row;   // row sums (x clusters)
  std::vector<atx::usize> col;   // col sums (y clusters)
  atx::usize nx = 0U;
  atx::usize ny = 0U;
  atx::usize total = 0U;
};

[[nodiscard]] inline Contingency build_contingency(std::span<const atx::f64> x,
                                                   std::span<const atx::f64> y) {
  const atx::usize n = std::min(x.size(), y.size());
  // Order-fixed dense remaps (std::map keeps ascending key order deterministic).
  std::map<atx::i64, atx::usize> remap_x;
  std::map<atx::i64, atx::usize> remap_y;
  std::vector<std::pair<atx::usize, atx::usize>> pairs;
  pairs.reserve(n);
  for (atx::usize i = 0; i < n; ++i) {
    if (std::isnan(x[i]) || std::isnan(y[i]) || x[i] < 0.0 || y[i] < 0.0) {
      continue; // unlabeled in either partition -> excluded
    }
    const atx::i64 lx = static_cast<atx::i64>(x[i]);
    const atx::i64 ly = static_cast<atx::i64>(y[i]);
    const atx::usize ix = remap_x.emplace(lx, remap_x.size()).first->second;
    const atx::usize iy = remap_y.emplace(ly, remap_y.size()).first->second;
    pairs.emplace_back(ix, iy);
  }

  Contingency c;
  c.nx = remap_x.size();
  c.ny = remap_y.size();
  c.total = pairs.size();
  c.table.assign(c.nx * c.ny, 0U);
  c.row.assign(c.nx, 0U);
  c.col.assign(c.ny, 0U);
  for (const auto &[ix, iy] : pairs) {
    ++c.table[ix * c.ny + iy];
    ++c.row[ix];
    ++c.col[iy];
  }
  return c;
}

// n-choose-2 as f64 (the Rand-index pair count). 0 for n < 2.
[[nodiscard]] inline atx::f64 choose2(atx::usize n) noexcept {
  return (n < 2U) ? 0.0 : static_cast<atx::f64>(n) * static_cast<atx::f64>(n - 1U) / 2.0;
}

} // namespace detail

// Adjusted Rand Index between two integer labelings (NaN/negative excluded). The
// Hubert-Arabie form: ARI = (Σ C(n_ij,2) − E) / (½(Σ C(a_i,2)+Σ C(b_j,2)) − E),
// E = Σ C(a_i,2)·Σ C(b_j,2) / C(N,2). 1.0 iff the partitions agree up to
// relabeling; ~0 for independent labelings; can be slightly negative for worse-
// than-chance agreement. A degenerate case (both partitions a single cluster, or
// fewer than two labeled instruments) is DEFINED to return 1.0 when the labelings
// are trivially identical and 0.0 otherwise — the standard sklearn convention.
// Order-fixed reductions; pure; no RNG.
[[nodiscard]] inline atx::f64 adjusted_rand_index(std::span<const atx::f64> a,
                                                  std::span<const atx::f64> b) noexcept {
  const detail::Contingency c = detail::build_contingency(a, b);
  if (c.total < 2U) {
    return 1.0; // 0/1 labeled instruments: trivially "identical"
  }

  atx::f64 sum_cells = 0.0;
  for (const atx::usize v : c.table) {
    sum_cells += detail::choose2(v);
  }
  atx::f64 sum_row = 0.0;
  for (const atx::usize v : c.row) {
    sum_row += detail::choose2(v);
  }
  atx::f64 sum_col = 0.0;
  for (const atx::usize v : c.col) {
    sum_col += detail::choose2(v);
  }

  const atx::f64 total_pairs = detail::choose2(c.total);
  const atx::f64 expected = (sum_row * sum_col) / total_pairs;
  const atx::f64 max_index = 0.5 * (sum_row + sum_col);
  const atx::f64 denom = max_index - expected;
  if (denom == 0.0) {
    // Both partitions degenerate (e.g. each a single cluster): the index and its
    // expectation coincide. Identical partitions -> 1.0 by convention.
    return 1.0;
  }
  return (sum_cells - expected) / denom;
}

// Normalized Mutual Information (arithmetic-mean normalization): NMI = I(X;Y) /
// ((H(X)+H(Y))/2), with natural logs. 1.0 iff the partitions are identical (up
// to relabeling); 0.0 when independent. Both entropies zero (both partitions a
// single cluster) -> 1.0 by convention (perfect, trivial agreement). NaN/negative
// labels excluded. Order-fixed; pure; no RNG.
[[nodiscard]] inline atx::f64 normalized_mutual_info(std::span<const atx::f64> a,
                                                     std::span<const atx::f64> b) noexcept {
  const detail::Contingency c = detail::build_contingency(a, b);
  if (c.total == 0U) {
    return 1.0;
  }
  const atx::f64 nf = static_cast<atx::f64>(c.total);

  // Mutual information I(X;Y) = Σ p_ij log(p_ij / (p_i p_j)), p in counts/N.
  atx::f64 mi = 0.0;
  for (atx::usize ix = 0; ix < c.nx; ++ix) {
    for (atx::usize iy = 0; iy < c.ny; ++iy) {
      const atx::usize nij = c.table[ix * c.ny + iy];
      if (nij == 0U) {
        continue;
      }
      const atx::f64 pij = static_cast<atx::f64>(nij) / nf;
      const atx::f64 pi = static_cast<atx::f64>(c.row[ix]) / nf;
      const atx::f64 pj = static_cast<atx::f64>(c.col[iy]) / nf;
      mi += pij * std::log(pij / (pi * pj));
    }
  }

  // Marginal entropies H(X), H(Y) (natural log).
  atx::f64 hx = 0.0;
  for (atx::usize ix = 0; ix < c.nx; ++ix) {
    if (c.row[ix] == 0U) {
      continue;
    }
    const atx::f64 pi = static_cast<atx::f64>(c.row[ix]) / nf;
    hx -= pi * std::log(pi);
  }
  atx::f64 hy = 0.0;
  for (atx::usize iy = 0; iy < c.ny; ++iy) {
    if (c.col[iy] == 0U) {
      continue;
    }
    const atx::f64 pj = static_cast<atx::f64>(c.col[iy]) / nf;
    hy -= pj * std::log(pj);
  }

  const atx::f64 denom = 0.5 * (hx + hy);
  if (denom <= 0.0) {
    return 1.0; // both single-cluster: trivially identical
  }
  // Clamp tiny negative MI from rounding; NMI is bounded to [0, 1].
  const atx::f64 nmi = mi / denom;
  return std::max(0.0, std::min(1.0, nmi));
}

// Per-date agreement of the cluster vs sector cross-sections of an augmented
// Panel. `cluster_field` / `sector_field` are the (group-typed) field names;
// each date's two label rows are scored with ARI + NMI. The mean is taken ONLY
// over SCORED dates — those where BOTH cross-sections carry at least one labeled
// (non-NaN) instrument. A warm-up / fully-unlabeled date yields the degenerate
// 1.0 ("trivially identical"), which is information-free; counting it would pull
// the mean toward 1.0 and manufacture a false "cluster ≈ GICS" agreement that this
// honest-evidence unit must not manufacture. `per_date` keeps EVERY date (including
// the degenerate ones) for inspection; `scored_dates` reports how many dates
// actually contributed to the mean so a caller sees the effective sample size.
struct AgreementOverTime {
  std::vector<AgreementResult> per_date; // one per panel date (degenerate dates kept)
  atx::f64 mean_ari = 0.0;               // mean ARI over scored dates only
  atx::f64 mean_nmi = 0.0;               // mean NMI over scored dates only
  atx::usize scored_dates = 0U;          // dates with >=1 labeled instrument in BOTH rows
};

namespace detail {

// A cross-section has at least one labeled (non-NaN, non-negative) instrument.
[[nodiscard]] inline bool has_labeled(std::span<const atx::f64> xs) noexcept {
  for (const atx::f64 v : xs) {
    if (!std::isnan(v) && v >= 0.0) {
      return true;
    }
  }
  return false;
}

} // namespace detail

[[nodiscard]] inline atx::core::Result<AgreementOverTime>
agreement_vs_gics(const alpha::Panel &panel, const std::string &cluster_field = "IndClass.cluster",
                  const std::string &sector_field = "IndClass.sector") {
  ATX_TRY(const alpha::FieldId cl, panel.field_id(cluster_field));
  ATX_TRY(const alpha::FieldId se, panel.field_id(sector_field));

  AgreementOverTime out;
  const atx::usize dates = panel.dates();
  out.per_date.reserve(dates);
  atx::f64 ari_sum = 0.0;
  atx::f64 nmi_sum = 0.0;
  for (atx::usize d = 0; d < dates; ++d) {
    const std::span<const atx::f64> cx = panel.field_cross_section(cl, d);
    const std::span<const atx::f64> sx = panel.field_cross_section(se, d);
    AgreementResult r;
    r.ari = adjusted_rand_index(cx, sx);
    r.nmi = normalized_mutual_info(cx, sx);
    out.per_date.push_back(r);
    // Only a date with labeled instruments on BOTH sides carries real agreement
    // information; a warm-up / unlabeled date is excluded from the mean.
    if (detail::has_labeled(cx) && detail::has_labeled(sx)) {
      ari_sum += r.ari;
      nmi_sum += r.nmi;
      ++out.scored_dates;
    }
  }
  if (out.scored_dates > 0U) {
    out.mean_ari = ari_sum / static_cast<atx::f64>(out.scored_dates);
    out.mean_nmi = nmi_sum / static_cast<atx::f64>(out.scored_dates);
  }
  return atx::core::Ok(std::move(out));
}

// ===========================================================================
//  3. Head-to-head harness — sector grouping vs cluster grouping
// ===========================================================================

// One arm's scorecard. The OOS performance summary of an alpha neutralized by a
// single classifier. Fields reuse the engine's one Sharpe / DSR conventions
// (compute_return_metrics + deflated_sharpe); turnover + adv_capacity are derived
// from the extracted positions with the same turnover definition the loop uses.
struct Scorecard {
  atx::f64 sharpe = 0.0;          // annualized Sharpe (eval::ReturnMetrics.sharpe)
  atx::f64 dsr = 0.0;            // deflated Sharpe ratio (PSR against N=1 selection)
  atx::f64 max_dd = 0.0;        // peak-to-trough drawdown fraction
  atx::f64 mean_turnover = 0.0; // mean per-period Σ|Δw| (gross-notional fraction traded)
  atx::f64 adv_capacity = 0.0;  // %ADV capacity proxy: 1 / mean_turnover (higher == roomier)
  atx::usize periods = 0U;      // PnL observations scored (== panel dates)
};

// The fair head-to-head: the SAME alpha grouped two ways, plus the delta.
struct HeadToHead {
  Scorecard sector;  // group_neutralize(sig, IndClass.sector)
  Scorecard cluster; // group_neutralize(sig, IndClass.cluster)
  Scorecard delta;   // cluster - sector, field by field (the honest difference)
};

// Eval knobs for the head-to-head. Aggregate; defaults give a frictionless,
// rank/dollar-neutral arm pair that differs ONLY in the grouping classifier.
struct HeadToHeadCfg {
  std::string cluster_field = "IndClass.cluster";
  std::string sector_field = "IndClass.sector";
  atx::f64 periods_per_year = 252.0; // annualization for the Sharpe convention
};

namespace detail {

// Evaluate `expr` over `panel`, extract the single root alpha's OOS PnL +
// positions, and fold them into a Scorecard. REUSES the standard VM + stream +
// metric path; adds no new performance definition.
[[nodiscard]] inline atx::core::Result<Scorecard>
score_arm(const std::string &expr, const alpha::Panel &panel, const WeightPolicy &policy,
          const exec::ExecutionSimulator &sim, atx::f64 periods_per_year) {
  alpha::Library dsl;
  ATX_TRY(alpha::Ast ast, alpha::parse_expr(expr, dsl));
  ATX_TRY(alpha::Analysis info, alpha::analyze(ast));
  ATX_TRY(alpha::Program prog, alpha::compile(ast, info));
  alpha::Engine engine{panel};
  ATX_TRY(alpha::SignalSet sig, engine.evaluate(prog));
  if (sig.alphas.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "score_arm: expression produced no alpha root");
  }
  ATX_TRY(alpha::AlphaStreams strm, alpha::extract_streams(sig, policy, panel, sim));

  const std::span<const atx::f64> pnl = strm.pnl(0);

  // Sharpe / drawdown via the shared metric (one convention). The DSR uses the
  // realized higher moments of the PnL stream (population skew / excess kurtosis,
  // the stats_ext definitions deflated_sharpe expects) with N=1 (this harness
  // scores ONE configuration per arm, so there is no selection inflation to
  // deflate beyond the single-stream PSR baseline).
  ReturnMetricsCfg mcfg;
  mcfg.periods_per_year = periods_per_year;
  const ReturnMetrics rm = compute_return_metrics(pnl, mcfg);

  // r = pnl[1..T) — the same index-0 exclusion compute_return_metrics applies, so
  // the moments fed to the DSR share the Sharpe observation window.
  Scorecard sc;
  sc.periods = strm.n_periods();
  sc.sharpe = rm.sharpe;
  sc.max_dd = rm.max_dd;
  if (pnl.size() > 1U) {
    const std::span<const atx::f64> r{pnl.data() + 1, pnl.size() - 1U};
    const atx::f64 skew = skewness(r);
    const atx::f64 exk = excess_kurtosis(r);
    // Per-period (non-annualized) Sharpe for the DSR: mean/std_pop over r.
    const MeanStd ms = mean_std_pop(r);
    const atx::f64 sr_pp = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
    const DsrResult dsr = deflated_sharpe(sr_pp, r.size(), skew, exk, /*N=*/1U, std::nullopt);
    sc.dsr = dsr.dsr;
  }

  // Turnover = mean over t>=1 of Σ_j |w_j[t] − w_j[t-1]| (the extract_streams
  // definition). The %ADV capacity proxy is its reciprocal: a book that churns
  // less of its gross each period has more room before it moves the market.
  const atx::usize dates = strm.n_periods();
  const atx::usize insts = strm.n_instruments();
  atx::f64 turn_sum = 0.0;
  for (atx::usize t = 1; t < dates; ++t) {
    const std::span<const atx::f64> prev = strm.positions(0, t - 1);
    const std::span<const atx::f64> cur = strm.positions(0, t);
    atx::f64 turn = 0.0;
    for (atx::usize j = 0; j < insts; ++j) {
      const atx::f64 dw = cur[j] - prev[j];
      turn += (dw < 0.0) ? -dw : dw;
    }
    turn_sum += turn;
  }
  sc.mean_turnover = (dates > 1U) ? turn_sum / static_cast<atx::f64>(dates - 1U) : 0.0;
  sc.adv_capacity = (sc.mean_turnover > 0.0)
                        ? 1.0 / sc.mean_turnover
                        : std::numeric_limits<atx::f64>::infinity();
  return atx::core::Ok(sc);
}

// Field-by-field cluster - sector difference. adv_capacity differences with an
// infinite operand (a zero-turnover arm) collapse to 0 rather than propagate an
// inf-minus-inf NaN — the report reads "no measurable capacity gap" honestly.
[[nodiscard]] inline Scorecard score_delta(const Scorecard &cluster, const Scorecard &sector) {
  Scorecard d;
  d.sharpe = cluster.sharpe - sector.sharpe;
  d.dsr = cluster.dsr - sector.dsr;
  d.max_dd = cluster.max_dd - sector.max_dd;
  d.mean_turnover = cluster.mean_turnover - sector.mean_turnover;
  const bool finite_caps = std::isfinite(cluster.adv_capacity) && std::isfinite(sector.adv_capacity);
  d.adv_capacity = finite_caps ? (cluster.adv_capacity - sector.adv_capacity) : 0.0;
  d.periods = (cluster.periods >= sector.periods) ? (cluster.periods - sector.periods)
                                                  : (sector.periods - cluster.periods);
  return d;
}

} // namespace detail

// Run the SAME signal neutralized by SECTOR and by CLUSTER through the existing
// eval path and emit both scorecards plus the delta. `panel` MUST carry BOTH
// group classifiers (build it with alpha::append_cluster_field over a source that
// already has IndClass.sector) plus the close/return fields extract_streams reads.
// `signal_expr` is the bare alpha (e.g. "rank(close)"); the harness wraps it as
// group_neutralize(signal_expr, <field>) for each arm. CALLABLE FOR THE DEFERRED
// REAL RUN: point `panel` at the p3-S2 real universe and the same call produces
// the real head-to-head — no code change.
//
// Err propagates a missing group/return field, a parse/analyze/compile failure,
// or a shape mismatch from extract_streams. Pure given (panel, expr, cfg); no RNG.
[[nodiscard]] inline atx::core::Result<HeadToHead>
run_head_to_head(const alpha::Panel &panel, const std::string &signal_expr,
                 const HeadToHeadCfg &cfg = {}) {
  // Frictionless rank/dollar-neutral default policy + sim: the two arms then
  // differ ONLY in the grouping classifier, which is exactly the contest. (A
  // caller wanting costs can score the arms directly via the public metrics.)
  const WeightPolicy policy{};
  const exec::ExecutionSimulator sim{};

  const std::string sector_expr = "group_neutralize(" + signal_expr + ", " + cfg.sector_field + ")";
  const std::string cluster_expr =
      "group_neutralize(" + signal_expr + ", " + cfg.cluster_field + ")";

  HeadToHead out;
  ATX_TRY(out.sector, detail::score_arm(sector_expr, panel, policy, sim, cfg.periods_per_year));
  ATX_TRY(out.cluster, detail::score_arm(cluster_expr, panel, policy, sim, cfg.periods_per_year));
  out.delta = detail::score_delta(out.cluster, out.sector);
  return atx::core::Ok(out);
}

// ===========================================================================
//  Report rendering — the §9 caveat is printed INLINE, not optional
// ===========================================================================

namespace detail {

// One fixed-width metric line "label: sector=.. cluster=.. delta=..".
[[nodiscard]] inline std::string metric_line(const char *label, atx::f64 s, atx::f64 c,
                                             atx::f64 d) {
  return std::string(label) + ": sector=" + std::to_string(s) + " cluster=" + std::to_string(c) +
         " delta=" + std::to_string(d);
}

} // namespace detail

// Render the head-to-head as report lines. The FIRST line is a header and the
// LAST content line is the research §9 caveat (kGicsCaveat) — both are always
// present so a reader cannot strip the warning from the numbers. Returned as a
// vector of lines so the caller can route them to a log, a file, or the
// atx-impl report without this header owning an output sink.
[[nodiscard]] inline std::vector<std::string> render_head_to_head(const HeadToHead &h) {
  std::vector<std::string> lines;
  lines.emplace_back("=== Cluster vs GICS head-to-head (group_neutralize: sector | cluster) ===");
  lines.push_back(detail::metric_line("OOS Sharpe   ", h.sector.sharpe, h.cluster.sharpe,
                                      h.delta.sharpe));
  lines.push_back(detail::metric_line("DSR          ", h.sector.dsr, h.cluster.dsr, h.delta.dsr));
  lines.push_back(detail::metric_line("Max drawdown ", h.sector.max_dd, h.cluster.max_dd,
                                      h.delta.max_dd));
  lines.push_back(detail::metric_line("Turnover     ", h.sector.mean_turnover,
                                      h.cluster.mean_turnover, h.delta.mean_turnover));
  lines.push_back(detail::metric_line("%ADV capacity", h.sector.adv_capacity,
                                      h.cluster.adv_capacity, h.delta.adv_capacity));
  lines.emplace_back(kGicsCaveat);
  return lines;
}

} // namespace atx::engine::eval
