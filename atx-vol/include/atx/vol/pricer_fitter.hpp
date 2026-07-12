#pragma once

// PricerFitter — the atx-vol library lifecycle in one object: fit a surface from
// an OptionChain, OWN it (unique_ptr<FittedSurface>), and price the chain in
// parallel with per-field output flags.
//
//   OptionChain chain = OptionChain::from_frame(frame, r, spot).value();
//   PricerFitter fitter{ PricerConfig{ .preset = FitPreset::Robust } };
//   fitter.fit(chain);                                   // stores the surface
//   auto val = fitter.value_chain(chain,                 // parallel valuation
//       OutputField::ModelPrice | OutputField::BidIV | OutputField::AskIV);
//   // ... a tick arrives ...
//   chain.update_quotes(ids, bids, asks);                // replace bid/ask by id
//   auto bands = fitter.value_chain(chain, OutputField::Bands);   // re-price
//
// The fitter composes the validated primitives — `VolaSession` (fit + cached
// query), `american_implied_vol` (cold IV inversion), `VolSurface` — so every
// number it produces is bit-consistent with those. It adds the two things the
// facade needs on top: ownership of the fitted surface, and a deterministic
// multi-threaded evaluator over the whole chain.
//
// Thread-safety: `fit` mutates (stores the surface) and needs exclusive access.
// `value_chain` is const and internally parallel; concurrent `value_chain` calls
// on one fitter are safe (all state read is immutable after `fit`).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"       // AmericanGreeks
#include "atx/vol/chain.hpp"          // OptionChain, OptionId
#include "atx/vol/curve.hpp"          // DividendEvent
#include "atx/vol/curve_selector.hpp" // SelectorConfig, SelectorResult
#include "atx/vol/fit_policy.hpp"     // FitContext, FitPolicyConfig, FitDecision
#include "atx/vol/session.hpp"        // VolaSession, FitPreset, SessionDiagnostics
#include "atx/vol/surface_policy.hpp" // explicit mark/risk purpose and quality policy
#include "atx/vol/types.hpp"          // Result, Status, Side
#include "atx/vol/vol_curve.hpp"      // CurveConfig, VolCurveKind

namespace atx::vol {

// ── Output-field selector ──────────────────────────────────────────────────
//
// Which per-option outputs a chain valuation should populate. Bitmask; combine
// with `|`. Unrequested scalar columns stay empty; a requested-but-failed cell
// is NaN, so a bad quote never sinks the rest of the valuation.
enum class OutputField : std::uint32_t {
  None = 0,
  ModelPrice = 1u << 0, // re-Americanized model fair value at (K, T, side)
  ModelIV = 1u << 1,    // surface European-equivalent IV at (K, T)
  BidIV = 1u << 2,      // American implied vol of the bid (on the fit's carry)
  AskIV = 1u << 3,      // American implied vol of the ask
  MidIV = 1u << 4,      // American implied vol of the mid
  Greeks = 1u << 5,     // full AmericanGreeks bundle at the model IV
  Prices = ModelPrice | ModelIV,
  Bands = BidIV | AskIV | MidIV,
  All = ModelPrice | ModelIV | BidIV | AskIV | MidIV | Greeks,
};

[[nodiscard]] constexpr OutputField operator|(OutputField a, OutputField b) noexcept {
  return static_cast<OutputField>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr OutputField operator&(OutputField a, OutputField b) noexcept {
  return static_cast<OutputField>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has(OutputField set, OutputField flag) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0u;
}

// ── SoA valuation result ────────────────────────────────────────────────────
//
// One row per queried OptionId (row i <-> ids[i]). Only the requested columns
// are populated (others empty); requested-but-failed cells are NaN.
struct ChainValuation {
  std::vector<OptionId> ids;
  std::vector<double> model_price;
  std::vector<double> model_iv;
  std::vector<double> bid_iv;
  std::vector<double> ask_iv;
  std::vector<double> mid_iv;
  std::vector<AmericanGreeks> greeks;
  OutputField filled{OutputField::None};

  [[nodiscard]] std::size_t size() const noexcept { return ids.size(); }

  // Row index for an id (linear scan), or nullopt.
  [[nodiscard]] std::optional<std::size_t> row_of(OptionId id) const;
};

// ── Fallback ladder ─────────────────────────────────────────────────────────
//
// Curve families a failed `primary` is retried with, in order, for any board the
// policy routed (a caller-pinned curve is never substituted). A profile is a
// latency prior, not permission to drop an underlier, so every kind declares at
// least one rung and no rung repeats its own primary. `FitDecision::used_fallback`
// and `::primary_curve` record what happened. Exposed so the routing contract is
// inspectable, not just observable after a failure.
[[nodiscard]] std::span<const VolCurveKind> fallback_curve_rungs(VolCurveKind primary) noexcept;

// ── Fit policy ──────────────────────────────────────────────────────────────
//
// Thin bundle over the session's fit knobs plus the evaluation thread count.
// Every field defaults so `PricerConfig{}` is a valid market-maker config. With
// no pinned curve, the unified policy routes dense ETF/index boards to the HFT
// linear-variance path, sparse boards to SVI, event boards to C8, and validates
// ambiguous boards out of sample.
struct PricerConfig {
  // Preset used for pinned curves and as the fallback policy for a forced
  // cross-validation. Auto policy may choose a profile-specific effective
  // preset; set Hft explicitly to retain the legacy hard-pinned dense route.
  FitPreset preset{FitPreset::Robust};
  // Curve family + per-kind knobs. std::nullopt (the default) => the CurveSelector
  // searches for the best kind + config for THIS board (out-of-sample
  // generalization; SPY-dense boards pick ConvexDense, sparse single-name boards
  // pick the parsimonious eSSVI backbone). Set it explicitly to pin a curve.
  std::optional<CurveConfig> curve{};
  // Search policy used only when `curve` is std::nullopt.
  SelectorConfig selector{};
  // Unified profile/session/event auto-selection policy and per-snapshot hints.
  FitPolicyConfig policy{};
  FitContext context{};
  // Optional overrides for the preset's cold-fit diagnostic/quality-speed knobs.
  // nullopt => use the preset default. false for `score_parity` skips the second
  // de-Am diagnostic pass; false for `enforce_calendar_floor` maximizes raw
  // in-band fit quality by fitting dense slices independently.
  std::optional<bool> use_correction_cache{};
  std::optional<bool> score_parity{};
  std::optional<bool> enforce_calendar_floor{};
  std::optional<bool> use_deam_cache_for_fit{};
  // Optional per-slice cap applied before American-IV de-Am inversion. nullopt
  // => use the preset default; 0 => no cap.
  std::optional<std::uint32_t> max_obs_per_slice{};
  // Reuse raw European IV when the estimated OTM early-exercise premium is at
  // most this fraction of the NBBO spread. nullopt => preset default.
  std::optional<double> max_otm_shortcut_premium_spread_frac{};
  // Worker count for value_chain. 0 => std::thread::hardware_concurrency();
  // 1 => serial. A per-call `value_chain(..., n_threads)` overrides this.
  unsigned n_threads{0};
  // Extra discrete cash dividends the surface build should honour. Usually left
  // empty — the chain's `MarketEnv` supplies the dividend schedule; a non-empty
  // value here overrides the env's divs.
  std::vector<DividendEvent> cash_divs{};
  // V2 product policy. Quality changes the work budget, never the mandatory
  // admission floor. The production default requests both a market-following
  // mark and an independently admitted Balanced risk surface.
  FitQualityMode quality_mode{FitQualityMode::Balanced};
  SurfaceOutputs outputs{SurfaceOutputs::MarketMarkAndRisk};
  RiskAdmission risk_admission{RiskAdmission::Required};
  SurfaceFallback fallback{SurfaceFallback::LastKnownGood};
};

// ── Fitted surface handle (the owned fit output) ────────────────────────────
//
// Wraps a `VolaSession` (the fitted VolSurface + per-slice carry + correction
// caches). Every query is a const read; move-only (heavy fitted state).
class FittedSurface {
public:
  FittedSurface(FittedSurface &&) noexcept = default;
  FittedSurface &operator=(FittedSurface &&) noexcept = default;
  FittedSurface(const FittedSurface &) = delete;
  FittedSurface &operator=(const FittedSurface &) = delete;

  [[nodiscard]] double iv(double K, double T) const { return sess_.iv(K, T); }
  [[nodiscard]] Result<double> fair_value(double K, double T, Side s) const {
    return sess_.fair_value(K, T, s);
  }
  [[nodiscard]] Result<AmericanGreeks> greeks(double K, double T, Side s) const {
    return sess_.greeks(K, T, s);
  }
  [[nodiscard]] const VolaSession &session() const noexcept { return sess_; }
  [[nodiscard]] const SessionDiagnostics &diagnostics() const noexcept {
    return sess_.diagnostics();
  }
  [[nodiscard]] SurfacePurpose purpose() const noexcept { return purpose_; }
  [[nodiscard]] FitQualityMode quality_mode() const noexcept { return quality_mode_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
  friend class PricerFitter;
  explicit FittedSurface(VolaSession &&sess, SurfacePurpose purpose,
                         FitQualityMode quality_mode, std::uint64_t generation)
      : sess_(std::move(sess)), purpose_(purpose), quality_mode_(quality_mode),
        generation_(generation) {}
  VolaSession sess_;
  SurfacePurpose purpose_{SurfacePurpose::Risk};
  FitQualityMode quality_mode_{FitQualityMode::Balanced};
  std::uint64_t generation_{};
};

// One immutable publication snapshot. Shared const generation leases keep both
// surfaces alive across subsequent fitter publications, so a retained bundle
// can never dangle while readers finish pricing the old generation.
struct FitPhaseTimings {
  double market_mark_build_ms{};
  double risk_build_ms{};
  double risk_validation_ms{};
  double total_ms{};
  double incremental_input_ms{};
  double incremental_refit_ms{};
  double incremental_validation_ms{};
  double incremental_publish_ms{};
  double incremental_total_ms{};
};

struct SurfaceBundle {
  std::shared_ptr<const FittedSurface> market_mark{};
  std::shared_ptr<const FittedSurface> risk{};
  SurfaceHealth market_mark_health{.purpose = SurfacePurpose::MarketMark};
  SurfaceHealth risk_health{};
  FitPhaseTimings timings{};
  std::uint64_t candidate_generation{};

  [[nodiscard]] bool has(SurfacePurpose purpose) const noexcept {
    return purpose == SurfacePurpose::MarketMark ? market_mark != nullptr : risk != nullptr;
  }
};

// ── PricerFitter ────────────────────────────────────────────────────────────
class PricerFitter {
public:
  explicit PricerFitter(PricerConfig cfg = {}) : cfg_(std::move(cfg)) {}

  // Fit the surface from `chain` and STORE it as unique_ptr<FittedSurface>,
  // replacing any prior fit. Maps the config onto SessionInputs and drives
  // `VolaSession::build`. Propagates the build error (the prior surface is left
  // intact on failure).
  [[nodiscard]] Status fit(const OptionChain &chain);

  // Copy-on-write update of one fitted risk expiry. The updated chain is used
  // to rebuild and certify that expiry's European observations. A carry move
  // requires a full fit; otherwise only the local slice and its adjacent
  // calendar pairs are refit before an independently validated generation is
  // atomically published. The prior generation remains served on every error.
  [[nodiscard]] Result<FitDiag> refit_risk_slice(const OptionChain &chain,
                                                 std::size_t slice_idx);

  // True iff the config's default-purpose surface is served (see surface()).
  [[nodiscard]] bool fitted() const noexcept;
  // Compatibility accessor, fail-closed on purpose: when the active config
  // requests a Risk output, only the admitted risk surface answers (nullptr
  // while risk is rejected/unserved — the LinearVariance market mark is never
  // silently substituted for it). A mark-only request (explicit outputs or the
  // legacy HFT mapping) receives its market surface. Purpose-specific state is
  // always available via risk_surface() / market_mark_surface().
  [[nodiscard]] const FittedSurface *surface() const noexcept;
  [[nodiscard]] const FittedSurface *risk_surface() const noexcept {
    return risk_surface_.get();
  }
  [[nodiscard]] const FittedSurface *market_mark_surface() const noexcept {
    return market_mark_surface_.get();
  }
  [[nodiscard]] SurfaceBundle bundle() const noexcept {
    return SurfaceBundle{market_mark_surface_, risk_surface_, market_mark_health_,
                         risk_health_, timings_, candidate_generation_};
  }

  [[nodiscard]] const PricerConfig &config() const noexcept { return cfg_; }
  void set_threads(unsigned n) noexcept { cfg_.n_threads = n; }

  // The curve-selection outcome from the most recent `fit`, when the config left
  // `curve` unset (auto-select). Empty if `curve` was pinned or no fit has run.
  // Lets a caller see WHICH curve the library chose for this board (and the
  // per-candidate out-of-sample scores).
  [[nodiscard]] const std::optional<SelectorResult> &selection() const noexcept {
    return served_selection_;
  }
  [[nodiscard]] const std::optional<SelectorResult> &candidate_selection() const noexcept {
    return selection_;
  }

  // Profile/features/effective preset+curve decision from the most recent auto
  // fit. Unlike selection(), this is populated for the O(N) direct routes too.
  [[nodiscard]] const std::optional<FitDecision> &decision() const noexcept {
    return served_decision_;
  }
  [[nodiscard]] const std::optional<FitDecision> &candidate_decision() const noexcept {
    return decision_;
  }

  // Price the chain's options for the requested `fields`, fanned out across
  // `n_threads` workers (0 => cfg.n_threads; final 0 => hardware_concurrency,
  // 1 => serial). DETERMINISTIC: the result is bit-identical for any thread
  // count (disjoint output slots, pure const reads).
  //
  // The purpose-less overload prices the config's default purpose FAIL-CLOSED:
  // a config that requested a Risk output is answered by the admitted risk
  // surface or with Unavailable — never by the market mark. Pass
  // SurfacePurpose::MarketMark explicitly to price the mark interpolant.
  //
  // @return Unavailable if the requested surface is unserved; otherwise
  //         Ok(valuation).
  [[nodiscard]] Result<ChainValuation> value_chain(const OptionChain &chain, OutputField fields,
                                                   unsigned n_threads = 0) const;
  [[nodiscard]] Result<ChainValuation> value_chain(const OptionChain &chain, OutputField fields,
                                                   SurfacePurpose purpose,
                                                   unsigned n_threads = 0) const;

private:
  // Effective §9 request after the one-release legacy-preset mapping. Computed
  // in one place so fit(), the default-purpose accessors, and value_chain can
  // never disagree about whether the config requested a risk output.
  struct EffectiveRequest {
    SurfaceOutputs outputs{SurfaceOutputs::MarketMarkAndRisk};
    FitQualityMode quality_mode{FitQualityMode::Balanced};
  };
  [[nodiscard]] EffectiveRequest effective_request() const noexcept;

  PricerConfig cfg_;
  std::shared_ptr<const FittedSurface> market_mark_surface_;
  std::shared_ptr<const FittedSurface> risk_surface_;
  SurfaceHealth market_mark_health_{.purpose = SurfacePurpose::MarketMark};
  SurfaceHealth risk_health_{};
  FitPhaseTimings timings_{};
  std::uint64_t candidate_generation_{};
  std::optional<SelectorResult> selection_; // last auto-select outcome (if any)
  std::optional<SelectorResult> served_selection_; // selector that produced served risk
  std::optional<FitDecision> decision_;     // last unified policy outcome
  std::optional<FitDecision> served_decision_; // policy that produced served risk generation
};

} // namespace atx::vol
