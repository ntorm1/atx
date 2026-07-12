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
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"

namespace atx::vol {

struct SurfaceParityInputs;

namespace detail {
struct PreparedSliceBuilder;
}

struct ObservationKey {
  std::uint32_t expiry_index{0};
  std::uint32_t strike_index{0};
  Side side{Side::Call};

  friend bool operator==(const ObservationKey &, const ObservationKey &) noexcept = default;
  friend std::strong_ordering operator<=>(const ObservationKey &left,
                                          const ObservationKey &right) noexcept;
};

using ObservationRejectionReason = ObsRejectionReason;

enum class PreparedObservationPolicy : std::uint8_t {
  Configured = 0,
  LegacyEssviCompatibility,
};

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

[[nodiscard]] Result<CanonicalPreparedExpiry>
prepare_expiry(const Chain &chain, std::uint32_t expiry_index,
               const SurfaceParityInputs &inputs, PreparedObservationPolicy policy);

} // namespace atx::vol
