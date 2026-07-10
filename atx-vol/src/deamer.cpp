#include "atx/vol/deamer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
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

namespace {

// One step of the borrow fixed-point: de-Americanize both legs at the forward
// implied by `borrow`, reprice European, and re-solve the European PCP borrow.
struct DeAmStep {
  double borrow_next = 0.0;
  double forward = 0.0;
  double call_eu = 0.0;
  double put_eu = 0.0;
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
  return Ok(DeAmStep{b_next, F, call_eu, put_eu});
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
                                     const AmericanCorrectionCaches& caches) noexcept {
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !std::isfinite(r) ||
      !(call_mid > 0.0) || !(put_mid > 0.0) || !std::isfinite(call_mid) ||
      !std::isfinite(put_mid)) {
    return Err(ErrorCode::InvalidArgument,
               "imply_term_borrow: non-finite or non-positive input");
  }

  // Fixed point borrow -> F -> q_eff -> vols -> European mids -> borrow. The
  // injected-borrow round-trip is an exact fixed point; the map is contractive
  // near it, so a handful of iterations converge for any sane co-terminal pair.
  double borrow = 0.0;
  double warm_c = 0.0;  // per-leg Newton warm starts, persisted across iterations
  double warm_p = 0.0;
  bool converged = false;
  for (int it = 0; it < kBorrowMaxIter; ++it) {
    ATX_TRY(const DeAmStep step,
            deam_pcp_step(borrow, call_mid, put_mid, S, K, T, r, cash_divs,
                          expiry_ns, now_ts_ns, hyb, method, opts, caches, warm_c,
                          warm_p));
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

  // One final self-consistent evaluation at the converged borrow, so `forward`
  // and the PCP residual are reported on a single coherent state.
  ATX_TRY(const DeAmStep final_step,
          deam_pcp_step(borrow, call_mid, put_mid, S, K, T, r, cash_divs,
                        expiry_ns, now_ts_ns, hyb, method, opts, caches, warm_c,
                        warm_p));
  const double df = std::exp(-r * T);
  const double residual =
      (final_step.call_eu - final_step.put_eu) - df * (final_step.forward - K);
  return Ok(TermBorrow{borrow, final_step.forward, std::fabs(residual)});
}

// ── Chain driver ────────────────────────────────────────────────────────

namespace {

// True iff the chosen leg's quote is invertible: strictly positive, non-crossed
// bid/ask and a finite positive mid. `idx` is chain_index(strike_idx, side).
[[nodiscard]] bool leg_quote_valid(const Chain& chain, std::size_t idx) noexcept {
  const double bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  const double mid = chain.mids[idx];
  return (bid > 0.0) && (ask > 0.0) && (ask >= bid) && std::isfinite(mid) &&
         (mid > 0.0);
}

// Resolve the borrow for the whole chain: the mean over the n_atm near-ATM
// co-terminal pairs whose BOTH legs are quotable, or borrow_fixed when implying
// is disabled.
[[nodiscard]] Result<double>
resolve_chain_borrow(const Chain& chain, double S, double r,
                     std::span<const DividendEvent> cash_divs,
                     std::int64_t now_ts_ns, const DeAmOptions& opts) noexcept {
  if (!opts.imply_borrow) {
    return Ok(opts.borrow_fixed);
  }

  const std::size_t n = chain.n_strikes();
  const double T = chain.T;

  // Indices of strikes with both legs quotable, ranked by |K − S| (spot is a
  // fine ATM proxy since the forward sits near spot for equities).
  std::vector<std::size_t> both_valid;
  both_valid.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t ci = chain_index(static_cast<std::uint16_t>(i), Side::Call);
    const std::size_t pi = chain_index(static_cast<std::uint16_t>(i), Side::Put);
    if (leg_quote_valid(chain, ci) && leg_quote_valid(chain, pi)) {
      both_valid.push_back(i);
    }
  }
  if (both_valid.empty()) {
    return Err(ErrorCode::Unavailable,
               "de_americanize_chain: no near-ATM co-terminal pair for borrow");
  }

  // Robust multi-strike carry solve. Use EVERY near-ATM co-terminal pair inside a
  // ±6% moneyness band (falling back to the opts.n_atm nearest when too few), and
  // average their implied borrows. Two deliberate departures from a single-pair
  // solve, both required to hit the sub-tick forward accuracy dense boards need:
  //   (1) the COLD Andersen-Lake de-Am is used for the carry solve (empty caches),
  //       NOT the query correction cache — the cache's small American→European
  //       Chebyshev bias, fed through the put-call parity solve, shifts the implied
  //       forward by ~$1–2 and injects a systematic near-ATM put/call IV step
  //       (measured on the SPY board). The carry solve is once per expiry, so the
  //       cold path is affordable.
  //   (2) many pairs, not one: a single ATM pair is quote-noise-fragile; the band
  //       average is the robust, put-call-IV-agreement-consistent forward.
  const AmericanCorrectionCaches cold_caches{};
  std::size_t band = 0;
  for (std::size_t j = 0; j < both_valid.size(); ++j) {
    if (std::fabs(chain.strikes[both_valid[j]] / S - 1.0) <= 0.06) ++band;
  }
  // Cap the solve at the 12 nearest pairs: a dozen near-money pairs already pin
  // the forward to sub-tick accuracy, and the cold per-pair de-Am is the cost
  // driver, so an unbounded ±6% band (80+ pairs on a $1-strike near-dated chain)
  // would bloat the fit for no accuracy gain.
  const std::size_t max_carry_pairs =
      opts.max_borrow_pairs == 0 ? std::size_t{1} : opts.max_borrow_pairs;
  const std::size_t k_min = (opts.n_atm == 0 ? std::size_t{1} : opts.n_atm);
  const std::size_t k =
      std::min(std::min(std::max(k_min, band), max_carry_pairs), both_valid.size());
  std::partial_sort(both_valid.begin(),
                    both_valid.begin() + static_cast<std::ptrdiff_t>(k),
                    both_valid.end(),
                    [&](std::size_t a, std::size_t b) noexcept {
                      return std::fabs(chain.strikes[a] - S) <
                             std::fabs(chain.strikes[b] - S);
                    });

  double sum = 0.0;
  std::size_t hits = 0;
  for (std::size_t j = 0; j < k; ++j) {
    const std::size_t i = both_valid[j];
    const double K = chain.strikes[i];
    const std::size_t ci = chain_index(static_cast<std::uint16_t>(i), Side::Call);
    const std::size_t pi = chain_index(static_cast<std::uint16_t>(i), Side::Put);
    const Result<TermBorrow> tb = imply_term_borrow(
        chain.mids[ci], chain.mids[pi], S, K, T, r, cash_divs, chain.expiry_ns,
        now_ts_ns, opts.hyb, opts.method, opts.al_opts, cold_caches);
    if (tb) {
      sum += tb->borrow;
      ++hits;
    }
  }
  if (hits == 0) {
    return Err(ErrorCode::Unavailable,
               "de_americanize_chain: term-borrow solve failed on all ATM pairs");
  }
  return Ok(sum / static_cast<double>(hits));
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
  ATX_TRY(const double borrow,
          resolve_chain_borrow(chain, S, r, cash_divs, now_ts_ns, opts));
  const double F = hybrid_forward(S, r, borrow, T, cash_divs, chain.expiry_ns,
                                  now_ts_ns, opts.hyb);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::Internal,
               "resolve_chain_forward: non-positive or non-finite term forward");
  }
  return Ok(ChainForward{F, borrow});
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

  ATX_TRY(const double borrow,
          resolve_chain_borrow(chain, S, r, cash_divs, now_ts_ns, opts));

  const double F =
      hybrid_forward(S, r, borrow, T, cash_divs, chain.expiry_ns, now_ts_ns, opts.hyb);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::Internal,
               "de_americanize_chain: non-positive or non-finite term forward");
  }

  // q_eff bridge: one scalar carry reproduces the discrete-div forward exactly.
  const double q_eff = r - std::log(F / S) / T;
  const double df = std::exp(-r * T);

  DeAmResult out;
  out.forward = F;
  out.borrow = borrow;
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

    const Result<double> iv =
        european_equiv_iv(chain.mids[idx], S, K, T, r, q_eff, side, opts.method,
                          opts.al_opts, opts.caches.for_side(side), opts.iv_tol,
                          opts.iv_max_iter);
    if (!iv) {
      ++out.n_dropped;
      continue;
    }

    // Weight hint: Black-76 vega / bid-ask spread — rewards tight, high-vega
    // quotes. Optional and not load-bearing; the caller may ignore it.
    const double vega =
        black76_value_and_vega(F, K, T, *iv, df, side).vega;
    const double spread = chain.asks[idx] - chain.bids[idx];
    const double weight = vega / std::fmax(spread, kMinSpread);

    out.k_log.push_back(k);
    out.iv.push_back(*iv);
    out.weight.push_back(weight);
    ++out.n_used;
  }

  return Ok(std::move(out));
}

}  // namespace atx::vol
