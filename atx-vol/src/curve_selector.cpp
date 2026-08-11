#include "atx/vol/curve_selector.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"    // american_price
#include "atx/vol/arb.hpp"         // arb_check_butterfly_svi_mm, arb_check_butterfly_slice
#include "atx/vol/deamer.hpp"      // resolve_chain_forward
#include "atx/vol/fit_metrics.hpp" // slice_fit_metrics, SliceFitMetrics
#include "atx/vol/fit_policy.hpp"  // FitAdmissionPolicy
#include "atx/vol/detail/prepared_fitting.hpp"
#include "atx/vol/universe.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

std::vector<CurveConfig> default_selector_candidates() {
  std::vector<CurveConfig> candidates;
  CurveConfig convex;
  convex.kind = VolCurveKind::ConvexDense;
  convex.convex.node_cap = 40;
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  CurveConfig linear;
  linear.kind = VolCurveKind::LinearVariance;
  CurveConfig svi;
  svi.kind = VolCurveKind::Svi;
  CurveConfig c8;
  c8.kind = VolCurveKind::C8;
  // Establish a broad-coverage, low-dimensional baseline first. A bounded
  // selector can then return a usable eSSVI surface after one completed fit;
  // the remaining families must demonstrate a real common-population gain.
  candidates.push_back(essvi);
  candidates.push_back(linear);
  candidates.push_back(convex);
  candidates.push_back(svi);
  candidates.push_back(c8);
  return candidates;
}

std::vector<CurveConfig> bounded_selector_candidates() {
  std::vector<CurveConfig> candidates;
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  CurveConfig svi;
  svi.kind = VolCurveKind::Svi;
  CurveConfig linear;
  linear.kind = VolCurveKind::LinearVariance;
  // eSSVI's broad-coverage baseline first (so a budgeted caller that stops after
  // one candidate still gets the safe family), then the two other cheap
  // parametric forms. ConvexDense and C8 are deliberately absent: the dense
  // per-expiry QP and the eight-parameter fit are what make
  // `default_selector_candidates()` a research ladder, and excluding them bounds
  // the cost DETERMINISTICALLY -- a wall-clock `time_budget_ms` would make the
  // chosen family depend on host load.
  candidates.push_back(essvi);
  candidates.push_back(svi);
  candidates.push_back(linear);
  return candidates;
}

SelectorConfig production_selector_config() {
  SelectorConfig config;
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  config.candidates.push_back(essvi);
  return config;
}

namespace detail {

// Per-kind butterfly violation count for a fitted slice (the selection-time
// mapping): closed-form Martini-Mingone for raw-SVI, grid Durrleman g-check for
// C8, the fitted post-fit Lee/Roper diagnostic count carried on the params for
// SplineVol (see spline_curve.hpp's file-top comment, step 6 -- NOT projected,
// just reported), and 0 for the by-construction / out-of-scope kinds
// (ConvexDense, eSSVI, LinearVariance). `k_lo`/`k_hi` bound the C8 grid (padded
// by the caller).
[[nodiscard]] std::uint32_t slice_butterfly_violations(const IVolCurve &cv, double T, double k_lo,
                                                       double k_hi) noexcept {
  switch (cv.kind()) {
  case VolCurveKind::Svi: {
    const auto &sp = static_cast<const SviCurve &>(cv).slice();
    std::uint32_t nv = arb_check_butterfly_svi_mm(sp, T).n_violations;
    // FT-C2: the 5-condition Martini-Mingone tally is necessary-only and blind to
    // wing butterfly arb the closed form extrapolates past the tradeable band.
    // Grid-scan the served w(k) over the padded quoted range too (same policy and
    // grid as the C8 branch and the fit_slice_curve SVI serving gate).
    const auto bf =
        arb_check_butterfly_slice([&cv](double kk) { return cv.w(kk); }, T, k_lo, k_hi, 64u);
    if (bf.has_value()) {
      nv += static_cast<std::uint32_t>(bf->size());
    }
    return nv;
  }
  case VolCurveKind::C8: {
    const auto bf =
        arb_check_butterfly_slice([&cv](double kk) { return cv.w(kk); }, T, k_lo, k_hi, 64u);
    return bf.has_value() ? static_cast<std::uint32_t>(bf->size()) : 0u;
  }
  case VolCurveKind::ConvexDense:
  case VolCurveKind::Essvi:
  case VolCurveKind::LinearVariance:
    return 0u; // by-construction arb-free (LinearVariance g-check out of scope)
  case VolCurveKind::SplineVol: {
    const auto &sv = static_cast<const SplineVolCurve &>(cv);
    const std::size_t n = sv.params().n_butterfly_viol;
    return n > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(n);
  }
  }
  return 0u;
}

} // namespace detail

namespace {

// Running out-of-sample accumulator for one candidate across expiries.
struct Accum {
  std::size_t n{0u};
  std::size_t in_band{0u};
  double win{0.0};
  std::size_t dof_sum{0u};
  std::size_t n_slices{0u};
  // Task C2.5 — held-out fit-metric collection + butterfly disqualification.
  std::vector<double> iv_model, iv_mkt, bid, ask, vega;
  std::uint32_t n_butterfly_viol{0u};
  bool disqualified{false};
  // T10b (D5): fit-diagnostic census, reported not ranked (CandidateScore).
  std::uint32_t n_slices_converged{0u};
  std::uint32_t n_slices_termination_unknown{0u};
  std::uint32_t n_slices_projection_moved{0u};
  std::uint32_t n_slices_reverted_to_seed{0u};

  void reserve(std::size_t count) {
    iv_model.reserve(count);
    iv_mkt.reserve(count);
    bid.reserve(count);
    ask.reserve(count);
    vega.reserve(count);
  }

  void reset() noexcept {
    n = 0u;
    in_band = 0u;
    win = 0.0;
    dof_sum = 0u;
    n_slices = 0u;
    iv_model.clear();
    iv_mkt.clear();
    bid.clear();
    ask.clear();
    vega.clear();
    n_butterfly_viol = 0u;
    disqualified = false;
    n_slices_converged = 0u;
    n_slices_termination_unknown = 0u;
    n_slices_projection_moved = 0u;
    n_slices_reverted_to_seed = 0u;
  }
};

struct PreparedExpiry {
  std::size_t chain_index{0u};
  double rate{0.0};
  double q_eff{0.0};
  double df{0.0};
  PreparedSlice slice{};
  std::vector<FitObs> fit_rows{};
  std::vector<std::size_t> holdout_rows{};
  std::size_t n_required_holdout{0u};
  double required_vega_weight{0.0};
  double fit_k_lo{0.0};
  double fit_k_hi{0.0};
};

[[nodiscard]] bool valid_expiry_rates(const SurfaceParityInputs &in,
                                      const Underlying &under) noexcept {
  if (in.expiry_rates.empty() && in.expiry_rate_T.empty()) {
    return true;
  }
  if (in.expiry_rates.size() != under.chains.size() ||
      in.expiry_rate_T.size() != under.chains.size()) {
    return false;
  }
  for (std::size_t i = 0; i < under.chains.size(); ++i) {
    if (!std::isfinite(in.expiry_rates[i]) || !std::isfinite(in.expiry_rate_T[i]) ||
        !(in.expiry_rate_T[i] > 0.0) || in.expiry_rate_T[i] != under.chains[i].T) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] double expiry_rate(const SurfaceParityInputs &in, std::size_t index) noexcept {
  return in.expiry_rates.empty() ? in.r : in.expiry_rates[index];
}

// Split the maturity axis into equal-count tenor strata, then take the most
// liquid chain (strike count) from each stratum. Ties prefer the longer tenor,
// which prevents an ascending-prefix bias while remaining fully deterministic.
[[nodiscard]] std::vector<std::size_t> sample_expiry_indices(const Underlying &under,
                                                             unsigned max_expiries) {
  std::vector<std::size_t> eligible;
  eligible.reserve(under.chains.size());
  for (std::size_t i = 0; i < under.chains.size(); ++i) {
    const double T = under.chains[i].T;
    if (std::isfinite(T) && T > 0.019) {
      eligible.push_back(i);
    }
  }
  std::stable_sort(eligible.begin(), eligible.end(), [&](std::size_t left, std::size_t right) {
    return under.chains[left].T < under.chains[right].T;
  });
  if (max_expiries == 0u || eligible.size() <= max_expiries) {
    return eligible;
  }

  const std::size_t count = static_cast<std::size_t>(max_expiries);
  std::vector<std::size_t> sampled;
  sampled.reserve(count);
  for (std::size_t stratum = 0u; stratum < count; ++stratum) {
    const std::size_t begin = stratum * eligible.size() / count;
    const std::size_t end = (stratum + 1u) * eligible.size() / count;
    std::size_t best = eligible[begin];
    for (std::size_t p = begin + 1u; p < end; ++p) {
      const std::size_t candidate = eligible[p];
      const std::size_t candidate_liquidity = under.chains[candidate].n_strikes();
      const std::size_t best_liquidity = under.chains[best].n_strikes();
      if (candidate_liquidity > best_liquidity ||
          (candidate_liquidity == best_liquidity &&
           under.chains[candidate].T > under.chains[best].T)) {
        best = candidate;
      }
    }
    sampled.push_back(best);
  }
  std::sort(sampled.begin(), sampled.end(), [&](std::size_t left, std::size_t right) {
    return under.chains[left].T < under.chains[right].T;
  });
  return sampled;
}

[[nodiscard]] std::optional<PreparedExpiry>
prepare_expiry(const Underlying &under, const SurfaceParityInputs &in, std::size_t chain_index) {
  const Chain &chain = under.chains[chain_index];
  const double rate = expiry_rate(in, chain_index);
  const auto resolved =
      resolve_chain_forward(chain, in.S, rate, in.cash_divs, in.now_ts_ns, in.deam);
  if (!resolved || !(resolved->forward > 0.0) || !std::isfinite(resolved->forward)) {
    return std::nullopt;
  }
  const double q_eff = rate - std::log(resolved->forward / in.S) / chain.T;
  const double df = std::exp(-rate * chain.T);

  PreparedSliceInputs inputs;
  inputs.expiry_index = static_cast<std::uint32_t>(chain_index);
  inputs.S = in.S;
  inputs.r = rate;
  inputs.F = resolved->forward;
  inputs.q_eff = q_eff;
  inputs.df = df;
  inputs.calib = in.calib;
  inputs.caches = in.use_deam_cache_for_fit ? in.deam.caches : AmericanCorrectionCaches{};
  inputs.al_opts = in.deam.al_opts;
  inputs.iv_tolerance = in.deam.iv_tol;
  inputs.iv_max_iterations = in.deam.iv_max_iter;
  inputs.method = in.deam.method;
  // R-35: the selector INTENTIONALLY scores every candidate under Configured
  // preparation — NOT `in.fit_prep_policy` — so the family comparison uses one
  // common, audited row population (cross-candidate comparability, per the
  // invariant #4.9 note in prepared_fitting.hpp). `in.fit_prep_policy` governs
  // only the SERVED build in fit_curve_surface; consulting it here would let the
  // permissive Legacy predicate feed different populations to different
  // candidates. Hardcoded on purpose, not an oversight.
  //
  // W1-A re-examined this and KEPT it, with the cost now written down. Measured
  // (lqbench 2026-08-03/05, both snapshot minutes): every single `select_curve`
  // refusal on real OPRA data is `sampled=8 prepared=0` -- all eight sampled
  // expiries fail below, none of them at the holdout split. The same boards fit
  // cleanly with `--pin essvi` (served under the permissive
  // LegacyEssviCompatibility prep) and fail with `--pin svi` /
  // `--pin linear-variance` under this same strict cascade
  // ("no expiry produced a usable slice"). So the selector is scoring a
  // materially stricter population than the eSSVI route it is selecting FOR,
  // and on a wide-spread board that costs it every expiry.
  //
  // Scoring each candidate under the policy it would actually be served with is
  // the honest fix, but it is a preparation-policy decision (F02/W2-B, where
  // prep strictness stops riding on family choice) and it would trade this
  // failure for a silent one: candidates ranked on different row populations.
  // Until prep policy is an explicit input, the comparability pin stays and
  // PricerFitter treats the refusal as ADVISORY -- a refusal here can no longer
  // drop the board (see `FitDecision::selector_fallback`).
  inputs.policy = PreparedObservationPolicy::Configured;
  inputs.prepare_scoring = true;
  Result<PreparedSlice> prepared = PreparedSlice::create(chain, inputs);
  if (!prepared || prepared->fit_observations().size() < 8u) {
    return std::nullopt;
  }

  PreparedExpiry out;
  out.chain_index = chain_index;
  out.rate = rate;
  out.q_eff = q_eff;
  out.df = df;
  out.slice = std::move(*prepared);
  const std::span<const FitObs> rows = out.slice.fit_observations();
  std::vector<std::size_t> strike_order(rows.size());
  std::iota(strike_order.begin(), strike_order.end(), std::size_t{0u});
  std::stable_sort(
      strike_order.begin(), strike_order.end(),
      [rows](std::size_t left, std::size_t right) { return rows[left].K < rows[right].K; });
  out.fit_rows.reserve((rows.size() + 1u) / 2u);
  out.holdout_rows.reserve(rows.size() / 2u);
  const PreparedScoreColumns &score = out.slice.score_columns();
  for (std::size_t p = 0u; p < strike_order.size(); ++p) {
    const std::size_t row_index = strike_order[p];
    if ((p % 2u) == 0u) {
      out.fit_rows.push_back(rows[row_index]);
      continue;
    }
    if (!(score.bid[row_index] > 0.0) || !(score.ask[row_index] > score.bid[row_index])) {
      continue;
    }
    out.holdout_rows.push_back(row_index);
    ++out.n_required_holdout;
    const double vega = rows[row_index].vega;
    out.required_vega_weight += vega > 0.0 ? vega * vega : 0.0;
  }
  if (!out.fit_rows.empty()) {
    out.fit_k_lo = out.fit_rows.front().k;
    out.fit_k_hi = out.fit_rows.front().k;
    for (const FitObs &row : out.fit_rows) {
      out.fit_k_lo = std::min(out.fit_k_lo, row.k);
      out.fit_k_hi = std::max(out.fit_k_hi, row.k);
    }
  }
  return out;
}

[[nodiscard]] double average_dof(const CandidateScore &score) noexcept {
  return score.n_slices > 0u
             ? static_cast<double>(score.dof_sum) / static_cast<double>(score.n_slices)
             : 1.0e18;
}

// Distance of the reduced chi-square from the ideal 1.0; candidates without a
// valid reduced chi-square do not compete on this axis (treated as +inf).
[[nodiscard]] double chi2_distance(const CandidateScore &score) noexcept {
  return score.metrics_valid ? std::fabs(score.chi2_reduced - 1.0)
                             : std::numeric_limits<double>::infinity();
}

constexpr double kChi2Tol = 1.0e-9; // fall through to DoF when chi2 ~equal

} // namespace

namespace detail {

FitAdmissionPolicy selector_served_admission_policy(const FitAdmissionPolicy &base,
                                                    const SelectorConfig &selector) noexcept {
  FitAdmissionPolicy effective = base;
  effective.min_quote_coverage =
      std::max(effective.min_quote_coverage, selector.min_served_quote_coverage);
  return effective;
}

} // namespace detail

Result<std::size_t> select_candidate_index(std::span<const CandidateScore> scores,
                                           double parsimony_margin) {
  if (scores.empty() || !std::isfinite(parsimony_margin) || parsimony_margin < 0.0) {
    return Err(ErrorCode::InvalidArgument, "select_candidate_index: invalid score set or margin");
  }
  double max_expiry_coverage = -1.0;
  double max_holdout_coverage = -1.0;
  double quality_leader = -1.0;
  for (std::size_t i = 0u; i < scores.size(); ++i) {
    const CandidateScore &candidate = scores[i];
    if (!candidate.admitted || candidate.disqualified ||
        !std::isfinite(candidate.expiry_coverage) || !std::isfinite(candidate.holdout_coverage) ||
        !std::isfinite(candidate.oos_vw) || candidate.expiry_coverage < 0.0 ||
        candidate.expiry_coverage > 1.0 || candidate.holdout_coverage < 0.0 ||
        candidate.holdout_coverage > 1.0 || candidate.oos_vw < 0.0 || candidate.oos_vw > 1.0) {
      continue;
    }
    if (candidate.expiry_coverage > max_expiry_coverage) {
      max_expiry_coverage = candidate.expiry_coverage;
      max_holdout_coverage = candidate.holdout_coverage;
      quality_leader = candidate.oos_vw;
    } else if (candidate.expiry_coverage == max_expiry_coverage) {
      if (candidate.holdout_coverage > max_holdout_coverage) {
        max_holdout_coverage = candidate.holdout_coverage;
        quality_leader = candidate.oos_vw;
      } else if (candidate.holdout_coverage == max_holdout_coverage) {
        quality_leader = std::max(quality_leader, candidate.oos_vw);
      }
    }
  }
  std::optional<std::size_t> best;
  for (std::size_t i = 0u; i < scores.size(); ++i) {
    const CandidateScore &candidate = scores[i];
    if (!candidate.admitted || candidate.disqualified || !std::isfinite(candidate.oos_vw) ||
        candidate.expiry_coverage != max_expiry_coverage ||
        candidate.holdout_coverage != max_holdout_coverage ||
        candidate.oos_vw < quality_leader - parsimony_margin) {
      continue;
    }
    if (!best.has_value()) {
      best = i;
      continue;
    }
    // Task C2.5 tie-break inside the parsimony band: reduced chi-square closest
    // to 1 first, then fewer average DoF, then higher oos_vw.
    const CandidateScore &incumbent = scores[*best];
    const double dc = chi2_distance(candidate);
    const double di = chi2_distance(incumbent);
    if (dc < di - kChi2Tol) {
      best = i;
      continue;
    }
    if (dc > di + kChi2Tol) {
      continue;
    }
    if (average_dof(candidate) < average_dof(incumbent) ||
        (average_dof(candidate) == average_dof(incumbent) && candidate.oos_vw > incumbent.oos_vw)) {
      best = i;
    }
  }
  if (!best.has_value()) {
    return Err(ErrorCode::NotFound, "select_candidate_index: no candidate met admission");
  }
  return Ok(*best);
}

std::size_t select_best_candidate(const std::vector<CandidateScore> &scores,
                                  double parsimony_margin) noexcept {
  double best_vw = -1.0;
  for (const CandidateScore &s : scores) {
    if (s.n_holdout > 0 && !s.disqualified) {
      best_vw = std::max(best_vw, s.oos_vw);
    }
  }
  if (best_vw < 0.0) {
    return 0; // caller gates on scorability; nothing selectable
  }

  std::size_t best_i = 0;
  bool have = false;
  for (std::size_t i = 0; i < scores.size(); ++i) {
    const CandidateScore &s = scores[i];
    if (s.disqualified || s.n_holdout == 0 || s.oos_vw < best_vw - parsimony_margin) {
      continue; // failed / disqualified / outside the tie band of the leader
    }
    if (!have) {
      best_i = i;
      have = true;
      continue;
    }
    const CandidateScore &b = scores[best_i];
    const double ds = chi2_distance(s);
    const double db = chi2_distance(b);
    if (ds < db - kChi2Tol) {
      best_i = i;
      continue;
    }
    if (ds > db + kChi2Tol) {
      continue;
    }
    const double as = average_dof(s);
    const double ab = average_dof(b);
    if (as < ab) {
      best_i = i;
      continue;
    }
    if (as > ab) {
      continue;
    }
    if (s.oos_vw > b.oos_vw) {
      best_i = i;
    }
  }
  return best_i;
}

Result<SelectorResult> select_curve(const Underlying &under, const SurfaceParityInputs &in,
                                    const SelectorConfig &sel) {
  const auto selector_start = std::chrono::steady_clock::now();
  if (!(in.S > 0.0) || !std::isfinite(in.r) || !valid_expiry_rates(in, under)) {
    return Err(ErrorCode::InvalidArgument, "select_curve: invalid spot, rate, or term rates");
  }
  const auto valid_fraction = [](double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
  };
  if (!valid_fraction(sel.min_expiry_coverage) || !valid_fraction(sel.min_holdout_coverage) ||
      !valid_fraction(sel.relaxed_min_expiry_coverage) ||
      !valid_fraction(sel.relaxed_min_holdout_coverage) ||
      !valid_fraction(sel.min_served_quote_coverage) || !std::isfinite(sel.time_budget_ms) ||
      sel.time_budget_ms < 0.0) {
    return Err(ErrorCode::InvalidArgument, "select_curve: invalid coverage floor");
  }
  std::vector<CurveConfig> candidates =
      sel.candidates.empty() ? default_selector_candidates() : sel.candidates;
  if (candidates.empty()) {
    return Err(ErrorCode::InvalidArgument, "select_curve: no candidates");
  }

  // Gated SplineVol candidacy (Task I5): append a SplineVol candidate to the
  // working ladder when ANY supplied candidate opts in via
  // `CurveConfig::spline_candidate`. `default_selector_candidates()` itself
  // never sets the flag, so a caller passing an empty (default) candidate
  // list is completely unaffected -- bit-identical selection to pre-task.
  // The appended candidate's `SplineFitOpts` are copied from the FIRST
  // candidate that set the flag (not left default-constructed), so a
  // caller's tuned spline knobs (grid/lambda/mult_floor/min_obs) actually
  // reach the fit it asked for.
  {
    bool want_spline = false;
    bool has_spline = false;
    const CurveConfig *spline_source = nullptr;
    for (const CurveConfig &c : candidates) {
      if (c.spline_candidate && spline_source == nullptr) {
        spline_source = &c;
      }
      want_spline = want_spline || c.spline_candidate;
      has_spline = has_spline || (c.kind == VolCurveKind::SplineVol);
    }
    if (want_spline && !has_spline) {
      CurveConfig spline_cfg;
      spline_cfg.kind = VolCurveKind::SplineVol;
      if (spline_source != nullptr) {
        spline_cfg.spline = spline_source->spline;
      }
      candidates.push_back(spline_cfg);
    }
  }

  SelectorResult out;
  out.sampled_expiry_indices = sample_expiry_indices(under, sel.oos_max_expiries);
  std::vector<PreparedExpiry> prepared;
  prepared.reserve(out.sampled_expiry_indices.size());
  std::size_t required_holdout = 0u;
  double required_vega_weight = 0.0;
  for (const std::size_t chain_index : out.sampled_expiry_indices) {
    std::optional<PreparedExpiry> expiry = prepare_expiry(under, in, chain_index);
    if (!expiry.has_value()) {
      continue;
    }
    required_holdout += expiry->n_required_holdout;
    required_vega_weight += expiry->required_vega_weight;
    prepared.push_back(std::move(*expiry));
  }
  if (prepared.empty() || required_holdout == 0u) {
    // P1 (typed refusal names the board condition): "no expiry prepared" and
    // "prepared expiries carry no two-sided holdout quote" are different board
    // defects with different remedies, and the single opaque message could not
    // tell them apart in the field. Carry the counts.
    std::string detail = "select_curve: no common prepared holdout keys (sampled=";
    detail += std::to_string(out.sampled_expiry_indices.size());
    detail += " prepared=";
    detail += std::to_string(prepared.size());
    detail += " holdout_keys=";
    detail += std::to_string(required_holdout);
    detail += ")";
    return Err(ErrorCode::NotFound, std::move(detail));
  }

  out.scores.reserve(candidates.size());
  bool has_admitted_candidate = false;
  Accum accum;
  accum.reserve(required_holdout);
  for (const CurveConfig &candidate : candidates) {
    if (sel.time_budget_ms > 0.0 && !out.scores.empty()) {
      const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - selector_start)
                                    .count();
      if (elapsed_ms >= sel.time_budget_ms) {
        out.budget_exhausted = true;
        break;
      }
    }
    accum.reset();
    for (const PreparedExpiry &expiry : prepared) {
      const std::span<const FitObs> rows = expiry.slice.fit_observations();
      // T10b (D5): the selector fits every family on every sampled expiry and
      // used to discard everything the fit said about ITSELF, keeping only how
      // well the result repriced. Collect the fit's own verdict so a family that
      // hit its iteration cap, was repaired by the admissibility projection, or
      // fell back to its seed is distinguishable from one that converged.
      FitDiag cand_diag{};
      Result<std::unique_ptr<IVolCurve>> fitted =
          fit_slice_curve(candidate, expiry.fit_rows, expiry.slice.forward(),
                          expiry.slice.maturity(), expiry.df, {}, {},
                          {-std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::infinity()},
                          &cand_diag);
      if (!fitted) {
        continue;
      }
      const IVolCurve &curve = **fitted;
      accum.dof_sum += curve.dof();
      ++accum.n_slices;
      // Counted only for a slice that FIT — a failed fit is already counted by
      // expiry_coverage, and folding it in here would let "did not run" and
      // "ran without reporting" share a bucket.
      switch (cand_diag.termination) {
      case FitTermination::Converged:
        ++accum.n_slices_converged;
        break;
      case FitTermination::Unknown:
        ++accum.n_slices_termination_unknown;
        break;
      case FitTermination::IterationCap:
      case FitTermination::Stalled:
        break;
      }
      if (cand_diag.projection == SliceProjection::Moved) {
        ++accum.n_slices_projection_moved;
      }
      if (cand_diag.reverted_to_seed) {
        ++accum.n_slices_reverted_to_seed;
      }

      // Butterfly disqualification (Task C2.5 fit-metrics selection signal): a
      // family with any butterfly-violating fitted slice scores as a
      // fit-failure. Grid bounds for the C8 check are the fitted strikes padded
      // by 0.5 in log-moneyness (matches the fit_slice_curve gate).
      const std::uint32_t nv = detail::slice_butterfly_violations(
          curve, expiry.slice.maturity(), expiry.fit_k_lo - 0.5, expiry.fit_k_hi + 0.5);
      accum.n_butterfly_viol += nv;
      if (nv > 0u) {
        accum.disqualified = true;
      }

      const PreparedScoreColumns &score = expiry.slice.score_columns();
      for (const std::size_t row_index : expiry.holdout_rows) {
        const double bid = score.bid[row_index];
        const double ask = score.ask[row_index];
        const FitObs &observation = rows[row_index];
        const double model_iv = curve.iv(observation.k);
        if (!std::isfinite(model_iv)) {
          continue;
        }
        const auto fair_value =
            american_price(in.S, observation.K, expiry.slice.maturity(), model_iv, expiry.rate,
                           expiry.q_eff, observation.side, in.deam.method, in.deam.al_opts);
        if (!fair_value) {
          continue;
        }
        const bool in_band = *fair_value >= bid && *fair_value <= ask;
        ++accum.n;
        accum.in_band += in_band ? 1u : 0u;
        const double weight = observation.vega > 0.0 ? observation.vega * observation.vega : 0.0;
        if (in_band) {
          accum.win += weight;
        }
        // Held-out fit metrics (vol space, self-consistent European bid/ask/vega
        // from the de-Americanized obs). Feeds slice_fit_metrics -> chi2_reduced.
        // Market IV is the anchor-independent European scoring vol (score column),
        // not `observation.sigma_mkt`, which carries the fit anchor's bias off Mid.
        const double iv_mkt = score.market_iv[row_index];
        const double eu_half = 0.5 * observation.spread;
        const double eu_bid = observation.mid - eu_half;
        const double eu_ask = observation.mid + eu_half;
        if (std::isfinite(iv_mkt) && iv_mkt > 0.0 && observation.vega > 0.0 && eu_bid > 0.0 &&
            eu_ask > eu_bid) {
          accum.iv_model.push_back(model_iv);
          accum.iv_mkt.push_back(iv_mkt);
          accum.bid.push_back(eu_bid);
          accum.ask.push_back(eu_ask);
          accum.vega.push_back(observation.vega);
        }
      }
    }

    CandidateScore score;
    score.kind = candidate.kind;
    score.dof_sum = accum.dof_sum;
    score.n_slices = accum.n_slices;
    score.n_required_slices = prepared.size();
    score.n_holdout = required_holdout;
    score.n_successful_holdout = accum.n;
    score.n_required_holdout = required_holdout;
    score.n_in_band = accum.in_band;
    // The in-band vega weight is a subset sum of the total holdout vega weight
    // (the in-band rows are a subset of all scored holdout rows), so
    // vega_weight_in_band <= vega_weight_total holds by construction. When every
    // holdout row is in-band the two are mathematically equal but accumulated in
    // different groupings (per-expiry `required_vega_weight` vs. the running
    // `accum.win`), so floating-point round-off can leave `accum.win` a few ULP
    // above the total. Clamp to the definitional bound so the invariant survives:
    // both the selector admission guard (oos_vw in [0,1], curve_selector.cpp
    // select_candidate_index) and the quality-report round-trip
    // (oos_vega_weight_in_band <= oos_vega_weight_total, corpus.cpp
    // quality_evidence_consistent) reject a candidate whose weight overshoots.
    const double vega_weight_in_band = std::min(accum.win, required_vega_weight);
    score.vega_weight_in_band = vega_weight_in_band;
    score.vega_weight_total = required_vega_weight;
    score.oos_in_band = static_cast<double>(accum.in_band) / static_cast<double>(required_holdout);
    score.oos_vw = required_vega_weight > 0.0 ? vega_weight_in_band / required_vega_weight : 0.0;
    score.expiry_coverage =
        static_cast<double>(accum.n_slices) / static_cast<double>(score.n_required_slices);
    score.holdout_coverage =
        static_cast<double>(accum.n) / static_cast<double>(score.n_required_holdout);
    score.admitted = accum.n > 0u && score.expiry_coverage >= sel.min_expiry_coverage &&
                     score.holdout_coverage >= sel.min_holdout_coverage;
    score.n_butterfly_viol = accum.n_butterfly_viol;
    score.disqualified = accum.disqualified;
    score.n_slices_converged = accum.n_slices_converged;
    score.n_slices_termination_unknown = accum.n_slices_termination_unknown;
    score.n_slices_projection_moved = accum.n_slices_projection_moved;
    score.n_slices_reverted_to_seed = accum.n_slices_reverted_to_seed;
    // Reduced chi-square (and companion metrics) over the held-out sample. Needs
    // N > dof for a positive denominator; otherwise the metrics stay invalid and
    // chi2_reduced does not participate in the tie-break.
    if (accum.iv_model.size() > accum.dof_sum) {
      const auto m_fit = slice_fit_metrics(accum.iv_model, accum.iv_mkt, accum.bid, accum.ask,
                                           accum.vega, accum.dof_sum);
      if (m_fit.has_value()) {
        score.chi2_reduced = m_fit->chi2_reduced;
        score.rmse_vol = m_fit->rmse_vol;
        score.avE5_vol = m_fit->avE5_vol;
        score.n_within_band = m_fit->n_within_band;
        score.metrics_valid = true;
      }
    }
    out.scores.push_back(score);
    ++out.scores_evaluated;
    has_admitted_candidate = has_admitted_candidate || (score.admitted && !score.disqualified);
  }

  // F05 second stage. The strict floors are a CROSS-CANDIDATE comparability
  // guarantee (see SelectorConfig), and comparability is worth nothing once the
  // alternative is refusing the board. When nobody cleared them, re-admit at
  // the relaxed floors: `select_candidate_index` ranks expiry coverage, then
  // holdout coverage, before any quality term, so the widest-covering family
  // still wins and a candidate that scored strictly more of the board can never
  // lose to one that scored less.
  if (!has_admitted_candidate) {
    for (CandidateScore &score : out.scores) {
      score.admitted = score.n_successful_holdout > 0u &&
                       score.expiry_coverage >= sel.relaxed_min_expiry_coverage &&
                       score.holdout_coverage >= sel.relaxed_min_holdout_coverage;
      has_admitted_candidate = has_admitted_candidate || (score.admitted && !score.disqualified);
    }
    out.relaxed_coverage_admission = has_admitted_candidate;
  }

  if (out.budget_exhausted && !has_admitted_candidate) {
    return Err(ErrorCode::Unavailable,
               "select_curve: time budget expired before an admitted candidate");
  }

  ATX_TRY(const std::size_t chosen, select_candidate_index(out.scores, sel.parsimony_margin));
  out.chosen_index = chosen;
  out.chosen = candidates[chosen];
  return Ok(std::move(out));
}

Result<CandidateScore> score_curve_oos(const Underlying &under, const SurfaceParityInputs &in,
                                       const CurveConfig &curve, const SelectorConfig &scoring) {
  SelectorConfig one = scoring;
  one.candidates = {curve};
  ATX_TRY(SelectorResult selected, select_curve(under, in, one));
  if (selected.scores.size() != 1u) {
    return Err(ErrorCode::Internal, "score_curve_oos: one-family score unavailable");
  }
  return Ok(selected.scores.front());
}

} // namespace atx::vol
