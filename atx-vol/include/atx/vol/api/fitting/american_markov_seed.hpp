#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/fitting/american_markov_fit.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {

inline constexpr std::size_t kAmericanMarkovSeedMaxExpiries = 1024;
inline constexpr std::size_t kAmericanMarkovSeedMaxInversions = 3072;
inline constexpr std::size_t kAmericanMarkovSeedMaxIterationSlots = 393216;

struct AmericanMarkovSeedLayout {
  AmericanMarkovFitTimeShape time_shape = AmericanMarkovFitTimeShape::Shared;
  std::span<const double> log_state_knots; // Empty means {0}, as in the fitter.
  double min_sigma = 0.01;
  double max_sigma = 3.0;
  std::size_t max_parameters = 32;
};

// BidMidAsk (default) inverts bid, mid and ask of tight two-sided near-forward
// quotes and refuses bidless or wide ones. SigmaInterval is opt-in for sparse
// markets: each near-forward quote contributes one-sided bounds on its
// flat-to-expiry equivalent sigma (bid -> lower bound when bid exceeds the
// intrinsic floor, ask -> upper bound), so bidless and wide quotes still count.
// max_relative_spread is not applied. See estimate_american_markov_quote_seeds.
enum class AmericanMarkovSeedEstimator { BidMidAsk, SigmaInterval };

struct AmericanMarkovSeedConfig {
  double max_abs_log_forward_moneyness = 0.2;
  double max_relative_spread = 0.5;
  double min_time_value_over_spot = 1.0e-8;
  double iv_tolerance = 1.0e-6;
  std::uint16_t max_iv_iterations = 32;
  std::size_t max_input_quotes = kAmericanMarkovFitMaxQuotes;
  std::size_t max_expiries = 128;
  std::size_t candidates_per_expiry = 5;
  std::size_t max_inversions = 384;
  // Reserve max_iv_iterations per attempted inversion, including failed calls.
  // Counts root-loop capacity, not actual iterations or all pricing work.
  std::size_t max_root_iteration_slots = 12288;
  AlOpts inversion_options{.n_collocation = 7, .n_quadrature = 16,
      .n_quad_price = 16, .max_newton_iter = 4, .tol = 1.0e-8};
  AmericanMarkovSeedEstimator estimator = AmericanMarkovSeedEstimator::BidMidAsk;
};

enum class AmericanMarkovSeedQuoteStatus {
  Used, InvalidQuote, DuplicateContract, UnsupportedCashSchedule, ZeroVariance,
  Bidless, WideSpread, FarMoneyness, IntrinsicPlateau, CandidateLimit,
  InversionBudget, InversionFailure, IvFloor, SigmaBounds, InconsistentIvInterval,
  NonfiniteVariance
};

// OneSidedInterval (SigmaInterval only): the market bounds sigma on one side
// only. Closing the open side with a layout sigma bound would make an admission
// constant the seed, so the parameter is refused and the bound is reported.
enum class AmericanMarkovSeedParameterStatus {
  Ready, MissingEndpoint, NegativeForwardVariance, UncertainForwardVariance,
  SigmaBounds, UnsupportedCashSchedule, NonfiniteVariance, OneSidedInterval
};

// Bid/mid/ask estimates, or propagated support bounds. These are heuristic
// support/dispersion measures, NOT statistical confidence or arbitrage bounds.
struct AmericanMarkovSeedVarianceBand {
  double lower = 0.0;
  double midpoint = 0.0;
  double upper = 0.0;
};

struct AmericanMarkovSeedQuoteDiagnostics {
  AmericanMarkovSeedQuoteStatus status = AmericanMarkovSeedQuoteStatus::InvalidQuote;
  bool eligible = false; // Eligible before deterministic candidate/budget selection.
  std::size_t inversion_calls = 0;
  // Each attempted successful IV: bid/mid/ask. SigmaInterval never inverts mid.
  std::array<std::optional<double>, 3> equivalent_vols;
  std::string inversion_error; // Original library error code and message.
  // SigmaInterval only: which successful IVs became bounds. An IV below
  // min_sigma (bid) or above max_sigma (ask) is uninformative, not a bound.
  bool lower_bound = false;
  bool upper_bound = false;
};

// SigmaInterval only. Feasible equivalent-sigma interval over a quote set:
// lower = max bid-implied bound, upper = min ask-implied bound. A missing side
// is replaced by the layout sigma bound ("effective") for the descriptive
// fields only. The seed is the geometric midpoint
// sqrt(effective_lower * effective_upper) ONLY when both sides are market
// bounds; a one-sided interval's midpoint depends on a layout constant and is
// never a seed (OneSidedInterval). crossed means effective_lower >
// effective_upper (inconsistent quotes or term structure); the midpoint is then
// still between the two crossed bounds and is reported, never repaired.
// log_width = log(effective_upper / effective_lower).
struct AmericanMarkovSeedSigmaInterval {
  std::optional<double> lower;
  std::optional<double> upper;
  double effective_lower = 0.0;
  double effective_upper = 0.0;
  double geometric_midpoint = 0.0;
  double log_width = 0.0;
  bool crossed = false;
  std::size_t lower_bound_quotes = 0;
  std::size_t upper_bound_quotes = 0;
};

struct AmericanMarkovSeedExpiryDiagnostics {
  double calendar_end = 0.0;
  double variance_end = 0.0;
  double integrated_rate = 0.0;
  double integrated_yield = 0.0;
  std::size_t eligible_quotes = 0;
  std::size_t used_quotes = 0;
  std::optional<AmericanMarkovSeedVarianceBand> total_variance;
  double total_variance_mad = 0.0;
  double median_relative_spread = 0.0;
  std::optional<AmericanMarkovSeedSigmaInterval> sigma_interval; // SigmaInterval only.
};

struct AmericanMarkovSeedEndpointDiagnostics {
  double variance_end = 0.0;
  std::vector<std::size_t> expiry_indices; // Includes stopped-variance expiries.
  std::size_t supported_expiries = 0;
  std::optional<AmericanMarkovSeedVarianceBand> total_variance;
  double total_variance_mad = 0.0;
  std::optional<AmericanMarkovSeedSigmaInterval> sigma_interval; // SigmaInterval only.
};

struct AmericanMarkovSeedParameter {
  AmericanMarkovSeedParameterStatus status = AmericanMarkovSeedParameterStatus::MissingEndpoint;
  std::optional<double> sigma;
  std::optional<AmericanMarkovSeedVarianceBand> variance_rate;
  // OneSidedInterval only: the market bounds of the refusing interval, in
  // equivalent flat-to-expiry sigma. Exactly one is set; the other side is
  // open. Shared: the global interval. PerVarianceInterval: the first
  // one-sided endpoint of the pair this parameter differences.
  std::optional<double> interval_lower;
  std::optional<double> interval_upper;
};

struct AmericanMarkovQuoteSeeds {
  // Complete group-major vector only. Empty if any parameter remains unseeded.
  // No clipping, missing-expiry interpolation, default fills or prior inference.
  std::vector<double> initial_sigmas;
  std::vector<double> log_state_knots;
  std::vector<AmericanMarkovSeedParameter> parameters;
  std::vector<AmericanMarkovSeedQuoteDiagnostics> quotes; // Original input order.
  std::vector<AmericanMarkovSeedExpiryDiagnostics> expiries;
  std::vector<AmericanMarkovSeedEndpointDiagnostics> variance_endpoints;
  AmericanMarkovFitTimeShape time_shape = AmericanMarkovFitTimeShape::Shared;
  double min_sigma = 0.0;
  double max_sigma = 0.0;
  std::size_t parameter_capacity = 0;
  std::size_t time_groups = 0;
  std::size_t eligible_quotes = 0;
  std::size_t used_quotes = 0;
  std::size_t refused_quotes = 0; // Includes eligible but unselected quotes.
  std::size_t inversion_calls = 0;
  std::size_t reserved_root_iteration_slots = 0;
  // SigmaInterval + Shared only: the interval over every used quote, whose
  // geometric midpoint is the single seed. Absent when no quote bounded sigma.
  std::optional<AmericanMarkovSeedSigmaInterval> sigma_interval;
  AmericanMarkovSeedConfig configuration;
};

// Pure, bounded, opt-in offline initializer. No model is fitted or compiled;
// state_grid and model_config are never changed or used to relax fitter gates.
// All returned data is owned. Invalid global config/schedule returns an error;
// unsupported dated cash and unusable quotes produce explicit diagnostics.
// VarianceTimeKnots is explicitly unsupported and returns before inversion;
// Shared/PerVarianceInterval seeds are never silently projected to knot controls.
//
// Inversion convention: Andersen-Lake at T=variance_end, r=integral(r dt)/T,
// q=integral(q dt)/T. Aggregate-PV schedules already converted to continuous
// yield may be used; dated cash is refused. Flattening carry/exercise is an
// approximation. Equivalent IV is not local volatility or an arbitrage test.
// Each endpoint contributes equally to Shared; stopped-variance expiries first
// aggregate within their endpoint. PerVarianceInterval differences total variance
// only when every adjacent support interval is strictly positive. All state
// knots receive the same time-group seed; no smile recovery is claimed.
// Medians use equal quote weights; fitter quote weights are validated but never
// repurposed as statistical confidence. Reported support/MAD is descriptive only.
//
// SigmaInterval (opt-in): same convention, candidate cap, round-robin and
// budgets, but every near-forward quote is eligible (bidless and wide too),
// a bid at or below the intrinsic floor gives no lower bound, and each quote
// needs only the inversions of its informative sides (1 or 2, never mid).
// A quote is Used when it yields at least one bound. Shared seeds every knot
// with the geometric midpoint of the interval over ALL used quotes (all
// expiries, equal quote weight). PerVarianceInterval intersects within each
// variance endpoint, then differences geometric-midpoint total variance
// (no adjacent-support requirement: Ready iff the forward rate is positive,
// finite and inside the sigma bounds). A parameter is refused when its
// interval is unbounded on both sides (MissingEndpoint), bounded on one side
// only (OneSidedInterval: no point seed from a layout bound; for
// PerVarianceInterval either differenced endpoint being one-sided refuses),
// or the forward difference fails. Interval width is descriptive, not
// confidence.
//
// IMPORTANT: this function never applies its output. The fitter interprets an
// empty prior_sigmas as INITIAL. A caller applying these seeds while preserving
// its old regularization prior MUST first materialize that old prior explicitly.
// All original quotes, including seed refusals, must remain in the eventual fit.
[[nodiscard]] Result<AmericanMarkovQuoteSeeds> estimate_american_markov_quote_seeds(
    const AmericanMarkovFitMarket &market, std::span<const AmericanMarkovFitQuote> quotes,
    const AmericanMarkovSeedLayout &layout = {}, const AmericanMarkovSeedConfig &config = {});

[[nodiscard]] const char *to_string(AmericanMarkovSeedQuoteStatus status) noexcept;
[[nodiscard]] const char *to_string(AmericanMarkovSeedParameterStatus status) noexcept;

} // namespace atx::vol
