#pragma once

// Canonical, family-neutral fitting preparation records.
//
// `PreparedSlice` assigns a stable key to every preferred quote row, retains
// accepted European-equivalent fit rows and rejected-row provenance, and owns
// the raw scoring columns used after a curve is fit. Curve families consume
// only `fit_observations()`; family choice cannot change the prepared
// population. `PreparedBoard` canonicalizes slice order by expiry key.
//
// `LegacyEssviCompatibility` is a bounded migration seam for the historical
// eSSVI surface driver. It reproduces that driver's permissive quote predicate,
// direct de-Americanization, and exact weight arithmetic. New consumers must
// use `Configured`, which applies `CalibOpts` through the shared calibration
// builder. The two policies are intentionally not semantically equivalent.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/calib.hpp"
#include "atx/vol/deamer.hpp"
#include "atx/vol/detail/prepared_policy.hpp" // PreparedObservationPolicy (leaf definition)
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"

namespace atx::vol {

struct SurfaceParityInputs;

namespace detail {
struct PreparedSliceBuilder;

// Deterministic moneyness-spread subsample used by the legacy observation-prep
// de-Am cap (`CalibOpts::max_deam_strikes_per_expiry`). Given the log-moneyness
// (log(K/F)) of each candidate strike, in candidate order, returns a parallel
// mask (1 = keep / de-Americanize, 0 = drop) selecting at most `cap` strikes:
//   - the two extreme-wing candidates are always kept (outer spline knots);
//   - the near-ATM core is kept densely (highest signal + vega);
//   - the intermediate strikes are thinned by an even stride in moneyness.
// The result is deterministic (same input → same mask) and never keeps more
// than `cap`. With `cap == 0` or `moneyness.size() <= cap` every candidate is
// kept (all-ones mask), so the caller's uncapped path stays bit-identical.
[[nodiscard]] std::vector<char> select_deam_spread(const std::vector<double> &moneyness,
                                                   std::uint32_t cap);
} // namespace detail

struct ObservationKey {
  std::uint32_t expiry_index{0};
  std::uint32_t strike_index{0};
  Side side{Side::Call};

  friend bool operator==(const ObservationKey &, const ObservationKey &) noexcept = default;
  friend std::strong_ordering operator<=>(const ObservationKey &left,
                                          const ObservationKey &right) noexcept;
};

using ObservationRejectionReason = ObsRejectionReason;

// PreparedObservationPolicy is defined in atx/vol/detail/prepared_policy.hpp (included
// above) — split into a leaf header so structs whose default member initializer
// names ::Configured (SurfaceParityInputs, SessionInputs, PricerConfig) get the
// complete enum without the include cycle prepared_fitting.hpp would create.
// DEFERRED (invariant #4.9 — explicit, not silent): the served eSSVI path
// deliberately prepares under LegacyEssviCompatibility for byte-compatibility
// with the historical cold driver, even though the family selector scores
// candidates under Configured. Unifying the two so the served path also runs
// Configured (identical bits) is a planned follow-up (WP4 residual).

inline constexpr std::size_t kMinPreparedFitRows = 5u;

struct ObservationRejection {
  ObservationKey key{};
  ObservationRejectionReason reason{ObservationRejectionReason::None};

  friend bool operator==(const ObservationRejection &,
                         const ObservationRejection &) noexcept = default;
};

struct PreparedObservation {
  ObservationKey key{};
  ObservationRejectionReason rejection{ObservationRejectionReason::None};
  double bid{0.0};
  double ask{0.0};
  double raw_mid{0.0};
  double european_iv{0.0};
  double weight_w{0.0};

  [[nodiscard]] bool accepted() const noexcept {
    return rejection == ObservationRejectionReason::None;
  }
};

// Prepared scoring SoA. `score_keys()` is its parallel stable-key column;
// chain_parity consumes these contiguous values without rebuilding temporary
// vectors after every fit.
struct PreparedScoreColumns {
  std::vector<double> strike{};
  std::vector<double> bid{};
  std::vector<double> ask{};
  std::vector<double> mid{};
  std::vector<double> k_log{};
  std::vector<double> market_iv{};
  std::vector<Side> side{};
};

struct SlicePreparationProvenance {
  PreparedObservationPolicy policy{PreparedObservationPolicy::Configured};
  ExerciseStyle exercise_style{ExerciseStyle::American};
  AmericanMethod method{AmericanMethod::AndersenLake};
  std::optional<AlOpts> al_opts{};
  double iv_tolerance{1.0e-7};
  std::uint16_t iv_max_iterations{64};
  double S{0.0};
  double r{0.0};
  double q_eff{0.0};
  double df{0.0};
  bool call_cache{false};
  bool put_cache{false};
  std::uint32_t n_score_inversions{0};
};

struct PreparedSliceInputs {
  std::uint32_t expiry_index{0};
  double S{0.0};
  double r{0.0};
  double F{0.0};
  double q_eff{0.0};
  double df{0.0};
  CalibOpts calib{};
  AmericanCorrectionCaches caches{};
  std::optional<AlOpts> al_opts{};
  double iv_tolerance{1.0e-7};
  std::uint16_t iv_max_iterations{64};
  AmericanMethod method{AmericanMethod::AndersenLake};
  PreparedObservationPolicy policy{PreparedObservationPolicy::Configured};
  bool prepare_scoring{true};
  // Correctness-first serving (charter §8.1): under `audit_fit_inversions` the
  // LegacyEssviCompatibility builder reprices every fitted IV proposal against
  // the cold Andersen-Lake reference; a failed proposal is recomputed
  // accurately and re-audited, and a row that still misses the half-spread
  // budget is DROPPED, never fitted. Default false keeps the historical fit
  // bit-identical. Configured preparation is always audited inside either
  // build_observations (direct European) or build_observations_european
  // (American de-Am) and ignores these two knobs.
  bool audit_fit_inversions{false};
  double max_iv_residual_half_spreads{0.25};
  // OUT (optional): legacy-path preparation tallies, written just before the
  // usable-row floor check so a caller can distinguish an audit-starved thin
  // slice (rows + audit drops would have met the floor) from a genuinely
  // sparse one even when create() fails the floor. Configured preparation
  // leaves them untouched (its audit ledger lives in `deam_audit()`).
  std::uint32_t *out_legacy_fit_rows{nullptr};
  std::uint32_t *out_legacy_audit_dropped{nullptr};
};

class PreparedSlice {
public:
  // Pure preparation. The returned value owns every row and contains no
  // references into `chain` or the optional correction caches.
  [[nodiscard]] static Result<PreparedSlice> create(const Chain &chain,
                                                    const PreparedSliceInputs &inputs);

  [[nodiscard]] std::uint32_t expiry_index() const noexcept { return expiry_index_; }
  [[nodiscard]] double maturity() const noexcept { return maturity_; }
  [[nodiscard]] double forward() const noexcept { return forward_; }
  [[nodiscard]] std::uint32_t n_dropped() const noexcept { return n_dropped_; }
  [[nodiscard]] const SlicePreparationProvenance &provenance() const noexcept {
    return provenance_;
  }
  [[nodiscard]] std::span<const PreparedObservation> observations() const noexcept {
    return observations_;
  }
  [[nodiscard]] std::span<const ObservationRejection> rejections() const noexcept {
    return rejections_;
  }
  [[nodiscard]] std::span<const FitObs> fit_observations() const noexcept { return fit_rows_; }
  [[nodiscard]] std::span<const ObservationKey> score_keys() const noexcept { return score_keys_; }
  [[nodiscard]] const PreparedScoreColumns &score_columns() const noexcept {
    return score_columns_;
  }
  // Row-level de-Americanization audit for the Configured policy (route
  // counters, residual quantiles, accepted/dropped ledger) — the certification
  // layer's `deam_inversion_certified` input. Default (never certifies) under
  // LegacyEssviCompatibility, which has no audited inversion route.
  [[nodiscard]] const DeAmAuditDiagnostics &deam_audit() const noexcept { return deam_audit_; }

private:
  friend struct detail::PreparedSliceBuilder;

  std::uint32_t expiry_index_{0};
  double maturity_{0.0};
  double forward_{0.0};
  std::uint32_t n_dropped_{0};
  SlicePreparationProvenance provenance_{};
  std::vector<PreparedObservation> observations_{};
  std::vector<ObservationRejection> rejections_{};
  std::vector<FitObs> fit_rows_{};
  std::vector<ObservationKey> score_keys_{};
  PreparedScoreColumns score_columns_{};
  DeAmAuditDiagnostics deam_audit_{};
};

class PreparedBoard {
public:
  // Sorts by expiry key and rejects duplicate keys. Strong guarantee: input is
  // consumed only into the returned value or destroyed on error.
  [[nodiscard]] static Result<PreparedBoard> create(std::vector<PreparedSlice> slices);

  [[nodiscard]] std::span<const PreparedSlice> slices() const noexcept { return slices_; }

private:
  std::vector<PreparedSlice> slices_{};
};

// Fully resolved, owned preparation for one expiry. This is the canonical seam
// shared by cold drivers and facade-owned incremental refit: callers never
// construct American fit rows or recompute carry independently.
struct CanonicalPreparedExpiry {
  PreparedSlice slice{};
  double rate{0.0};
  double borrow{0.0};
  double q_eff{0.0};
  double df{0.0};
};

// Optional preparation-outcome diagnostics for prepare_expiry: filled on both
// success and failure so a caller can SURFACE why an expiry produced no slice
// (§5.2: carry-dropped and audit-starved expiries are counted, never hidden)
// and reuse the fit resolve's carry diagnostics without a second
// resolve_chain_forward call.
struct PrepareExpiryDiagnostics {
  // Carry resolution failed (resolve error or degenerate forward): the expiry
  // was dropped by carry, not by observation counts.
  bool carry_failed{false};
  bool carry_available{false}; // `carry` below is meaningful
  CarryDiagnostics carry{};    // the fit resolve's carry diagnostics
  // Legacy-path tallies (see PreparedSliceInputs out-params). Zero under
  // Configured preparation.
  std::uint32_t n_fit_rows{0};
  std::uint32_t n_audit_dropped{0};
  // Populated only when SurfaceParityInputs::collect_stage_timings is true.
  double carry_solve_ms{0.0};
  double observation_deam_ms{0.0};
};

[[nodiscard]] Result<CanonicalPreparedExpiry>
prepare_expiry(const Chain &chain, std::uint32_t expiry_index, const SurfaceParityInputs &inputs,
               PreparedObservationPolicy policy, PrepareExpiryDiagnostics *diag = nullptr);

} // namespace atx::vol
