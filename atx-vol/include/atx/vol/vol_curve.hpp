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
// A fitted `IVolCurve` / `CurveSurface` is an immutable value after construction;
// concurrent `w`/`iv` reads are safe. `fit_slice_curve` is a pure function of its
// inputs.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "atx/vol/c8.hpp"          // C8Params
#include "atx/vol/calib.hpp"       // CalibOpts, FitObs
#include "atx/vol/dense_slice.hpp" // ConvexSliceFit, ConvexFitOpts
#include "atx/vol/types.hpp"       // Result
#include "atx/vol/vol_surface.hpp" // EssviParams, SviParams, essvi_total_w, svi_total_w

namespace atx::vol {

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
  ConvexSliceFit fit_;
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
  // Locate the bracket for T; returns {lo, hi, weight} where the interpolated
  // value is (1-w)*val(lo) + w*val(hi). Clamps to an endpoint outside the range.
  struct Bracket {
    std::size_t lo{0}, hi{0};
    double frac{0.0};
  };
  [[nodiscard]] Bracket locate(double T) const noexcept;

  std::vector<std::unique_ptr<IVolCurve>> slices_; // ascending T
};

// ── Curve configuration ─────────────────────────────────────────────────────
//
// A tagged bundle: the kind plus every per-kind knob. `ConvexFitOpts` and
// `CalibOpts` are reused verbatim so a caller who already tunes those needs no
// new vocabulary. A default `CurveConfig` is a Convex-QP dense fit at node_cap 40.
struct CurveConfig {
  VolCurveKind kind{VolCurveKind::ConvexDense};
  ConvexFitOpts convex{}; // ConvexDense knobs (lambda, node_cap, ...)
  CalibOpts parametric{}; // Essvi / Svi knobs (shared LM/IRLS/filter policy)
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
// w(k) as a log-moneyness callback. For ConvexDense it becomes a per-node
// calendar floor. LinearVariance additionally accepts the previous curve's
// breakpoints in `calendar_floor_knots`; fitting on the union of both node sets
// makes the piecewise-linear calendar floor hold between nodes as well.
[[nodiscard]] Result<std::unique_ptr<IVolCurve>>
fit_slice_curve(const CurveConfig &cfg, std::span<const FitObs> obs_eu, double F, double T,
                double df, const std::function<double(double)> &w_prev = {},
                std::span<const double> calendar_floor_knots = {});

} // namespace atx::vol
