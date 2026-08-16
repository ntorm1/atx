#include "atx/vol/api/fitting/curve_fit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "core/log_emit.hpp"
#include "atx/vol/api/fitting/calib.hpp"  // build_observations_european, ObsSet, FitObs
#include "atx/vol/api/fitting/deamer.hpp" // resolve_chain_forward, european_equiv_iv, otm_side, DeAmOptions
#include "core/parallel_for.hpp"     // parallel_for (block-partition fan-out)
#include "atx/vol/api/fitting/parity.hpp"           // chain_parity, ParityInputs, ParityReport
#include "fitting/prepared_fitting.hpp" // canonical configured preparation
#include "atx/vol/api/marketdata/universe.hpp"         // Chain, chain_index

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Minimum usable strikes to attempt a slice fit (mirrors run_surface_parity).
using ProfileClock = std::chrono::steady_clock;

[[nodiscard]] bool profile_enabled() noexcept {
#if defined(_WIN32)
  char *raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, "ATX_VOL_PROFILE") != 0 || raw == nullptr) {
    return false;
  }
  const bool enabled = len > 0 && raw[0] != '\0' && raw[0] != '0';
  std::free(raw);
  return enabled;
#else
  const char *v = std::getenv("ATX_VOL_PROFILE");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
#endif
}

[[nodiscard]] bool slice_debug_enabled() noexcept {
#if defined(_WIN32)
  char *raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, "ATX_SLICE_DEBUG") != 0 || raw == nullptr) {
    return false;
  }
  const bool enabled = len > 0 && raw[0] != '\0' && raw[0] != '0';
  std::free(raw);
  return enabled;
#else
  const char *v = std::getenv("ATX_SLICE_DEBUG");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
#endif
}

[[nodiscard]] double elapsed_ms(ProfileClock::time_point t0, ProfileClock::time_point t1) noexcept {
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// W3.3 (F3): per-chain preparation outcome. The prepass records EXACTLY why a
// chain did or did not become a fittable slice so phase 2 can (a) count truthful
// rescue/starvation tallies and (b) — under
// `SurfaceParityInputs::fail_board_on_hard_slice_error` — propagate a genuine
// preparation defect instead of silently converting it to missing coverage.
enum class SlicePrepOutcome : std::uint8_t {
  Skipped,              // degenerate maturity (T<=0) — never attempted
  Prepared,             // the primary policy produced a fittable slice
  PreparedLegacyRescue, // recovered via the opt-in Legacy-prep rescue
  Starved,              // below the usable-row floor even after any rescue (thin)
  Uncovered,            // admitted rows fail the k-coverage criterion (Task 1)
  CarryFailed,          // carry / forward resolution failed
  Failed,               // HARD preparation error (defect) — error retained
};

// Error taxonomy (REVIEW §6.2). An EXPECTED preparation failure is a genuinely
// thin / data-shaped outcome (`NotFound` = fewer than the usable-row floor;
// `Unavailable` = non-positive forward, audit-rejected rows) — eligible for the
// Legacy rescue and, when unrescued, a truthful drop. Every other code
// (`InvalidArgument`, `Internal`, `OutOfRange`, `Unknown`, …) is a real defect
// that must be retained and surfaced, never silently swallowed.
[[nodiscard]] bool prep_error_is_expected(ErrorCode code) noexcept {
  return code == ErrorCode::NotFound || code == ErrorCode::Unavailable;
}

// Per-chain output slot for the parallel de-Am pre-pass (phase 1). `usable`
// mirrors EXACTLY the set of `continue` gates the old sequential loop applied
// (T<=0, forward resolve failed, F non-finite/non-positive, obs below the
// shared kMinPreparedFitRows contract)
// so phase 2 skips precisely the chains the pre-S0-1 code skipped. Written by
// AT MOST one worker (its own chain index) and read only after every worker has
// joined (parallel_for's scope-exit barrier) — no cross-thread reduction, pure
// const reads of `under`/`in`, disjoint writes into `slot[i]`. Bit-identical for
// any worker count (the value_chain / parallel_for determinism pattern).
struct ChainPrepass {
  bool usable = false;
  // Carry resolution failed (or produced a degenerate forward): the chain is
  // unusable AND the skip must be surfaced in the report, never hidden (§5.2).
  bool carry_failed = false;
  double T = 0.0;
  double rate = 0.0;
  double F = 0.0;
  double borrow = 0.0;
  double q_eff = 0.0;
  double df = 0.0;
  std::optional<PreparedSlice> prepared; // meaningful only when `usable`
  double ms_forward_borrow = 0.0;
  double ms_obs_eu = 0.0;
  // Perf C1: the certification-layer's derived data, captured HERE (same
  // per-chain task) instead of a second serial pass in VolaSession::build's
  // (now-removed, for this driver) collect_input_diagnostics. Meaningful only
  // when `usable`. `carry` is the CERTIFICATION resolve — re-run with
  // `in.deam_cert_caches` substituted when those differ from the fit's own
  // caches (review fix; see run_deam_prepass) — and `carry_available` false
  // means that certification resolve failed (the historical serial pass's
  // carry-unavailable case; a cached fit resolve can succeed where the
  // certification resolve does not).
  CarryDiagnostics carry;
  bool carry_available = false;
  // Decision B (board-level term-structure carry fallback). `carry_confident` is
  // this expiry's OWN carry-solve confidence — a confident expiry anchors the
  // borrow-vs-T structure the repair pass interpolates. `needs_carry_repair`
  // marks an expiry deferred by the board-level confidence gate (non-confident
  // under require_carry_confidence) awaiting a fallback borrow instead of the
  // historical hard-drop. `carry_source` is the provenance of the (F, borrow)
  // finally committed for this slice (Solved / TermStructureInterp/Extrap).
  bool carry_confident = false;
  bool needs_carry_repair = false;
  CarrySource carry_source = CarrySource::Solved;
  // T5c. `carry_bounded` marks an expiry whose OWN solve succeeded and whose
  // carry uncertainty is inside the standard-deviation-moneyness budget
  // (`carry_moneyness_bounded`) although it missed the rate-unit confidence
  // gate. Such an expiry is a SECOND-TIER anchor: used only when the board has
  // no confident one, and never laundered as confident. `solved_borrow` retains
  // that solve's borrow so the repair pass can commit it without a second
  // resolve. `carry_solve_failed` distinguishes an expiry whose solve
  // ERRORED (no quotable co-terminal pair, or every pair's solve failed) from
  // one that merely missed the gate — the former has no borrow of its own but is
  // still repairable from the board term structure.
  bool carry_bounded = false;
  bool carry_solve_failed = false;
  double solved_borrow = 0.0;
  std::vector<double> source_mids;        // ‖ prepared fit rows; raw chain.mids at (K, side)
  std::vector<std::uint8_t> source_flags; // ‖ prepared fit rows; raw chain.flags at (K, side)
  std::vector<double> chain_mids;         // full-chain snapshot
  std::vector<std::uint8_t> chain_flags;
  std::vector<double> chain_bids;
  std::vector<double> chain_asks;
  std::vector<std::int64_t> chain_ts;
  // W3.3/W3.4 (F3/F4): why this chain did or did not become a fittable slice.
  // Default Skipped (degenerate maturity, never attempted). `prep_error` retains
  // a HARD preparation failure verbatim so phase 2 can propagate it truthfully.
  SlicePrepOutcome prep_outcome = SlicePrepOutcome::Skipped;
  std::optional<atx::core::Error> prep_error;
  // W2-B: the PRIMARY (configured-policy) attempt's error, retained separately
  // and even when it was `expected` (thin). `prep_error` above is the error that
  // DECIDED the outcome — after a rescue that is the rescue's — so without this
  // the strict funnel's own rejection histogram, the evidence that distinguishes
  // "the filter emptied this slice" from "this slice has no two-sided quotes",
  // is lost the moment a rescue runs.
  std::optional<atx::core::Error> primary_prep_error;
  // Legacy-rescue preparation tallies (meaningful only when a rescue ran):
  // rows the permissive predicate kept, and rows its fit-inversion audit dropped.
  std::uint32_t legacy_fit_rows = 0;
  std::uint32_t legacy_audit_dropped = 0;
  // W1-B (F21): this slice's population came from the ITM-leg retry. Orthogonal
  // to `prep_outcome` — the leg rule and the preparation POLICY are independent
  // axes, and a slice can be rescued on both — so it is its own flag rather
  // than another `SlicePrepOutcome` enumerator.
  bool itm_leg_rescued = false;
  // Fitter stability: this slice FAILED the ConvexDense k-coverage predicate but
  // was retained for parametric demotion rather than dropped
  // (`CalibOpts::per_slice_uncovered_parametric`). Orthogonal to `prep_outcome`
  // — preparation SUCCEEDED, so the outcome stays `Prepared`/`PreparedLegacyRescue`
  // and only the family this slice may be fitted with is restricted. Always false
  // when the flag is off, where the refusal still sets `prep_outcome = Uncovered`
  // and clears `usable`.
  bool k_uncovered = false;
};

// W2-B: the board-level refusal string. `NotFound: no expiry produced a usable
// slice` told an operator nothing — a 23-row board and a 6000-row board whose
// quotes cannot be de-Americanized under negative carry produced the identical
// message, and the per-expiry evidence the driver already collected was thrown
// away at the boundary. This names the outcome census, the preparation policy
// that produced it (so a strictness choice is visible in its own failure), and
// the single most informative per-expiry detail: the thin/starved expiry with
// the most quote rows, whose retained error carries the builder's rejection
// histogram. Failure path only — no cost on a board that fits.
[[nodiscard]] std::string describe_board_refusal(const Underlying &under,
                                                 const SurfaceParityInputs &in,
                                                 const CurveConfig &cfg,
                                                 std::span<const ChainPrepass> prepass,
                                                 std::span<const ExpiryFitReport> reports) {
  std::size_t n_starved = 0, n_uncovered = 0, n_carry_failed = 0, n_prep_failed = 0;
  std::size_t n_fit_failed = 0, n_calendar_refused = 0, n_skipped = 0;
  for (const ExpiryFitReport &rep : reports) {
    switch (rep.outcome) {
    case ExpiryFitOutcome::PrepStarved:
      ++n_starved;
      break;
    case ExpiryFitOutcome::PrepUncovered:
      ++n_uncovered;
      break;
    case ExpiryFitOutcome::CarryFailed:
      ++n_carry_failed;
      break;
    case ExpiryFitOutcome::PrepFailed:
      ++n_prep_failed;
      break;
    case ExpiryFitOutcome::FitFailed:
      ++n_fit_failed;
      break;
    case ExpiryFitOutcome::FitRefusedCalendar:
      ++n_calendar_refused;
      break;
    case ExpiryFitOutcome::Skipped:
      ++n_skipped;
      break;
    case ExpiryFitOutcome::Fitted:
    case ExpiryFitOutcome::FittedFallbackCurve:
    case ExpiryFitOutcome::FittedLegacyPrep:
      break; // unreachable here: a fitted slice means the surface is non-empty
    }
  }

  std::string out = "fit_curve_surface: no expiry produced a usable slice; chains=" +
                    std::to_string(reports.size()) + " starved=" + std::to_string(n_starved) +
                    " uncovered=" + std::to_string(n_uncovered) +
                    " carry_failed=" + std::to_string(n_carry_failed) +
                    " prep_failed=" + std::to_string(n_prep_failed) +
                    " fit_failed=" + std::to_string(n_fit_failed) +
                    " calendar_refused=" + std::to_string(n_calendar_refused) +
                    " skipped=" + std::to_string(n_skipped);
  out += "; kind=";
  out += to_string(cfg.kind);
  out += " prep=";
  out += (in.fit_prep_policy == PreparedObservationPolicy::LegacyEssviCompatibility) ? "permissive"
                                                                                     : "configured";
  out += " legacy_prep_rescue=";
  out += in.per_slice_legacy_prep_fallback       ? "every-slice"
         : in.board_starved_legacy_prep_fallback ? "board-starved"
                                                 : "off";
  out += " linear_fallback=";
  out += in.calib.per_slice_linear_fallback ? "on" : "off";
  out += " itm_fallback=";
  out += in.per_slice_itm_leg_fallback       ? "every-slice"
         : in.board_starved_itm_leg_fallback ? "board-starved"
                                             : "off";

  // Widest board evidence available: the retained preparation error of the
  // non-fitting expiry with the most strikes.
  const std::size_t n = std::min(prepass.size(), under.chains.size());
  std::size_t widest_index = n;
  std::size_t widest_strikes = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!prepass[i].prep_error.has_value()) {
      continue;
    }
    const std::size_t strikes = under.chains[i].n_strikes();
    if (widest_index == n || strikes > widest_strikes) {
      widest_index = i;
      widest_strikes = strikes;
    }
  }
  if (widest_index < n) {
    const ChainPrepass &widest = prepass[widest_index];
    out += "; widest non-fitting expiry #" + std::to_string(widest_index) +
           " T=" + std::to_string(under.chains[widest_index].T) +
           " strikes=" + std::to_string(widest_strikes) + ": ";
    // Both ends of the preparation ladder, when they differ. The primary
    // carries the configured cascade's rejection histogram (which filter
    // emptied the slice); the second is what the most relaxed attempt still
    // could not save (how much of the loss is the board rather than the
    // policy). The ladder's arming state is printed above, so the reader knows
    // which relaxations that last attempt actually carried.
    out += widest.primary_prep_error.has_value() ? widest.primary_prep_error->to_string()
                                                 : widest.prep_error->to_string();
    if (widest.primary_prep_error.has_value() &&
        widest.prep_error->message() != widest.primary_prep_error->message()) {
      out += " | after the last rescue attempt: " + widest.prep_error->to_string();
    }
  }
  return out;
}

// Recover each fit observation's SOURCE chain quote (raw American mid + kill-
// mask flags) by strike, so the incremental-cache certification data is
// captured in the SAME parallel per-chain task that built `obs` instead of a
// second serial pass over the fitted rows (perf finding C1). Mirrors the
// lookup `VolaSession::build`'s (session.cpp) `collect_input_diagnostics`
// used to perform after the fact: binary search `chain.strikes` for
// `fit_obs.K`, then read `chain.mids`/`chain.flags` at
// `chain_index(strike_idx, fit_obs.side)`. Clears BOTH outputs (defensive) if
// any row fails to map back onto the chain.
void source_quote_lookup(const Chain &chain, std::span<const FitObs> obs,
                         std::vector<double> &out_mids, std::vector<std::uint8_t> &out_flags) {
  out_mids.clear();
  out_flags.clear();
  out_mids.reserve(obs.size());
  out_flags.reserve(obs.size());
  for (const FitObs &fit_obs : obs) {
    const auto strike_it = std::lower_bound(chain.strikes.begin(), chain.strikes.end(), fit_obs.K);
    if (strike_it == chain.strikes.end() || *strike_it != fit_obs.K) {
      out_mids.clear();
      out_flags.clear();
      return;
    }
    const auto strike_idx =
        static_cast<std::uint16_t>(std::distance(chain.strikes.begin(), strike_it));
    const std::size_t quote_idx = chain_index(strike_idx, fit_obs.side);
    if (quote_idx >= chain.mids.size() || quote_idx >= chain.flags.size()) {
      out_mids.clear();
      out_flags.clear();
      return;
    }
    out_mids.push_back(chain.mids[quote_idx]);
    out_flags.push_back(chain.flags[quote_idx]);
  }
}

[[nodiscard]] bool valid_expiry_rates(const SurfaceParityInputs &in,
                                      const Underlying &under) noexcept {
  return (in.expiry_rates.empty() && in.expiry_rate_T.empty()) ||
         (in.expiry_rates.size() == under.chains.size() &&
          in.expiry_rate_T.size() == under.chains.size() &&
          std::all_of(in.expiry_rates.begin(), in.expiry_rates.end(),
                      [](double rate) { return std::isfinite(rate); }) &&
          std::equal(in.expiry_rate_T.begin(), in.expiry_rate_T.end(), under.chains.begin(),
                     [](double T, const Chain &chain) {
                       return std::isfinite(T) && T > 0.0 && T == chain.T;
                     }));
}

[[nodiscard]] double expiry_rate(const SurfaceParityInputs &in, std::size_t index) noexcept {
  return in.expiry_rates.empty() ? in.r : in.expiry_rates[index];
}

// The fit cache is a proposal accelerator only. Term-rate boards cannot reuse
// the scalar-rate correction box, and ConvexDense remains cold because its
// near-interpolating fit is materially more sensitive to proposal error. The
// observation builder still cold-audits every accepted Andersen-Lake proposal.
[[nodiscard]] bool allow_fit_cache(const SurfaceParityInputs &in, VolCurveKind kind) noexcept {
  return in.use_deam_cache_for_fit && in.expiry_rates.empty() && in.deam.caches.any() &&
         kind != VolCurveKind::ConvexDense;
}

// ── Decision B: board-level term-structure carry fallback ────────────────

// One confident expiry's (maturity, solved borrow) — a node of the board's
// borrow term structure the repair pass reads off.
struct CarryAnchor {
  double T{0.0};
  double borrow{0.0};
};

struct CarryFallback {
  double borrow{0.0};
  CarrySource source{CarrySource::Solved};
};

// Derive a non-confident expiry's borrow from the borrow-vs-T term structure of
// the board's CONFIDENT expiries. INTERIOR (a confident anchor brackets its T on
// both sides): LINEAR interpolation of the borrow in maturity. EDGE (no
// confident anchor on one side): FLAT extension of the nearest confident borrow.
//
// A per-maturity borrow (equivalently the option-implied forward / carry) is
// identified from put-call parity exactly as in van Binsbergen, Diamond &
// Grotteria, "Risk-Free Interest Rates," Journal of Financial Economics
// 143(1):1-29 (2022). Linear-in-rate interpolation across maturities with flat
// (constant-rate) extrapolation beyond the first/last identified node is the
// market-standard curve-construction choice analysed in Hagan & West,
// "Interpolation Methods for Curve Construction," Applied Mathematical Finance
// 13(2):89-129 (2006) — §3 (linear on rates) and their treatment of the curve
// past the extreme nodes (held flat). `anchors` MUST be non-empty and sorted
// ascending in T (guaranteed here: chains are loaded ascending-T).
[[nodiscard]] CarryFallback term_structure_fallback_borrow(double T,
                                                           std::span<const CarryAnchor> anchors) {
  if (T <= anchors.front().T) {
    return CarryFallback{anchors.front().borrow, CarrySource::TermStructureExtrap};
  }
  if (T >= anchors.back().T) {
    return CarryFallback{anchors.back().borrow, CarrySource::TermStructureExtrap};
  }
  std::size_t hi = 0;
  while (hi < anchors.size() && anchors[hi].T <= T) {
    ++hi; // first anchor strictly beyond T (exists: T < anchors.back().T here)
  }
  const CarryAnchor &a_lo = anchors[hi - 1];
  const CarryAnchor &a_hi = anchors[hi];
  const double span = a_hi.T - a_lo.T;
  const double alpha = span > 0.0 ? (T - a_lo.T) / span : 0.0;
  return CarryFallback{a_lo.borrow + alpha * (a_hi.borrow - a_lo.borrow),
                       CarrySource::TermStructureInterp};
}

// Build the European de-Am fit strip for a chain whose carry is already resolved
// into slot.{rate, F, q_eff, df}, committing the PreparedSlice + per-chain
// snapshot data into `slot` (usable=true) or stamping the truthful non-fit
// outcome (Starved / Failed). SHARED by the confident phase-1 path and the
// phase-1.5 carry-fallback repair so both de-Americanize through ONE preparation
// body. Does NOT touch slot.carry / slot.borrow: the caller owns the
// certification carry (which differs between a solved and a fallback expiry) and
// the committed borrow.
// W2-B: `allow_legacy_rescue` is the caller's decision, not a re-read of
// `in.per_slice_legacy_prep_fallback`, because the board-level last-resort pass
// needs to force the rescue on for a second attempt at chains the first pass
// already declined it for. W1-B: `allow_itm_rescue` is the same contract for
// the ITM-leg retry.
void prepare_fit_slice_into_slot(const Chain &chain, const SurfaceParityInputs &in,
                                 VolCurveKind kind, bool use_fit_cache, bool allow_legacy_rescue,
                                 bool allow_itm_rescue, bool time_stages, std::size_t i,
                                 ChainPrepass &slot) {
  const AmericanCorrectionCaches fit_caches =
      use_fit_cache ? in.deam.caches : AmericanCorrectionCaches{};
  PreparedSliceInputs prepare_inputs;
  prepare_inputs.expiry_index = static_cast<std::uint32_t>(i);
  prepare_inputs.S = in.S;
  prepare_inputs.r = slot.rate;
  prepare_inputs.F = slot.F;
  prepare_inputs.q_eff = slot.q_eff;
  prepare_inputs.df = slot.df;
  prepare_inputs.calib = in.calib;
  prepare_inputs.caches = fit_caches;
  prepare_inputs.al_opts = in.deam.al_opts;
  prepare_inputs.iv_tolerance = in.deam.iv_tol;
  prepare_inputs.iv_max_iterations = in.deam.iv_max_iter;
  prepare_inputs.method = in.deam.method;
  prepare_inputs.policy = in.fit_prep_policy;
  prepare_inputs.audit_fit_inversions = in.deam.audit_fit_inversions;
  prepare_inputs.max_iv_residual_half_spreads = in.deam.max_iv_residual_half_spreads;
  prepare_inputs.prepare_scoring = in.score_parity;
  // ── Preparation ladder (W2-B, W1-B) ───────────────────────────────────────
  // Attempts in strictly increasing order of relaxation; the FIRST one to clear
  // the usable-row floor wins, so a slice is served on the widest population
  // the least-relaxed rule can build:
  //
  //   1. the caller's policy, OTM leg only              (always)
  //   2. + the permissive predicate     (W2-B, allow_legacy_rescue)
  //   3. + the ITM leg, caller's policy (W1-B, allow_itm_rescue)
  //   4. + both
  //
  // The ITM relaxation is ordered AFTER the permissive one, and the ordering
  // was MEASURED rather than assumed. Running the ITM retry first is tempting
  // (it keeps the strict cascade's provenance) but on lqbench 2026-08-03 it
  // changed thirteen boards W2-B already recovered: some gained slices, but DNA
  // fell 6 -> 4 and EVGO 2 -> 1 because a thinner ITM population cleared the
  // floor and pre-empted the wider permissive one, and PACB left the measured
  // set entirely. In this order every W2-B outcome is unchanged and W1-B is
  // purely additive.
  struct PrepAttempt {
    PreparedObservationPolicy policy;
    bool itm_leg;
  };
  const bool rescue_eligible =
      allow_legacy_rescue &&
      in.fit_prep_policy != PreparedObservationPolicy::LegacyEssviCompatibility;
  const bool itm_eligible = allow_itm_rescue && !prepare_inputs.calib.itm_leg_fallback;
  std::array<PrepAttempt, 4> ladder{};
  std::size_t n_ladder = 0;
  ladder[n_ladder++] = PrepAttempt{in.fit_prep_policy, prepare_inputs.calib.itm_leg_fallback};
  if (rescue_eligible) {
    ladder[n_ladder++] = PrepAttempt{PreparedObservationPolicy::LegacyEssviCompatibility,
                                     prepare_inputs.calib.itm_leg_fallback};
  }
  if (itm_eligible) {
    ladder[n_ladder++] = PrepAttempt{in.fit_prep_policy, true};
    if (rescue_eligible) {
      ladder[n_ladder++] = PrepAttempt{PreparedObservationPolicy::LegacyEssviCompatibility, true};
    }
  }

  const auto meets_floor = [](const Result<PreparedSlice> &r) noexcept {
    return r.has_value() && r->fit_observations().size() >= kMinPreparedFitRows;
  };
  const auto run_attempt = [&](const PrepAttempt &a) {
    PreparedSliceInputs attempt_inputs = prepare_inputs;
    attempt_inputs.policy = a.policy;
    attempt_inputs.calib.itm_leg_fallback = a.itm_leg;
    if (a.policy == PreparedObservationPolicy::LegacyEssviCompatibility &&
        in.fit_prep_policy != PreparedObservationPolicy::LegacyEssviCompatibility) {
      // A rescued row was admitted by a predicate the configured cascade
      // refused, so it is repriced-audited unconditionally (charter §8.1).
      attempt_inputs.audit_fit_inversions = true;
      attempt_inputs.out_legacy_fit_rows = &slot.legacy_fit_rows;
      attempt_inputs.out_legacy_audit_dropped = &slot.legacy_audit_dropped;
    }
    const ProfileClock::time_point t0 =
        time_stages ? ProfileClock::now() : ProfileClock::time_point{};
    Result<PreparedSlice> r = PreparedSlice::create(chain, attempt_inputs);
    if (time_stages) {
      slot.ms_obs_eu += elapsed_ms(t0, ProfileClock::now());
    }
    return r;
  };

  Result<PreparedSlice> prepared = run_attempt(ladder[0]);
  std::size_t accepted = 0;
  // W2-B: retain the EXPECTED (thin) preparation error too. The builder folds
  // its rejection histogram into that message, and it is the only surviving
  // evidence of why the funnel emptied — the board-level refusal quotes it.
  // `prep_outcome` stays the discriminator, so no consumer that keys on
  // `Failed` sees a behaviour change.
  if (!prepared.has_value()) {
    slot.primary_prep_error = prepared.error();
  }
  for (std::size_t a = 1; a < n_ladder && !meets_floor(prepared); ++a) {
    // A HARD error is a real defect, never a thin slice: stop and surface it
    // rather than papering over it with a more permissive retry.
    if (!prepared.has_value() && !prep_error_is_expected(prepared.error().code())) {
      break;
    }
    Result<PreparedSlice> next = run_attempt(ladder[a]);
    // Adopt a clearing attempt; adopt a failing one only for its ERROR, because
    // the last attempt's message names the binding condition after every
    // relaxation. A valued-but-thin attempt is discarded: the ladder is
    // monotone in permissiveness, so it cannot beat what we already hold.
    if (meets_floor(next) || !next.has_value()) {
      prepared = std::move(next);
      accepted = a;
    }
  }

  if (!meets_floor(prepared)) {
    if (!prepared.has_value()) {
      slot.prep_error = prepared.error();
      if (!prep_error_is_expected(prepared.error().code())) {
        slot.prep_outcome = SlicePrepOutcome::Failed;
        return;
      }
    }
    slot.prep_outcome = SlicePrepOutcome::Starved;
    return;
  }
  slot.itm_leg_rescued = ladder[accepted].itm_leg && !ladder[0].itm_leg;
  slot.prep_outcome =
      (ladder[accepted].policy == PreparedObservationPolicy::LegacyEssviCompatibility &&
       in.fit_prep_policy != PreparedObservationPolicy::LegacyEssviCompatibility)
          ? SlicePrepOutcome::PreparedLegacyRescue
          : SlicePrepOutcome::Prepared;

  // Task 1 (k-coverage): count alone (kMinPreparedFitRows) waves through
  // stress-day husks whose belly the absolute spread filters evacuated
  // (2020-03-18) and one-sided freshly-listed expiries (2025-04-10); the
  // ConvexDense chord / power tails then serve the missing region
  // extrapolated. Refuse such slices into the same truthful-drop lane as
  // Starved. ConvexDense only: every other family's population and admission
  // are byte-identical.
  //
  // Fitter stability: the predicate is a hard boolean over an ORDER STATISTIC of
  // the surviving rows (removing one row MERGES two adjacent gaps), so DELETING
  // the expiry on it makes the surface's composition flip on a single quote.
  // Under `per_slice_uncovered_parametric` the slice is retained and demoted to
  // the parametric backbone in phase 2 instead — the dense chord the predicate
  // exists to prevent is still never drawn, because the demoted slice is not
  // fitted with ConvexDense at all.
  if (kind == VolCurveKind::ConvexDense &&
      !slice_k_coverage(prepared->fit_observations()).admissible()) {
    if (!in.calib.per_slice_uncovered_parametric) {
      slot.prep_outcome = SlicePrepOutcome::Uncovered;
      return;
    }
    slot.k_uncovered = true;
  }

  slot.prepared.emplace(std::move(*prepared));
  slot.chain_mids = chain.mids;
  slot.chain_flags = chain.flags;
  slot.chain_bids = chain.bids;
  slot.chain_asks = chain.asks;
  slot.chain_ts = chain.ts_ns;
  source_quote_lookup(chain, slot.prepared->fit_observations(), slot.source_mids,
                      slot.source_flags);
  slot.usable = true;
}

// Phase 1: per-chain de-Am (resolve_chain_forward + the European observation
// build) fanned out over `n_threads` workers. Pure per-chain work, disjoint
// output slots — see `ChainPrepass` above.
[[nodiscard]] std::vector<ChainPrepass> run_deam_prepass(const Underlying &under,
                                                         const SurfaceParityInputs &in,
                                                         VolCurveKind kind, unsigned n_threads,
                                                         bool time_stages) {
  const bool use_fit_cache = allow_fit_cache(in, kind);
  // Decision B: resolve every expiry's carry WITHOUT the per-expiry hard-drop.
  // The confidence gate is re-applied at BOARD level below (defer, not drop) so a
  // non-confident expiry can be repaired from the term structure of the confident
  // ones. For a CONFIDENT expiry the probe returns a byte-identical ChainForward
  // (require_carry_confidence only gates the Err path, never the computed
  // borrow/forward/diagnostics) — the confident path stays bit-identical.
  DeAmOptions probe_deam = in.deam;
  probe_deam.require_carry_confidence = false;
  std::vector<ChainPrepass> prepass(under.chains.size());
  // Long/dense expiries have highly variable Andersen-Lake cost. Run the
  // largest boards first and let workers dynamically claim the next chain;
  // output remains deterministic because every task owns prepass[i].
  std::vector<std::size_t> schedule(under.chains.size());
  std::iota(schedule.begin(), schedule.end(), std::size_t{0});
  std::stable_sort(schedule.begin(), schedule.end(), [&](std::size_t a, std::size_t b) {
    return under.chains[a].n_strikes() > under.chains[b].n_strikes();
  });
  parallel_for_dynamic(schedule.size(), n_threads, [&](std::size_t task) {
    const std::size_t i = schedule[task];
    const Chain &chain = under.chains[i];
    ChainPrepass &slot = prepass[i];
    const double T = chain.T;
    const double rate = expiry_rate(in, i);
    if (!(T > 0.0)) {
      return;
    }
    const ProfileClock::time_point t_forward0 =
        time_stages ? ProfileClock::now() : ProfileClock::time_point{};
    const auto d_res =
        resolve_chain_forward(chain, in.S, rate, in.cash_divs, in.now_ts_ns, probe_deam);
    if (time_stages) {
      slot.ms_forward_borrow = elapsed_ms(t_forward0, ProfileClock::now());
    }
    if (!d_res) {
      // A real carry failure — NOT the confidence gate, which the probe disarmed.
      // T5c (B3ii). Split the failure by kind. `Unavailable` is the DATA-shaped
      // one — this expiry carries no quotable co-terminal pair, or every pair's
      // fixed point failed — and it says nothing at all about the BOARD's carry,
      // which the other expiries may pin down perfectly well. Historically it
      // hard-dropped here, so an expiry with a one-sided strip was unreachable by
      // the phase-1.5 term-structure repair even on a board with a dozen
      // confident anchors: measured on lqbench 2026-08-03, 29 of the 211 expiries
      // on the 28 carry-lost boards took this path. Defer it to the repair pass
      // instead. Any other code (Internal / InvalidArgument — a degenerate
      // forward base, a malformed chain) is a genuine defect for THIS expiry and
      // still hard-drops: a fallback borrow cannot repair a broken forward.
      //
      // Deferral is armed by the same flag that arms the repair pass itself
      // (`require_carry_confidence`), so every non-risk caller — market-mark,
      // selector, backtest — keeps the historical hard-drop bit-for-bit.
      if (in.deam.require_carry_confidence && d_res.error().code() == ErrorCode::Unavailable) {
        slot.T = T;
        slot.rate = rate;
        slot.carry_solve_failed = true;
        slot.needs_carry_repair = true;
        return;
      }
      slot.carry_failed = true;
      slot.prep_outcome = SlicePrepOutcome::CarryFailed;
      return;
    }
    const double F = d_res->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      slot.carry_failed = true;
      slot.prep_outcome = SlicePrepOutcome::CarryFailed;
      return;
    }
    const double q_eff = rate - std::log(F / in.S) / T;
    const double df = std::exp(-rate * T);

    slot.carry_confident = d_res->carry.confident;
    // Decision B: board-level confidence gate. Under the risk build
    // (require_carry_confidence), a NON-confident expiry is DEFERRED to the
    // phase-1.5 term-structure repair pass (borrow derived from the confident
    // expiries) instead of being hard-dropped here. Its own (unreliable) solve is
    // NOT used as a term-structure anchor.
    if (in.deam.require_carry_confidence && !d_res->carry.confident) {
      slot.T = T;
      slot.rate = rate;
      slot.carry = d_res->carry; // raw solve tallies, restamped as fallback later
      slot.needs_carry_repair = true;
      // T5c: retain this solve so the repair pass can offer it as a SECOND-TIER
      // anchor when the board has no confident expiry at all. Whether it may be
      // is `carry_moneyness_bounded`'s decision, not this one, and a bounded
      // carry is still never `confident`.
      slot.carry_bounded = carry_moneyness_bounded(d_res->carry, in.deam);
      slot.solved_borrow = d_res->borrow;
      return;
    }

    // Confident (or the gate is off): byte-identical to the pre-change path.
    // Record the anchor fields up front so a confident expiry still contributes
    // to the borrow term structure even if its OWN preparation later starves.
    slot.T = T;
    slot.rate = rate;
    slot.F = F;
    slot.borrow = d_res->borrow;
    slot.q_eff = q_eff;
    slot.df = df;

    prepare_fit_slice_into_slot(chain, in, kind, use_fit_cache, in.per_slice_legacy_prep_fallback,
                                in.per_slice_itm_leg_fallback, time_stages, i, slot);
    if (!slot.usable) {
      return; // Starved / Failed already stamped; a confident anchor is still recorded
    }
    slot.carry_source = CarrySource::Solved;

    // Perf C1 + review fix: the CERTIFICATION carry must be bit-identical to
    // what the historical serial certification pass produced — a resolve with
    // the CALLER's caches (in.deam_cert_caches), never the session-built
    // hot-path caches this prepass's own resolve may have consulted. When the
    // two cache sets are the same pointers, this resolve IS that resolve
    // (pure function of identical arguments): reuse it. When they differ,
    // re-resolve with the certification caches substituted — same per-chain
    // parallel task, so the work the old pass did serially is fanned out, and
    // a certification-resolve failure only marks this slice's carry
    // unavailable (the old pass's behavior), never drops the chain.
    const AmericanCorrectionCaches cert_caches =
        in.deam_cert_caches.has_value() ? *in.deam_cert_caches : in.deam.caches;
    if (cert_caches.call == in.deam.caches.call && cert_caches.put == in.deam.caches.put) {
      slot.carry = d_res->carry;
      slot.carry_available = true;
    } else {
      DeAmOptions cert_deam = in.deam;
      cert_deam.caches = cert_caches;
      const ProfileClock::time_point t_cert0 =
          time_stages ? ProfileClock::now() : ProfileClock::time_point{};
      const auto cert_res =
          resolve_chain_forward(chain, in.S, rate, in.cash_divs, in.now_ts_ns, cert_deam);
      if (time_stages) {
        slot.ms_forward_borrow += elapsed_ms(t_cert0, ProfileClock::now());
      }
      if (cert_res) {
        slot.carry = cert_res->carry;
        slot.carry_available = true;
      }
    }
  });
  return prepass;
}

} // namespace

Result<CurveSurfaceReport> fit_curve_surface(const Underlying &under, const SurfaceParityInputs &in,
                                             const CurveConfig &cfg) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument, "fit_curve_surface: non-positive S or non-finite r");
  }
  if (!valid_expiry_rates(in, under)) {
    return Err(ErrorCode::InvalidArgument, "fit_curve_surface: invalid expiry rate vectors");
  }
  if (under.chains.empty()) {
    return Err(ErrorCode::NotFound, "fit_curve_surface: underlying carries no chains");
  }

  CurveSurfaceReport out;
  out.context.reserve(under.chains.size());
  out.per_expiry.reserve(under.chains.size());
  out.input_certification.reserve(under.chains.size());
  double worst = std::numeric_limits<double>::infinity();
  const bool profile = profile_enabled();
  const bool time_stages = profile || in.collect_stage_timings;
  const ProfileClock::time_point t_fit0 =
      time_stages ? ProfileClock::now() : ProfileClock::time_point{};

  // Phase 1 (PARALLEL): the cold per-chain de-Am — resolve_chain_forward (term
  // forward/borrow) + build_observations_european (the 99.5% recipe's European
  // fit observations) — is independent per chain, so it fans out over
  // `in.fit_workers` disjoint output slots (0 => hardware_concurrency; 1 =>
  // serial, bit-identical to the pre-S0-1 path). MUST stay COLD (no correction
  // cache): a near-interpolating convex fit propagates any de-Am bias straight
  // into the served IV, so the small carry-bias of the cached-de-Am hot path
  // (fine for the coarse eSSVI backbone) knocks the penny-tight dense fit out
  // of band. Correctness over speed here — cold Andersen-Lake per strike, just
  // run concurrently across chains.
  const ProfileClock::time_point t_pre0 =
      time_stages ? ProfileClock::now() : ProfileClock::time_point{};
  // The prepass fans out over jthreads; parallel_for_dynamic now rethrows the
  // first worker exception on this thread (e.g. std::bad_alloc from an
  // allocating de-Am body). Convert it to an Err so this Result-returning API
  // stays exception-transparent to its callers rather than unwinding past them.
  // Non-const: phase 2 below MOVES the larger per-chain certification vectors
  // (perf C1) out of each committed slot instead of copying them again.
  std::vector<ChainPrepass> prepass;
  try {
    prepass = run_deam_prepass(under, in, cfg.kind, in.fit_workers, time_stages);
  } catch (const std::exception &e) {
    return Err(ErrorCode::Internal,
               std::string("fit_curve_surface: de-Am prepass failed: ") + e.what());
  } catch (...) {
    return Err(ErrorCode::Internal, "fit_curve_surface: de-Am prepass failed (unknown exception)");
  }
  const double ms_prepass = time_stages ? elapsed_ms(t_pre0, ProfileClock::now()) : 0.0;

  // Phase 1.5 (Decision B — board-level term-structure carry fallback). An
  // expiry deferred by the board confidence gate (needs_carry_repair) is admitted
  // with a borrow DERIVED from the borrow-vs-T structure of the board's CONFIDENT
  // expiries, then de-Americanized/prepared like any other slice — instead of the
  // historical hard-drop that cost thin boards most of their term structure. With
  // ZERO confident anchors nothing is fabricated: the deferred expiries stay
  // dropped (behaviour unchanged). This runs BEFORE the skip/starve tally below so
  // the counts reflect the post-repair board.
  //
  // T5c extends it on two axes, both measured on lqbench 2026-08-03 (the run
  // that produced 28 boards losing EVERY expiry to carry):
  //
  //   (i) an expiry whose own solve returned `Unavailable` (no quotable
  //       co-terminal pair) now arrives here too, instead of hard-dropping in
  //       phase 1 where no repair could reach it — 29 of those 211 expiries;
  //  (ii) when the board has NO confident expiry at all — true of all 28 boards,
  //       0 confident expiries out of 211 — the anchor set falls back to the
  //       expiries whose carry is MEASURED to inside the standard-deviation-
  //       moneyness budget (`carry_moneyness_bounded`). That is a real second
  //       measurement, not a relaxation of the first: it asks whether the forward
  //       this expiry implies is pinned to inside 1% of the slice's own width,
  //       which is the unit the fit consumes, and it keeps the same
  //       `min_confident_borrow_pairs` floor so a single-pair solve — whose
  //       dispersion and leave-one-out read 0 only because nothing disputes them
  //       — can never anchor anything.
  //
  // A board with neither tier still fabricates nothing and stays dropped. Every
  // expiry served off a second-tier anchor carries a non-Solved CarrySource and
  // `confident = false`, so the session counts it in `n_carry_fallback_expiries`
  // and admission publishes Degraded + CarryGap, never Healthy.
  {
    std::vector<CarryAnchor> anchors; // ascending T (chains load ascending-T)
    for (const ChainPrepass &pre : prepass) {
      if (pre.carry_confident) {
        anchors.push_back(CarryAnchor{pre.T, pre.borrow});
      }
    }
    // Second tier, consulted ONLY when the first is empty, so a board with even
    // one confident expiry is bit-identical to the pre-T5c path.
    const bool second_tier = anchors.empty();
    if (second_tier) {
      for (const ChainPrepass &pre : prepass) {
        if (pre.carry_bounded) {
          anchors.push_back(CarryAnchor{pre.T, pre.solved_borrow});
        }
      }
    }
    std::vector<std::size_t> repair_idx;
    for (std::size_t i = 0; i < prepass.size(); ++i) {
      ChainPrepass &pre = prepass[i];
      if (!pre.needs_carry_repair) {
        continue;
      }
      if (anchors.empty()) {
        pre.carry_failed = true; // no confident expiry to borrow a carry from
        pre.prep_outcome = SlicePrepOutcome::CarryFailed;
        continue;
      }
      const Chain &chain = under.chains[i];
      const double rate = expiry_rate(in, i);
      // A second-tier anchor commits its OWN solved borrow rather than reading
      // itself off the interpolant it is a node of — same number either way, but
      // the provenance is honest about where the carry came from.
      const CarryFallback fb =
          (second_tier && pre.carry_bounded)
              ? CarryFallback{pre.solved_borrow, CarrySource::MoneynessBounded}
              : term_structure_fallback_borrow(chain.T, anchors);
      const double F = hybrid_forward(in.S, rate, fb.borrow, chain.T, in.cash_divs, chain.expiry_ns,
                                      in.now_ts_ns, in.deam.hyb);
      if (!(F > 0.0) || !std::isfinite(F)) {
        pre.carry_failed = true;
        pre.prep_outcome = SlicePrepOutcome::CarryFailed;
        continue;
      }
      pre.T = chain.T;
      pre.rate = rate;
      pre.F = F;
      pre.borrow = fb.borrow;
      pre.q_eff = rate - std::log(F / in.S) / chain.T;
      pre.df = std::exp(-rate * chain.T);
      pre.carry_source = fb.source;
      repair_idx.push_back(i);
    }
    // De-Americanize + prepare each repaired expiry (disjoint slots; same parallel
    // pattern as phase 1). A slice too thin even with a valid fallback carry still
    // starves truthfully.
    const bool use_fit_cache = allow_fit_cache(in, cfg.kind);
    parallel_for_dynamic(repair_idx.size(), in.fit_workers, [&](std::size_t task) {
      const std::size_t i = repair_idx[task];
      ChainPrepass &pre = prepass[i];
      // Stamp the fallback certification carry FIRST (retain the raw solve's
      // tallies for diagnostics), marked available but NEVER confident and NEVER
      // Solved — a fallback carry must not be laundered as a solved one.
      CarryDiagnostics fb_carry;
      fb_carry.n_candidates = pre.carry.n_candidates;
      fb_carry.n_attempted = pre.carry.n_attempted;
      fb_carry.n_solved = pre.carry.n_solved;
      fb_carry.n_retained = pre.carry.n_retained;
      fb_carry.effective_pair_count = pre.carry.effective_pair_count;
      fb_carry.dispersion = pre.carry.dispersion;
      fb_carry.max_leave_one_out_shift = pre.carry.max_leave_one_out_shift;
      fb_carry.confidence_half_width = pre.carry.confidence_half_width;
      fb_carry.max_pcp_residual = pre.carry.max_pcp_residual;
      fb_carry.atm_sigma = pre.carry.atm_sigma;
      fb_carry.dispersion_moneyness = pre.carry.dispersion_moneyness;
      fb_carry.max_leave_one_out_moneyness = pre.carry.max_leave_one_out_moneyness;
      fb_carry.confidence_half_width_moneyness = pre.carry.confidence_half_width_moneyness;
      fb_carry.confident = false;
      fb_carry.source = pre.carry_source;
      pre.carry = std::move(fb_carry);
      pre.carry_available = true;
      prepare_fit_slice_into_slot(under.chains[i], in, cfg.kind, use_fit_cache,
                                  in.per_slice_legacy_prep_fallback, in.per_slice_itm_leg_fallback,
                                  time_stages, i, pre);
      // T5c: an expiry that reached here because its OWN carry solve FAILED, and
      // that still produced no slice, must keep reporting the carry failure. The
      // attempted repair changes what we TRIED, not what went wrong: a chain with
      // no quotable co-terminal pair usually has no preparable strip either, and
      // relabelling it `Starved` would drop it out of `n_carry_skipped` and hence
      // out of the CarryGap reason — turning a surfaced gap into a silent one
      // (§5.2). Expiries deferred by the confidence gate are unaffected: their
      // carry solved, so `Starved` is already the truthful outcome for them.
      if (pre.carry_solve_failed && !pre.usable) {
        pre.carry_failed = true;
        pre.prep_outcome = SlicePrepOutcome::CarryFailed;
      }
    });
  }

  // ── Phase 1.75 (W2-B, W1-B): LAST-RESORT re-preparation ───────────────────
  // Only when the board is otherwise a TOTAL refusal — no chain became
  // fittable — do the starved expiries get a second preparation with the
  // relaxations the caller armed: the ITM-leg rule (W1-B) and/or the permissive
  // predicate (W2-B). Gating on "zero usable slices" is what makes both
  // default-safe: a board that already fits skips the loop entirely, so its
  // served slices, diagnostics and preparation cost are bit-identical, and no
  // recovered slice — whether it carries permissively-admitted quotes or wider
  // ITM legs — can drag a healthy board's worst-slice quality under an
  // admission floor. The aggressive per-slice forms stay available through
  // `per_slice_{legacy_prep,itm_leg}_fallback`, which already ran above.
  const bool board_itm_retry = in.board_starved_itm_leg_fallback && !in.per_slice_itm_leg_fallback;
  const bool board_legacy_retry =
      in.board_starved_legacy_prep_fallback && !in.per_slice_legacy_prep_fallback &&
      in.fit_prep_policy != PreparedObservationPolicy::LegacyEssviCompatibility;
  if ((board_itm_retry || board_legacy_retry) &&
      std::none_of(prepass.begin(), prepass.end(),
                   [](const ChainPrepass &pre) { return pre.usable; })) {
    std::vector<std::size_t> starved_idx;
    for (std::size_t i = 0; i < prepass.size(); ++i) {
      if (prepass[i].prep_outcome == SlicePrepOutcome::Starved) {
        starved_idx.push_back(i);
      }
    }
    // One pass, both relaxations: the ladder inside
    // `prepare_fit_slice_into_slot` walks them in increasing order, so a board
    // recovers under the least relaxation that can carry it.
    const bool allow_legacy = board_legacy_retry || in.per_slice_legacy_prep_fallback;
    const bool allow_itm = board_itm_retry || in.per_slice_itm_leg_fallback;
    const bool use_fit_cache = allow_fit_cache(in, cfg.kind);
    parallel_for_dynamic(starved_idx.size(), in.fit_workers, [&](std::size_t task) {
      const std::size_t i = starved_idx[task];
      prepare_fit_slice_into_slot(under.chains[i], in, cfg.kind, use_fit_cache, allow_legacy,
                                  allow_itm, time_stages, i, prepass[i]);
    });
  }

  for (const ChainPrepass &pre : prepass) {
    if (pre.carry_failed) {
      ++out.n_carry_skipped; // §5.2: carry-dropped expiries are surfaced
    }
    if (pre.prep_outcome == SlicePrepOutcome::Starved) {
      ++out.n_slices_starved; // W3.3: thin even after any rescue — surfaced, not hidden
    }
    if (pre.prep_outcome == SlicePrepOutcome::Uncovered) {
      ++out.n_slices_uncovered; // Task 1: coverage-refused — surfaced, not hidden
    }
  }

  // Phase 2 (SEQUENTIAL): the fit is order-dependent — each fitted slice's w(k)
  // becomes the calendar floor for the next (ascending-T) slice — so this walk
  // stays single-threaded, unchanged from the pre-S0-1 logic. It only reads the
  // phase-1 pre-pass results (skipping EXACTLY the chains phase 1 flagged) and
  // re-derives nothing the pre-pass already computed.
  double ms_fit_slice = 0.0;
  double ms_chain_parity = 0.0;
  // Data-supported k-range of the most recently COMMITTED slice's observations
  // — the previous-slice half of fit_slice_curve's tradeable pair band. Starts
  // unbounded (first slice has no pair anyway).
  std::pair<double, double> last_committed_obs_k{
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::infinity()};
  // D1 (Task 6): whether the most recently COMMITTED slice's admitted rows
  // pass Task 1's k-coverage predicate. A covered prev earns FULL calendar
  // floor authority (pre-Task-3 QP, floor rows everywhere); the Task 3
  // band+refusal arms only behind an UNCOVERED prev — a shape Task 1 refuses
  // at prep, so through this driver the armed branch is defense-in-depth for
  // prep-bypassing callers, not a live lane.
  bool last_committed_covered = true;
  for (std::size_t ci = 0; ci < under.chains.size(); ++ci) {
    ChainPrepass &pre = prepass[ci];
    if (!pre.usable) {
      // W3.4 (F4): record why this expiry produced no slice, and — under the
      // completeness contract — propagate a HARD preparation defect instead of
      // silently dropping it and publishing the rest as a clean partial fit.
      ExpiryFitReport rep{};
      rep.chain_index = ci;
      rep.maturity = under.chains[ci].T;
      rep.carry_source = pre.carry_source; // Decision B provenance (surfaced even on drop)
      switch (pre.prep_outcome) {
      case SlicePrepOutcome::CarryFailed:
        rep.outcome = ExpiryFitOutcome::CarryFailed;
        break;
      case SlicePrepOutcome::Starved:
        rep.outcome = ExpiryFitOutcome::PrepStarved;
        break;
      case SlicePrepOutcome::Uncovered:
        rep.outcome = ExpiryFitOutcome::PrepUncovered;
        break;
      case SlicePrepOutcome::Failed:
        rep.outcome = ExpiryFitOutcome::PrepFailed;
        rep.error = pre.prep_error.has_value() ? pre.prep_error->code() : ErrorCode::Unknown;
        break;
      default:
        rep.outcome = ExpiryFitOutcome::Skipped; // degenerate maturity (T<=0)
        break;
      }
      out.expiry_reports.push_back(rep);
      if (pre.prep_outcome == SlicePrepOutcome::Failed && in.fail_board_on_hard_slice_error) {
        return Err(rep.error, "fit_curve_surface: expiry " + std::to_string(ci) +
                                  " preparation failed (hard): " +
                                  (pre.prep_error.has_value() ? pre.prep_error->to_string()
                                                              : std::string("unknown")));
      }
      continue;
    }
    const double T = pre.T;
    const double F = pre.F;
    const double q_eff = pre.q_eff;
    const double df = pre.df;
    const PreparedSlice &prepared = *pre.prepared;
    out.n_score_inversions += prepared.provenance().n_score_inversions;

    // Fitter stability: a slice retained by the k-coverage demotion serves the
    // parsimonious parametric backbone, never the dense family whose chord /
    // power tails across the hole the predicate refused. Everything downstream
    // in this iteration — the calendar-authority branch, the fit, the linear
    // fallback, and the parity cache policy — reads `slice_cfg`, so the demoted
    // slice is treated as an eSSVI slice in full, not a ConvexDense slice fitted
    // by another family. `k_uncovered` is only ever set behind the flag.
    const CurveConfig slice_cfg = [&] {
      if (!pre.k_uncovered) {
        return cfg;
      }
      CurveConfig demoted = cfg;
      demoted.kind = VolCurveKind::Essvi;
      return demoted;
    }();

    // 3. Fit the configured curve kind from the European obs.
    //    Calendar floor: previous fitted slice's total variance (ascending T).
    //    Guard on the prior slice's T so a non-ascending input degrades to
    //    no-enforcement (never an inverted floor). The loader sorts ascending-T
    //    (data.cpp sort_chains_by_T), so on the standard board this is always
    //    the immediately-shorter expiry.
    std::function<double(double)> w_prev;
    std::span<const double> calendar_floor_knots;
    std::pair<double, double> prev_data_k_range{-std::numeric_limits<double>::infinity(),
                                                std::numeric_limits<double>::infinity()};
    if (in.enforce_calendar_floor && !out.surface.empty() && out.context.back().T < T) {
      const IVolCurve *prev = out.surface.slices().back().get();
      w_prev = [prev](double k) { return prev->w(k); };
      if (const auto *linear = dynamic_cast<const LinearVarianceCurve *>(prev); linear != nullptr) {
        calendar_floor_knots = linear->k_nodes();
      }
      // Every parametric calendar projection acts only on the tradeable overlap
      // of the two slices' data ranges (fit_slice_curve's tradeable_pair_band);
      // hand it the previous COMMITTED slice's observation range, tracked at
      // commit below. A crossing outside both slices' quoted ranges is
      // extrapolation-vs-extrapolation and must not move the level.
      prev_data_k_range = last_committed_obs_k;
      // The SplineVol projection keeps its own, spline-specific range source.
      if (const auto *sp = dynamic_cast<const SplineVolCurve *>(prev); sp != nullptr) {
        prev_data_k_range = sp->data_k_range();
      }
      if (slice_cfg.kind == VolCurveKind::ConvexDense && last_committed_covered) {
        // D1 (Task 6): coverage-admissible prev => unbounded floor authority.
        // ConvexDense only: prev_data_k_range also feeds the parametric arms'
        // tradeable_pair_band, whose semantics must not change.
        prev_data_k_range = {-std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::infinity()};
      }
    }
    // The calendar floor inside fit_convex_slice enforces w_curr >= w_prev at the
    // fit nodes (STRICT, per the served-surface policy: calendar-arb-free by
    // construction). It adds constraint ROWS to the N-node QP, not slack variables,
    // so enforcement does not materially slow the fit. On boards with genuine
    // calendar structure this trades some price-in-band tightness for no-arb — an
    // explicit product choice (see spy_bidask_regression_test's rebaselined floor).
    const ProfileClock::time_point t_slice0 =
        time_stages ? ProfileClock::now() : ProfileClock::time_point{};
    bool used_linear_fallback = false; // W3.4: FittedFallbackCurve vs Fitted taxonomy
    // T10b (D5): the primary fit's own verdict. `fit_slice_curve` clears this on
    // entry, so a slice that fails leaves it default ("not known") rather than
    // carrying the previous expiry's — this struct is reused across the walk.
    FitDiag slice_diag{};
    auto slice_res = fit_slice_curve(slice_cfg, prepared.fit_observations(), F, T, df, w_prev,
                                     calendar_floor_knots, prev_data_k_range, &slice_diag);
    if (time_stages) {
      ms_fit_slice += elapsed_ms(t_slice0, ProfileClock::now());
    }
    if (!slice_res) {
      // FIX A (opt-in coverage recovery): retry THIS slice with a linear-in-
      // variance curve before dropping it. A thin per-expiry-sparse name whose
      // primary (e.g. SplineVol) fit needs more usable de-Am rows than the funnel
      // (OTM-side-only + bid>0 + audit + floors) yields can still produce a served
      // LinearVariance slice from >=2 nodes. The fallback re-enters `fit_slice_curve`
      // with an unchanged CurveConfig except `kind`, so it passes the SAME
      // admission (>=2 nodes + union-grid calendar floor against the prior slice's
      // w_prev / calendar_floor_knots) as any LinearVariance slice — no
      // numerical-sanity check is bypassed. Gated on `per_slice_linear_fallback`
      // and skipped when the primary kind is already LinearVariance, so the
      // default path (flag off) is byte-identical to the historical drop.
      if (in.calib.per_slice_linear_fallback && slice_cfg.kind != VolCurveKind::LinearVariance) {
        CurveConfig fallback_cfg = slice_cfg;
        fallback_cfg.kind = VolCurveKind::LinearVariance;
        // Calendar consistency for the inserted linear slice: a LinearVariance
        // fit only floors w >= w_prev at its OWN nodes, so between/beyond those
        // nodes its flat-wing extrapolation can dip under w_prev, breaking the
        // mid-chain calendar order the SplineVol projection otherwise maintains
        // (measured: enabling the fallback without this drops calendar-arb-free
        // to ~37%). Union the risk-check grid (the exact points arb_check_calendar
        // samples, [-0.60, 0.60] / 64) into the floor knots so the served linear
        // slice dominates w_prev at every checked point. Only for the fallback;
        // the primary path is unchanged.
        std::vector<double> fb_floor_knots(calendar_floor_knots.begin(),
                                           calendar_floor_knots.end());
        if (w_prev) {
          constexpr double kFbCalMin = -0.60;
          constexpr double kFbCalMax = 0.60;
          constexpr int kFbCalIntervals = 64;
          constexpr double kFbCalDk =
              (kFbCalMax - kFbCalMin) / static_cast<double>(kFbCalIntervals);
          for (int gi = 0; gi <= kFbCalIntervals; ++gi) {
            fb_floor_knots.push_back(kFbCalMin + kFbCalDk * static_cast<double>(gi));
          }
        }
        const ProfileClock::time_point t_fb0 =
            time_stages ? ProfileClock::now() : ProfileClock::time_point{};
        // T10b (D5): the fallback OVERWRITES the primary's verdict, and only on
        // success below. A recovered slice serves the LinearVariance curve, so
        // the diagnostic that describes it must be the fallback's; keeping the
        // failed primary's would attribute one family's fit to another's curve.
        FitDiag fb_diag{};
        auto fb_res = fit_slice_curve(fallback_cfg, prepared.fit_observations(), F, T, df, w_prev,
                                      std::span<const double>{fb_floor_knots},
                                      std::pair<double, double>{
                                          -std::numeric_limits<double>::infinity(),
                                          std::numeric_limits<double>::infinity()},
                                      &fb_diag);
        if (time_stages) {
          ms_fit_slice += elapsed_ms(t_fb0, ProfileClock::now());
        }
        if (fb_res) {
          // Recovered: adopt the linear slice and fall through to the normal
          // commit path (parity scoring + w_prev carry for the next expiry).
          slice_res = std::move(fb_res);
          slice_diag = fb_diag;
          ++out.n_slice_linear_fallback;
          used_linear_fallback = true;
        }
      }
      // Fitter stability: the DOMINANT discontinuity measured on real boards is
      // not family reselection and not the k-coverage predicate — it is THIS
      // drop. A dense slice that marginally fails its own fit admission is
      // silently removed from the surface ("a slice that fails to fit
      // contributes no slice"), so a negligible input change flips the expiry
      // between present and absent. Measured on SP100 2025-09-11: 45 of 905
      // expiry slots flipped `Fitted <-> Missing` under a provably-negligible
      // de-Am perturbation (29 one way, 16 the other), every one a ConvexDense
      // slice, while ZERO boards changed curve family.
      //
      // Same remedy as the coverage refusal above, and deliberately the same
      // family: demote THAT SLICE to the parsimonious parametric backbone rather
      // than deleting the expiry. The demoted fit re-enters `fit_slice_curve`
      // with an unchanged config except `kind`, so it faces the SAME admission
      // (including the calendar floor against the prior slice) as any eSSVI
      // slice — a slice that cannot be fitted by either family is still dropped.
      if (!slice_res && in.calib.per_slice_uncovered_parametric &&
          slice_cfg.kind != VolCurveKind::Essvi) {
        CurveConfig demoted_cfg = slice_cfg;
        demoted_cfg.kind = VolCurveKind::Essvi;
        FitDiag demoted_diag{};
        auto demoted_res =
            fit_slice_curve(demoted_cfg, prepared.fit_observations(), F, T, df, w_prev,
                            calendar_floor_knots, prev_data_k_range, &demoted_diag);
        if (demoted_res) {
          slice_res = std::move(demoted_res);
          slice_diag = demoted_diag;
          ++out.n_slices_fit_demoted_parametric;
          used_linear_fallback = true; // FittedFallbackCurve: a different family serves
        }
      }
      if (!slice_res) {
        // Diagnostic-only (env-gated, failure path): surface WHY a slice was
        // dropped so a caller can tell a genuinely-thin expiry from a curve-family
        // fit defect (e.g. SplineVol ill-conditioning on boards eSSVI fits). No
        // behavioural change; getenv runs only on the already-failed branch.
        if (slice_debug_enabled()) {
          detail::log_emitf(LogLevel::Warn, LogStream::Stderr,
                            "[slice-drop] kind=%d T=%.4f F=%.2f obs=%zu err=%s",
                            static_cast<int>(cfg.kind), T, F, prepared.fit_observations().size(),
                            slice_res.error().to_string().c_str());
        }
        // W3.4 (F4): record the fit failure and, under the completeness contract,
        // propagate a HARD fit error (e.g. a non-converged QP `Internal`) instead
        // of dropping the slice and publishing the rest as a clean partial fit. A
        // SOFT code (NotFound / Unavailable — a genuinely thin or butterfly-
        // inadmissible slice) is still dropped, preserving the Mark tolerance.
        const ErrorCode fit_code = slice_res.error().code();
        const bool calendar_refusal =
            fit_code == ErrorCode::Unavailable &&
            slice_res.error().message() == kCalendarFloorUnsupportedMsg;
        if (calendar_refusal) {
          ++out.n_slice_calendar_unsupported; // Task 3: refused, not ratcheted
        }
        ExpiryFitReport rep{};
        rep.chain_index = ci;
        rep.maturity = T;
        rep.n_observations = prepared.fit_observations().size();
        // Task 6: a calendar refusal is a DISTINCT, expected outcome — it must
        // not hide among the anonymous fit failures the drop report lumps
        // together (the Task 5 observability concern). Every other failure
        // keeps FitFailed; `error` carries the same code either way.
        rep.outcome = calendar_refusal ? ExpiryFitOutcome::FitRefusedCalendar
                                       : ExpiryFitOutcome::FitFailed;
        rep.error = fit_code;
        rep.carry_source = pre.carry_source;
        out.expiry_reports.push_back(rep);
        if (in.fail_board_on_hard_slice_error && !prep_error_is_expected(fit_code)) {
          return Err(fit_code, "fit_curve_surface: expiry " + std::to_string(ci) +
                                   " slice fit failed (hard): " + slice_res.error().to_string());
        }
        continue;
      }
    }
    const IVolCurve *const slice = slice_res->get();

    // 4. Score re-Americanized parity off the fitted slice's own iv(k). A parity
    //    (diagnostic) failure is non-fatal: keep the slice, push a zeroed report.
    //    `in.score_parity` opts OUT of this block entirely. The prepared score
    //    rows are the same keyed population as the fit rows, so scoring cannot
    //    silently reintroduce a quote rejected by filtering or the cap.
    //    `parity` stays the default-constructed zeroed
    //    ParityReport{} (n == 0), so `worst` below never advances past its
    //    `infinity` init and `worst_frac_within_bidask` resolves to 0.0 -- the
    //    intended "no diagnostic" sentinel.
    ParityReport parity{};
    // D4: the dof the reduced chi-square was scored against, and whether the
    // scored population could actually support it. Surfaced on the slice's
    // diagnostic so a blanked chi2 is distinguishable from a measured zero.
    std::size_t scored_chi2_dof = 0;
    bool chi2_dof_underdetermined = false;
    if (in.score_parity) {
      const ProfileClock::time_point t_parity0 =
          time_stages ? ProfileClock::now() : ProfileClock::time_point{};
      const PreparedScoreColumns &score = prepared.score_columns();
      if (score.k_log.size() >= 4u) {
        std::vector<double> model_iv;
        model_iv.reserve(score.k_log.size());
        for (const double k_log : score.k_log) {
          model_iv.push_back(slice->iv(k_log));
        }
        ParityInputs pin{};
        pin.S = in.S;
        pin.r = pre.rate;
        pin.q_eff = q_eff;
        pin.T = T;
        pin.exercise_style = prepared.provenance().exercise_style;
        pin.method = in.deam.method;
        pin.al_opts = in.deam.al_opts;
        pin.band_k = in.band_k;
        // D4: the FITTED family's own dof, not a nominal 3. chi2_reduced is
        // chi2/(N - dof), so a dof that is too small inflates the denominator and
        // makes the served number systematically OPTIMISTIC. The constant 3 was
        // right only for eSSVI; raw SVI has 5, and the node-based families have
        // as many as their node count. `curve_selector.cpp` already scores itself
        // off `curve.dof()`, so this brings the served path in line with the
        // correct caller rather than inventing a convention.
        const std::size_t curve_dof = slice->dof();
        pin.n_curve_params = curve_dof;
        // Re-Americanize through the same cache policy that produced the one
        // canonical European row. A cold fit must not score against a cached
        // inverse map (or vice versa).
        pin.caches =
            allow_fit_cache(in, slice_cfg.kind) ? in.deam.caches : AmericanCorrectionCaches{};
        auto pr = chain_parity(score.strike, score.bid, score.ask, score.mid, score.side, model_iv,
                               score.market_iv, pin);
        // Witness the dof that was ACTUALLY scored by reading it back off the
        // struct that was passed, rather than recomputing it alongside. A
        // reported dof derived independently of the call can drift from the dof
        // the call used, and then it documents an intention instead of a fact.
        scored_chi2_dof = pin.n_curve_params;
        if (!pr) {
          // The ONLY dof-dependent failure `chain_parity` has is
          // `reduced_chi_square`'s N > dof precondition (fit_metrics.cpp:95), so
          // reaching here with a dof that just grew means the scored population
          // cannot support the true dof. An INTERPOLATING family hits this by
          // construction -- LinearVariance's dof IS its node count -- and for
          // those the reduced statistic is genuinely undefined, not merely
          // unavailable.
          //
          // The BAND evidence is a different matter: it does not depend on dof
          // at all, and dropping it is not free. `session.cpp` averages over
          // EVERY per_expiry entry and takes `worst = min(worst, ...)`, so a
          // default-constructed report publishes frac_fv_within_bidask == 0 --
          // indistinguishable from a surface that reprices nothing in band --
          // while `parity_state` stays Valid because the entry count still
          // matches. That is the D1 defect shape: no measurement laundered as a
          // measured failure. Measured on lqbench, letting that happen cost 9 of
          // 240 boards their in-band evidence and moved the corpus mean from
          // 0.9652 to 0.9293.
          //
          // So re-score with dof = 0. That keeps the band evidence AND leaves a
          // DEFINED goodness-of-fit number (chi2 per observation) in
          // chi2_reduced.
          //
          // Deliberately NOT blanked to 0. An exact zero chi-square reads as a
          // PERFECT fit — the same misleading-default defect one field over —
          // and W3-A exists precisely to stop this route publishing all-zero
          // diagnostics (pricer_fitter_test's
          // AutoRoutedLinearVarianceMarkAlwaysScoresParity asserts
          // mean_chi2_reduced > 0 on exactly the auto-routed LinearVariance
          // board). The under-determined MARKER on `SliceFitDiagnostics` is what
          // tells a reader this is chi2/N and not a true reduced chi-square;
          // that is the distinction a bare zero could not carry.
          pin.n_curve_params = 0;
          pr = chain_parity(score.strike, score.bid, score.ask, score.mid, score.side, model_iv,
                            score.market_iv, pin);
          chi2_dof_underdetermined = true;
        }
        if (pr) {
          parity = *pr;
        }
      }
      if (time_stages) {
        ms_chain_parity += elapsed_ms(t_parity0, ProfileClock::now());
      }
    }

    // 5. Commit the slice + its context (ascending T by construction).
    if (pre.prep_outcome == SlicePrepOutcome::PreparedLegacyRescue) {
      ++out.n_slices_legacy_rescued; // W3.3: a starved slice recovered under Legacy prep
    }
    if (pre.itm_leg_rescued) {
      ++out.n_slices_itm_rescued; // W1-B: recovered by reading the strike's ITM leg
    }
    // W3.4 (F4): the committed-slice outcome — LinearVariance fallback and
    // Legacy-prep rescue are surfaced distinctly from a clean primary fit.
    {
      ExpiryFitReport rep{};
      rep.chain_index = ci;
      rep.maturity = T;
      rep.n_observations = prepared.fit_observations().size();
      // A k-coverage demotion serves a DIFFERENT family than the board's
      // configured one, which is exactly what `FittedFallbackCurve` names, so it
      // shares that spelling with the linear fallback rather than inventing a
      // second one. It is counted separately on the report.
      rep.outcome = (used_linear_fallback || pre.k_uncovered)
                        ? ExpiryFitOutcome::FittedFallbackCurve
                    : (pre.prep_outcome == SlicePrepOutcome::PreparedLegacyRescue)
                        ? ExpiryFitOutcome::FittedLegacyPrep
                        : ExpiryFitOutcome::Fitted;
      if (pre.k_uncovered) {
        ++out.n_slices_uncovered_parametric;
      }
      rep.carry_source = pre.carry_source; // Decision B: Solved / TermStructureInterp/Extrap
      out.expiry_reports.push_back(rep);
    }
    {
      // T10b (D5): park the fit's own verdict alongside the committed slice.
      // `kind` comes from the CURVE, not from `cfg` — a slice recovered by the
      // LinearVariance fallback is served as a LinearVariance curve, and the
      // per-family coverage table is only readable against the family that
      // actually produced the diagnostic.
      SliceFitDiagnostics sd{};
      sd.chain_index = ci;
      sd.maturity = T;
      sd.kind = slice->kind();
      sd.diag = slice_diag;
      sd.chi2_dof = scored_chi2_dof;
      sd.chi2_dof_underdetermined = chi2_dof_underdetermined;
      out.slice_diagnostics.push_back(sd);
    }
    out.surface.push(std::move(*slice_res));
    out.context.push_back(SliceContext{T, F, pre.borrow, q_eff, prepared.fit_observations().size(),
                                       static_cast<std::size_t>(prepared.n_dropped())});
    {
      // Record this committed slice's quoted k-range for the NEXT pair's
      // tradeable-overlap calendar projection.
      double lo = std::numeric_limits<double>::infinity();
      double hi = -std::numeric_limits<double>::infinity();
      for (const FitObs &o : prepared.fit_observations()) {
        if (std::isfinite(o.k)) {
          lo = std::min(lo, o.k);
          hi = std::max(hi, o.k);
        }
      }
      if (lo <= hi) {
        last_committed_obs_k = {lo, hi};
      }
      // D1 (Task 6): and whether those rows are coverage-admissible — the
      // predicate that decides whether the NEXT ConvexDense slice is fitted
      // with full (pre-Task-3) floor authority or with the Task 3 support band.
      last_committed_covered = slice_k_coverage(prepared.fit_observations()).admissible();
    }
    out.per_expiry.push_back(parity);
    if (parity.n > 0) {
      worst = std::min(worst, parity.frac_fv_within_bidask);
    }

    // Perf C1: carry the prepass's already-computed input certification data
    // straight into the report -- VolaSession::build's certification layer
    // consumes this instead of a second serial resolve_chain_forward +
    // build_observations_european pass. `obs`/`inversion` are COPIED (not
    // moved) so the end-of-function ATX_VOL_PROFILE summary below -- which
    // still reads `prepass[*].prepared->fit_observations().size()` for every
    // usable chain, including fit failures never pushed here -- stays intact;
    // the larger per-chain snapshot vectors are moved (nothing downstream
    // reads them again).
    SliceInputCertification cert;
    cert.carry = pre.carry;
    cert.carry_available = pre.carry_available;
    cert.inversion = prepared.deam_audit();
    cert.obs.assign(prepared.fit_observations().begin(), prepared.fit_observations().end());
    cert.source_mids = std::move(pre.source_mids);
    cert.source_flags = std::move(pre.source_flags);
    cert.chain_mids = std::move(pre.chain_mids);
    cert.chain_flags = std::move(pre.chain_flags);
    cert.chain_bids = std::move(pre.chain_bids);
    cert.chain_asks = std::move(pre.chain_asks);
    cert.chain_ts = std::move(pre.chain_ts);
    out.input_certification.push_back(std::move(cert));
  }

  if (out.surface.empty()) {
    return Err(ErrorCode::NotFound,
               describe_board_refusal(under, in, cfg, prepass, out.expiry_reports));
  }
  out.n_slices = out.surface.n_slices();
  out.worst_frac_within_bidask = std::isfinite(worst) ? worst : 0.0;
  if (time_stages) {
    double ms_forward_borrow = 0.0;
    double ms_obs_eu = 0.0;
    std::size_t n_usable = 0;
    std::size_t n_quotes = 0;
    for (const ChainPrepass &pre : prepass) {
      ms_forward_borrow += pre.ms_forward_borrow;
      ms_obs_eu += pre.ms_obs_eu;
      if (pre.usable) {
        ++n_usable;
        n_quotes += pre.prepared->fit_observations().size();
      }
    }
    if (in.collect_stage_timings) {
      out.fit_timings.carry_solve_ms = ms_forward_borrow;
      out.fit_timings.observation_deam_ms = ms_obs_eu;
      out.fit_timings.slice_fit_ms = ms_fit_slice;
      out.fit_timings.audit_ms = ms_chain_parity;
      out.fit_timings.total_wall_ms = elapsed_ms(t_fit0, ProfileClock::now());
      out.fit_timings.collected = true;
    }
    if (!profile) {
      return Ok(std::move(out));
    }
    detail::log_emitf(LogLevel::Info, LogStream::Stderr,
                      "[ATX_VOL_PROFILE] curve_fit_total=%.3fms prepass_wall=%.3fms "
                      "forward_borrow_sum=%.3fms obs_eu_sum=%.3fms fit_slice_sum=%.3fms "
                      "chain_parity_sum=%.3fms usable=%zu "
                      "slices=%zu quotes=%zu workers=%u",
                      elapsed_ms(t_fit0, ProfileClock::now()), ms_prepass, ms_forward_borrow,
                      ms_obs_eu, ms_fit_slice, ms_chain_parity, n_usable, out.n_slices, n_quotes,
                      in.fit_workers);
  }
  return Ok(std::move(out));
}

} // namespace atx::vol
