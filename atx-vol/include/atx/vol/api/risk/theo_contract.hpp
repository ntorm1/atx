#pragma once

// theo_contract — a RELATIVE option template ("7 calendar days, 20-delta put",
// "0.25 years, log-moneyness -0.05") resolved on any fitted surface at any
// valuation instant into a concrete OptionLeg (absolute expiry, absolute strike)
// with its implied vol, mark and Greeks, and the series form that holds (T, k) or
// (T, delta) fixed across a vector of historical surfaces.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner).
//
// The user's walk-through:
//
//   TheoContract p20{uid, MaturitySpec::absolute(exp_2026_10_30),
//                    StrikeCoord::delta(-0.20, DeltaConvention::Forward), Side::Put};
//   ResolvedTheo today = resolve_theo(p20, today_surface, ts_today, policy).value();
//   TheoContract same{uid, MaturitySpec::years(today.leg_T),
//                     StrikeCoord::log_moneyness(today.k_log), Side::Put};
//   project_series(same, hist_surfaces, hist_ts, policy, out); // same TTE and k on
//                                                               // every historical state
//
// Greeks on a resolved theo contract come from the surface's COLD analytic route
// (PricedSurface::greeks_analytic): a theoretical contract is a definition, not a
// hot path. For HFT-rate Greeks put the resolved legs in a RiskBook and evaluate it
// with the family kernel.

#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp" // AmericanGreeks
#include "atx/vol/api/risk/risk_book.hpp"   // OptionLeg
#include "atx/vol/api/risk/risk_types.hpp"
#include "atx/vol/api/risk/strike_coord.hpp"

namespace atx::vol {

enum class MaturityKind : std::uint8_t {
  Years = 0,          // T = value (year-fraction under the policy's TimeSpec; the
                      // absolute instant is valuation + value years, Calendar365 inverse)
  CalendarDays = 1,   // expiry = valuation + days, 16:00 ET settlement of that date
  CalendarMonths = 2, // same calendar day `months` later (clamped to month end)
  Absolute = 3,       // expiry_ns given
};

struct MaturitySpec {
  MaturityKind kind{MaturityKind::Years};
  double years{0.25};
  std::int32_t calendar_count{0};
  std::int64_t expiry_ns{0};

  [[nodiscard]] static MaturitySpec years_of(double value) noexcept;
  [[nodiscard]] static MaturitySpec days(std::int32_t value) noexcept;
  [[nodiscard]] static MaturitySpec months(std::int32_t value) noexcept;
  [[nodiscard]] static MaturitySpec absolute(std::int64_t expiry_ns) noexcept;
  [[nodiscard]] bool operator==(const MaturitySpec &) const = default;
};

struct TheoContract {
  std::uint32_t uid{0};
  MaturitySpec maturity{};
  StrikeCoord strike{};
  Side side{Side::Call};
  double multiplier{100.0};
  [[nodiscard]] bool operator==(const TheoContract &) const = default;
};

enum class TheoStatus : std::uint8_t {
  Ok = 0,
  InvalidSpec = 1,
  SurfaceUnavailable = 2,
  MaturityUnavailable = 3, // T <= 0 at valuation, or Forbid + tenor extrapolation
  DeltaUnreachable = 4,
  PricingError = 5,
};

struct ResolvedTheo {
  OptionLeg leg{};          // uid, K, expiry_ns, side, multiplier; qty = 0
  std::int64_t valuation_ns{0};
  double T{0.0};            // residual year-fraction at valuation
  double forward{0.0};
  double k_log{0.0};
  double standard_moneyness{0.0};
  double delta{0.0};        // signed, under the policy's delta_conv
  double forward_delta{0.0}; // ALWAYS the Black-76 forward delta at (K, T, iv): the
                             // vendor quote coordinate, kept beside `delta` so a series
                             // can be re-keyed without a second solve
  double iv{0.0};
  double mark{0.0};         // American model mark (cold)
  AmericanGreeks greeks{};  // cold analytic bundle at the resolved point
  std::uint16_t delta_evaluations{0};
  RiskFlags flags{0};
  TheoStatus status{TheoStatus::Ok};
  std::uint64_t fingerprint{0}; // nonzero identity of (uid, K bits, expiry_ns, side)
  [[nodiscard]] bool operator==(const ResolvedTheo &) const = default;
};

// Absolute expiry instant for `spec` anchored at `valuation_ns` under `time`.
// MaturityKind::Years under TimeConvention::VolTime is NotImplemented (the vol-time
// clock has no closed inverse; use CalendarDays / Absolute there).
[[nodiscard]] Result<std::int64_t> resolve_maturity(std::int64_t valuation_ns,
                                                    const MaturitySpec &spec,
                                                    const TimeSpec &time);

// Resolve ONE template on ONE surface at `valuation_ns`. A market failure (delta
// unreachable, surface unavailable, pricing error, maturity unavailable) is reported
// in `status`; every non-Ok row follows ONE rule: identity fields, T, forward and
// flags are kept, every strike-level number (K, k_log, moneyness, deltas, iv, mark,
// Greeks) is NaN and `fingerprint` is 0. The Result is an error only for a
// structural problem (null surface, malformed spec or policy).
[[nodiscard]] Result<ResolvedTheo> resolve_theo(const TheoContract &spec,
                                                const SurfaceRef &surface,
                                                std::int64_t valuation_ns,
                                                const ProjectionPolicy &policy = {});

// Many templates on one surface. `out.size() == specs.size()`; per-row market
// failures land in `out[i].status`. Rows are resolved independently (fanned over the
// pricing executor); each row's result is bit-equal to `resolve_theo` and independent
// of batch composition.
[[nodiscard]] Status resolve_theo_many(std::span<const TheoContract> specs,
                                       const SurfaceRef &surface, std::int64_t valuation_ns,
                                       const ProjectionPolicy &policy,
                                       std::span<ResolvedTheo> out);

// One template across a HISTORY: surfaces[i] at valuation ts[i] -> out[i]. For a
// Years / CalendarDays / CalendarMonths maturity the TTE is held fixed; for a
// LogMoneyness / StandardMoneyness strike the moneyness is held fixed; for a Delta
// strike the delta is re-solved on each surface. Structural errors fail the call;
// per-row market failures land in `out[i].status`.
[[nodiscard]] Status project_series(const TheoContract &spec, std::span<const SurfaceRef> surfaces,
                                    std::span<const std::int64_t> valuation_ns,
                                    const ProjectionPolicy &policy,
                                    std::span<ResolvedTheo> out);

[[nodiscard]] const char *to_string(TheoStatus value) noexcept;
[[nodiscard]] const char *to_string(MaturityKind value) noexcept;

} // namespace atx::vol
