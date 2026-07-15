#include "atx/vol/deamer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/arb.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/dividend.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Accuracy of the inner American inversions used by the borrow fixed-point.
// Comfortably inside the borrow's 1e-4 target while staying at/above the fast
// ALO preset's ~1e-4 price-accuracy floor — a tighter tol than the pricer can
// resolve only burns Newton/bisection iterations (each a full American solve)
// without moving the borrow, so this is capped deliberately.
constexpr double kInnerIvTol = 1.0e-6;
constexpr std::uint16_t kInnerIvMaxIter = std::uint16_t{48};

// Borrow fixed-point controls. Borrow itself is only meaningful to ~1e-4, but
// the reported PCP residual (rmse_pcp) scales with |Δborrow| at convergence, and
// the default-accuracy contract holds it below 1e-6 (deamer_test). 1e-8 clears
// that with ~50x margin while, thanks to the per-leg Newton warm starts, adding
// only a couple of fixed-point iterations over the loose 1e-6 setting.
constexpr double kBorrowFpTol = 1.0e-8;  // |Δborrow| convergence on the map
constexpr int kBorrowMaxIter = 64;       // bounded-loop guard (JPL Rule 2)

// PCP-solver bracket for the inner European borrow solve. Wide enough for any
// realistic hard-to-borrow name; a root outside it surfaces as OutOfRange.
constexpr double kBorrowLo = -0.5;
constexpr double kBorrowHi = 0.5;
constexpr double kPcpTol = 1.0e-10;

// Floor on the bid/ask spread used in the weight hint, so a locked (bid == ask)
// but otherwise valid quote does not divide by zero.
constexpr double kMinSpread = 1.0e-8;

}  // namespace

// ── Single-quote European-equivalent IV ─────────────────────────────────

Result<double> european_equiv_iv(double american_mid, double S, double K,
                                 double T, double r, double q_eff, Side side,
                                 AmericanMethod method,
                                 const std::optional<AlOpts>& opts,
                                 const CorrectionCache* correction, double tol,
                                 std::uint16_t max_iter) noexcept {
  // The recovered lognormal sigma IS the European-equivalent vol. `tol`/`max_iter`
  // default to the american_implied_vol defaults (1e-7 / 64) so a caller with the
  // defaults gets a bit-identical result. `correction` (when it matches `side`)
  // routes the inversion through the cached hot path.
  return american_implied_vol(american_mid, S, K, T, r, q_eff, side, method, tol,
                              max_iter, opts, correction);
}

Result<IvRepricingAudit> audit_european_equiv_iv(
    double american_mid, double bid_ask_spread, double sigma, double S,
    double K, double T, double r, double q_eff, Side side,
    double max_residual_half_spreads) noexcept {
  if (!(american_mid > 0.0) || !(bid_ask_spread > 0.0) || !(sigma > 0.0) ||
      !(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !std::isfinite(r) ||
      !std::isfinite(q_eff) || !(max_residual_half_spreads >= 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "audit_european_equiv_iv: invalid price/model/budget input");
  }
  ATX_TRY(const double price,
          american_price(S, K, T, sigma, r, q_eff, side,
                         AmericanMethod::AndersenLake, std::nullopt));
  if (!std::isfinite(price)) {
    return Err(ErrorCode::Internal,
               "audit_european_equiv_iv: accurate pricer returned non-finite price");
  }
  const double residual = std::fabs(price - american_mid);
  const double normalized = residual / (0.5 * bid_ask_spread);
  return Ok(IvRepricingAudit{price, residual, normalized,
                             normalized <= max_residual_half_spreads});
}

namespace {

// One step of the borrow fixed-point: de-Americanize both legs at the forward
// implied by `borrow`, reprice European, and re-solve the European PCP borrow.
struct DeAmStep {
  double borrow_next = 0.0;
  double forward = 0.0;
  double call_eu = 0.0;
  double put_eu = 0.0;
  double sigma_c = 0.0;  // recovered call vol at this iterate (cross-pair warm seed)
  double sigma_p = 0.0;  // recovered put vol at this iterate (cross-pair warm seed)
};

[[nodiscard]] Result<DeAmStep>
deam_pcp_step(double borrow, double call_mid, double put_mid, double S, double K,
              double T, double r, std::span<const DividendEvent> cash_divs,
              std::int64_t expiry_ns, std::int64_t now_ts_ns,
              const HybridDivParams& hyb, AmericanMethod method,
              const std::optional<AlOpts>& opts,
              const AmericanCorrectionCaches& caches, double& warm_c,
              double& warm_p) noexcept {
  const double F =
      hybrid_forward(S, r, borrow, T, cash_divs, expiry_ns, now_ts_ns, hyb);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::Internal,
               "imply_term_borrow: non-positive or non-finite forward");
  }

  // q_eff bridge: S·e^{(r−q_eff)T} == F exactly (see header).
  const double q_eff = r - std::log(F / S) / T;

  // Warm-start each leg's inversion from the previous fixed-point iterate: as the
  // borrow (hence q_eff) converges the recovered vols barely move, so Newton
  // lands in ~1 step. The result is identical to a cold seed — only iterations,
  // each a full American solve, are saved.
  ATX_TRY(const double sigma_c,
          american_implied_vol(call_mid, S, K, T, r, q_eff, Side::Call, method,
                               kInnerIvTol, kInnerIvMaxIter, opts,
                               caches.for_side(Side::Call), warm_c));
  ATX_TRY(const double sigma_p,
          american_implied_vol(put_mid, S, K, T, r, q_eff, Side::Put, method,
                               kInnerIvTol, kInnerIvMaxIter, opts,
                               caches.for_side(Side::Put), warm_p));
  warm_c = sigma_c;
  warm_p = sigma_p;

  // De-Americanized (European-equivalent) premiums on the term forward.
  const double df = std::exp(-r * T);
  const double call_eu = black76_price(F, K, T, sigma_c, df, Side::Call);
  const double put_eu = black76_price(F, K, T, sigma_p, df, Side::Put);

  ATX_TRY(const double b_next,
          imply_borrow_european_pcp(call_eu, put_eu, S, K, T, r, cash_divs,
                                    expiry_ns, now_ts_ns, hyb, kBorrowLo,
                                    kBorrowHi, kPcpTol));
  return Ok(DeAmStep{b_next, F, call_eu, put_eu, sigma_c, sigma_p});
}

}  // namespace

// ── Per-term borrow ─────────────────────────────────────────────────────

Result<TermBorrow> imply_term_borrow(double call_mid, double put_mid, double S,
                                     double K, double T, double r,
                                     std::span<const DividendEvent> cash_divs,
                                     std::int64_t expiry_ns,
                                     std::int64_t now_ts_ns,
                                     const HybridDivParams& hyb,
                                     AmericanMethod method,
                                     const std::optional<AlOpts>& opts,
                                     const AmericanCorrectionCaches& caches,
                                     double borrow_seed, double sigma_c_seed,
                                     double sigma_p_seed,
                                     bool skip_redundant_final) noexcept {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !std::isfinite(r) ||
      !(call_mid > 0.0) || !(put_mid > 0.0) || !std::isfinite(call_mid) ||
      !std::isfinite(put_mid)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_term_borrow: non-finite or non-positive input");
  }

  // Fixed point borrow -> F -> q_eff -> vols -> European mids -> borrow. The
  // injected-borrow round-trip is an exact fixed point; the map is contractive
  // near it, so a handful of iterations converge for any sane co-terminal pair.
  double borrow = borrow_seed;
  double warm_c = sigma_c_seed;  // per-leg Newton warm starts, persisted across iterations
  double warm_p = sigma_p_seed;
  bool converged = false;
  DeAmStep last_step{};  // loop's final evaluation, reused by the fast path
  for (int it = 0; it < kBorrowMaxIter; ++it) {
    ATX_TRY(const DeAmStep step,
            deam_pcp_step(borrow, call_mid, put_mid, S, K, T, r, cash_divs,
                          expiry_ns, now_ts_ns, hyb, method, opts, caches, warm_c,
                          warm_p));
    last_step = step;
    const double delta = step.borrow_next - borrow;
    borrow = step.borrow_next;
    if (std::fabs(delta) < kBorrowFpTol) {
      converged = true;
      break;
    }
  }
  if (!converged) {
    return Err(ErrorCode::Unavailable,
               "imply_term_borrow: borrow fixed point did not converge");
  }

  const double df = std::exp(-r * T);
  if (skip_redundant_final) {
    // Fast path: drop the extra self-consistent de-Am (2 American solves per
    // pair). The reported `forward` is hybrid_forward(converged borrow) — the
    // SAME value the final step would compute (same function, same borrow), so
    // it is bit-identical. Only the diagnostic rmse_pcp and the returned per-leg
    // vols are read off the loop's last step, evaluated one iterate
    // (< kBorrowFpTol = 1e-8) before the converged borrow — a sub-1e-8 shift in a
    // diagnostic / warm-seed value. Load-bearing outputs (borrow, forward) are
    // unchanged. Gated with warm_start_carry, so the default path is untouched.
    const double F_final =
        hybrid_forward(S, r, borrow, T, cash_divs, expiry_ns, now_ts_ns, hyb);
    const double residual =
        (last_step.call_eu - last_step.put_eu) - df * (F_final - K);
    TermBorrow result{borrow, F_final, std::fabs(residual)};
    result.sigma_call = last_step.sigma_c;
    result.sigma_put = last_step.sigma_p;
    return Ok(result);
  }

  // One final self-consistent evaluation at the converged borrow, so `forward`
  // and the PCP residual are reported on a single coherent state.
  ATX_TRY(const DeAmStep final_step,
          deam_pcp_step(borrow, call_mid, put_mid, S, K, T, r, cash_divs,
                        expiry_ns, now_ts_ns, hyb, method, opts, caches, warm_c,
                        warm_p));
  const double residual =
      (final_step.call_eu - final_step.put_eu) - df * (final_step.forward - K);
  TermBorrow result{borrow, final_step.forward, std::fabs(residual)};
  result.sigma_call = final_step.sigma_c;
  result.sigma_put = final_step.sigma_p;
  return Ok(result);
}

// ── Chain driver ────────────────────────────────────────────────────────

namespace {

// True iff the chosen leg's quote is invertible: strictly positive, non-crossed
// bid/ask and a finite positive mid. `idx` is chain_index(strike_idx, side).
[[nodiscard]] bool leg_quote_valid(const Chain& chain, std::size_t idx) noexcept {
  constexpr QuoteFlag kill_mask = QuoteFlag::Locked | QuoteFlag::Crossed |
                                  QuoteFlag::Stale | QuoteFlag::Halted |
                                  QuoteFlag::WideSpread | QuoteFlag::Penny |
                                  QuoteFlag::LowVega;
  if (idx < chain.flags.size() &&
      has_flag(static_cast<QuoteFlag>(chain.flags[idx]), kill_mask)) {
    return false;
  }
  const double bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  const double mid = chain.mids[idx];
  return (bid > 0.0) && (ask > 0.0) && (ask >= bid) && std::isfinite(mid) &&
         (mid > 0.0);
}

[[nodiscard]] double weighted_median(
    const std::vector<CarryPairDiagnostic>& pairs, std::size_t skip,
    bool robust_weights, bool absolute, double center) {
  std::vector<std::pair<double, double>> values;
  values.reserve(pairs.size());
  double total = 0.0;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i == skip) continue;
    const double weight = robust_weights ? pairs[i].robust_weight : pairs[i].base_weight;
    if (!(weight > 0.0) || !std::isfinite(weight)) continue;
    const double value = absolute ? std::fabs(pairs[i].borrow - center) : pairs[i].borrow;
    values.emplace_back(value, weight);
    total += weight;
  }
  if (values.empty() || !(total > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::sort(values.begin(), values.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  double cumulative = 0.0;
  for (const auto& [value, weight] : values) {
    cumulative += weight;
    if (cumulative >= 0.5 * total) return value;
  }
  return values.back().first;
}

[[nodiscard]] double robust_location(std::vector<CarryPairDiagnostic>& pairs,
                                     std::size_t skip, bool stamp_weights) {
  const double median = weighted_median(pairs, skip, false, false, 0.0);
  if (!std::isfinite(median)) return median;
  const double mad = weighted_median(pairs, skip, false, true, median);
  const double scale = std::fmax(1.0e-4, 1.4826 * (std::isfinite(mad) ? mad : 0.0));
  const double cutoff = 5.0 * scale;
  double sum_w = 0.0;
  double sum_wb = 0.0;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    double robust_weight = 0.0;
    if (i != skip) {
      const double z = std::fabs(pairs[i].borrow - median);
      if (z <= cutoff) {
        const double huber = (z <= 2.5 * scale || z == 0.0) ? 1.0 : (2.5 * scale / z);
        robust_weight = pairs[i].base_weight * huber;
        sum_w += robust_weight;
        sum_wb += robust_weight * pairs[i].borrow;
      }
    }
    if (stamp_weights) {
      pairs[i].robust_weight = robust_weight;
      pairs[i].retained = robust_weight > 0.0;
    }
  }
  return (sum_w > 0.0) ? (sum_wb / sum_w) : median;
}

[[nodiscard]] double quote_relative_spread(const Chain& chain,
                                           std::size_t call_idx,
                                           std::size_t put_idx) noexcept {
  const double call_rel = (chain.asks[call_idx] - chain.bids[call_idx]) /
                          std::fmax(chain.mids[call_idx], kMinSpread);
  const double put_rel = (chain.asks[put_idx] - chain.bids[put_idx]) /
                         std::fmax(chain.mids[put_idx], kMinSpread);
  return std::fmax(0.0, call_rel) + std::fmax(0.0, put_rel);
}

[[nodiscard]] double quote_age_seconds(const Chain& chain, std::size_t call_idx,
                                       std::size_t put_idx,
                                       std::int64_t now_ts_ns) noexcept {
  if (call_idx >= chain.ts_ns.size() || put_idx >= chain.ts_ns.size() ||
      now_ts_ns <= 0) {
    return 0.0;
  }
  const std::int64_t ts = std::min(chain.ts_ns[call_idx], chain.ts_ns[put_idx]);
  return ts > 0 && now_ts_ns > ts
             ? static_cast<double>(now_ts_ns - ts) * 1.0e-9
             : 0.0;
}

// The carry-pair candidate set and selection cut. `both_valid` holds every
// strike with BOTH legs quotable; its first `k` entries are the selected
// pairs, ordered by ascending |K - S| (spot is a fine ATM proxy since the
// forward sits near spot for equities). Shared verbatim between the carry
// solve and the public `carry_pair_strikes` accessor so the certified-cache
// invalidation can never drift from the selection the solve actually makes.
struct CarryPairSelection {
  std::vector<std::size_t> both_valid;
  std::size_t k{0};
};

[[nodiscard]] CarryPairSelection select_carry_pairs(const Chain& chain, double S,
                                                    const DeAmOptions& opts) {
  CarryPairSelection sel;
  const std::size_t n = chain.n_strikes();
  sel.both_valid.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t ci = chain_index(static_cast<std::uint16_t>(i), Side::Call);
    const std::size_t pi = chain_index(static_cast<std::uint16_t>(i), Side::Put);
    if (leg_quote_valid(chain, ci) && leg_quote_valid(chain, pi)) {
      sel.both_valid.push_back(i);
    }
  }
  if (sel.both_valid.empty()) {
    return sel;
  }

  // Use EVERY near-ATM co-terminal pair inside the ±carry_atm_band moneyness
  // band, falling back to the opts.n_atm nearest when too few sit inside it.
  std::size_t band = 0;
  for (std::size_t j = 0; j < sel.both_valid.size(); ++j) {
    if (std::fabs(chain.strikes[sel.both_valid[j]] / S - 1.0) <= opts.carry_atm_band) ++band;
  }
  // Cap the solve at the max_borrow_pairs nearest pairs: a dozen near-money
  // pairs already pin the forward to sub-tick accuracy, and the cold per-pair
  // de-Am is the cost driver, so an unbounded band (80+ pairs on a $1-strike
  // near-dated chain) would bloat the fit for no accuracy gain.
  const std::size_t max_carry_pairs =
      opts.max_borrow_pairs == 0 ? std::size_t{1} : opts.max_borrow_pairs;
  const std::size_t k_min = (opts.n_atm == 0 ? std::size_t{1} : opts.n_atm);
  sel.k = std::min(std::min(std::max(k_min, band), max_carry_pairs),
                   sel.both_valid.size());
  std::partial_sort(sel.both_valid.begin(),
                    sel.both_valid.begin() + static_cast<std::ptrdiff_t>(sel.k),
                    sel.both_valid.end(),
                    [&](std::size_t a, std::size_t b) noexcept {
                      return std::fabs(chain.strikes[a] - S) <
                             std::fabs(chain.strikes[b] - S);
                    });
  return sel;
}

// Resolve carry from a scored near-ATM strip. The robust center is a
// deterministic weighted Huber location, while dispersion and leave-one-out
// movement are retained for the independent admission layer.
[[nodiscard]] Result<ChainForward>
resolve_chain_carry(const Chain& chain, double S, double r,
                    std::span<const DividendEvent> cash_divs,
                    std::int64_t now_ts_ns, const DeAmOptions& opts) noexcept {
  if (!opts.imply_borrow) {
    const double F = hybrid_forward(S, r, opts.borrow_fixed, chain.T, cash_divs,
                                    chain.expiry_ns, now_ts_ns, opts.hyb);
    if (!(F > 0.0) || !std::isfinite(F)) {
      return Err(ErrorCode::Internal,
                 "resolve_chain_forward: invalid fixed-borrow forward");
    }
    CarryDiagnostics fixed{};
    fixed.confident = true;
    return Ok(ChainForward{F, opts.borrow_fixed, std::move(fixed)});
  }

  const double T = chain.T;

  // Robust multi-strike carry solve over `select_carry_pairs`' cut (every
  // near-ATM co-terminal pair inside the band, falling back to the opts.n_atm
  // nearest when too few), averaging their implied borrows. Two deliberate
  // departures from a single-pair solve, both required to hit the sub-tick
  // forward accuracy dense boards need:
  //   (1) the COLD Andersen-Lake de-Am is used for the carry solve (empty caches),
  //       NOT the query correction cache — the cache's small American→European
  //       Chebyshev bias, fed through the put-call parity solve, shifts the implied
  //       forward by ~$1–2 and injects a systematic near-ATM put/call IV step
  //       (measured on the SPY board). The carry solve is once per expiry, so the
  //       cold path is affordable.
  //   (2) many pairs, not one: a single ATM pair is quote-noise-fragile; the band
  //       average is the robust, put-call-IV-agreement-consistent forward.
  const CarryPairSelection selection = select_carry_pairs(chain, S, opts);
  const std::vector<std::size_t>& both_valid = selection.both_valid;
  const std::size_t k = selection.k;
  if (both_valid.empty()) {
    return Err(ErrorCode::Unavailable,
               "de_americanize_chain: no near-ATM co-terminal pair for borrow");
  }
  const AmericanCorrectionCaches cold_caches{};

  CarryDiagnostics diag{};
  diag.n_candidates = both_valid.size();
  diag.n_attempted = k;
  diag.pairs.reserve(k);
  // Cross-pair warm start: pairs are visited in ascending |K - S| order, so
  // adjacent borrows/vols are nearly equal. Seed each solve from the previous
  // pair's converged state to collapse the fixed-point + inner-Newton iteration
  // counts. Seeds change only the initial guesses of the safeguarded Newton /
  // borrow fixed-point; the converged root and per-leg vols are unchanged.
  double seed_borrow = 0.0, seed_sc = 0.0, seed_sp = 0.0;
  for (std::size_t j = 0; j < k; ++j) {
    const std::size_t i = both_valid[j];
    const double K = chain.strikes[i];
    const std::size_t ci = chain_index(static_cast<std::uint16_t>(i), Side::Call);
    const std::size_t pi = chain_index(static_cast<std::uint16_t>(i), Side::Put);
    const Result<TermBorrow> tb = imply_term_borrow(
        chain.mids[ci], chain.mids[pi], S, K, T, r, cash_divs, chain.expiry_ns,
        now_ts_ns, opts.hyb, opts.method, opts.al_opts, cold_caches, seed_borrow,
        seed_sc, seed_sp, opts.warm_start_carry);
    if (tb) {
      // Opt-in only: keep the default carry solve on the cold `borrow=0` /
      // cold-Newton seed so the reference de-Am stays bit-identical (mirrors
      // build_observations_european's `warm_start_deam` gate). When enabled,
      // thread this pair's converged state into the next pair's solve.
      if (opts.warm_start_carry) {
        seed_borrow = tb->borrow;
        seed_sc = tb->sigma_call;
        seed_sp = tb->sigma_put;
      }
      const double relative_spread = quote_relative_spread(chain, ci, pi);
      const double age = quote_age_seconds(chain, ci, pi, now_ts_ns);
      const double distance = std::fabs(std::log(K / S));
      const double distance_weight = 1.0 / (1.0 + std::pow(distance / 0.04, 2.0));
      const double quality_weight = 1.0 / std::pow(0.0025 + relative_spread, 2.0);
      const double freshness_weight = 1.0 / (1.0 + age / 5.0);
      const double base_weight =
          std::fmin(1.0e8, quality_weight * distance_weight * freshness_weight);
      diag.pairs.push_back(CarryPairDiagnostic{
          static_cast<std::uint16_t>(i), K, tb->borrow, tb->forward,
          tb->rmse_pcp, relative_spread, age, base_weight, 0.0, false});
      diag.max_pcp_residual = std::fmax(diag.max_pcp_residual, tb->rmse_pcp);
    }
  }
  diag.n_solved = diag.pairs.size();
  if (diag.pairs.empty()) {
    return Err(ErrorCode::Unavailable,
               "de_americanize_chain: term-borrow solve failed on all ATM pairs");
  }

  // A displayed one-tick spread must not let one internally inconsistent pair
  // own the estimator. Winsorize quality weights relative to the cross-section
  // before applying the robust location; freshness and ATM proximity still
  // differentiate pairs without allowing a single quote to exceed ~43% of a
  // three-pair strip solely through its claimed spread.
  std::vector<double> base_weights;
  base_weights.reserve(diag.pairs.size());
  for (const CarryPairDiagnostic& pair : diag.pairs) {
    base_weights.push_back(pair.base_weight);
  }
  std::sort(base_weights.begin(), base_weights.end());
  const double median_weight = base_weights[base_weights.size() / 2];
  const double weight_cap = 1.5 * median_weight;
  for (CarryPairDiagnostic& pair : diag.pairs) {
    pair.base_weight = std::fmin(pair.base_weight, weight_cap);
  }

  const double borrow = robust_location(diag.pairs, diag.pairs.size(), true);
  if (!std::isfinite(borrow)) {
    return Err(ErrorCode::Unavailable,
               "de_americanize_chain: robust term-borrow aggregation failed");
  }
  double sum_w = 0.0;
  double sum_w2 = 0.0;
  double sum_var = 0.0;
  for (const CarryPairDiagnostic& pair : diag.pairs) {
    if (!pair.retained) continue;
    ++diag.n_retained;
    sum_w += pair.robust_weight;
    sum_w2 += pair.robust_weight * pair.robust_weight;
    sum_var += pair.robust_weight * std::pow(pair.borrow - borrow, 2.0);
  }
  diag.effective_pair_count =
      (sum_w2 > 0.0) ? (sum_w * sum_w / sum_w2) : 0.0;
  diag.dispersion = (sum_w > 0.0) ? std::sqrt(sum_var / sum_w) : 0.0;

  if (diag.n_retained > 1) {
    for (std::size_t i = 0; i < diag.pairs.size(); ++i) {
      if (!diag.pairs[i].retained) continue;
      std::vector<CarryPairDiagnostic> loo_pairs = diag.pairs;
      const double loo = robust_location(loo_pairs, i, false);
      if (std::isfinite(loo)) {
        diag.max_leave_one_out_shift =
            std::fmax(diag.max_leave_one_out_shift, std::fabs(loo - borrow));
      }
    }
  }
  const double sampling =
      diag.effective_pair_count > 0.0
          ? 2.576 * diag.dispersion / std::sqrt(diag.effective_pair_count)
          : std::numeric_limits<double>::infinity();
  diag.confidence_half_width = std::fmax(sampling, diag.max_leave_one_out_shift);
  diag.confident = diag.n_retained >= opts.min_confident_borrow_pairs &&
                   diag.dispersion <= opts.max_carry_dispersion &&
                   diag.max_leave_one_out_shift <= opts.max_carry_leave_one_out;
  if (opts.require_carry_confidence && !diag.confident) {
    return Err(ErrorCode::Unavailable,
               "de_americanize_chain: robust carry confidence gate failed");
  }

  const double F = hybrid_forward(S, r, borrow, T, cash_divs, chain.expiry_ns,
                                  now_ts_ns, opts.hyb);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::Internal,
               "resolve_chain_forward: non-positive or non-finite term forward");
  }
  return Ok(ChainForward{F, borrow, std::move(diag)});
}

}  // namespace

Result<ChainForward> resolve_chain_forward(
    const Chain& chain, double S, double r,
    std::span<const DividendEvent> cash_divs, std::int64_t now_ts_ns,
    const DeAmOptions& opts) noexcept {
  const double T = chain.T;
  if (!(S > 0.0) || !(T > 0.0) || !std::isfinite(r) || chain.n_strikes() == 0) {
    return Err(ErrorCode::InvalidArgument,
               "resolve_chain_forward: non-finite/non-positive input or empty chain");
  }
  return resolve_chain_carry(chain, S, r, cash_divs, now_ts_ns, opts);
}

std::vector<std::uint16_t> carry_pair_strikes(const Chain& chain, double S,
                                              const DeAmOptions& opts) {
  if (!(S > 0.0) || !(chain.T > 0.0) || chain.n_strikes() == 0 ||
      !opts.imply_borrow) {
    return {};
  }
  const CarryPairSelection selection = select_carry_pairs(chain, S, opts);
  std::vector<std::uint16_t> out;
  out.reserve(selection.k);
  for (std::size_t j = 0; j < selection.k; ++j) {
    out.push_back(static_cast<std::uint16_t>(selection.both_valid[j]));
  }
  return out;
}

Result<DeAmResult> de_americanize_chain(const Chain& chain, double S, double r,
                                        std::span<const DividendEvent> cash_divs,
                                        std::int64_t now_ts_ns,
                                        const DeAmOptions& opts) noexcept {
  const double T = chain.T;
  if (!(S > 0.0) || !(T > 0.0) || !std::isfinite(r) || chain.n_strikes() == 0) {
    return Err(ErrorCode::InvalidArgument,
               "de_americanize_chain: non-finite/non-positive input or empty chain");
  }

  ATX_TRY(ChainForward chain_forward,
          resolve_chain_carry(chain, S, r, cash_divs, now_ts_ns, opts));
  const double borrow = chain_forward.borrow;
  const double F = chain_forward.forward;

  // q_eff bridge: one scalar carry reproduces the discrete-div forward exactly.
  const double q_eff = r - std::log(F / S) / T;
  const double df = std::exp(-r * T);

  DeAmResult out;
  out.forward = F;
  out.borrow = borrow;
  out.carry = std::move(chain_forward.carry);
  const std::size_t n = chain.n_strikes();
  out.k_log.reserve(n);
  out.iv.reserve(n);
  out.weight.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    const double K = chain.strikes[i];
    if (!(K > 0.0)) {
      ++out.n_dropped;
      continue;
    }
    const double k = std::log(K / F);
    const Side side = otm_side(k);
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);

    if (!leg_quote_valid(chain, idx)) {
      ++out.n_dropped;
      continue;
    }

    const double spread = chain.asks[idx] - chain.bids[idx];
    const CorrectionCache* cache = opts.caches.for_side(side);
    Result<double> iv =
        european_equiv_iv(chain.mids[idx], S, K, T, r, q_eff, side, opts.method,
                          opts.al_opts, cache, opts.iv_tol, opts.iv_max_iter);
    if (!iv) {
      ++out.n_dropped;
      continue;
    }
    const bool approximate_proposal =
        opts.method == AmericanMethod::AndersenLake &&
        (opts.al_opts.has_value() ||
         (cache != nullptr && cache->populated() && cache->side() == side));
    if (approximate_proposal) {
      ++out.n_iv_audited;
      Result<IvRepricingAudit> audit = audit_european_equiv_iv(
          chain.mids[idx], spread, *iv, S, K, T, r, q_eff, side,
          opts.max_iv_residual_half_spreads);
      if (audit && audit->passed) {
        out.max_iv_residual_half_spreads =
            std::fmax(out.max_iv_residual_half_spreads,
                      audit->residual_half_spreads);
      }
      if (!audit || !audit->passed) {
        ++out.n_iv_fallback;
        iv = american_implied_vol(chain.mids[idx], S, K, T, r, q_eff, side,
                                  AmericanMethod::AndersenLake, 1.0e-7, 64,
                                  std::nullopt, nullptr, *iv);
        if (!iv) {
          ++out.n_dropped;
          continue;
        }
        audit = audit_european_equiv_iv(
            chain.mids[idx], spread, *iv, S, K, T, r, q_eff, side,
            opts.max_iv_residual_half_spreads);
        if (!audit || !audit->passed) {
          ++out.n_dropped;
          continue;
        }
        out.max_iv_residual_half_spreads =
            std::fmax(out.max_iv_residual_half_spreads,
                      audit->residual_half_spreads);
      }
    }

    // Weight hint: Black-76 vega / bid-ask spread — rewards tight, high-vega
    // quotes. Optional and not load-bearing; the caller may ignore it.
    const double vega =
        black76_value_and_vega(F, K, T, *iv, df, side).vega;
    const double weight = vega / std::fmax(spread, kMinSpread);

    out.k_log.push_back(k);
    out.iv.push_back(*iv);
    out.weight.push_back(weight);
    ++out.n_used;
  }

  return Ok(std::move(out));
}

}  // namespace atx::vol
