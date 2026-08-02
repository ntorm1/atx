#pragma once

// Uniform, configurable volatility-curve family — the single abstraction the
// PricerFitter fits and serves, whatever the underlying or curve type.
//
// atx-vol historically had NO curve polymorphism: eSSVI, raw-SVI, and the
// arb-free convex dense fit were standalone concrete structs with divergent
// free-function evaluators (some expose total variance `w`, some `iv`, some only
// a call price), and four separate surface containers hand-duplicated the same
// linear-in-total-variance time interpolation. Worse, the 99.5%-in-band convex
// dense fit was only ever driven from bench/example code — `PricerFitter` /
// `VolaSession` were hardwired to eSSVI and could not serve it.
//
// This header unifies them behind one interface:
//
//   * `IVolCurve`   — one fitted expiry slice. `w(k)` / `iv(k)` at log-moneyness
//                     k = ln(K / F_slice); carries its own (T, F, df) and reports
//                     an effective `dof()` (the CurveSelector's parsimony signal).
//   * `ConvexDenseCurve` / `EssviCurve` / `SviCurve` — thin adapters that OWN the
//                     concrete params (`ConvexSliceFit` / `EssviParams` /
//                     `SviParams`) and forward to the existing evaluators.
//   * `CurveSurface` — one ascending-T stack of `IVolCurve`s with a SINGLE
//                     linear-in-total-variance time interpolation + the Sprint-26
//                     no-extrapolation guards (the logic copy-pasted 4x before).
//   * `CurveConfig` / `fit_slice_curve` — the per-slice fit dispatch: every kind
//                     fits from the SAME de-Americanized European observation set
//                     (`build_observations_european`), so the fold + downstream
//                     re-Americanization round-trip identically across kinds.
//
// ## Where the virtual dispatch sits (perf note)
//
// The interface is virtual ONLY at the per-slice query layer. A surface query
// resolves a T-bracket once and makes at most two `w`/`iv` vcalls; the option
// valuation that follows is an American re-pricing (~6 us/option — the correction
// eval dominates). Two vcalls against that are free, so the family keeps the
// house "no virtual on the arithmetic hot path" property: the closed-form
// evaluators (`essvi_total_w`, `svi_total_w`, Black-76 inversion) stay non-virtual;
// only the thin slice wrapper is polymorphic.
//
// ## Thread-safety
//
// A fitted `IVolCurve` / `CurveSurface` is logically immutable after construction;
// concurrent `w`/`iv` reads are safe. ConvexDense's numerical-wing fallback cache
// is constructed once and safely published on its first actual fallback.
// `fit_slice_curve` is a pure function of its inputs.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "atx/vol/c8.hpp"           // C8Params
#include "atx/vol/calib.hpp"        // CalibOpts, FitObs
#include "atx/vol/dense_slice.hpp"  // ConvexSliceFit, ConvexFitOpts
#include "atx/vol/spline_curve.hpp" // SplineVolParams, SplineFitOpts, kSrMoneynessGrid
#include "atx/vol/types.hpp"       // Result
#include "atx/vol/vol_surface.hpp" // EssviParams, SviParams, essvi_total_w, svi_total_w

namespace atx::vol {

// Calendar pair-projection diagnostics, defined in arb.hpp. Forward-declared
// here (not included: arb.hpp already includes THIS header) so SplineVolCurve
// can return it from `project_calendar` below — the type is only completed at
// the projection's definition site (spline_curve.cpp includes arb.hpp).
struct CalendarPairProjection;

// ── Curve family tag ────────────────────────────────────────────────────────
//
// The selectable curve types. ConvexDense is the penny-dense arb-free fit (SPY);
// Essvi/Svi are the parsimonious parametric backbones (single-name / sparse
// boards). C8 / CStar are deferred (their evaluators are partial/unported).
enum class VolCurveKind : std::uint8_t {
  ConvexDense = 0,
  Essvi = 1,
  Svi = 2,
  // Direct piecewise-linear interpolation of de-Americanized total variance.
  // This is the low-latency market-mark curve: O(M log M) construction, compact
  // contiguous nodes, and exact reproduction at retained quote strikes.
  LinearVariance = 3,
  // Event-capable SVI-JW backbone plus compact ATM/wing curvature bumps.
  // Unlike SVI/eSSVI this admits the negative ATM curvature seen around
  // earnings and scheduled announcements while remaining only eight DoF.
  C8 = 4,
  // SpiderRock SRCubic-style: a cubic natural spline over standardized
  // moneyness of the vol MULTIPLE sigma(K)/sigma_ATM, on a fixed 29-point
  // grid (see spline_curve.hpp). Not in `default_selector_candidates()` v1.
  SplineVol = 5,
};

// Human-readable tag (for diagnostics / bench output). Never nullptr.
[[nodiscard]] const char *to_string(VolCurveKind kind) noexcept;

// ── The uniform per-slice curve ─────────────────────────────────────────────
//
// One fitted expiry. Evaluated at log-moneyness k = ln(K / F()); `w` is total
// variance sigma^2 * T, `iv` is the European-equivalent lognormal vol.
class IVolCurve {
public:
  virtual ~IVolCurve() = default;

  // Total variance w = sigma^2 * T at log-moneyness `k_log`. NaN outside the
  // curve's valid domain (e.g. the convex fit's no-arb price band).
  [[nodiscard]] virtual double w(double k_log) const noexcept = 0;

  // European-equivalent implied vol. Default: sqrt(w(k_log) / T). Overridden by
  // ConvexDenseCurve to use the fit's native Black-76 inversion directly.
  [[nodiscard]] virtual double iv(double k_log) const noexcept;

  [[nodiscard]] virtual VolCurveKind kind() const noexcept = 0;

  // True iff `k_log` is outside the region this curve fit from tradeable quotes
  // (pure extrapolation). Default false — a parametric curve is defined and
  // meaningful across the whole line. Overridden by SplineVolCurve, whose natural
  // spline flat-extrapolates beyond its observed moneyness range; the served
  // calendar check uses this to avoid certifying a non-tradeable wing crossing as
  // arbitrage. Never used on the arithmetic hot path.
  [[nodiscard]] virtual bool is_extrapolated(double /*k_log*/) const noexcept { return false; }

  // Effective degrees of freedom — the CurveSelector's parsimony tie-break signal
  // (fewer DoF wins a near-tie, penalising an overfit dense curve on a sparse
  // board). Parametric curves return their parameter count; the dense fit returns
  // its node count.
  [[nodiscard]] virtual std::size_t dof() const noexcept = 0;

  // Deep copy — an independently-owned slice with identical params. Needed because
  // `CurveSurface` is move-only (it owns `unique_ptr` slices), so any consumer that
  // must duplicate a fitted surface (e.g. snapshot a live session for archiving)
  // clones each slice through this. The copy is a pure value with no aliasing.
  [[nodiscard]] virtual std::unique_ptr<IVolCurve> clone() const = 0;

  [[nodiscard]] double T() const noexcept { return T_; }
  [[nodiscard]] double F() const noexcept { return F_; }
  [[nodiscard]] double df() const noexcept { return df_; }

protected:
  IVolCurve(double T, double F, double df) noexcept : T_(T), F_(F), df_(df) {}
  double T_{0.0};
  double F_{0.0};
  double df_{0.0};
};

// ── Concrete adapters ───────────────────────────────────────────────────────

// Arb-free convex dense fit (the 99.5%-in-band SPY curve). Owns a ConvexSliceFit;
// `iv` is the fit's own Black-76 inversion of the convex call-price nodes.
class ConvexDenseCurve final : public IVolCurve {
public:
  explicit ConvexDenseCurve(ConvexSliceFit fit) noexcept;

  [[nodiscard]] double w(double k_log) const noexcept override;
  [[nodiscard]] double iv(double k_log) const noexcept override;
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::ConvexDense; }
  [[nodiscard]] std::size_t dof() const noexcept override { return fit_.u.size(); }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<ConvexDenseCurve>(fit_);
  }

  [[nodiscard]] const ConvexSliceFit &fit() const noexcept { return fit_; }

private:
  struct FiniteAnchors {
    std::vector<double> k;
    std::vector<double> w;
  };

  [[nodiscard]] const FiniteAnchors *finite_anchors() const noexcept;

  ConvexSliceFit fit_;
  // Finite total-variance anchors used only when price-to-IV inversion becomes
  // ill-conditioned at a deep intrinsic/zero-price wing. They turn a numerical
  // inversion boundary into a controlled Lee-slope wing, never a NaN. Ordinary
  // queries never touch the synchronization state or pay the anchor inversions.
  mutable std::once_flag finite_anchors_once_{};
  mutable std::unique_ptr<FiniteAnchors> finite_anchors_owner_{};
  mutable std::atomic<const FiniteAnchors *> finite_anchors_published_{nullptr};
};

// eSSVI backbone (3 DoF, or 4 with asymmetric rho). Owns an EssviParams slice.
class EssviCurve final : public IVolCurve {
public:
  EssviCurve(const EssviParams &slice, double df) noexcept;

  [[nodiscard]] double w(double k_log) const noexcept override {
    return essvi_total_w(slice_, k_log);
  }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::Essvi; }
  [[nodiscard]] std::size_t dof() const noexcept override {
    return slice_.rho_scale > 0.0 ? 4u : 3u;
  }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<EssviCurve>(slice_, df_);
  }

  [[nodiscard]] const EssviParams &slice() const noexcept { return slice_; }

private:
  EssviParams slice_;
};

// Raw-SVI backbone (5 DoF). Owns a SviParams slice.
class SviCurve final : public IVolCurve {
public:
  SviCurve(const SviParams &slice, double df) noexcept;

  [[nodiscard]] double w(double k_log) const noexcept override {
    return svi_total_w(slice_, k_log);
  }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::Svi; }
  [[nodiscard]] std::size_t dof() const noexcept override { return 5u; }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<SviCurve>(slice_, df_);
  }

  [[nodiscard]] const SviParams &slice() const noexcept { return slice_; }

private:
  SviParams slice_;
};

// Cache-friendly direct market curve. Nodes are sorted by log-moneyness and
// queried with linear total-variance interpolation (flat at the two wings).
class LinearVarianceCurve final : public IVolCurve {
public:
  LinearVarianceCurve(double T, double F, double df, std::vector<double> k,
                      std::vector<double> total_variance) noexcept;

  [[nodiscard]] double w(double k_log) const noexcept override;
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::LinearVariance; }
  [[nodiscard]] std::size_t dof() const noexcept override { return k_.size(); }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<LinearVarianceCurve>(T_, F_, df_, k_, w_);
  }

  [[nodiscard]] std::span<const double> k_nodes() const noexcept { return k_; }
  [[nodiscard]] std::span<const double> w_nodes() const noexcept { return w_; }

private:
  std::vector<double> k_;
  std::vector<double> w_;
};

// Event smile: SVI-JW plus three compact curvature bumps (8 DoF).
class C8Curve final : public IVolCurve {
public:
  C8Curve(const C8Params &slice, double df) noexcept;

  [[nodiscard]] double w(double k_log) const noexcept override { return c8_slice_w(slice_, k_log); }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::C8; }
  [[nodiscard]] std::size_t dof() const noexcept override { return 8u; }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<C8Curve>(slice_, df_);
  }

  [[nodiscard]] const C8Params &slice() const noexcept { return slice_; }

private:
  C8Params slice_;
};

// SpiderRock SRCubic-style vol-multiple cubic spline (active-knot count DoF).
// Owns a SplineVolParams slice; see spline_curve.hpp for the fit algorithm and
// the file-top comment on WHY this class lives here rather than in
// spline_curve.hpp itself (breaks a header cycle: SplineVolParams/SplineFitOpts
// have no IVolCurve dependency and are consumed by CurveConfig below).
class SplineVolCurve final : public IVolCurve {
public:
  SplineVolCurve(SplineVolParams p, double T, double F, double df);

  [[nodiscard]] double w(double k_log) const noexcept override;
  [[nodiscard]] VolCurveKind kind() const noexcept override { return VolCurveKind::SplineVol; }
  [[nodiscard]] std::size_t dof() const noexcept override { return p_.z.size(); }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<SplineVolCurve>(p_, T_, F_, df_);
  }

  [[nodiscard]] const SplineVolParams &params() const noexcept { return p_; }

  // Project this served slice onto the calendar cone above a previously admitted
  // total-variance curve `w_prev`, the SplineVol analogue of the Essvi/Svi/C8
  // `arb_project_calendar_*_pair` calls in fit_slice_curve. Computes the max
  // additive shared-grid deficit max_k [w_prev(k) - w(k)]_+ over the comparable
  // ([-k,k] / n_grid) points and, if positive, applies a SINGLE uniform additive
  // total-variance offset (SplineVolParams::w_offset) that clears the whole grid
  // at once -- exactly the parallel level shift SVI's `a` / C8's `v` apply.
  // A uniform offset (vs a per-knot multiple lift) is immune to the natural
  // cubic's between-knot ringing and needs no iteration, so it always converges;
  // it preserves the fitted skew SHAPE (only the level rises) and fires only on
  // a genuine crossing (offset stays 0 otherwise, incl. the front expiry with no
  // w_prev). Grid points where either curve is non-finite (deep-wing spline
  // undershoot) are skipped, matching the downstream calendar check. Returns
  // Unavailable only if NO grid point is comparable (degenerate pair). Defined in
  // spline_curve.cpp, beside the natural-spline core it reuses.
  [[nodiscard]] Result<CalendarPairProjection>
  project_calendar(const std::function<double(double)> &w_prev, double k_min,
                   double k_max, std::uint32_t n_grid,
                   double kprev_lo = -std::numeric_limits<double>::infinity(),
                   double kprev_hi = std::numeric_limits<double>::infinity());

  // Data-supported log-moneyness range [z_lo_valid, z_hi_valid] * (atm*sqrt(T)):
  // the span the observed strikes cover. Outside it the served spline is
  // extrapolation. Used to intersect adjacent slices' tradeable ranges for the
  // calendar projection + check (see is_extrapolated).
  [[nodiscard]] std::pair<double, double> data_k_range() const noexcept;

  // True iff `k_log` lies outside this slice's data-supported range (pure
  // extrapolation / flat wing). The served-surface calendar check skips any grid
  // point where EITHER adjacent slice is extrapolating, so calendar
  // no-arbitrage is certified only where both slices carry tradeable quotes.
  [[nodiscard]] bool is_extrapolated(double k_log) const noexcept override;

private:
  SplineVolParams p_;
  // Natural-spline 2nd derivatives at (p_.z, p_.mult), cached once at
  // construction so `w()` stays a pure O(log K) lookup + eval (no per-query
  // solve) — the "no virtual on the arithmetic hot path" house rule extends to
  // "no per-query linear solve" for this family too.
  std::vector<double> m2nd_;
};

// ── Unified surface container ───────────────────────────────────────────────
//
// An ascending-T stack of polymorphic slices with ONE linear-in-total-variance
// time interpolation. A query at T locates T among the slice T's, interpolates
// total variance linearly across the two bracketing slices (never in sigma), and
// applies the same no-extrapolation guards as `VolSurface`/`Surface<>`: a query
// past the last slice, or more than 50% below the first, returns NaN. Slices must
// be pushed in ascending T (the fit driver guarantees it).
class CurveSurface {
public:
  CurveSurface() = default;
  CurveSurface(CurveSurface &&) noexcept = default;
  CurveSurface &operator=(CurveSurface &&) noexcept = default;
  CurveSurface(const CurveSurface &) = delete;
  CurveSurface &operator=(const CurveSurface &) = delete;

  // Append a slice; precondition (documented, not verified): non-decreasing T.
  void push(std::unique_ptr<IVolCurve> slice);

  // Replace one pillar without exposing a partially-mutated surface. The caller
  // normally applies this to a staged clone and publishes the clone only after
  // adjacent calendar and independent strike-shape admission pass.
  [[nodiscard]] Status replace(std::size_t index, std::unique_ptr<IVolCurve> slice);

  // Deep copy — every slice cloned into a fresh independently-owned surface. Lets
  // a caller duplicate this move-only container (e.g. snapshot a live session's
  // fitted surface for archiving without disturbing the session).
  [[nodiscard]] CurveSurface clone() const;

  [[nodiscard]] std::size_t n_slices() const noexcept { return slices_.size(); }
  [[nodiscard]] bool empty() const noexcept { return slices_.empty(); }

  // Total variance / implied vol at log-moneyness `k_log` and year-fraction `T`.
  // `iv(k,T) = sqrt(w(k,T)/T)`. NaN where the surface declines to extrapolate or
  // has no slices.
  [[nodiscard]] double w(double k_log, double T) const noexcept;
  [[nodiscard]] double iv(double k_log, double T) const noexcept;

  // The forward stored on the slice bracketing (or clamped to) T — used to turn
  // an absolute strike K into the log-moneyness the slices expect. Linearly
  // interpolated between brackets, clamped outside. 0 if empty.
  [[nodiscard]] double forward_at(double T) const noexcept;

  [[nodiscard]] std::span<const std::unique_ptr<IVolCurve>> slices() const noexcept {
    return slices_;
  }

private:
  friend class PricedSurface;

  // Internal equal-tenor query token. Keeping it private prevents callers from
  // forging or retaining a cross-surface/stale bracket; PricedSurface resolves
  // and consumes it inside one const batch operation.
  struct Bracket {
    std::size_t lo{0};
    std::size_t hi{0};
    double upper_weight{0.0};

    [[nodiscard]] bool is_single_slice() const noexcept { return lo == hi; }
  };

  [[nodiscard]] Bracket bracket(double T) const noexcept;
  [[nodiscard]] double w(double k_log, double T, Bracket resolved) const noexcept;
  [[nodiscard]] double iv(double k_log, double T, Bracket resolved) const noexcept;

  std::vector<std::unique_ptr<IVolCurve>> slices_; // ascending T
  // Contiguous mirror of slice maturities. Queries binary-search this array
  // instead of chasing unique_ptrs through the polymorphic slice stack.
  std::vector<double> maturities_; // == slices_.size(), ascending T
};

// ── Curve configuration ─────────────────────────────────────────────────────
//
// A tagged bundle: the kind plus every per-kind knob. `ConvexFitOpts` and
// `CalibOpts` are reused verbatim so a caller who already tunes those needs no
// new vocabulary. A default `CurveConfig` is a Convex-QP dense fit at node_cap 40.
struct CurveConfig {
  VolCurveKind kind{VolCurveKind::ConvexDense};
  ConvexFitOpts convex{};  // ConvexDense knobs (lambda, node_cap, ...)
  CalibOpts parametric{};  // Essvi / Svi knobs (shared LM/IRLS/filter policy)
  SplineFitOpts spline{};  // SplineVol knobs (grid, lambda, mult_floor, min_obs)

  // Selector gate (Task I5), NOT a per-slice fit knob: when `true` on ANY
  // config in a `SelectorConfig::candidates` list passed to `select_curve`,
  // the selector appends a `VolCurveKind::SplineVol` candidate to that call's
  // working ladder if one is not already present, carrying the `spline`
  // field FROM THE FIRST config in that list that set this flag (so a
  // caller's own SplineFitOpts knobs actually take effect, not a defaulted
  // `SplineFitOpts{}`). `default_selector_candidates()` itself is untouched
  // (every entry it returns defaults this to `false`), so a caller who
  // supplies no
  // candidates -- the common case, `SelectorConfig::candidates.empty()` --
  // sees NO behavior change: SplineVol is not yet OOS-proven as a default
  // family (07-11/07-12 sprint evidence pending), so this is an explicit,
  // deliberate opt-in for research / evidence-gathering callers, not a
  // default flip. See curve_selector.cpp::select_curve.
  bool spline_candidate{false};
};

// ── Per-slice fit dispatch ──────────────────────────────────────────────────
//
// Fit ONE expiry's curve of the configured kind from the de-Americanized
// European observation set `obs_eu` (produced by `build_observations_european`,
// which strips the early-exercise premium so the fold + re-Americanization
// round-trips for every kind). `F`/`T`/`df` are the slice's forward /
// year-fraction / discount factor.
//
// @return InvalidArgument on a degenerate (F/T/df <= 0) or empty `obs_eu`; the
//         underlying fitter's error (Unavailable / Internal) on a fit failure;
//         otherwise the fitted polymorphic slice.
//
// `w_prev` (optional): the immediately-shorter expiry's total-variance curve
// w(k) as a log-moneyness callback. ConvexDense first applies it at candidate
// nodes, then checks a shared 64-interval k lattice and promotes every residual
// breach to an exact constrained QP node until admitted. This preserves the
// per-slice price cone while closing between-node calendar crossings.
// eSSVI, SVI, and C8 use their native shape-preserving parameter projection on
// the same lattice and then undergo an independent served-value butterfly check.
// LinearVariance additionally accepts the previous curve's breakpoints in
// `calendar_floor_knots`; fitting on the union of both node sets makes its
// piecewise-linear calendar floor hold between nodes as well.
// SplineVol projects the fitted slice onto the calendar cone above `w_prev`
// (uniform total-variance offset over the tradeable overlap — see
// SplineVolCurve::project_calendar), matching the Essvi/Svi/C8 branches; the
// front slice (null `w_prev`) is unchanged. `calendar_floor_knots` is still
// ignored by the v1 spline. `prev_data_k_range` (default the whole line) is the
// previous slice's data-supported log-moneyness range; SplineVol intersects it
// with its own so the calendar projection acts only where BOTH slices carry
// quotes (a non-tradeable wing crossing is left to the extrapolation).
[[nodiscard]] Result<std::unique_ptr<IVolCurve>>
fit_slice_curve(const CurveConfig &cfg, std::span<const FitObs> obs_eu, double F, double T,
                double df, const std::function<double(double)> &w_prev = {},
                std::span<const double> calendar_floor_knots = {},
                std::pair<double, double> prev_data_k_range = {
                    -std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::infinity()});

// Local/warm analogue of fit_slice_curve. Reuses the current curve's state where
// the family supports it: eSSVI/C8 parameters seed LM directly and ConvexDense
// retains its active knot lattice; SVI remains a local one-slice solve. The
// result has already passed the same previous-expiry calendar projection and
// independent strike-shape admission as a cold build. It is not published.
[[nodiscard]] Result<std::unique_ptr<IVolCurve>> refit_slice_curve(
    const CurveConfig &cfg, const IVolCurve &current,
    std::span<const FitObs> obs_eu, double F, double T, double df,
    const std::function<double(double)> &w_prev = {}, FitDiag *diag = nullptr);

} // namespace atx::vol
