#include "atx/vol/surface_parity.hpp"

#include <algorithm>
#include <chrono> // ATX_VOL_PROFILE phase timing (temporary)
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>  // ATX_VOL_PROFILE stderr report (temporary)
#include <cstdlib> // getenv (ATX_VOL_PROFILE)
#include <limits>
#include <optional> // std::nullopt (cold accurate re-inversion on audit failure)
#include <span>     // prepared.fit_observations() view (C1 input certification)
#include <string>   // typed board-refusal message (W2-B)
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/detail/log_emit.hpp"
#include "atx/vol/detail/risk_surface_validation.hpp" // RiskSurfaceValidationConfig (repair band tie)
#include "atx/vol/detail/strip_grid.hpp"              // strip::kCertifiedWingHalfBand (band tie)
#include "atx/vol/arb.hpp"              // arb_check_calendar, ArbViolation
#include "atx/vol/calib.hpp"            // FitObs, FitDiag, CalibOpts
#include "atx/vol/deamer.hpp"           // de_americanize_chain, european_equiv_iv, otm_side
#include "atx/vol/essvi_calib.hpp"      // essvi_fit_slice
#include "atx/vol/detail/parallel_for.hpp"     // parallel_for_dynamic (per-chain prepass fan-out)
#include "atx/vol/parity.hpp"           // chain_parity, ParityInputs, ParityReport
#include "atx/vol/detail/prepared_fitting.hpp" // PreparedSlice legacy compatibility seam
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"    // Underlying, Chain, chain_index
#include "atx/vol/vol_surface.hpp" // VolSurface, EssviParams, Parametrization

// PORT / PARITY NOTES
// -------------------
// * Per-expiry pattern reuse. The de-Americanize -> prepared-slice -> eSSVI-fit
//   path uses PreparedObservationPolicy::LegacyEssviCompatibility to preserve
//   the historical single-expiry row population and arithmetic. The policy
//   is explicit and isolated; new family-neutral consumers use Configured.
//
// * Model-IV read-back. Per-slice re-Am parity reads the model IV from the
//   ASSEMBLED surface via `VolSurface::iv_on_slice(idx, k)` (= sqrt(w_slice/
//   T_slice)), proving the number scored is the one the surface actually
//   serves, not a side computation.
//
// * Calendar no-arb checker (arb.hpp signature used):
//     Result<std::vector<ArbViolation>>
//     arb_check_calendar(const VolSurface& s, double k_min, double k_max,
//                        std::uint32_t n_grid);
//   An EMPTY violation vector means "no calendar arbitrage" (arb.hpp's C
//   `out_n_violations == 0` convention). We sample k in [-3, 3] over 25 grid
//   points (the spec grid) and set calendar_arb_free = violations.empty().

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Calendar no-arb sampling grid (spec: +/-3 over ~25 steps). DIAGNOSTIC ONLY:
// `calendar_arb_free` keeps reporting the strict wing state over this grid.
constexpr double kArbKMin = -3.0;
constexpr double kArbKMax = 3.0;
constexpr std::uint32_t kArbNGrid = 25;
constexpr double kMonotoneKMin = -0.7;
constexpr double kMonotoneKMax = 0.7;

// REPAIR domain for CalendarRepair::Project — exactly the independent risk
// oracle's admission band (RiskSurfaceValidationConfig, +-0.50), which is also
// the strip's certified wing band. Until 2026-08 the Project repair ran over
// the +-3.0 DIAGNOSTIC grid above: 6x beyond any band a gate certifies, where
// both slices' eSSVI wings are pure extrapolation — so it closed
// extrapolation-vs-extrapolation "crossings" by scaling slice ATM total
// variance, fabricating served levels (the sp100-2026 XOM/CVX defect, +8..25
// ATM vol pts). Repair now covers exactly what admission checks; crossings
// beyond the certified band remain visible in the +-3 diagnostic and are
// deliberately not "repaired" by moving levels.
constexpr double kRepairKMax = RiskSurfaceValidationConfig{}.k_max;
constexpr double kRepairKMin = -kRepairKMax;
static_assert(kRepairKMin == RiskSurfaceValidationConfig{}.k_min,
              "Project repair band must equal the risk-admission band");
static_assert(kRepairKMax == strip::kCertifiedWingHalfBand,
              "Project repair band must equal the certified wing band the "
              "variance strip trusts (detail/strip_grid.hpp)");

// W3.4 (F4): an EXPECTED slice failure is a genuinely thin / data-shaped outcome
// (`NotFound` = fewer than the usable-row floor; `Unavailable` = non-positive
// forward, audit-rejected rows). Every other code (`InvalidArgument`, `Internal`,
// `OutOfRange`, …) is a real defect that the completeness contract propagates.
[[nodiscard]] bool slice_error_is_expected(ErrorCode code) noexcept {
  return code == ErrorCode::NotFound || code == ErrorCode::Unavailable;
}

// ── Decision B on the eSSVI lane: board-level term-structure carry ────────
//
// `require_carry_confidence` is a BOARD policy that this driver applied
// PER EXPIRY. `prepare_expiry` resolves its own carry, so a non-confident
// expiry came back as an `Unavailable` carry failure and the whole chain was
// discarded — while `fit_curve_surface`, running the SAME policy on the SAME
// board, probes carry with the gate disarmed and re-applies it at board level
// (curve_fit.cpp:570-577 and its phase-1.5 repair at :737-810), admitting the
// expiry on a borrow read off the term structure of the board's CONFIDENT
// expiries. Measured on VZ-lqbench, 15 chains offered to both lanes:
// convex-dense fitted 14 (1 `PrepStarved`, 0 `CarryFailed`); eSSVI fitted 5 and
// lost 10, every one of them `CarryFailed`. Same policy, same data, opposite
// treatment of the same condition — so the asymmetry is this driver's, and this
// is where it is closed.
//
// The gate is NOT relaxed. A non-confident expiry still never contributes its
// own solve, is never an anchor, and is never stamped `Solved` or `confident`;
// it is admitted only where confident neighbours can price its carry, and it
// carries `TermStructureInterp`/`Extrap` provenance out to admission.
//
// DUPLICATION, declared. `CarryAnchor` / `CarryFallback` /
// `term_structure_fallback_borrow` mirror `curve_fit.cpp:344-391` line for line
// — that file is the SOURCE OF TRUTH for the interpolation rule and its
// citations (van Binsbergen–Diamond–Grotteria 2022 for the PCP-identified
// per-maturity carry; Hagan–West 2006 §3 for linear-on-rates with flat
// extension past the extreme nodes). They are duplicated rather than shared
// because a shared home would have to be carved out of `curve_fit.cpp`, which
// this task does not own. Unifying them is a follow-up, and until then a change
// to one is a change owed to the other.
struct CarryAnchor {
  double T{0.0};
  double borrow{0.0};
};

struct CarryFallback {
  double borrow{0.0};
  CarrySource source{CarrySource::Solved};
};

// INTERIOR (bracketed by confident anchors): linear interpolation of the borrow
// in maturity. EDGE: flat extension of the nearest confident borrow. `anchors`
// MUST be non-empty and ascending in T (guaranteed: chains load ascending-T).
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

// The expiry-specific continuously-compounded rate. `run_surface_parity` has
// already validated that `expiry_rate_T[i] == chains[i].T` for every i when the
// vectors are present, so the chain index IS the rate index — the same lookup
// `prepare_expiry` performs internally.
[[nodiscard]] double expiry_rate_for(const SurfaceParityInputs &in, std::size_t i) noexcept {
  return in.expiry_rates.empty() ? in.r : in.expiry_rates[i];
}

// Re-stamp a repaired expiry's certification carry: keep the raw solve's
// tallies (they describe what the board actually offered) but record that the
// borrow used was BORROWED from the term structure. A fallback carry must never
// be laundered as a solved or a confident one.
[[nodiscard]] CarryDiagnostics as_fallback_carry(const CarryDiagnostics &raw, CarrySource source) {
  CarryDiagnostics out;
  out.n_candidates = raw.n_candidates;
  out.n_attempted = raw.n_attempted;
  out.n_solved = raw.n_solved;
  out.n_retained = raw.n_retained;
  out.effective_pair_count = raw.effective_pair_count;
  out.dispersion = raw.dispersion;
  out.max_leave_one_out_shift = raw.max_leave_one_out_shift;
  out.confidence_half_width = raw.confidence_half_width;
  out.max_pcp_residual = raw.max_pcp_residual;
  out.confident = false;
  out.source = source;
  return out;
}

// ── Calendar-floor-constrained slice fit (active-set) ────────────────────
//
// Fit this slice subject to a calendar floor w(k) >= w_prev(k) over the slice's
// own data k-range, so it does not calendar-cross the previous (shorter-T)
// slice where THIS expiry is quoted. Approach: fit normally, then iterate an
// ACTIVE SET of one-sided floor pseudo-observations — at each currently-violating
// grid point add a heavily-weighted obs targeting w = w_prev(k), and refit. The
// least-squares LM lifts only the violating region toward the floor, giving up
// the minimal fit error needed to stay monotone (vs. a global theta bump that
// lifts the whole slice). Converges when no grid point violates, or after a
// bounded number of passes. `prev == nullptr` (first slice) => a plain fit.
//
// The floor is compared on TOTAL variance (what the surface serves / arb checks)
// but the pseudo-obs is a backbone target with the residual layer DISABLED for
// the floored refit: a heavy pseudo-obs must not be absorbed by (or distort) the
// small additive wing residual. The returned slice keeps its residual from the
// initial fit only if it never needed flooring.
[[nodiscard]] Result<EssviParams> fit_slice_calendar_floored(const PreparedSlice &prepared,
                                                             double T, double F,
                                                             const CalibOpts &opts, FitDiag *diag,
                                                             const EssviParams *prev, double df) {
  const std::span<const FitObs> observations = prepared.fit_observations();
  const std::vector<double> &score_k = prepared.score_columns().k_log;
  const double theta_floor = (prev != nullptr) ? prev->theta : 0.0;
  Result<EssviParams> res = essvi_fit_slice(observations, T, F, opts, diag, theta_floor);
  if (!res || prev == nullptr || score_k.empty()) {
    return res;
  }

  // Enforce the floor only over this slice's own quoted k-range (+ a small
  // margin) — the region where the calendar cross is economically real. Outside
  // it, the wing extrapolation is left free.
  double k_lo = score_k.front();
  double k_hi = score_k.front();
  for (const double k_log : score_k) {
    k_lo = std::min(k_lo, k_log);
    k_hi = std::max(k_hi, k_log);
  }
  constexpr double kMargin = 0.10;
  // Enforce over the slice's own quoted range PLUS a near-money band, so a
  // crossing that sits just outside a narrow (short-expiry) slice's strikes is
  // still caught. The near-money band is where calendar crossings are
  // economically real; deep wings are left free (documented extrapolation).
  constexpr double kNearMoneyK = 0.7;
  k_lo = std::min(k_lo - kMargin, -kNearMoneyK);
  k_hi = std::max(k_hi + kMargin, kNearMoneyK);

  double w_base = 0.0; // heaviest base weight → penalty scale
  for (const FitObs &o : observations) {
    w_base = std::max(w_base, o.weight_w);
  }
  const double penalty = (w_base > 0.0 ? w_base : 1.0) * 300.0;

  constexpr int kNGrid = 80;
  constexpr int kMaxPass = 8;
  const double dk = (k_hi - k_lo) / static_cast<double>(kNGrid);

  CalibOpts floored_opts = opts;
  floored_opts.residual_disable = true; // keep pseudo-obs out of the residual

  std::vector<FitObs> aug;
  aug.reserve(observations.size() + static_cast<std::size_t>(kNGrid) + 1);
  for (int pass = 0; pass < kMaxPass; ++pass) {
    aug.assign(observations.begin(), observations.end());
    bool violated = false;
    for (int gi = 0; gi <= kNGrid; ++gi) {
      const double k = k_lo + static_cast<double>(gi) * dk;
      const double wp = essvi_total_w(*prev, k);
      const double wc = essvi_total_w(*res, k);
      if (wp > wc + 1.0e-12) {
        violated = true;
        FitObs o{};
        o.k = k;
        o.w_mkt = wp;
        o.sigma_mkt = std::sqrt(std::fmax(wp, 1.0e-12) / T);
        o.weight_w = penalty;
        o.active_weight_w = penalty;
        o.F = F;
        o.K = F * std::exp(k);
        o.df = df;
        o.side = otm_side(k);
        aug.push_back(o);
      }
    }
    if (!violated) {
      break; // floor satisfied over the whole grid
    }
    Result<EssviParams> r2 = essvi_fit_slice(aug, T, F, floored_opts, diag, theta_floor);
    if (!r2) {
      break; // keep the last good fit rather than fail the whole surface
    }
    res = std::move(r2);
  }
  return res;
}

// Perf C1: capture the de-Am input certification for one committed slice from the
// fit's own `PreparedSlice`, so `VolaSession::build` reuses it instead of running
// a SECOND, independent de-Am pass (finding 10). The obs are the exact rows the
// fit de-Americanized; source_mids/flags are the raw chain quotes at each row's
// (strike, side); the chain snapshot lets the incremental refit cache detect a
// carry-coordinate change. A malformed row index (should not happen for a fitted
// slice) leaves the obs cache EMPTY, so the consumer's size checks fall back to
// the full certified path rather than reading out of range.
[[nodiscard]] EssviInputCertification make_input_certification(const PreparedSlice &prepared,
                                                               const Chain &chain) {
  EssviInputCertification cert;
  cert.inversion = prepared.deam_audit();
  const std::span<const FitObs> rows = prepared.fit_observations();
  cert.obs.assign(rows.begin(), rows.end());
  cert.source_mids.reserve(rows.size());
  cert.source_flags.reserve(rows.size());
  for (const FitObs &row : rows) {
    const std::size_t quote_index =
        chain_index(static_cast<std::uint16_t>(row.source_strike_index), row.side);
    if (quote_index >= chain.mids.size() || quote_index >= chain.flags.size()) {
      cert.obs.clear();
      cert.source_mids.clear();
      cert.source_flags.clear();
      break;
    }
    cert.source_mids.push_back(chain.mids[quote_index]);
    cert.source_flags.push_back(chain.flags[quote_index]);
  }
  cert.chain_mids = chain.mids;
  cert.chain_flags = chain.flags;
  cert.chain_bids = chain.bids;
  cert.chain_asks = chain.asks;
  cert.chain_ts = chain.ts_ns;
  return cert;
}

} // namespace

Result<SurfaceParityReport> run_surface_parity(const Underlying &under,
                                               const SurfaceParityInputs &in) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument, "run_surface_parity: non-positive S or non-finite r");
  }
  const std::size_t n_chains = under.chains.size();
  if (n_chains == 0) {
    return Err(ErrorCode::NotFound, "run_surface_parity: underlying carries no chains");
  }
  if (!((in.expiry_rates.empty() && in.expiry_rate_T.empty()) ||
        (in.expiry_rates.size() == n_chains && in.expiry_rate_T.size() == n_chains))) {
    return Err(ErrorCode::InvalidArgument, "run_surface_parity: invalid expiry rate vectors");
  }
  for (std::size_t i = 0u; i < in.expiry_rates.size(); ++i) {
    if (!std::isfinite(in.expiry_rates[i]) || !std::isfinite(in.expiry_rate_T[i]) ||
        !(in.expiry_rate_T[i] > 0.0) || in.expiry_rate_T[i] != under.chains[i].T) {
      return Err(ErrorCode::InvalidArgument, "run_surface_parity: invalid expiry rate value");
    }
  }

  ATX_TRY(VolSurface surface, VolSurface::create(under.uid, Parametrization::Essvi, n_chains));

  std::vector<double> expiry_T;
  std::vector<ParityReport> per_expiry;
  std::vector<SliceContext> context;
  std::vector<CarryDiagnostics> carry_diag;
  std::vector<EssviInputCertification> input_certs; // C1: ‖ context/carry_diag
  expiry_T.reserve(n_chains);
  per_expiry.reserve(n_chains);
  context.reserve(n_chains);
  carry_diag.reserve(n_chains);
  input_certs.reserve(n_chains);

  // Everything a slice needs to be SCORED after the surface is fully assembled
  // (and possibly calendar-repaired). We defer scoring out of the fit loop so
  // the model IV read back is the one the FINAL surface serves — if a repair
  // pass moves a slice, the parity number reflects the moved slice, not a stale
  // pre-repair read.
  struct PendingSlice {
    PreparedSlice prepared; // keyed fit rows + raw scoring population
    double T{0.0};          // slice maturity
    double rate{0.0};       // expiry-specific continuously-compounded rate
    double q_eff{0.0};      // effective carry for the re-Am scoring
    ExerciseStyle exercise_style{ExerciseStyle::American};
    std::uint16_t slice_idx{0}; // surface write index for iv_on_slice read-back
  };
  std::vector<PendingSlice> pending;
  pending.reserve(n_chains);

  // W3.4 (F4): per-expiry build outcome for EVERY chain walked (‖ under.chains,
  // in chain order). Lets admission distinguish a thin/absent expiry from a real
  // defect and stop treating a partial fit as a clean success.
  std::vector<ExpiryFitReport> expiry_reports;
  expiry_reports.reserve(n_chains);

  double worst = std::numeric_limits<double>::infinity();
  std::size_t idx = 0; // ascending write index / fitted-slice count
  // Expiries dropped because carry could not be resolved (confidence gate /
  // no quotable pair / degenerate forward). Surfaced through the report so a
  // risk surface missing an expiry never reports clean health with no trace
  // (§5.2: "uncertain carry is surfaced, not hidden").
  std::size_t n_carry_skipped = 0;
  // Expiries dropped because the fit-inversion AUDIT starved the slice below
  // the usable-observation floor: it would have fit but for audit drops. The
  // same silent-gap pattern as a carry skip, reached through §8.1's audit —
  // counted and surfaced identically. Genuinely sparse slices (too few valid
  // quotes regardless of the audit) keep the historical silent-skip shape.
  std::size_t n_audit_starved = 0;

  // Calendar-monotone fit (CalendarRepair::MonotoneFit): carry the previous
  // fitted slice forward so the next slice can be fit with a calendar floor
  // w(k) >= w_prev(k) over its data range (theta floor + active-set w-floor).
  EssviParams prev_slice{};
  bool has_prev = false;

  // ── Optional phase profile (ATX_VOL_PROFILE=1) ─────────────────────────
  // Temporary build-cost breakdown for the perf-tuning pass; zero cost when the
  // env var is unset. Times the three per-chain phases + the calendar check.
  std::size_t env_sz = 0;
  char env_buf[8] = {};
  const bool profile =
      getenv_s(&env_sz, env_buf, sizeof(env_buf), "ATX_VOL_PROFILE") == 0 && env_sz > 0;
  const bool time_stages = profile || in.collect_stage_timings;
  const auto now_ns = []() noexcept {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  const double fit_start = time_stages ? now_ns() : 0.0;
  double ms_carry = 0.0;
  double ms_deam = 0.0;
  double ms_fit = 0.0;

  // 1-2. One shared carry + observation seam per expiry. FT-C8: honor the
  // configured `fit_prep_policy` (the full CalibOpts filter cascade) when the
  // flag-guarded rollout is enabled; default stays the permissive
  // LegacyEssviCompatibility predicate (byte-identical).
  const PreparedObservationPolicy prep_policy =
      in.essvi_serve_configured_prep ? in.fit_prep_policy
                                     : PreparedObservationPolicy::LegacyEssviCompatibility;

  // FT-P: the per-expiry preparation (carry solve + de-Am observation build) is
  // the dominant per-chain cost and INDEPENDENT across expiries, so fan it out
  // over `in.fit_workers` into DISJOINT per-chain slots (prepare_expiry is a pure
  // function of (chain, in) and reads in.deam.caches read-only — the same
  // concurrent-read contract fit_curve_surface's prepass relies on). The fit +
  // set_slice + scoring pass below stays SEQUENTIAL in ascending T (the
  // MonotoneFit calendar floor is a loop-carry), so the assembled surface, its
  // per-slice diagnostics and slice order are BIT-IDENTICAL for any worker count
  // — only the embarrassingly-parallel prepass fans out. (fit_workers: 0 => auto,
  // 1 => serial byte-for-byte, N => N workers.)
  struct PreppedSlot {
    std::optional<Result<CanonicalPreparedExpiry>> result; // nullopt => T<=0 skip
    PrepareExpiryDiagnostics prep_diag{};
    // Decision B state. `confident` + `borrow` make this expiry an ANCHOR of the
    // board's borrow term structure; `needs_carry_repair` marks one the board
    // gate DEFERRED (its own solve exists but is not trusted, so it is neither
    // used nor an anchor) awaiting a borrowed carry.
    bool carry_confident{false};
    double borrow{0.0};
    bool needs_carry_repair{false};
    CarrySource carry_source{CarrySource::Solved};
  };
  std::vector<PreppedSlot> slots(n_chains);
  parallel_for_dynamic(n_chains, in.fit_workers, [&](std::size_t i, unsigned) {
    if (!(under.chains[i].T > 0.0)) {
      return; // degenerate maturity: leave nullopt; phase 2 records the skip
    }
    try {
      slots[i].result = prepare_expiry(under.chains[i], static_cast<std::uint32_t>(i), in,
                                       prep_policy, &slots[i].prep_diag);
    } catch (...) {
      // A worker escape would std::terminate the jthread; record a failed slot
      // (the essvi parallel precedent). prepare_expiry uses Result, so this is
      // defensive only.
      slots[i].result.emplace(
          Err(ErrorCode::Internal, "run_surface_parity: prepare_expiry threw"));
    }
    PreppedSlot &slot = slots[i];
    if (slot.result->has_value()) {
      // A confident expiry anchors the term structure even if it is the only one
      // that fits; `prepare_expiry` cannot succeed under the gate without it.
      slot.carry_confident = slot.prep_diag.carry.confident;
      slot.borrow = (*slot.result)->borrow;
      return;
    }
    if (!in.deam.require_carry_confidence || !slot.prep_diag.carry_failed) {
      return; // starved / hard defect / gate off: nothing for Decision B to do
    }
    // The gate refused, but `prepare_expiry` cannot say whether the carry was
    // merely UNTRUSTED or genuinely unresolvable, and only the first is
    // repairable. Re-probe THIS expiry with the gate disarmed — a solve the
    // current code pays for and then discards outright, so it is spent only
    // where an expiry is otherwise lost, and a board with no carry drops runs
    // bit-identically and at bit-identical cost.
    DeAmOptions probe = in.deam;
    probe.require_carry_confidence = false;
    const Result<ChainForward> probed = resolve_chain_forward(
        under.chains[i], in.S, expiry_rate_for(in, i), in.cash_divs, in.now_ts_ns, probe);
    if (!probed.has_value() || !(probed->forward > 0.0) || !std::isfinite(probed->forward)) {
      return; // a REAL carry failure: not the gate, and not repairable
    }
    slot.prep_diag.carry = probed->carry; // raw tallies, re-stamped after repair
    slot.needs_carry_repair = true;
  });

  // ── Phase 1.5: the board-level term-structure carry repair ────────────────
  // A deferred expiry is admitted on a borrow DERIVED from the confident
  // expiries' borrow-vs-T structure and then de-Americanized/prepared like any
  // other slice. With ZERO confident anchors nothing is fabricated: the deferred
  // expiries stay dropped and the board behaves exactly as it did before.
  {
    std::vector<CarryAnchor> anchors; // ascending T (chains load ascending-T)
    for (std::size_t i = 0; i < n_chains; ++i) {
      if (slots[i].carry_confident) {
        anchors.push_back(CarryAnchor{under.chains[i].T, slots[i].borrow});
      }
    }
    std::vector<std::size_t> repair_idx;
    std::vector<double> repair_borrow;
    if (!anchors.empty()) {
      for (std::size_t i = 0; i < n_chains; ++i) {
        if (!slots[i].needs_carry_repair) {
          continue;
        }
        const CarryFallback fb = term_structure_fallback_borrow(under.chains[i].T, anchors);
        if (!std::isfinite(fb.borrow)) {
          continue; // leave the expiry on its existing carry failure
        }
        slots[i].carry_source = fb.source;
        repair_idx.push_back(i);
        repair_borrow.push_back(fb.borrow);
      }
    }
    // Same disjoint-slot fan-out as phase 1. `imply_borrow = false` is how the
    // borrowed carry is injected: `resolve_chain_forward` then returns
    // hybrid_forward(S, r, borrow_fixed, T, ...) — bit-identical to what the
    // curve lane computes at curve_fit.cpp:766 — and validates it is positive
    // and finite, so an unusable fallback forward fails safe into the existing
    // carry-skip path rather than fitting garbage. A slice too thin even with a
    // valid carry still starves truthfully.
    parallel_for_dynamic(repair_idx.size(), in.fit_workers, [&](std::size_t task, unsigned) {
      const std::size_t i = repair_idx[task];
      PreppedSlot &slot = slots[i];
      SurfaceParityInputs repair_in = in;
      repair_in.deam.imply_borrow = false;
      repair_in.deam.borrow_fixed = repair_borrow[task];
      // `require_carry_confidence` is deliberately LEFT AS THE CALLER SET IT.
      // It does not need clearing and must not be cleared: with `imply_borrow`
      // false both carry solvers return the fixed-borrow forward before the
      // confidence gate is reached (deamer.cpp:523 and :688, against the gate at
      // :790), so the flag is unreachable on this path. Clearing it would change
      // nothing except how this code reads.
      const CarryDiagnostics raw = slot.prep_diag.carry;
      PrepareExpiryDiagnostics repair_diag{};
      std::optional<Result<CanonicalPreparedExpiry>> repaired;
      try {
        repaired = prepare_expiry(under.chains[i], static_cast<std::uint32_t>(i), repair_in,
                                  prep_policy, &repair_diag);
      } catch (...) {
        // Defensive only, as in phase 1: a worker escape would terminate.
      }
      if (!repaired.has_value() || !repaired->has_value()) {
        // The borrowed carry did not rescue this expiry. Keep the ORIGINAL
        // outcome — the repair may add slices, never re-label a failure.
        return;
      }
      // CERTIFIABILITY IS A PRECONDITION OF SERVING, and this is the one place
      // the eSSVI lane must NOT copy the curve lane, which commits blind.
      //
      // MEASURED, not assumed. The eSSVI lane FITS under the permissive
      // predicate but its inversion certificate is recomputed under the
      // CONFIGURED cascade (`collect_input_diagnostics`, session.cpp:526-541 —
      // the C1 reuse is disabled precisely when `audit_fit_inversions` is on,
      // session.cpp:1542). A slice only the permissive predicate can build
      // therefore has no certificate at all: the recompute returns `NotFound`,
      // `inversion_available` stays false, and `inversion_certified` is
      // ALL-slices-or-nothing (session.cpp:603), so ONE such slice sets
      // `InversionResidual` for the whole board. Combined with the `CarryGap`
      // the fallback itself raises, that turns a Degraded-but-published
      // candidate into a rejected one. On HBAN-lqbench the repair added three
      // expiries whose own de-Am audit was clean (0 drops) but whose Configured
      // recompute kept 1, 3 and 4 rows of 22, 17 and 13 — and the board fell
      // from 3 served eSSVI slices to a 1-slice SVI.
      //
      // So the repaired slice is asked, HERE, the same question admission will
      // ask later, with the same call: a repaired expiry that cannot be
      // certified stays dropped exactly as it was. That makes the repair
      // strictly additive — a board can gain expiries, never lose its
      // certification grade. It costs one extra Configured de-Am per repaired
      // expiry, on boards that have carry drops at all; the expiry currently
      // yields nothing, so the pass is spent only where there is something to
      // win. When the audit is not armed nothing can certify on any path and the
      // question is meaningless, so it is asked only under
      // `audit_fit_inversions`.
      if (in.deam.audit_fit_inversions) {
        const Result<ObsSet> cert_obs = build_observations_european(
            under.chains[i], in.S, (*repaired)->rate, (*repaired)->slice.forward(),
            under.chains[i].T, (*repaired)->df, in.calib, in.deam.caches, in.deam.al_opts,
            in.deam.iv_tol, in.deam.iv_max_iter, in.deam.method);
        if (!cert_obs.has_value() ||
            !deam_inversion_certified(cert_obs->deam_audit,
                                      in.calib.max_certified_deam_drop_fraction)) {
          return;
        }
      }
      // Retain the fallback's own timings; the carry provenance is re-stamped so
      // certification and admission see a borrowed carry for what it is.
      repair_diag.carry_solve_ms += slot.prep_diag.carry_solve_ms;
      repair_diag.carry = as_fallback_carry(raw, slot.carry_source);
      repair_diag.carry_available = true;
      repair_diag.carry_failed = false;
      slot.prep_diag = std::move(repair_diag);
      slot.result.reset();
      slot.result.emplace(std::move(*repaired));
    });
  }

  // Chains are stored ascending in T; consume them in that order so slices land
  // in the surface ascending as set_slice_essvi requires. This pass is serial:
  // the calendar floor (MonotoneFit) is a loop-carry and the surface writes are
  // ordered, so the result is invariant to the prepass worker count.
  for (std::size_t chain_index = 0u; chain_index < under.chains.size(); ++chain_index) {
    const Chain &chain = under.chains[chain_index];
    const double T = chain.T;
    if (!(T > 0.0)) {
      expiry_reports.push_back(
          ExpiryFitReport{chain_index, T, ExpiryFitOutcome::Skipped, 0, ErrorCode::Unknown});
      continue; // degenerate maturity: skip (not fatal)
    }

    PrepareExpiryDiagnostics &prep_diag = slots[chain_index].prep_diag;
    Result<CanonicalPreparedExpiry> &prepared_result = *slots[chain_index].result;
    if (time_stages) {
      ms_carry += prep_diag.carry_solve_ms;
      ms_deam += prep_diag.observation_deam_ms;
    }
    if (!prepared_result.has_value()) {
      const ErrorCode prep_code = prepared_result.error().code();
      if (prep_diag.carry_failed) {
        // No slice, but the skip is counted, not hidden (§5.2).
        ++n_carry_skipped;
        expiry_reports.push_back(
            ExpiryFitReport{chain_index, T, ExpiryFitOutcome::CarryFailed, 0, prep_code});
      } else if (!slice_error_is_expected(prep_code)) {
        // W3.4 (F4): a HARD preparation defect (not carry, not thin) — surface it
        // and, under the completeness contract, fail the board instead of
        // silently publishing the rest as a clean partial fit.
        expiry_reports.push_back(
            ExpiryFitReport{chain_index, T, ExpiryFitOutcome::PrepFailed, 0, prep_code});
        if (in.fail_board_on_hard_slice_error) {
          return Err(prep_code, "run_surface_parity: expiry preparation failed (hard): " +
                                    prepared_result.error().to_string());
        }
      } else {
        if (prep_diag.n_fit_rows + prep_diag.n_audit_dropped >= kMinPreparedFitRows) {
          // Fewer than the minimum usable strikes survived, but the slice would
          // have reached the floor but for rows the fit-inversion audit dropped
          // — the gap was CREATED by the audit; count it so admission can
          // surface it instead of serving a silently thinner surface. Genuinely
          // sparse slices keep the historical silent-skip shape.
          ++n_audit_starved;
        }
        expiry_reports.push_back(
            ExpiryFitReport{chain_index, T, ExpiryFitOutcome::PrepStarved, 0, prep_code});
      }
      continue;
    }
    const double rate = prepared_result->rate;
    const double F = prepared_result->slice.forward();
    const double q_eff = prepared_result->q_eff;
    const double borrow = prepared_result->borrow;
    const double df = prepared_result->df;
    PreparedSlice prepared = std::move(prepared_result->slice);

    // 3. Fit the eSSVI slice (natural form, T/F stamped in). MonotoneFit adds a
    //    calendar floor vs. the previous fitted slice (theta floor + active-set
    //    w-floor over the data range); Project constrains the fit CONSTRUCTIVELY
    //    instead, with plan §2.1's (N1)+(N2) backbone ordering against the
    //    previous slice, so its post-hoc projection (step 6) is left a residual
    //    inside its fidelity budget rather than a term structure that inverted.
    //    `None` stays the historical independent per-slice fit, byte for byte.
    const double t_fit = time_stages ? now_ns() : 0.0;
    FitDiag diag{};
    const EssviParams *const calendar_prev =
        (in.repair == CalendarRepair::Project && has_prev) ? &prev_slice : nullptr;
    // (N1) fidelity budget — the same cumulative ATM-level scale contract
    // `arb_project_calendar_essvi` enforces post-fit (arb.hpp,
    // kCalendarRepairMaxAtmShiftFrac), applied to the level the in-fit floor
    // is about to REQUIRE. The projected LM keeps every trial calendar-
    // feasible, so a genuine market inversion at ATM would otherwise be
    // "repaired" inside the fit — exactly the fabrication the projection's
    // budget exists to refuse — with the budget never consulted (measured on
    // the T7a unservable-board fixture: served 3/3 with worst_in_band 0.0 and
    // mean_chi2 5.6e4 before this guard).
    //
    // TWO-STAGE, ON PURPOSE. The nearest-|k| observation's market total
    // variance (the cold seed's own anchor) is only a cheap TRIGGER: one raw
    // quote is deliberately NOT the budget denominator — measured on sp100/CL
    // slice 3 it sat 26% below the slice's true fitted ATM level and refused a
    // board whose floor never bound (adjacent fitted-theta ratio 1.19, served
    // in-band 1.0). A triggered slice is confirmed with ONE unconstrained
    // `essvi_fit_slice` (pure, deterministic): its theta is the slice's own
    // pre-floor ATM level, the exact analogue of the projection's PRE-REPAIR
    // theta that `arb_project_calendar_essvi` sizes its budget off. Within
    // budget the constrained fit proceeds bit-identically; beyond it the
    // BOARD is refused loudly with the projection's own error shape — never a
    // quiet 2/3 drop — matching the pre-(N1) behavior where the post-fit
    // projection's refusal failed the whole build. A failed probe fit falls
    // through to the constrained fit: no new refusal class on probe failure.
    if (calendar_prev != nullptr && calendar_prev->theta > 0.0) {
      double atm_w = 0.0;
      double atm_absk = std::numeric_limits<double>::infinity();
      for (const FitObs &o : prepared.fit_observations()) {
        if (std::fabs(o.k) < atm_absk) {
          atm_absk = std::fabs(o.k);
          atm_w = o.w_mkt;
        }
      }
      const bool triggered =
          atm_w > 0.0 &&
          calendar_prev->theta / atm_w >
              1.0 + std::max(kCalendarRepairMaxAtmShiftFrac, kCalendarRepairMinBudgetW / atm_w);
      if (triggered) {
        const Result<EssviParams> probe =
            essvi_fit_slice(prepared.fit_observations(), T, F, in.calib);
        if (probe.has_value() && probe->theta > 0.0) {
          const double needed_scale = calendar_prev->theta / probe->theta;
          const double budget = 1.0 + std::max(kCalendarRepairMaxAtmShiftFrac,
                                               kCalendarRepairMinBudgetW / probe->theta);
          detail::log_emitf(LogLevel::Info, LogStream::Stderr,
                            "[n1-budget-probe] %s slice=%zu T=%.6f trigger_scale=%.6f "
                            "floor_over_theta_unconstrained=%.6f budget=%.6f verdict=%s",
                            under.ticker.c_str(), idx, T, calendar_prev->theta / atm_w,
                            needed_scale, budget, needed_scale > budget ? "refuse" : "pass");
          if (needed_scale > budget) {
            return Err(ErrorCode::Unavailable,
                       "run_surface_parity: slice " + std::to_string(idx) +
                           " (T=" + std::to_string(T) +
                           ") (N1) calendar floor needs an ATM level scale of " +
                           std::to_string(needed_scale) + ", beyond the fidelity budget " +
                           std::to_string(budget) +
                           " (a genuine market inversion is not servable without fabricating "
                           "the level)");
          }
        }
      }
    }
    Result<EssviParams> slice_res =
        (in.repair == CalendarRepair::MonotoneFit)
            ? fit_slice_calendar_floored(prepared, T, F, in.calib, &diag,
                                         has_prev ? &prev_slice : nullptr, df)
            : essvi_fit_slice(prepared.fit_observations(), T, F, in.calib, &diag,
                              /*theta_floor=*/0.0, /*warm=*/nullptr, calendar_prev);
    if (time_stages)
      ms_fit += now_ns() - t_fit;
    if (!slice_res) {
      // W3.4 (F4): record the fit failure and, under the completeness contract,
      // propagate a HARD fit error instead of silently dropping the slice.
      const ErrorCode fit_code = slice_res.error().code();
      expiry_reports.push_back(ExpiryFitReport{chain_index, T, ExpiryFitOutcome::FitFailed,
                                               prepared.fit_observations().size(), fit_code});
      if (in.fail_board_on_hard_slice_error && !slice_error_is_expected(fit_code)) {
        return Err(fit_code, "run_surface_parity: expiry slice fit failed (hard): " +
                                 slice_res.error().to_string());
      }
      continue; // a slice that fails to fit contributes no slice
    }
    // Decision B provenance: `Solved` for a directly-inferred carry, a
    // TermStructure* value for one this board borrowed from its confident
    // expiries. Admission reads this to tell the two apart.
    expiry_reports.push_back(ExpiryFitReport{chain_index, T, ExpiryFitOutcome::Fitted,
                                             prepared.fit_observations().size(),
                                             ErrorCode::Unknown,
                                             slots[chain_index].carry_source});

    // Stamp this slice's real listed-expiry instant (+ its dense surface
    // write index) so downstream event-bucketing (solve_implied_emove,
    // count_events_at in session.cpp) can bracket against the actual
    // expiry instead of a Calendar365-inverse synthesis from T.
    slice_res->expiry_ns = chain.expiry_ns;
    slice_res->expiry_id = static_cast<std::uint16_t>(idx);

    prev_slice = *slice_res; // carry forward for the next slice's calendar floor
    has_prev = true;

    // 4. Write the slice into the surface at the next ascending index.
    ATX_TRY_VOID(surface.set_slice_essvi(idx, *slice_res));
    expiry_T.push_back(T);

    // 5. Retain the per-slice re-pricing context for the composable facade, and
    //    stash the aligned obs so this slice can be SCORED after the surface is
    //    fully assembled and (optionally) calendar-repaired.
    context.push_back(SliceContext{T, F, borrow, q_eff, prepared.fit_observations().size(),
                                   prepared.n_dropped()});
    // Perf C1: retain the carry diagnostics the preparation's
    // `resolve_chain_forward` already produced for this chain, ‖ context, so
    // `VolaSession::build`'s certification layer can reuse it instead of a
    // second, identical `resolve_chain_forward` call.
    carry_diag.push_back(prep_diag.carry);
    // Perf C1: capture the fit's own de-Am certification for this slice BEFORE
    // `prepared` is moved into `pending`, so VolaSession::build reuses it.
    input_certs.push_back(make_input_certification(prepared, chain));
    pending.push_back(PendingSlice{std::move(prepared), T, rate, q_eff, chain.exercise_style,
                                   static_cast<std::uint16_t>(idx)});

    ++idx;
  }

  if (idx == 0) {
    // W2-B: name the board condition. This driver already built a truthful
    // per-expiry census; discarding it at the boundary is what made a thin board
    // and a de-Americanization-refused board look identical to an operator.
    // (It can only emit Skipped / CarryFailed / PrepStarved / PrepFailed /
    // FitFailed / Fitted — it implements no per-slice rescue, so the four
    // rescue/coverage outcomes are structurally absent here, not merely zero.)
    std::size_t n_starved = 0, n_prep_failed = 0, n_fit_failed = 0, n_skipped = 0;
    for (const ExpiryFitReport &rep : expiry_reports) {
      switch (rep.outcome) {
      case ExpiryFitOutcome::PrepStarved:
        ++n_starved;
        break;
      case ExpiryFitOutcome::PrepFailed:
        ++n_prep_failed;
        break;
      case ExpiryFitOutcome::FitFailed:
        ++n_fit_failed;
        break;
      case ExpiryFitOutcome::Skipped:
        ++n_skipped;
        break;
      case ExpiryFitOutcome::CarryFailed:
      case ExpiryFitOutcome::Fitted:
      case ExpiryFitOutcome::FittedFallbackCurve:
      case ExpiryFitOutcome::FittedLegacyPrep:
      case ExpiryFitOutcome::PrepUncovered:
      case ExpiryFitOutcome::FitRefusedCalendar:
        break; // carry skips are tallied by n_carry_skipped; the rest cannot occur
      }
    }
    return Err(ErrorCode::NotFound,
               "run_surface_parity: no expiry produced a usable eSSVI slice; chains=" +
                   std::to_string(expiry_reports.size()) + " starved=" + std::to_string(n_starved) +
                   " carry_failed=" + std::to_string(n_carry_skipped) + " prep_failed=" +
                   std::to_string(n_prep_failed) + " fit_failed=" + std::to_string(n_fit_failed) +
                   " skipped=" + std::to_string(n_skipped) + "; prep=" +
                   ((prep_policy == PreparedObservationPolicy::LegacyEssviCompatibility)
                        ? "permissive"
                        : "configured"));
  }

  // 6. Calendar no-arbitrage on the assembled surface. Count the raw crossings
  //    BEFORE any repair (independent per-slice fits + wing extrapolation can
  //    cross in total variance), then optionally repair to arb-free.
  const double t_cal = time_stages ? now_ns() : 0.0;
  ATX_TRY(const std::vector<ArbViolation> pre_viols,
          arb_check_calendar(surface, kArbKMin, kArbKMax, kArbNGrid));
  const std::size_t n_calendar_viol_pre = pre_viols.size();
  double ms_repair = 0.0;

  if (in.repair == CalendarRepair::Project && n_calendar_viol_pre > 0) {
    // Backbone theta-bump restores calendar monotonicity of the eSSVI backbone;
    // the residual damper is a no-op for a backbone-only slice but keeps the
    // pass correct if a residual basis is ever fit here. Both are no-ops on a
    // non-eSSVI surface. Repair covers the CERTIFIED band only (kRepairK*):
    // the +-3 diagnostic count above may include wing-extrapolation crossings
    // this pass deliberately leaves alone, so gate the repair on violations
    // WITHIN its own domain. (MonotoneFit needs no post-hoc pass — its theta
    // floor already enforced ATM monotonicity during the fit.)
    const double t_rep = time_stages ? now_ns() : 0.0;
    ATX_TRY(const std::vector<ArbViolation> in_band_viols,
            arb_check_calendar(surface, kRepairKMin, kRepairKMax, kArbNGrid));
    if (!in_band_viols.empty()) {
      ATX_TRY_VOID(arb_project_calendar_essvi(surface, kRepairKMin, kRepairKMax, kArbNGrid));
      ATX_TRY_VOID(arb_repair_calendar_residual(surface, kRepairKMin, kRepairKMax, kArbNGrid));
    }
    if (time_stages)
      ms_repair = now_ns() - t_rep;
  } else if (in.repair == CalendarRepair::MonotoneFit) {
    // The active-set fit uses penalty observations and can leave small
    // between-node crossings. Close those residuals over MonotoneFit's
    // documented near-money guarantee without projecting the extrapolated
    // wings, where a global theta bump materially degrades held fit quality.
    const double t_rep = time_stages ? now_ns() : 0.0;
    ATX_TRY(const std::vector<ArbViolation> near_money_viols,
            arb_check_calendar(surface, kMonotoneKMin, kMonotoneKMax, kArbNGrid));
    if (!near_money_viols.empty()) {
      ATX_TRY_VOID(arb_project_calendar_essvi(surface, kMonotoneKMin, kMonotoneKMax, kArbNGrid));
      ATX_TRY_VOID(arb_repair_calendar_residual(surface, kMonotoneKMin, kMonotoneKMax, kArbNGrid));
    }
    if (time_stages)
      ms_repair = now_ns() - t_rep;
  }
  double ms_calendar = time_stages ? (now_ns() - t_cal) : 0.0;

  // 7. Score per-expiry re-Am parity off the FINAL (possibly repaired) surface:
  //    the model IV is read back via iv_on_slice, so the number scored is the
  //    one the surface actually serves.
  const double t_parity = time_stages ? now_ns() : 0.0;
  // T4 escalation (T10c): banded evidence counters over the same population the
  // per_expiry reports score. Additive observability — nothing below feeds an
  // admission, a score, or the family selection.
  std::size_t n_scored_quotes = 0;
  std::size_t n_in_band_quotes = 0;
  for (const PendingSlice &ps : pending) {
    const PreparedScoreColumns &score = ps.prepared.score_columns();
    std::vector<double> model_iv;
    model_iv.reserve(score.k_log.size());
    for (const double k_log : score.k_log) {
      model_iv.push_back(surface.iv_on_slice(ps.slice_idx, k_log));
    }

    ParityInputs pin{};
    pin.S = in.S;
    pin.r = ps.rate;
    pin.q_eff = ps.q_eff;
    pin.T = ps.T;
    pin.exercise_style = ps.exercise_style;
    pin.method = in.deam.method;
    pin.al_opts = in.deam.al_opts;
    pin.band_k = in.band_k;
    // D4 (T10c): score chi2 against the SERVED slice's own fitted parameter
    // count, read off the FINAL (possibly repaired) surface — the same object
    // `iv_on_slice` just evaluated — not a nominal 3. The hardcode was right
    // only for a backbone-only slice; a residual-armed profile (SPY-like /
    // LiquidSingleName keep `residual_disable = false`) serves 3 + 4 HingeQuad
    // coefficients through `essvi_total_w`, and a too-small dof inflates the
    // (N - dof) denominator, making the served number systematically
    // OPTIMISTIC. Per-slice, not per-surface: `fit_slice_calendar_floored`
    // strips the residual on a floored refit, so dof can differ slice to slice
    // within one board.
    pin.n_curve_params = essvi_slice_dof(surface.essvi_slices()[ps.slice_idx]);
    pin.caches = in.deam.caches; // re-Am through the same hot-path caches
    Result<ParityReport> scored = chain_parity(score.strike, score.bid, score.ask, score.mid,
                                               score.side, model_iv, score.market_iv, pin);
    bool chi2_dof_underdetermined = false;
    if (!scored) {
      // The ONLY dof-dependent failure `chain_parity` has is
      // `reduced_chi_square`'s N > dof precondition, so a failure that a dof-0
      // re-score fixes means the scored population cannot support the true
      // dof. Failing the WHOLE board here (the pre-D4 ATX_TRY) would discard
      // the band evidence, which does not depend on dof at all — the D1 shape:
      // no measurement laundered as a measured failure. Re-score with dof = 0:
      // the band evidence survives and `chi2_reduced` stays a DEFINED number
      // (chi2 per observation), marked by `chi2_dof_underdetermined` rather
      // than blanked to a perfect-looking 0.0 (W3-A). A failure the re-score
      // does NOT fix was never about dof; it propagates exactly as before.
      pin.n_curve_params = 0;
      scored = chain_parity(score.strike, score.bid, score.ask, score.mid, score.side, model_iv,
                            score.market_iv, pin);
      chi2_dof_underdetermined = true;
    }
    ATX_TRY(ParityReport parity, std::move(scored));
    parity.chi2_dof_underdetermined = chi2_dof_underdetermined;
    worst = std::min(worst, parity.frac_fv_within_bidask);
    n_scored_quotes += parity.n;
    n_in_band_quotes += parity.n_within;
    per_expiry.push_back(parity);
  }
  const double ms_parity = time_stages ? (now_ns() - t_parity) : 0.0;

  // 8. Final calendar check on the surface the caller receives.
  const double final_calendar_start = time_stages ? now_ns() : 0.0;
  ATX_TRY(const std::vector<ArbViolation> cal_viols,
          arb_check_calendar(surface, kArbKMin, kArbKMax, kArbNGrid));
  const bool calendar_arb_free = cal_viols.empty();
  if (time_stages) {
    ms_calendar += now_ns() - final_calendar_start;
  }

  if (profile) {
    detail::log_emitf(LogLevel::Info, LogStream::Stderr,
                      "[ATX_VOL_PROFILE] slices=%zu carry=%.1f deam=%.1f fit=%.1f "
                      "repair=%.1f parity=%.1f calendar=%.1f ms viol_pre=%zu "
                      "(carry=forward/borrow solve; deam=per-strike invert; "
                      "parity=re-Am score)",
                      idx, ms_carry, ms_deam, ms_fit, ms_repair, ms_parity, ms_calendar,
                      n_calendar_viol_pre);
  }

  SurfaceParityReport out{
      .surface = std::move(surface),
      .expiry_T = std::move(expiry_T),
      .per_expiry = std::move(per_expiry),
      .context = std::move(context),
      .carry = std::move(carry_diag),
      .input_certification = std::move(input_certs),
      .expiry_reports = std::move(expiry_reports), // W3.4 (F4): ‖ under.chains, chain order
      .worst_frac_within_bidask = worst,
      .calendar_arb_free = calendar_arb_free,
      .n_slices = idx,
      .n_calendar_viol_pre = n_calendar_viol_pre,
      .n_carry_skipped = n_carry_skipped,
      .n_audit_starved = n_audit_starved,
      .n_scored = n_scored_quotes,
      .n_in_band = n_in_band_quotes,
      .n_out_of_band = n_scored_quotes - n_in_band_quotes,
  };
  if (in.collect_stage_timings) {
    out.fit_timings.carry_solve_ms = ms_carry;
    out.fit_timings.observation_deam_ms = ms_deam;
    out.fit_timings.slice_fit_ms = ms_fit;
    out.fit_timings.audit_ms = ms_parity;
    out.fit_timings.calendar_validation_ms = ms_calendar;
    out.fit_timings.total_wall_ms = now_ns() - fit_start;
    out.fit_timings.collected = true;
  }
  return Ok(std::move(out));
}

} // namespace atx::vol
