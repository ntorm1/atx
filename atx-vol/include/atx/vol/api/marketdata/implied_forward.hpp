#pragma once

// Per-expiry option-implied forward / carry ("sdiv") from raw put-call quotes.
//
// Every listed equity option chain prices a forward. Put-call parity reads it
// straight off co-terminal call/put mids at one strike:
//
//     C - P = DF * (F - K)           (European, exact)
//
// so each two-sided strike yields a model-free forward F_i = K + (C_i - P_i)/DF.
// The estimator below turns those per-strike forwards into ONE robust forward per
// expiry, then restates it as the carry quantities callers consume: a continuous
// yield on the pricer clock (SpiderRock's `sdiv`), the prepaid deficit
// S - F*DF (the PV of every dividend and borrow the market is pricing), and a
// `uOff`-style offset against a caller-supplied reference carry.
//
// ## American exercise (the approximation, stated once)
//
// For American options parity is a band, not an equality: C_Am - P_Am differs
// from DF*(F - K) by the difference of the two early-exercise premia (EEP). With
// positive rates the put carries an EEP even at the money, so mid parity biases
// the forward LOW, by an amount that grows with rate, tenor and moneyness. Two
// methods are offered:
//
//   * Parity — the robust European fit. For American chains it additionally
//     MEASURES its own early-exercise bias (one Andersen-Lake correction per
//     selected pair at the parity forward) and refuses as `Contaminated` when that
//     bias exceeds `max_exercise_shift_sd` standard deviations of moneyness. It
//     never silently publishes an EEP-biased forward.
//   * AmericanAlignment — Parity seeds a per-pair fixed point that removes the
//     EEP: at each selected strike it finds the forward at which the call and the
//     put have the SAME American implied volatility under the library's
//     continuous-carry Andersen-Lake model (the q_eff bridge of deamer.hpp). At
//     that point C_eu(sigma) - P_eu(sigma) = DF*(F - K) holds exactly, so this is
//     parity on de-Americanized prices. The update
//         F <- F + s * (ITM_Am(sigma_otm(F); F) - ITM_mid) / DF
//     (s = +1 for an ITM put, -1 for an ITM call) is Newton's step in the European
//     limit, because the total derivative of the ITM model price through both the
//     forward and the OTM leg's implied vol is -s*DF.
//
// The alignment forward is a MODEL-CONSISTENT carry, not a model-free one: under
// discrete cash dividends it absorbs the part of the exercise premium that a
// continuous-yield model cannot represent (exactly what a vendor's call/put
// `sdiv` calibration does). Pairs are restricted to near-the-money strikes where
// both legs carry time value; a leg at its exercise boundary carries no forward
// information and is excluded, as are pairs whose correction or IV inversion
// fails (deep ITM, very long dated, high carry: arXiv 1611.06181).
//
// ## Contract
//
// One expiry per call. Pairs must be sorted by strictly increasing positive
// strike (one entry per strike); a leg with a missing bid is NOT a point quote and
// is never used. Malformed shared inputs return `Invalid`; data that cannot
// support an estimate returns a typed refusal (`InsufficientPairs`, `Dispersion`,
// `Contaminated`) so the caller can fall back to its own carry. The estimator
// never guesses for a sparse expiry and never reads vendor fair values.
//
// Deterministic: equal inputs give bit-identical outputs. The Parity stage is
// allocation-free (bounded fixed-capacity buffers inside the result). The
// American stage calls the Andersen-Lake pricer and implied-vol inverter, which
// reuse thread-local scratch; only their error paths allocate (error text).
// Thread-safe: no shared mutable state.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {

// Capacity of the selected near-money strip (and of the per-pair diagnostics).
inline constexpr std::size_t kImpliedForwardMaxSelected = 32;
// Hard bound on the input strip of one expiry.
inline constexpr std::size_t kImpliedForwardMaxInput = 16384;

// One expiry's market context. `rate` and every reported yield are continuously
// compounded on `years`, the clock the American pricer uses; the discount factor
// is DF = exp(-rate * years). A caller with separate variance/funding clocks
// passes years = variance_years and rate = funding_rate * calendar_years /
// variance_years, which preserves DF and the pricer's forward exactly.
struct ImpliedForwardMarket {
  double spot = 0.0;
  double years = 0.0;
  double rate = 0.0;
  // Optional reference carry on the same clock (for example a supplied or vendor
  // yield). Used only to report forward_offset = F - spot*exp((rate-ref)*years).
  std::optional<double> reference_yield{};
};

// Co-terminal call and put quotes at one strike.
struct ImpliedForwardPair {
  double strike = 0.0;
  double call_bid = 0.0;
  double call_ask = 0.0;
  double put_bid = 0.0;
  double put_ask = 0.0;
};

enum class ImpliedForwardExercise : std::uint8_t { European, American };

enum class ImpliedForwardMethod : std::uint8_t {
  Parity,           // robust European parity; American chains are bias-screened
  AmericanAlignment // parity seed, then per-pair American call/put IV alignment
};

struct ImpliedForwardConfig {
  ImpliedForwardExercise exercise = ImpliedForwardExercise::American;
  ImpliedForwardMethod method = ImpliedForwardMethod::AmericanAlignment;
  // Pair counts. min_pairs retained pairs are required; at most max_pairs (<=
  // kImpliedForwardMaxSelected) nearest-to-forward pairs are used.
  std::size_t min_pairs = 3;
  std::size_t max_pairs = 8;
  // Selection band in ATM standard deviations of log-moneyness around the
  // provisional forward. The min_pairs nearest pairs are kept even outside it.
  double band_sd = 0.75;
  // Per-leg price floor for the bid/ask uncertainty that weights each pair.
  double spread_floor = 0.01;
  // Winsorize pair weights at this multiple of their median, so one claimed
  // tight quote cannot own the estimate.
  double max_weight_ratio = 4.0;
  double huber_k = 1.345;
  // Refusal tolerances, in ATM standard deviations of log-moneyness
  // (x_sd = (x / F) / (sigma * sqrt(years))), the unit a fitted smile consumes.
  // The defaults mirror deamer.hpp's second-tier carry budget (0.01 sd for a
  // single-pair leave-one-out move, a looser bound on the raw dispersion).
  double max_dispersion_sd = 0.05;
  double max_leave_one_out_sd = 0.02;
  // Refuse (Dispersion) when more than this fraction of the evaluated pairs is
  // rejected as outliers: a bimodal strip has no honest single forward, and a
  // robust location would otherwise just pick one mode.
  double max_outlier_fraction = 0.34;
  // Parity method on an American chain: refuse when the measured EEP bias of
  // the parity forward exceeds this.
  double max_exercise_shift_sd = 0.01;
  // AmericanAlignment: exclude a pair whose own EEP correction exceeds this.
  double max_pair_exercise_shift_sd = 0.25;
  // A leg is at its exercise boundary (excluded) unless its mid exceeds immediate
  // intrinsic by more than min_time_value + min_time_value_fraction * intrinsic.
  double min_time_value = 1.0e-4;
  double min_time_value_fraction = 0.01;
  // American pricer and inverter controls. The carry is an economically ~1e-4
  // input, so the fast Andersen-Lake preset is the default (as for deamer.hpp's
  // carry solve); pass nullopt for the accurate preset.
  std::optional<AlOpts> al_opts = al_fast_opts();
  double iv_tolerance = 1.0e-7;
  std::uint16_t max_iv_iterations = 64;
  // Per-pair fixed point: relative forward tolerance and iteration cap.
  double fixed_point_tolerance = 1.0e-9;
  std::uint16_t max_fixed_point_iterations = 12;
};

enum class ImpliedForwardStatus : std::uint8_t {
  Ok,
  InsufficientPairs, // too few usable two-sided near-money pairs
  Dispersion,        // retained pairs disagree beyond max_dispersion_sd
  Contaminated,      // early exercise prevents an honest estimate
  Invalid            // malformed market, configuration or strike ordering
};

enum class ImpliedForwardPairStatus : std::uint8_t {
  Retained,      // contributes to the estimate
  Outlier,       // rejected by the robust location
  ExerciseBound, // a leg sits at its exercise boundary (no forward information)
  Contaminated,  // EEP correction too large or fixed point did not converge
  Unidentified   // American IV inversion or repricing failed
};

[[nodiscard]] const char *to_string(ImpliedForwardStatus value) noexcept;
[[nodiscard]] const char *to_string(ImpliedForwardPairStatus value) noexcept;
[[nodiscard]] const char *to_string(ImpliedForwardMethod value) noexcept;

struct ImpliedForwardPairDiagnostic {
  double strike = 0.0;
  // K + (C_mid - P_mid) / DF (European parity forward of this pair).
  double parity_forward = std::numeric_limits<double>::quiet_NaN();
  // Early-exercise-corrected forward (== parity_forward unless American).
  double forward = std::numeric_limits<double>::quiet_NaN();
  // forward - parity_forward.
  double exercise_shift = 0.0;
  // Bid/ask channel of this pair's forward, shifted by exercise_shift.
  double lower = std::numeric_limits<double>::quiet_NaN();
  double upper = std::numeric_limits<double>::quiet_NaN();
  double weight = 0.0;        // winsorized bid/ask x proximity weight
  double robust_weight = 0.0; // final Huber factor (0 when not retained)
  double sigma = std::numeric_limits<double>::quiet_NaN(); // aligned American IV
  Side anchor_side = Side::Call; // leg whose IV anchored the correction (OTM)
  std::uint8_t iterations = 0;
  ImpliedForwardPairStatus status = ImpliedForwardPairStatus::Outlier;
};

struct ImpliedForwardResult {
  ImpliedForwardStatus status = ImpliedForwardStatus::Invalid;
  ImpliedForwardMethod method = ImpliedForwardMethod::AmericanAlignment;
  // Static human-readable detail for the status; never allocated.
  const char *reason = "not evaluated";
  double forward = std::numeric_limits<double>::quiet_NaN();
  double parity_forward = std::numeric_limits<double>::quiet_NaN();
  // Intersection of the retained pairs' (exercise-corrected) bid/ask channels.
  // channel_consistent is false when the intersection is empty.
  double forward_lower = std::numeric_limits<double>::quiet_NaN();
  double forward_upper = std::numeric_limits<double>::quiet_NaN();
  bool channel_consistent = false;
  double discount = std::numeric_limits<double>::quiet_NaN();
  // rate - ln(F / spot) / years: the total implied carry on the market clock
  // (dividends + borrow as one continuous yield, SpiderRock's `sdiv` analogue).
  double implied_yield = std::numeric_limits<double>::quiet_NaN();
  // spot - F * DF: present value of all carry the forward implies.
  double prepaid_deficit = std::numeric_limits<double>::quiet_NaN();
  // F - spot * exp((rate - reference_yield) * years); NaN without a reference.
  double forward_offset = std::numeric_limits<double>::quiet_NaN();
  // ATM total volatility sigma*sqrt(years): a straddle estimate for Parity, the
  // weighted aligned American IV for AmericanAlignment.
  double atm_total_vol = std::numeric_limits<double>::quiet_NaN();
  double dispersion = std::numeric_limits<double>::quiet_NaN(); // price units
  double dispersion_sd = std::numeric_limits<double>::quiet_NaN();
  double max_leave_one_out_shift_sd = std::numeric_limits<double>::quiet_NaN();
  // Robust (forward - parity_forward) in ATM standard deviations; for Parity on
  // an American chain this is the measured bias that the method did NOT remove.
  double exercise_shift_sd = std::numeric_limits<double>::quiet_NaN();
  std::size_t pairs_input = 0;
  std::size_t pairs_valid = 0;    // two-sided call AND put, finite
  std::size_t pairs_selected = 0; // near-money strip actually evaluated
  std::size_t pairs_retained = 0;
  std::size_t pairs_outlier = 0;
  std::size_t pairs_exercise_bound = 0;
  std::size_t pairs_contaminated = 0;
  std::size_t pairs_unidentified = 0;
  // The first pairs_selected entries describe the selected strip, in increasing
  // distance from the provisional forward.
  std::array<ImpliedForwardPairDiagnostic, kImpliedForwardMaxSelected> pairs{};
};

// Estimate one expiry's implied forward from its call/put quote pairs.
//
// @param market  spot (> 0), years (> 0), rate (finite) and an optional finite
//                reference yield, all on the pricer clock
// @param pairs   strictly increasing positive strikes, at most
//                kImpliedForwardMaxInput entries; quotes may be bidless/missing
// @param config  method, exercise style and tolerances (validated)
// @return        always a result; `status` is Ok only when `forward` is a
//                supported estimate. Never throws for control flow (the
//                American stage may propagate std::bad_alloc from the pricer).
[[nodiscard]] ImpliedForwardResult
estimate_implied_forward(const ImpliedForwardMarket &market,
                         std::span<const ImpliedForwardPair> pairs,
                         const ImpliedForwardConfig &config = {});

} // namespace atx::vol
