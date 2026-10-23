#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "atx/vol/api/pricing/implied_vol.hpp"
#include "pricing/american_discrete_div_detail.hpp"

// The discrete-dividend lattice KERNEL objects (header: contract, measured bounds,
// why). Every rollback here is the seam's — pricing/american_discrete_div_detail.hpp,
// defined in american_discrete_div.cpp — so a price through a workspace is the
// allocating entry's price bit for bit; this file adds ownership, a tier and an
// inversion, no lattice arithmetic.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using ddiv_detail::LatticeBuffers;
using ddiv_detail::LatticeOutcome;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Rows are reserved for `max_steps + 2` nodes: a lattice of N steps has N + 1
// nodes and the Leisen-Reimer scheme runs an even N at N + 1.
[[nodiscard]] std::size_t nodes_for(int max_steps) noexcept {
  return static_cast<std::size_t>(max_steps) + 2U;
}

[[nodiscard]] constexpr double payoff_sign(Side side) noexcept {
  return (side == Side::Call) ? 1.0 : -1.0;
}

[[nodiscard]] double intrinsic(Side side, double S, double K) noexcept {
  return std::max(payoff_sign(side) * (S - K), 0.0);
}

// The one pricing path of this TU: the allocating entry's validation and scheme
// admission (same texts), the workspace's admission, then the seam.
[[nodiscard]] Result<double> price_on(double S, double K, double T, double sigma, double r,
                                      double q, Side side, std::span<const CashDividend> dividends,
                                      int steps, ExerciseStyle exercise, DiscreteDivScheme scheme,
                                      DiscreteDivWorkspace &workspace) {
  const Status ok = ddiv_detail::validate(S, K, T, sigma, r, q, dividends, steps);
  if (!ok) {
    return Err(ok.error());
  }
  if (!discrete_div_scheme_admits(scheme, steps)) {
    return Err(ErrorCode::InvalidArgument,
               "unknown discrete-dividend lattice scheme, or too few steps for it");
  }
  if (!workspace.admits(steps)) {
    return Err(ErrorCode::InvalidArgument,
               "discrete-dividend workspace: steps exceed the max_steps it was sized for");
  }
  LatticeBuffers &buffers = workspace.buffers();
  if (scheme == DiscreteDivScheme::SplicedBbsr) {
    return ddiv_detail::richardson_price(S, K, T, sigma, r, q, side, dividends, steps, exercise,
                                         buffers);
  }
  const Result<LatticeOutcome> out = ddiv_detail::run_single_rollback(
      scheme, S, K, T, sigma, r, q, side, dividends, steps, exercise, buffers);
  if (!out) {
    return Err(out.error());
  }
  return Ok(out->price);
}

// dated_cash_iv's lower endpoint: the CRR probability window of the coarsest
// lattice a scheme runs, sigma > |r - q| sqrt(T / steps), nudged inside.
[[nodiscard]] double lattice_sigma_floor(double T, double r, double q,
                                         int coarsest_steps) noexcept {
  const double min_lattice_sigma =
      std::abs(r - q) * std::sqrt(T / static_cast<double>(coarsest_steps));
  return std::max(kIvMin, min_lattice_sigma * (1.0 + 1.0e-6));
}

} // namespace

// ── 1. Workspace ────────────────────────────────────────────────────────────

DiscreteDivWorkspace::DiscreteDivWorkspace(int max_steps)
    : buffers_(std::make_unique<LatticeBuffers>()),
      max_steps_(std::clamp(max_steps, 1, kDiscreteDivMaxSteps)) {
  const std::size_t n = nodes_for(max_steps_);
  buffers_->step_amount.reserve(n);
  buffers_->level.reserve(n);
  buffers_->value.reserve(n);
  buffers_->scratch.reserve(n);
}

DiscreteDivWorkspace::~DiscreteDivWorkspace() = default;

DiscreteDivWorkspace::DiscreteDivWorkspace(DiscreteDivWorkspace &&other) noexcept
    : buffers_(std::move(other.buffers_)), max_steps_(std::exchange(other.max_steps_, 0)) {}

DiscreteDivWorkspace &DiscreteDivWorkspace::operator=(DiscreteDivWorkspace &&other) noexcept {
  if (this != &other) {
    buffers_ = std::move(other.buffers_);
    max_steps_ = std::exchange(other.max_steps_, 0);
  }
  return *this;
}

std::size_t DiscreteDivWorkspace::reserved_bytes() const noexcept {
  if (buffers_ == nullptr) {
    return 0;
  }
  const std::size_t doubles = buffers_->step_amount.capacity() + buffers_->level.capacity() +
                              buffers_->value.capacity() + buffers_->scratch.capacity();
  return doubles * sizeof(double);
}

Result<double> american_discrete_div_price(double S, double K, double T, double sigma, double r,
                                           double q, Side side,
                                           std::span<const CashDividend> dividends, int steps,
                                           ExerciseStyle exercise, DiscreteDivScheme scheme,
                                           DiscreteDivWorkspace &workspace) noexcept {
  return price_on(S, K, T, sigma, r, q, side, dividends, steps, exercise, scheme, workspace);
}

double discrete_div_dated_forward(double S, double T, double r, double q,
                                  std::span<const CashDividend> dividends) noexcept {
  const double carry = r - q;
  double forward = S * std::exp(carry * T);
  const double tau_at_expiry = T * (1.0 + kTauAtExpiryRelTol);
  for (const CashDividend &dv : dividends) {
    if (!(dv.tau > 0.0) || dv.tau > tau_at_expiry || !(dv.amount > 0.0)) {
      continue;
    }
    forward -= dv.amount * std::exp(carry * (T - std::min(dv.tau, T)));
  }
  return forward;
}

// ── 2. Tiers ────────────────────────────────────────────────────────────────

Result<DiscreteDivConverged>
american_discrete_div_converged_price(double S, double K, double T, double sigma, double r,
                                      double q, Side side, std::span<const CashDividend> dividends,
                                      ExerciseStyle exercise, DiscreteDivWorkspace &workspace) {
  const Result<double> fine =
      price_on(S, K, T, sigma, r, q, side, dividends, kDiscreteDivConvergedSteps, exercise,
               DiscreteDivScheme::SplicedBbs, workspace);
  if (!fine) {
    return Err(fine.error());
  }
  const Result<double> coarse =
      price_on(S, K, T, sigma, r, q, side, dividends, kDiscreteDivConvergedCoarseSteps, exercise,
               DiscreteDivScheme::SplicedBbs, workspace);
  if (!coarse) {
    return Err(coarse.error());
  }
  const double n = static_cast<double>(kDiscreteDivConvergedSteps);
  const double m = static_cast<double>(kDiscreteDivConvergedCoarseSteps);
  const double extrapolated = (n * *fine - m * *coarse) / (n - m);
  // The same floor the SplicedBbsr scheme applies: extrapolation can step under an
  // exercise plateau or, in a far wing, under zero.
  const double floor = (exercise == ExerciseStyle::American) ? intrinsic(side, S, K) : 0.0;
  DiscreteDivConverged out;
  out.fine = *fine;
  out.coarse = *coarse;
  out.price = std::max(extrapolated, floor);
  return Ok(out);
}

Result<int> discrete_div_screen_steps(double T, std::span<const CashDividend> dividends,
                                      int steps_accurate, int screen_steps) {
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "year-fraction must be finite and > 0");
  }
  if (steps_accurate < 1 || steps_accurate > kDiscreteDivMaxSteps || screen_steps < 2 ||
      screen_steps > kDiscreteDivMaxSteps) {
    return Err(ErrorCode::InvalidArgument, "Screen tier: step counts outside the lattice range");
  }
  if (screen_steps >= steps_accurate) {
    return Err(ErrorCode::OutOfRange,
               "Screen tier: the screen step count is not below the accurate count");
  }
  const int coarsest = discrete_div_coarsest_steps(screen_steps, kDiscreteDivScreenScheme);
  const double dt = T / static_cast<double>(coarsest);
  const double tau_at_expiry = T * (1.0 + kTauAtExpiryRelTol);
  for (const CashDividend &dv : dividends) {
    if (!(dv.tau > 0.0) || dv.tau > tau_at_expiry || !(dv.amount > 0.0)) {
      continue; // outside (0, T] or a zero amount: the lattice ignores it too
    }
    // The lattice's own step rule (round-half-to-even into [1, N]).
    const double nearest = std::nearbyint(dv.tau / dt);
    if (nearest < static_cast<double>(kDiscreteDivScreenMinExStep)) {
      return Err(ErrorCode::OutOfRange,
                 "Screen tier: an ex-date lands before the second step of the coarse lattice "
                 "(a first-step splice never converges); price on the accurate tier");
    }
  }
  return Ok(screen_steps);
}

Result<double> american_discrete_div_screen_price(double S, double K, double T, double sigma,
                                                  double r, double q, Side side,
                                                  std::span<const CashDividend> dividends,
                                                  int screen_steps,
                                                  DiscreteDivWorkspace &workspace) noexcept {
  return price_on(S, K, T, sigma, r, q, side, dividends, screen_steps, ExerciseStyle::American,
                  kDiscreteDivScreenScheme, workspace);
}

// ── 3. The seeded inversion ─────────────────────────────────────────────────

const char *to_string(DiscreteDivIvFallback value) noexcept {
  switch (value) {
  case DiscreteDivIvFallback::None:
    return "none";
  case DiscreteDivIvFallback::NotApplicable:
    return "not_applicable";
  case DiscreteDivIvFallback::RangeEdge:
    return "range_edge";
  case DiscreteDivIvFallback::NotIncreasing:
    return "not_increasing";
  case DiscreteDivIvFallback::VolBound:
    return "vol_bound";
  case DiscreteDivIvFallback::PricerFailure:
    return "pricer_failure";
  case DiscreteDivIvFallback::NoConvergence:
    return "no_convergence";
  }
  return "unknown";
}

namespace {

// Screen-leg controls. The Screen root is only a seed for the accurate confirm —
// it sits a ripple's amplitude (~1e-4 vol) from the accurate root anyway — so it
// is converged to 100x the caller's tolerance (the accurate Newton polish closes
// the rest), with its own iteration cap inside the caller's budget.
constexpr int kScreenMaxIter = 16;
constexpr double kScreenTolFactor = 100.0;
// Accurate Newton polish steps after the confirm, each accepted only when it
// reduces the accurate residual. The accurate SplicedCrr lattice is rippled in
// sigma, so a third step rescues roots the first two leave a few tol short of.
constexpr int kMaxPolish = 3;
// A chord slope is trusted only from two points at least this far apart in
// sigma: the smoothed lattice is piecewise smooth (exercise decisions flip node
// by node), and two points 1e-7 apart would difference rounding, not vega.
constexpr double kChordMinSeparation = 1.0e-4;
// The second Screen point sits this fraction of the seed away from it (floored
// in absolute vol), on the side the first residual points to.
constexpr double kSeedStepRel = 0.05;
constexpr double kSeedStepMin = 1.0e-3;
// Extrapolation outside a bracket may at most double the last step and never
// move more than this in one go.
constexpr double kMaxExtrapolationStep = 0.5;
// dated_cash_iv's bracket search: doublings from max(0.5, 2 lo) until the price
// is reached, at most this many.
constexpr unsigned kMaxExpansions = 16;
// A flat or downhill Screen chord (the CRR ripple) is re-chorded this many times
// before the leg hands over (NotIncreasing).
constexpr int kNotIncreasingRetries = 1;

// One contract's inversion state. Every price call runs through `acc` or `scr`
// so the two counters and the shared budget are kept in one place.
class Inverter {
public:
  Inverter(double price, double S, double K, double T, double r, double q, Side side,
           std::span<const CashDividend> dividends, int steps, DiscreteDivScheme scheme,
           const DiscreteDivIvOpts &opts, DiscreteDivWorkspace &workspace) noexcept
      : price_(price), S_(S), K_(K), T_(T), r_(r), q_(q), side_(side), dividends_(dividends),
        steps_(steps), scheme_(scheme), opts_(opts), ws_(workspace) {}

  [[nodiscard]] Result<DiscreteDivIvResult> run();

private:
  // A residual on a tier; the typed refusal of the lattice propagates unchanged.
  [[nodiscard]] Result<double> acc(double sigma) {
    ++out_.accurate_lattices;
    const Result<double> p = price_on(S_, K_, T_, sigma, r_, q_, side_, dividends_, steps_,
                                      ExerciseStyle::American, scheme_, ws_);
    if (!p) {
      return Err(p.error());
    }
    return Ok(*p - price_);
  }
  [[nodiscard]] Result<double> scr(double sigma) {
    ++out_.screen_lattices;
    const Result<double> p = price_on(S_, K_, T_, sigma, r_, q_, side_, dividends_, screen_steps_,
                                      ExerciseStyle::American, kDiscreteDivScreenScheme, ws_);
    if (!p) {
      return Err(p.error());
    }
    return Ok(*p - price_);
  }
  [[nodiscard]] bool budget_left() const noexcept {
    return static_cast<unsigned>(out_.screen_lattices) +
               static_cast<unsigned>(out_.accurate_lattices) <
           opts_.max_iter;
  }
  [[nodiscard]] static bool finite_residual(double f) noexcept { return std::isfinite(f); }

  // The Screen leg: returns true with `sigma_s_` / `slope_` set when it reached
  // its root; false (with `fallback_` set) when the accurate leg must take over.
  [[nodiscard]] Result<bool> screen_leg();
  // The accurate confirm + polish: true when a root was accepted into `out_`.
  [[nodiscard]] Result<bool> confirm_leg();
  // dated_cash_iv on the accurate kernel, seeded with the accurate points seen.
  [[nodiscard]] Result<double> accurate_secant();

  void note_accurate_point(double sigma, double residual) noexcept {
    if (residual < 0.0) {
      if (!(sigma <= acc_lo_)) {
        acc_lo_ = sigma;
        acc_f_lo_ = residual;
      }
    } else if (!(sigma >= acc_hi_)) {
      acc_hi_ = sigma;
      acc_f_hi_ = residual;
    }
  }

  double price_;
  double S_, K_, T_, r_, q_;
  Side side_;
  std::span<const CashDividend> dividends_;
  int steps_;
  DiscreteDivScheme scheme_;
  const DiscreteDivIvOpts &opts_;
  DiscreteDivWorkspace &ws_;

  DiscreteDivIvResult out_;
  DiscreteDivIvFallback fallback_ = DiscreteDivIvFallback::None;
  int screen_steps_ = 0;
  double lo_acc_ = kNaN;  // the accurate lattice's lower endpoint
  double lo_scr_ = kNaN;  // the Screen tier's (>= lo_acc_)
  double sigma_s_ = kNaN; // the Screen root
  double slope_ = kNaN;   // the trusted chord slope (dP/dsigma), > 0
  // Accurate points seen so far, as a bracket for the fallback secant.
  double acc_lo_ = kNaN, acc_f_lo_ = kNaN; // largest sigma with residual < 0
  double acc_hi_ = kNaN, acc_f_hi_ = kNaN; // smallest sigma with residual >= 0
};

Result<bool> Inverter::screen_leg() {
  // Bracket ends (NaN until seen) and the last two points, for extrapolation and
  // the chord. A SEEDED leg never prices the lower edge up front: an iterate that
  // runs into it is handed to the accurate leg (VolBound), which makes the
  // identifiable-range ruling itself — one Screen call saved on every normal root.
  double bl = kNaN;
  double fbl = kNaN;
  double bh = kNaN;
  double fbh = kNaN;
  double a = kNaN;
  double fa = kNaN;
  double b = kNaN;
  double fb = kNaN;
  const auto note_chord = [&](double x0, double f0, double x1, double f1) noexcept {
    const double dx = x1 - x0;
    if (std::abs(dx) >= kChordMinSeparation) {
      const double s = (f1 - f0) / dx;
      if (s > 0.0 && std::isfinite(s)) {
        slope_ = s;
      }
    }
  };
  const auto take = [&](double x, double fx) noexcept {
    if (fx < 0.0) {
      bl = x;
      fbl = fx;
    } else {
      bh = x;
      fbh = fx;
    }
    if (std::isfinite(b)) {
      note_chord(b, fb, x, fx);
    } else {
      note_chord(a, fa, x, fx);
    }
    a = b;
    fa = fb;
    b = x;
    fb = fx;
  };

  // Seed: the caller's, else Black-76 at the dated-cash forward.
  double s0 = opts_.seed_sigma;
  bool seeded = std::isfinite(s0) && s0 > lo_scr_ && s0 < kIvMax;
  if (!seeded) {
    const double forward = discrete_div_dated_forward(S_, T_, r_, q_, dividends_);
    if (forward > 0.0) {
      const Result<double> european =
          implied_vol(price_, forward, K_, T_, std::exp(-r_ * T_), side_);
      if (european && *european > lo_scr_ && *european < kIvMax) {
        s0 = *european;
        seeded = true;
      }
    }
  }
  if (seeded) {
    const Result<double> f0 = scr(s0);
    if (!f0) {
      fallback_ = DiscreteDivIvFallback::PricerFailure;
      return Ok(false);
    }
    if (!finite_residual(*f0)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
    }
    if (*f0 == 0.0) {
      // Exactly on the quote: a chord is still needed for the confirm.
      take(s0, *f0);
      const double s1 = std::min(s0 + std::max(kSeedStepRel * s0, kSeedStepMin), kIvMax);
      const Result<double> f1 = scr(s1);
      if (!f1 || !finite_residual(*f1)) {
        fallback_ = DiscreteDivIvFallback::PricerFailure;
        return Ok(false);
      }
      take(s1, *f1);
      sigma_s_ = s0;
      return Ok(std::isfinite(slope_));
    }
    take(s0, *f0);
    // Second point, towards the root.
    const double h = std::max(kSeedStepRel * s0, kSeedStepMin);
    double s1 = (*f0 < 0.0) ? s0 + h : s0 - h;
    s1 = std::clamp(s1, lo_scr_, kIvMax);
    if (s1 == s0) {
      s1 = std::clamp(s0 + h, lo_scr_, kIvMax);
    }
    const Result<double> f1 = scr(s1);
    if (!f1) {
      fallback_ = DiscreteDivIvFallback::PricerFailure;
      return Ok(false);
    }
    if (!finite_residual(*f1)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
    }
    if (*f1 == 0.0) {
      take(s1, *f1);
      sigma_s_ = s1;
      return Ok(std::isfinite(slope_));
    }
    take(s1, *f1);
  } else {
    // No seed: dated_cash_iv's lower-edge test and doubling bracket search, on the
    // Screen tier.
    const Result<double> f_lo_r = scr(lo_scr_);
    if (!f_lo_r) {
      fallback_ = DiscreteDivIvFallback::PricerFailure;
      return Ok(false);
    }
    if (!finite_residual(*f_lo_r)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite bracket price");
    }
    if (*f_lo_r >= 0.0) {
      // At or under the Screen's lowest price: the accurate leg rules on it.
      fallback_ = DiscreteDivIvFallback::RangeEdge;
      return Ok(false);
    }
    take(lo_scr_, *f_lo_r);
    double hi = std::min(kIvMax, std::max(0.5, 2.0 * lo_scr_));
    for (unsigned expansion = 0; expansion < kMaxExpansions; ++expansion) {
      if (!budget_left()) {
        fallback_ = DiscreteDivIvFallback::NoConvergence;
        return Ok(false);
      }
      const Result<double> f_hi = scr(hi);
      if (!f_hi) {
        fallback_ = DiscreteDivIvFallback::PricerFailure;
        return Ok(false);
      }
      if (!finite_residual(*f_hi)) {
        return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite bracket price");
      }
      take(hi, *f_hi);
      if (*f_hi >= 0.0) {
        break;
      }
      if (hi == kIvMax) {
        // Above the maximum-volatility Screen price: the accurate leg rules.
        fallback_ = DiscreteDivIvFallback::VolBound;
        return Ok(false);
      }
      hi = std::min(kIvMax, 2.0 * hi);
    }
    if (!std::isfinite(bh)) {
      fallback_ = DiscreteDivIvFallback::NoConvergence;
      return Ok(false);
    }
  }

  // Safeguarded secant: Illinois regula falsi inside a bracket, capped secant
  // extrapolation outside one. Converged to kScreenTolFactor x tol.
  const double tol_s = kScreenTolFactor * opts_.tol;
  int last_side = 0; // +1: the high end moved last, -1: the low end did
  int notinc_retries = 0;
  for (int iter = 0; iter < kScreenMaxIter; ++iter) {
    if (!budget_left()) {
      fallback_ = DiscreteDivIvFallback::NoConvergence;
      return Ok(false);
    }
    double x = kNaN;
    const bool bracketed = std::isfinite(bl) && std::isfinite(bh);
    if (bracketed) {
      const double width = bh - bl;
      if (width <= 2.0 * tol_s) {
        sigma_s_ = 0.5 * (bl + bh);
        break;
      }
      const double fraction = std::clamp(-fbl / (fbh - fbl), 0.1, 0.9);
      x = bl + fraction * width;
    } else {
      // Both points so far sit on one side of the quote: extrapolate along the
      // chord (the step's sign follows the residual's, either way).
      const double dx = b - a;
      const double s = (std::abs(dx) > 0.0) ? (fb - fa) / dx : kNaN;
      const double span = std::max(std::abs(dx), kSeedStepMin);
      double step = kNaN;
      if (s > 0.0 && std::isfinite(s)) {
        const double cap = std::min(kMaxExtrapolationStep, 2.0 * span);
        step = std::clamp(-fb / s, -cap, cap);
      } else if (notinc_retries++ < kNotIncreasingRetries) {
        // The coarse CRR lattice is rippled in sigma (LEDGER L247): two points
        // inside one ripple period can chord flat or downhill. One retry four
        // spans further along the residual's direction re-chords across the
        // ripple; a second failure is the typed fallback.
        step = ((fb < 0.0) ? 1.0 : -1.0) * 4.0 * span;
      } else {
        fallback_ = DiscreteDivIvFallback::NotIncreasing;
        return Ok(false);
      }
      x = std::clamp(b + step, lo_scr_, kIvMax);
      if (x == b) {
        // Pinned at a bound with the quote still unreached.
        fallback_ = DiscreteDivIvFallback::VolBound;
        return Ok(false);
      }
    }
    const Result<double> fx = scr(x);
    if (!fx) {
      fallback_ = DiscreteDivIvFallback::PricerFailure;
      return Ok(false);
    }
    if (!finite_residual(*fx)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
    }
    if (*fx == 0.0) {
      take(x, *fx);
      sigma_s_ = x;
      break;
    }
    const bool had_bracket = bracketed;
    const int side_now = (*fx < 0.0) ? -1 : +1;
    if (had_bracket && side_now == last_side) {
      // Illinois: the retained end's residual is halved so a convex function
      // cannot pin one end and degrade the secant to a one-sided crawl.
      if (side_now < 0) {
        fbh *= 0.5;
      } else {
        fbl *= 0.5;
      }
    }
    last_side = had_bracket ? side_now : 0;
    take(x, *fx);
    if (std::isfinite(bl) && std::isfinite(bh) && std::isfinite(a) &&
        std::abs(b - a) <= tol_s) {
      sigma_s_ = x;
      break;
    }
  }
  if (!std::isfinite(sigma_s_)) {
    fallback_ = DiscreteDivIvFallback::NoConvergence;
    return Ok(false);
  }
  if (!std::isfinite(slope_)) {
    fallback_ = DiscreteDivIvFallback::NotIncreasing;
    return Ok(false);
  }
  return Ok(true);
}

Result<bool> Inverter::confirm_leg() {
  // ONE accurate confirm at the Screen root, then at most kMaxPolish Newton polish
  // steps on the accurate lattice, each accepted only when it REDUCES the accurate
  // residual (L2e's acceptance). Acceptance: Newton distance |r| / slope <= tol.
  double sigma = sigma_s_;
  Result<double> r = acc(sigma);
  if (!r) {
    return Err(r.error());
  }
  if (!finite_residual(*r)) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
  }
  note_accurate_point(sigma, *r);
  double slope = slope_;
  for (int polish = 0; polish <= kMaxPolish; ++polish) {
    if (std::abs(*r) / slope <= opts_.tol) {
      out_.sigma = sigma;
      return Ok(true);
    }
    if (polish == kMaxPolish || !budget_left()) {
      break;
    }
    const double next = std::clamp(sigma - *r / slope, lo_acc_, kIvMax);
    if (next == sigma) {
      break;
    }
    const Result<double> r_next = acc(next);
    if (!r_next) {
      return Err(r_next.error());
    }
    if (!finite_residual(*r_next)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
    }
    note_accurate_point(next, *r_next);
    if (!(std::abs(*r_next) < std::abs(*r))) {
      break; // not residual-reducing: the bracketed secant takes over
    }
    // Two accurate points: their secant is the most local slope available.
    const double s = (*r_next - *r) / (next - sigma);
    if (s > 0.0 && std::isfinite(s)) {
      slope = s;
    }
    sigma = next;
    r = r_next;
  }
  fallback_ = DiscreteDivIvFallback::NoConvergence;
  return Ok(false);
}

Result<double> Inverter::accurate_secant() {
  // dated_cash_iv (src/fitting/quote_slice.cpp) on the accurate kernel: the lower
  // endpoint test, the doubling bracket search, the [0.1, 0.9]-clamped secant to a
  // bracket of width 2 tol — seeded with whatever accurate points the legs above
  // already paid for, so a fallback rarely re-brackets from scratch.
  double lo = lo_acc_;
  double f_lo = kNaN;
  if (std::isfinite(acc_lo_) && acc_lo_ >= lo_acc_) {
    lo = acc_lo_;
    f_lo = acc_f_lo_;
  } else {
    const Result<double> r = acc(lo);
    if (!r) {
      return Err(r.error());
    }
    f_lo = *r;
    if (!finite_residual(f_lo) || f_lo >= 0.0) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: quote at or below the identifiable range");
    }
  }
  double hi = acc_hi_;
  double f_hi = acc_f_hi_;
  if (!std::isfinite(hi)) {
    hi = std::min(kIvMax, std::max(0.5, 2.0 * lo));
    for (unsigned expansion = 0; expansion < kMaxExpansions; ++expansion) {
      if (!budget_left()) {
        return Err(ErrorCode::Unavailable, "dated-cash IV: iteration budget exhausted");
      }
      const Result<double> r = acc(hi);
      if (!r) {
        return Err(r.error());
      }
      f_hi = *r;
      if (!finite_residual(f_hi)) {
        return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite bracket price");
      }
      if (f_hi >= 0.0) {
        break;
      }
      if (hi == kIvMax) {
        return Err(ErrorCode::OutOfRange, "dated-cash IV: price exceeds maximum-volatility price");
      }
      lo = hi;
      f_lo = f_hi;
      hi = std::min(kIvMax, 2.0 * hi);
    }
    if (!(f_hi >= 0.0)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: price exceeds maximum-volatility price");
    }
  }
  while (budget_left()) {
    const double width = hi - lo;
    if (width <= 2.0 * opts_.tol) {
      return Ok(0.5 * (lo + hi));
    }
    const double fraction = std::clamp(-f_lo / (f_hi - f_lo), 0.1, 0.9);
    const double sigma = lo + fraction * width;
    const Result<double> r = acc(sigma);
    if (!r) {
      return Err(r.error());
    }
    const double residual = *r;
    if (!finite_residual(residual)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
    }
    if (residual == 0.0) {
      return Ok(sigma);
    }
    if (residual < 0.0) {
      lo = sigma;
      f_lo = residual;
    } else {
      hi = sigma;
      f_hi = residual;
    }
  }
  return Err(ErrorCode::Unavailable, "dated-cash IV: iteration budget exhausted");
}

Result<DiscreteDivIvResult> Inverter::run() {
  if (!(price_ > 0.0) || !std::isfinite(price_)) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: quote must be finite and > 0");
  }
  if (!(opts_.tol > 0.0) || !std::isfinite(opts_.tol)) {
    return Err(ErrorCode::InvalidArgument, "dated-cash IV: tol must be finite and > 0");
  }
  // The allocating entry's validation, once, with a placeholder sigma inside the
  // admissible range; every later price call revalidates for free.
  const Status ok = ddiv_detail::validate(S_, K_, T_, 1.0, r_, q_, dividends_, steps_);
  if (!ok) {
    return Err(ok.error());
  }
  if (!discrete_div_scheme_admits(scheme_, steps_)) {
    return Err(ErrorCode::InvalidArgument,
               "unknown discrete-dividend lattice scheme, or too few steps for it");
  }
  if (!ws_.admits(steps_)) {
    return Err(ErrorCode::InvalidArgument,
               "discrete-dividend workspace: steps exceed the max_steps it was sized for");
  }
  lo_acc_ = lattice_sigma_floor(T_, r_, q_, discrete_div_coarsest_steps(steps_, scheme_));
  if (!(lo_acc_ < kIvMax)) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: lattice has no admissible volatility range");
  }

  bool screen = false;
  if (opts_.tier == DiscreteDivTier::Screen) {
    const Result<int> ns = discrete_div_screen_steps(T_, dividends_, steps_, opts_.screen_steps);
    if (ns && ws_.admits(*ns)) {
      screen_steps_ = *ns;
      lo_scr_ = std::max(
          lo_acc_, lattice_sigma_floor(T_, r_, q_, discrete_div_coarsest_steps(
                                                       screen_steps_, kDiscreteDivScreenScheme)));
      screen = lo_scr_ < kIvMax;
    }
  }
  if (!screen) {
    fallback_ = DiscreteDivIvFallback::NotApplicable;
  }
  if (screen) {
    const Result<bool> reached = screen_leg();
    if (!reached) {
      return Err(reached.error());
    }
    if (*reached) {
      const Result<bool> accepted = confirm_leg();
      if (!accepted) {
        return Err(accepted.error());
      }
      if (*accepted) {
        out_.fallback = DiscreteDivIvFallback::None;
        return Ok(out_);
      }
    }
  }
  const Result<double> root = accurate_secant();
  if (!root) {
    return Err(root.error());
  }
  out_.sigma = *root;
  out_.fallback = fallback_;
  return Ok(out_);
}

} // namespace

Result<DiscreteDivIvResult>
american_discrete_div_implied_vol(double price, double S, double K, double T, double r, double q,
                                  Side side, std::span<const CashDividend> dividends, int steps,
                                  DiscreteDivScheme scheme, const DiscreteDivIvOpts &opts,
                                  DiscreteDivWorkspace &workspace) {
  Inverter inverter(price, S, K, T, r, q, side, dividends, steps, scheme, opts, workspace);
  Result<DiscreteDivIvResult> out = inverter.run();
  if (out) {
    out->convention = discrete_div_convention(scheme);
  }
  return out;
}

} // namespace atx::vol
