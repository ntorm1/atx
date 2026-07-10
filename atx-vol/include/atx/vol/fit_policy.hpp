#pragma once

// Unified cold-fit policy selection.  This is the single seam between caller
// intent, observable board features, the seven underlier profiles, fit presets,
// and curve families.  It deliberately performs no de-Americanization or curve
// fitting: high-confidence boards route in O(number of quotes), while ambiguous
// boards ask PricerFitter to run the more expensive held-out selector.

#include <cstdint>
#include <optional>
#include <string_view>

#include "atx/vol/profile.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol {

enum class FitSelectionMode : std::uint8_t {
  Auto = 0,           // profile-first; held-out validation only when ambiguous
  CrossValidated = 1, // always run the held-out curve selector
};

enum class MarketSessionPhase : std::uint8_t {
  Unknown = 0,
  Opening = 1, // opening auction / first minutes: incomplete, rapidly moving board
  Continuous = 2,
  Closing = 3, // final minutes / closing-auction transition
};

enum class EventPhase : std::uint8_t {
  None = 0,
  PreAnnouncement = 1,
  PostAnnouncement = 2,
};

enum class FitDecisionSource : std::uint8_t {
  ProfileOverride = 0,
  TickerPrior = 1,
  BoardFeatures = 2,
  SparseGuard = 3,
  CrossValidation = 4,
};

// Optional facts not recoverable reliably from one OPRA snapshot.  A feed
// handler/calendar service can populate these without changing the fit API.
struct FitContext {
  std::optional<ProfileKind> profile_override{};
  MarketSessionPhase session_phase{MarketSessionPhase::Unknown};
  EventPhase event_phase{EventPhase::None};
  std::optional<std::uint16_t> event_distance_days{};
  std::optional<double> forward_dispersion_bp{};
  std::optional<double> median_q_eff{};
  std::optional<bool> htb{};
  bool vol_product{false};
};

struct FitPolicyConfig {
  FitSelectionMode mode{FitSelectionMode::Auto};
  // A low-confidence liquid/ordinary classification is validated out of sample.
  double min_direct_confidence{0.70};
  bool validate_ambiguous{true};
  // Below this many live quote legs a board is "thin", with two consequences: a
  // board-voted verdict cannot support a useful even/odd holdout (route it
  // directly to the parsimonious SVI family instead of paying for a doomed
  // validation pass), and no board -- however confidently classified -- can
  // identify C8's eight free parameters.
  std::uint32_t sparse_validation_floor{600};
  // Adaptive knot budget for the dense index/ETF HFT route. The cap is a
  // calibration input rather than a curve-local field, so PricerFitter applies it
  // when materializing SessionInputs -- and an explicit PricerConfig
  // ::max_obs_per_slice overrides it.
  std::uint32_t dense_node_cap{48};
};

struct FitDecision {
  ClassifierInputs features{};
  ProfileVerdict profile{};
  FitDecisionSource source{FitDecisionSource::BoardFeatures};
  FitPreset preset{FitPreset::Fast};
  CurveConfig curve{};
  CurveConfig primary_curve{};
  bool needs_cross_validation{false};
  bool used_fallback{false};
};

[[nodiscard]] FitDecision select_fit_policy(const Underlying &under, std::string_view ticker,
                                            const FitContext &context = {},
                                            const FitPolicyConfig &config = {}) noexcept;

} // namespace atx::vol
