#pragma once

// European-only outputs on a declared common-model surface (lane A1, r6 2026-10-23; deep
// review F13; docs/analytics-contracts.md section 5).
//
// A risk-neutral density, its CDF, a variance strip and a local-volatility (Dupire-style)
// read describe a EUROPEAN price curve. Breeden-Litzenberger applied to Black-76 prices
// built from American quoting vols describes that constructed curve, not a density
// recovered from the market. So these outputs run ONLY on a declared European-equivalent
// common model - the Markov common model (american_markov.hpp: one underlying process for
// every strike and expiry, compiled by american_markov_fit / compile_american_markov) -
// and REFUSE, with a typed reason and no number, on the American quoting surface (a
// SliceServeRecord or a ServeBook expiry: the (sigma, q) family quoting vols). Nothing
// here calibrates: a caller that wants these outputs for a quoted expiry fits the common
// model itself (its own accuracy, calibration and memory budgets) and passes it; the
// quoting product is never silently replaced by the heavier calibration.
//
// ## What the outputs are, on the compiled model
//
//   density        the model's terminal distribution at the expiry layer: the unit mass at
//                  the initial state pushed forward through every transition (the
//                  transpose action, apply_american_markov_transition Forward), on the
//                  layer's states S_i = growth x X_i. Exact for the compiled finite chain;
//                  its distance to the continuous model is the grid's.
//   cdf            the cumulative sum of the density, ascending S.
//   variance strip the model's fair total variance -2 E[ln(X_T / X_0)] of the log-
//                  martingale X = S / G (the log-contract strip of a continuous model),
//                  reported on both clocks. Refused across a cash event (the strip assumes
//                  no jump) and when the terminal mass at S = 0 exceeds
//                  kCommonModelMaxZeroMass (the log contract is unbounded there).
//   local vol      the model's own local volatility per compiled interval and state (the
//                  Dupire inverse of its European prices, up to the discretisation): the
//                  interval's state volatilities on the interval's first layer.
//
// ## Budgets (reported, not tuned)
//
//   accuracy     the density's mass and mean errors on the compiled chain
//                (|sum mass - 1|, |E[X_T] / X_0 - 1|) beside the model's own
//                diagnostics; the A1 tests measure the constant-vol model against the
//                lognormal it discretises;
//   memory       the compiled model's logical rows / states / stored volatilities and the
//                bytes this call allocated;
//   calibration  none here (the model is supplied);
//   scenario     none: a common-model scenario is a recompiled model, not a read.
//
// Pure: concurrent calls on one model are safe.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/api/analytics/served_analytics.hpp" // AnalyticsDeclaration
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/american_markov.hpp"

namespace atx::vol {

// The terminal mass at S = 0 above which the variance strip refuses (AtomAtZero).
inline constexpr double kCommonModelMaxZeroMass = 1.0e-12;

enum class EuropeanSurfaceKind : std::uint8_t {
  None = 0,
  CommonModel = 1,      // a compiled AmericanMarkovModel: European-equivalent, declared
  QuotingRecord = 2,    // a SliceServeRecord: the American quoting surface (refused)
  QuotingBookExpiry = 3 // a ServeBook expiry: the American quoting surface (refused)
};

enum class EuropeanOutputRefusal : std::uint8_t {
  None = 0,
  AmericanQuotingSurface = 1, // F13: American quoting vols are not a European input
  NoSurface = 2,              // the reference names no surface
  NoExpiryLayer = 3,          // the expiry index is not one of the model's expiries
  CashDividendJump = 4,       // variance strip: a cash event in (0, T]
  AtomAtZero = 5,             // variance strip: terminal mass at S = 0 above the cap
  NumericalFailure = 6,       // a transition action refused (its error)
};

// What a European-only output is asked to run on. Borrowed: the referenced object must
// outlive the call.
struct EuropeanSurfaceRef {
  EuropeanSurfaceKind kind = EuropeanSurfaceKind::None;
  const AmericanMarkovModel *model = nullptr;
  std::size_t expiry_index = 0;
  const SliceServeRecord *record = nullptr;
  const ServeBook *book = nullptr;
  std::uint64_t expiry_id = 0;
  std::uint64_t surface_version = 0; // the caller's version stamp (declaration.entry_tag)

  [[nodiscard]] static EuropeanSurfaceRef common_model(const AmericanMarkovModel &model,
                                                       std::size_t expiry_index,
                                                       std::uint64_t surface_version = 0);
  [[nodiscard]] static EuropeanSurfaceRef quoting_record(const SliceServeRecord &record);
  [[nodiscard]] static EuropeanSurfaceRef quoting_book_expiry(const ServeBook &book,
                                                              std::uint64_t expiry_id);
};

struct EuropeanOutputsRequest {
  bool density = true; // density and cdf
  bool variance_strip = true;
  bool local_vol = true;
};

struct CommonModelDensity {
  std::vector<double> spot; // terminal stock levels, ascending
  std::vector<double> mass; // probability at each level
  std::vector<double> cdf;  // cumulative, ascending
  double total_mass = std::numeric_limits<double>::quiet_NaN();
  double mean = std::numeric_limits<double>::quiet_NaN();    // E[S_T]
  double forward = std::numeric_limits<double>::quiet_NaN(); // growth x X_0 less the cash
  double discount = std::numeric_limits<double>::quiet_NaN(); // to the expiry layer
  double zero_mass = std::numeric_limits<double>::quiet_NaN(); // mass at S = 0
};

struct CommonModelVarianceStrip {
  double total_variance = std::numeric_limits<double>::quiet_NaN(); // -2 E[ln(X_T / X_0)]
  double variance_years = std::numeric_limits<double>::quiet_NaN(); // the layer's clocks
  double calendar_years = std::numeric_limits<double>::quiet_NaN();
  double vol_variance_clock = std::numeric_limits<double>::quiet_NaN(); // sqrt(w / T_var)
  double vol_calendar_clock = std::numeric_limits<double>::quiet_NaN();
  double excluded_zero_mass = 0.0; // the mass at S = 0 left out (<= the cap)
};

struct CommonModelLocalVol {
  std::vector<double> calendar_time; // per row: the interval's first layer
  std::vector<double> spot;          // growth x X
  std::vector<double> volatility;    // the model's local volatility there
};

struct CommonModelBudgets {
  double mass_error = std::numeric_limits<double>::quiet_NaN(); // |sum mass - 1|
  double mean_error = std::numeric_limits<double>::quiet_NaN(); // |E[X_T] / X_0 - 1|
  double model_max_forward_mass_error = std::numeric_limits<double>::quiet_NaN();
  double model_max_relative_mean_error = std::numeric_limits<double>::quiet_NaN();
  std::size_t transitions_applied = 0;
  std::size_t stored_rows = 0;  // the model's logical rows
  std::size_t max_states = 0;
  std::size_t stored_volatility_values = 0;
  std::size_t output_bytes = 0; // the vectors this call returned
};

struct EuropeanOutputs {
  AnalyticsStatus status = AnalyticsStatus::Refused;
  EuropeanOutputRefusal refusal = EuropeanOutputRefusal::NoSurface;
  // The strip's own refusal when the density was produced but the strip was not.
  EuropeanOutputRefusal strip_refusal = EuropeanOutputRefusal::None;
  AnalyticsDeclaration declaration{};
  CommonModelDensity density{};
  CommonModelVarianceStrip strip{};
  CommonModelLocalVol local_vol{};
  CommonModelBudgets budgets{};
};

// The European-only outputs of `surface` (file header). A quoting surface is refused
// (status Refused, AmericanQuotingSurface, declaration.model QuotingRecord, every output
// empty / NaN). Never throws past std::bad_alloc.
[[nodiscard]] EuropeanOutputs european_outputs(const EuropeanSurfaceRef &surface,
                                               const EuropeanOutputsRequest &request = {});

[[nodiscard]] const char *to_string(EuropeanOutputRefusal value) noexcept;

} // namespace atx::vol
