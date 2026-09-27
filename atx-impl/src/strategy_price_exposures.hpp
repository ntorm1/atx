#pragma once

#include <array>
#include <span>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

// Causal ex-ante price-risk exposures (trailing market beta, volatility, log
// dollar ADV) and neutralization of one decision's desired target against them.
// Deterministic: fixed loop order, no threads, no hidden state between calls.
namespace atx::impl::strategy {
// Column order of every row-major instruments x kPriceExposureCount matrix.
inline constexpr atx::usize kPriceExposureCount = 3;
inline constexpr atx::usize kExposureBeta = 0, kExposureVol = 1, kExposureLogAdv = 2;

struct PriceExposureConfig {
  atx::usize beta_window{252}, vol_window{63}, adv_window{63}, min_return_pairs{126};
  atx::usize min_names{50};
  atx::f64 clip_z{5.0};
};
// All spans are borrowed for one synchronous call, date-major dates x instruments,
// immutable. close is adjusted, raw_close unadjusted, volume raw shares, present
// the physical vendor row flag (0/1; absent rows carry NaN prices). Decision d is
// known after the close of session d.
struct PriceExposureInput {
  atx::usize dates{}, instruments{};
  std::span<const atx::f64> close, raw_close, volume;
  std::span<const atx::u8> present;
};

// Working storage owned by the caller and reused across calls. Its contents are
// private to the implementation: default-construct once per replay and pass it to
// every call. Buffers only grow (to the largest geometry seen), so repeated calls
// on one panel geometry allocate nothing after the first.
struct PriceExposureScratch {
  std::vector<atx::f64> returns; // instrument-major valid interval returns, NaN if invalid
  std::vector<atx::f64> market;  // equal-weight market return per interval
  std::vector<atx::f64> logs;    // previous/current session log adjusted and raw closes
  std::vector<atx::f64> dollars; // per-instrument dollar volume sum over the ADV window
};
struct NeutralizeScratch {
  std::vector<atx::usize> rows;   // regressed row indices, ascending
  std::vector<atx::f64> z;        // rows x kPriceExposureCount clipped z-scores
  std::vector<atx::f64> residual; // rows
};
// compute_price_exposures + neutralize_target state for the one-call step below.
struct PriceRiskScratch {
  PriceExposureScratch exposure;
  NeutralizeScratch neutralize;
  std::vector<atx::f64> exposures; // instruments x kPriceExposureCount at the last decision
  std::vector<atx::u8> ok;         // instruments at the last decision
};
struct NeutralizeStats {
  atx::usize used{};     // member && ok rows regressed and rescaled
  atx::usize excluded{}; // member && !ok rows forced to zero
  atx::f64 gross{};          // sum |target| over members on entry; also the exit gross
  atx::f64 excluded_gross{}; // share of that gross that sat on excluded rows
  atx::f64 residual_gross{}; // sum |OLS residual| over used rows, before rescaling
  // OLS coefficients on [1, z_beta, z_vol, z_log_adv] (clipped z-scores), after
  // one refinement step. All zero for a flat target.
  std::array<atx::f64, kPriceExposureCount + 1> coefficients{};
};

// Exposures known at decision d; every window ends at session d inclusive and no
// session after d is read (nor validated). Interval t is (t-1, t]. Its adjusted
// simple return is valid iff both endpoints are present with finite positive
// close and raw_close, the return is finite, and the interval is not guarded:
// |log adjusted ratio| > 1.5 or > |log raw ratio| + 0.10 (the target replay's
// rough-return guard, identical operations).
//  - market(t): equal-weight mean of valid returns across ALL instruments; NaN
//    when none is valid.
//  - beta: cov(r_i, market)/var(market) (sample, two-pass) over valid pairs in the
//    last beta_window intervals; NaN below min_return_pairs pairs or var == 0.
//  - vol: sample SD of valid r_i over the last vol_window intervals; NaN below
//    max(2, ceil(vol_window / 2)) returns.
//  - log_adv: log of the mean of raw_close * volume over the adv_window sessions
//    ending at d. A session counts 0 unless present with finite positive raw_close,
//    finite nonnegative volume and a finite product. NaN if the full window is not
//    inside the panel (d + 1 < adv_window) or the mean is not positive.
// Windows are clipped at the panel start; the pair minimums then gate beta/vol.
// out: row-major instruments x kPriceExposureCount (beta, vol, log_adv), each
// finite or NaN. ok[i] = 1 iff all three are finite, else 0.
// Errors (out/ok unspecified): InvalidArgument for config (windows in [2, 4096],
// adv_window in [1, 4096], 2 <= min_return_pairs <= beta_window, min_names >= 5,
// finite clip_z > 0), span geometry, d >= dates or a presence byte > 1 in the
// read sessions; OutOfRange if instruments x max(beta_window, vol_window) exceeds
// 2^26 return cells or scratch allocation fails.
// Cost: O((max(beta_window, vol_window) + adv_window) x instruments) per call.
[[nodiscard]] atx::core::Status compute_price_exposures(
    const PriceExposureInput&, const PriceExposureConfig&, atx::usize d,
    PriceExposureScratch&, std::span<atx::f64> out, std::span<atx::u8> ok);

// In-place cross-sectional neutralization of one decision's target. Rows used:
// member && ok. Each exposure is z-scored over those rows (mean, sample SD) and
// clipped to +-clip_z; the target is regressed by OLS on [1, z_beta, z_vol,
// z_log_adv] (Jacobi-equilibrated Cholesky with a pivot floor, one iterative
// refinement step) and replaced by the residual, rescaled so sum |target| over
// the used rows equals the entry gross (sum |target| over all members).
// member && !ok rows become 0; nonmembers are untouched and must already be 0.
// A flat (zero-gross) target is returned unchanged.
// Preconditions (else InvalidArgument): equal lengths, exposures row-major
// target.size() x kPriceExposureCount, member/ok bytes 0/1, members finite,
// nonmembers exactly 0, ok rows finite exposures.
// Data refusals (Unavailable): fewer than min_names used rows, a constant
// exposure column, an ill-conditioned normal matrix, or a residual that is a
// negligible fraction of the gross. OutOfRange if scratch allocation fails.
// Strong guarantee: on any error target is unmodified. stats is reset on entry;
// on a refusal it still reports used/excluded/gross.
[[nodiscard]] atx::core::Status neutralize_target(
    std::span<atx::f64> target, std::span<const atx::u8> member,
    std::span<const atx::f64> exposures, std::span<const atx::u8> ok,
    const PriceExposureConfig&, NeutralizeScratch&, NeutralizeStats&);

// One-call decision step: compute_price_exposures at d, then neutralize_target on
// the target (instruments long) with that decision's member row. Same contracts
// and errors as the two primitives; target is unmodified on any error.
[[nodiscard]] atx::core::Status neutralize_price_risk(
    const PriceExposureInput&, const PriceExposureConfig&, atx::usize d,
    std::span<atx::f64> target, std::span<const atx::u8> member, PriceRiskScratch&,
    NeutralizeStats&);
} // namespace atx::impl::strategy
