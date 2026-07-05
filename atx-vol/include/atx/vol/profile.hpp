#pragma once

// Underlier profile spine: the taxonomy that classifies each optionable
// underlier into a liquidity/kind bucket, plus the per-bucket calibration /
// filter / pricing policy the calibrator and pricer read.
//
// Ported from the C `ats-vol` library (ats_vol_profile.h / ats_vol_profile.c,
// plus the `ats_vol_profile_tier_priority` helper that lives in
// ats_calibrate_pool.c). The refactor to the atx house style
// (.agents/cpp/agent.md) drops the C's negative-integer `AtsVolStatus` channel
// for `Result<T>`, promotes the `uint8_t`-tagged enums to `enum class`, and its
// `uint8_t` boolean knobs to `bool`.
//
// One global `CalibOpts` cannot carry production policy across the whole
// optionable universe: SPY/SPX/QQQ should not be fit like a sparse small-cap,
// and earnings names should not be forced through the same smooth curve as an
// index ETF. The 7-profile taxonomy below installs that spine. A `Profile`:
//   - tells the classifier where each underlier sits;
//   - gives the calibrator its quote-filter (`FilterOpts`) and LM/robust-loss
//     knobs (`CalibOpts`);
//   - tells the pricer which American route to take (`PricingRoute`);
//   - carries the cadence at which a universe scan should refit the underlier
//     (`full_refit_ms` / `local_refit_us`).
//
// ## Reuse (nothing here is redefined)
//   - `CalibOpts` / `OptimizationLevel` / `CalibLossKind`  (calib.hpp)
//   - `FilterOpts`                                          (arb.hpp)
//   - `Parametrization` / `ResidualBasisKind`              (vol_surface.hpp)
//   - `Underlying` / `Chain` / `chain_index`               (universe.hpp)
//
// ## Thread-safety
//
// The built-in registry is immutable from process start (a function-local
// static, thread-safe under the C++11 "magic static" guarantee). The pointers
// / reference returned by `profile_lookup` / `profile_default` are stable for
// the life of the process and safe to share across threads. `classify_*` are
// pure reads of their inputs ("many readers OR one writer", matching the C
// chain contract) — safe to call concurrently against a fixed underlier.
//
// ── PORT NOTES ────────────────────────────────────────────────────────────
//  - Universe override hook (`ats_vol_universe_set_profile` /
//    `ats_vol_universe_profile_for`) is NOT ported: it mutates/reads
//    `AtsVolUnderlying::profile_ptr`, a field atx-vol's `Underlying`
//    deliberately omits (universe.hpp: "profile_ptr ... out of scope"). Adding
//    it would mean editing an existing file, which this port must not do.
//  - The profile-cadence min-heap (`AtsVolCadenceQueue`) is NOT ported here:
//    it is defined in the calibrate-pool module (ats_calibrate_pool.h), not in
//    the profile header, so it is out of this module's scope.
//  - Several C per-profile `calib.*` writes target fields that the ported
//    `CalibOpts` intentionally omits (tenor_buckets, residual_candidate_select,
//    fengler_*, selector_*, fallback_use_quality_score, use_source_vol_seed,
//    fallback_local_anchored — see the calib.hpp PORT NOTE). Those writes have
//    no destination here and are skipped; every field `CalibOpts` DOES carry is
//    set to the exact C value.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "atx/vol/arb.hpp"          // FilterOpts (reused, not redefined)
#include "atx/vol/calib.hpp"        // CalibOpts, OptimizationLevel, CalibLossKind
#include "atx/vol/curve.hpp"        // kFwdLowTDefaultYears
#include "atx/vol/types.hpp"        // Result, ErrorCode
#include "atx/vol/universe.hpp"     // Underlying, Chain, chain_index
#include "atx/vol/vol_surface.hpp"  // Parametrization, ResidualBasisKind

namespace atx::vol {

// ── Profile kind taxonomy (research §2) ──────────────────────────────────
//
// Numeric values line up with the C `AtsVolUnderlierProfileKind` enum so a
// kind round-trips across the ABI boundary unchanged.
enum class ProfileKind : std::uint8_t {
  IndexEtfUltraLiquid = 0,  // SPY, SPX, QQQ, IWM, sector ETFs
  MegaCapEvent = 1,         // AAPL, AMZN, NVDA, TSLA, META
  LiquidSingleName = 2,     // MSFT, JPM, XOM
  OrdinarySingleName = 3,   // mid-cap optionable names (the v1 default)
  IlliquidSmallCap = 4,     // sparse small caps
  HtbDividendName = 5,      // borrow-sensitive / heavy dividends
  VolProduct = 6,           // VXX, UVXY, SVIX
};

// Number of profile kinds (ATS_VOL_PROFILE_KIND_COUNT).
inline constexpr std::size_t kProfileKindCount = 7u;

// HTB underlier-flag bit. atx-vol's `Underlying::flags` stores the C
// `ATS_VOL_UFLAG_*` bitfield as a raw u32, but universe.hpp names no constant
// for the bits; the classifier reads the HTB bit, so name it here (ports
// `ATS_VOL_UFLAG_HTB`).
inline constexpr std::uint32_t kUflagHtb = 0x01u;

// ── Pricing route (research §8) ──────────────────────────────────────────
//
// `PricingRoute` is defined in types.hpp (the shared vocabulary header) so this
// config header and portfolio.hpp's per-lane diagnostic agree on one type.

// ── Profile struct ───────────────────────────────────────────────────────
//
// Aggregate value type (Rule of Zero, trivially copyable). Every member is
// initialized; the built-in registry overwrites them with the exact per-kind
// table values.
struct Profile {
  ProfileKind kind{ProfileKind::OrdinarySingleName};
  OptimizationLevel optimization_level{OptimizationLevel::Trading};
  Parametrization base_surface{Parametrization::Essvi};
  PricingRoute pricing_route{PricingRoute::B76AlCache};

  // Quote-filter knobs (fed to the pre-fit filters).
  FilterOpts filter{};

  // Calibration knobs (per-level iteration caps live inside `.calib`).
  CalibOpts calib{};

  // Economic-precision controller (research §5.3).
  double price_noise_ticks{0.0};
  double spread_vol_fraction{0.0};
  double max_residual_ticks{0.0};
  double marginal_improvement_ticks{0.0};

  // Forward / pricing.
  double forward_atm_band{0.0};
  double ewma_alpha{0.0};             // per-second EWMA decay
  double low_T_years{kFwdLowTDefaultYears};

  // Update cadence (consumed by the universe scan's resequenced warm-start).
  std::uint32_t full_refit_ms{0u};
  std::uint32_t local_refit_us{0u};

  // Sub-tick zeroing for the ILLIQUID_SMALL_CAP route: when non-zero, the
  // pricer zeroes correction values below this many ticks.
  double subtick_zeroing_ticks{0.0};
};

// ── Built-in registry ────────────────────────────────────────────────────

// The process-lifetime default profile — ORDINARY_SINGLE_NAME, the v1 fallback
// the C's `ats_vol_universe_profile_for` returns when no override is set. The
// reference is stable for the life of the process.
[[nodiscard]] const Profile& profile_default() noexcept;

// Borrowed, immutable pointer to the compiled-in default profile for `kind`
// (ports `ats_vol_profile_default`). The pointer is stable for the life of the
// process. Kinds without their own table entry route to the nearest default
// (VOL_PRODUCT -> spy-like, HTB_DIVIDEND_NAME -> ordinary), matching the C.
// @return NotFound if `kind` is not a valid enumerator (the C's NULL-on-invalid).
[[nodiscard]] Result<const Profile*> profile_lookup(ProfileKind kind);

// Cold-fast profile factory (Sprint 15a Phase B): derive a latency-prioritized
// COLD_FAST variant from `base`. Returns a VALUE the caller owns. Ports
// `ats_vol_profile_make_cold_fast` (which took a nullable pointer; a const-ref
// cannot be null, so the C's zero-init-on-NULL branch is unreachable here).
[[nodiscard]] Profile profile_make_cold_fast(const Profile& base) noexcept;

// Rebuild-scheduler tier priority for `kind` (lower = fits sooner). Ports
// `ats_vol_profile_tier_priority` (ats_calibrate_pool.c) — a pure ProfileKind
// mapping folded into the profile spine.
[[nodiscard]] std::uint8_t profile_tier_priority(ProfileKind kind) noexcept;

// ── OPRA tick-size lookup (research §1.1, §5.1) ──────────────────────────

// Minimum quoting increment (USD) for a contract whose mid-price is `price`
// (Nasdaq ISE Options 3 Section 3 + SEC Rel. 34-87681). Ports
// `ats_vol_tick_size`:
//   - penny-pilot underliers (SPY, QQQ, IWM, ...): 1c across all prices;
//   - otherwise (Penny Interval Program): 1c below $3, 5c at/above $3;
//   - a non-finite or negative `price` falls through to 5c.
[[nodiscard]] double tick_size(double price, bool is_penny_pilot) noexcept;

// ── Classifier ───────────────────────────────────────────────────────────

// Inputs the heuristic classifier consumes (ports `AtsVolClassifierInputs`).
// All "rolling" metrics are supplied by the caller — the classifier keeps no
// window. It is policy-free: same inputs in, same kind out.
struct ClassifierInputs {
  std::uint32_t n_live_quotes{0u};
  std::uint32_t n_live_expiries{0u};
  std::uint32_t n_atm_quotes{0u};     // quote count inside |k_log| < 0.05
  double median_spread_pct{0.0};      // (ask-bid)/mid, median across active
  bool has_zerodte{false};
  bool has_weeklies{false};
  bool htb_flag{false};               // kUflagHtb on the underlier
  bool vol_product{false};            // operator hint: VXX-family ticker?
  std::uint16_t n_dividends{0u};      // upcoming divs in the next 12mo
  std::uint16_t event_distance_days{0u};  // days until next earnings/event
  double forward_dispersion_bp{0.0};  // p90 PCP-RMSE / F across expiries
  double median_q_eff{0.0};           // median q across expiries (HTB sniff)
};

// Classifier verdict: the chosen kind plus a [0, 1] "how many heuristics agree"
// confidence. A confidence below 0.7 means the caller should fall back to the
// previous profile and request an operator override.
//
// PORT NOTE — the C surfaced (out_kind, out_confidence) through out-params and
// an `int` status. The classifier never fails, so this port returns the pair by
// value with no `Result` wrapper (hard rule 2: "classifier returns the enum
// directly", i.e. not through the error channel).
struct ProfileVerdict {
  ProfileKind kind{ProfileKind::OrdinarySingleName};
  double confidence{0.0};
};

// Classify from supplied rolling metrics (ports `ats_vol_classify_profile`).
// Simple liquidity / event / borrow heuristics; not a learned model.
[[nodiscard]] ProfileVerdict classify_profile(const ClassifierInputs& in) noexcept;

// Convenience: build a `ClassifierInputs` from an underlier's live chain state
// and dispatch (ports `ats_vol_classify_underlier`). The underlier's profile is
// NOT updated — the caller decides whether the kind is stable enough to commit.
//
// PORT NOTE — the C also read `n_dividends` / `median_q_eff` from
// `under->curves`; atx-vol's `Underlying` carries no curve link, so those two
// (and `event_distance_days` / `forward_dispersion_bp`, which the C set to 0)
// stay 0. None are consumed by the vote, so the verdict matches the C.
[[nodiscard]] ProfileVerdict classify_underlier(const Underlying& under) noexcept;

// Ticker-aware classifier (Sprint 24): when `ticker` matches a compiled-in seed
// entry, return the seed kind with confidence 0.95 and skip quote aggregation.
// Falls back to `classify_underlier` when `ticker` is empty or not seeded.
[[nodiscard]] ProfileVerdict classify_underlier_with_ticker(
    const Underlying& under, std::string_view ticker) noexcept;

}  // namespace atx::vol
