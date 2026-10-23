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

// ── The D1 route rule and the production pricing core (header section 4) ───

// The band, in standard deviations of log spot at the ex-date, whose nodes must
// read their post-dividend continuation inside the grid.
constexpr double kBandSd = 6.0;
// Bounded loops: the per-step cash aggregation below is O(events^2).
constexpr int kMaxRouteEvents = 256;
// A `need` far beyond any cap, so the double -> int conversion stays in range.
constexpr double kNeedCeiling = 1.0e6;

// The production lattice's placement of one ex-date (american_discrete_div.cpp,
// `run_production_lattice`): nearest step, onto step 0 only with >= 4 pre-steps.
[[nodiscard]] double placed_step(double tau, double dt, int steps, bool step_zero) noexcept {
  return std::clamp(std::nearbyint(tau / dt), step_zero ? 0.0 : 1.0,
                    static_cast<double>(steps));
}

// The lattice window ((0, T] up to kTauAtExpiryRelTol) on the shifted clock.
[[nodiscard]] bool in_window(const CashDividend &dv, double T, double tau_shift) noexcept {
  const double tau = dv.tau - tau_shift;
  return tau > 0.0 && tau <= T * (1.0 + kTauAtExpiryRelTol) && dv.amount > 0.0;
}

// Strictly inside the life: a splice (not the analytic terminal payoff) prices it.
[[nodiscard]] bool in_life(const CashDividend &dv, double T, double tau_shift) noexcept {
  return in_window(dv, T, tau_shift) && dv.tau - tau_shift < T * (1.0 - kTauAtExpiryRelTol);
}

struct PreStepNeed {
  int pre_steps = 2;
  double depth = 0.0; // node spacings under the grid's second node after the cap
  bool capped = false;
  bool step_zero = false;
  int first_step = -1;
};

// Header section 4, the pre-step rule. At an ex-step k the nodes the root reads lie
// within min(6 sqrt(k), k) rungs of S (the 6-sd band, clipped to the cone of the
// root); the post level of the band's lowest node, x = S e^{-w L} - cash, must sit
// at or above the grid's second node S u^{2 - k - m}, L = kDiscreteDivRouteSigma
// sqrt(dt): m >= 2 - k - ln(x / S) / L. Evaluated at a FIXED sigma, so the route is
// one discretisation for every sigma of a root find or a batch group.
[[nodiscard]] PreStepNeed pre_steps_for(double S, double T, std::span<const CashDividend> divs,
                                        int steps, double tau_shift) noexcept {
  const double dt = T / static_cast<double>(steps);
  const double rung = kDiscreteDivRouteSigma * std::sqrt(dt);
  PreStepNeed need;
  for (const CashDividend &dv : divs) {
    if (in_window(dv, T, tau_shift) && std::nearbyint((dv.tau - tau_shift) / dt) == 0.0) {
      need.step_zero = true;
    }
  }
  double worst = need.step_zero ? 4.0 : 2.0;
  for (std::size_t i = 0; i < divs.size(); ++i) {
    if (!in_window(divs[i], T, tau_shift)) {
      continue;
    }
    const double k = placed_step(divs[i].tau - tau_shift, dt, steps, need.step_zero);
    if (k >= static_cast<double>(steps)) {
      continue; // the terminal step: the analytic payoff, no splice
    }
    double cash = 0.0;
    for (const CashDividend &other : divs) {
      if (in_window(other, T, tau_shift) &&
          placed_step(other.tau - tau_shift, dt, steps, need.step_zero) == k) {
        cash += other.amount;
      }
    }
    const int ki = static_cast<int>(k);
    need.first_step = (need.first_step < 0) ? ki : std::min(need.first_step, ki);
    const double band = std::min(kBandSd * std::sqrt(k), k);
    const double x = std::exp(-band * rung) - cash / S;
    const double log_x = (x > 0.0) ? std::log(x) : std::log(std::numeric_limits<double>::min());
    worst = std::max(worst, std::min(2.0 - k - log_x / rung, kNeedCeiling));
  }
  int m = static_cast<int>(std::ceil(worst));
  m += (m % 2 != 0) ? 1 : 0;
  if (m > kDiscreteDivMaxPreSteps) {
    need.capped = true;
    need.depth = 0.5 * (worst - static_cast<double>(kDiscreteDivMaxPreSteps));
    m = kDiscreteDivMaxPreSteps;
  }
  need.pre_steps = m;
  return need;
}

[[nodiscard]] Status validate_route_inputs(double S, double T,
                                           std::span<const CashDividend> dividends,
                                           int base_steps) {
  if (!(S > 0.0) || !std::isfinite(S)) {
    return Err(ErrorCode::InvalidArgument, "spot must be finite and > 0");
  }
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "year-fraction must be finite and > 0");
  }
  if (base_steps < 4 || base_steps > kDiscreteDivMaxSteps) {
    return Err(ErrorCode::InvalidArgument,
               "production route: base_steps must lie in [4, kDiscreteDivMaxSteps]");
  }
  for (const CashDividend &dv : dividends) {
    if (!std::isfinite(dv.tau) || !std::isfinite(dv.amount)) {
      return Err(ErrorCode::InvalidArgument, "dividend tau and amount must be finite");
    }
    if (dv.amount < 0.0) {
      return Err(ErrorCode::InvalidArgument, "dividend amount must be >= 0");
    }
  }
  return Ok();
}

// The route of header section 4. `raise_for_events` is false only for a Screen
// tier (a seed lattice keeps its coarse count). `tau_shift` routes the contract
// as seen `tau_shift` years later (the calendar roll).
[[nodiscard]] Result<DiscreteDivLatticeRoute>
build_route(double S, double T, std::span<const CashDividend> dividends,
            const DiscreteDivProductionOpts &opts, bool raise_for_events, double tau_shift) {
  const Status ok = validate_route_inputs(S, T, dividends, opts.base_steps);
  if (!ok) {
    return Err(ok.error());
  }
  DiscreteDivLatticeRoute route;
  for (const CashDividend &dv : dividends) {
    if (in_life(dv, T, tau_shift)) {
      ++route.events;
      route.max_cash_ratio = std::max(route.max_cash_ratio, dv.amount / S);
    }
  }
  if (route.events > kMaxRouteEvents) {
    return Err(ErrorCode::InvalidArgument,
               "production route: more than 256 ex-dates inside the life");
  }
  if (route.events == 0) {
    route.flags |= bit(DiscreteDivRouteFlag::NoInLifeEvent);
  }
  if (opts.kill_switch) {
    route.method = DiscreteDivMethod::HistoricalSpliced;
    route.steps = opts.base_steps;
    route.flags |= bit(DiscreteDivRouteFlag::KillSwitch);
    const double dt = T / static_cast<double>(route.steps);
    for (const CashDividend &dv : dividends) {
      if (in_life(dv, T, tau_shift)) {
        const int k = static_cast<int>(placed_step(dv.tau - tau_shift, dt, route.steps, false));
        route.first_ex_step = (route.first_ex_step < 0) ? k : std::min(route.first_ex_step, k);
      }
    }
    return Ok(route);
  }
  int steps = opts.base_steps;
  if (raise_for_events && route.events > 0) {
    const int want = kDiscreteDivStepsPerEvent * route.events;
    const int cap = std::max(opts.base_steps, kDiscreteDivMaxRouteSteps);
    if (want > steps) {
      steps = std::min(want, cap);
      route.flags |= bit(DiscreteDivRouteFlag::StepsRaisedForEvents);
      if (want > cap) {
        route.flags |= bit(DiscreteDivRouteFlag::StepsCapped);
      }
    }
  }
  if (opts.accuracy == DiscreteDivAccuracy::Refined) {
    route.method = DiscreteDivMethod::CubicBbsRichardson;
    steps = std::max(steps, kDiscreteDivRefinedSteps);
    steps += (steps % 2 == 0) ? 1 : 0; // same parity for the Richardson pair
    route.coarse_steps = (steps + 1) / 2;
  } else {
    route.method = DiscreteDivMethod::CubicCrr;
  }
  route.steps = steps;
  const PreStepNeed need = pre_steps_for(S, T, dividends, steps, tau_shift);
  route.pre_steps = need.pre_steps;
  route.first_ex_step = need.first_step;
  route.grid_bottom_depth = need.depth;
  if (need.pre_steps > 2) {
    route.flags |= bit(DiscreteDivRouteFlag::PreStepsForEarlyExDate);
  }
  if (need.capped) {
    route.flags |= bit(DiscreteDivRouteFlag::GridBottomExtrapolated);
  }
  if (need.step_zero) {
    route.flags |= bit(DiscreteDivRouteFlag::ExDateOnStepZero);
  }
  return Ok(route);
}

// The ProductionSpec of one route (the method's knobs, no hooks).
[[nodiscard]] ddiv_detail::ProductionSpec route_spec(const DiscreteDivLatticeRoute &route) noexcept {
  ddiv_detail::ProductionSpec spec;
  spec.splice = ddiv_detail::SpliceInterp::Cubic;
  spec.pre_steps = route.pre_steps;
  switch (route.method) {
  case DiscreteDivMethod::HistoricalSpliced:
    spec.splice = ddiv_detail::SpliceInterp::Linear;
    spec.pre_steps = 0;
    spec.align_forward = false;
    spec.align_exercise = false;
    break;
  case DiscreteDivMethod::CubicCrr:
    break;
  case DiscreteDivMethod::CubicBbsRichardson:
    spec.bbs = true;
    break;
  }
  return spec;
}

// One rollback (or the Richardson pair) of a route. Validated inputs, buffers sized.
// `bump_index` / `bump_amount` / `tau_shift`: the ProductionSpec hooks; `maps`: a
// batch group's prebuilt step maps (only without hooks).
[[nodiscard]] Result<LatticeOutcome>
route_outcome(const DiscreteDivLatticeRoute &route, double S, double K, double T, double sigma,
              double r, double q, Side side, std::span<const CashDividend> dividends,
              ExerciseStyle exercise, LatticeBuffers &buffers,
              std::size_t bump_index = ddiv_detail::ProductionSpec::kNoBump,
              double bump_amount = 0.0, double tau_shift = 0.0,
              const ddiv_detail::RouteMaps *maps = nullptr) {
  ddiv_detail::ProductionSpec spec = route_spec(route);
  spec.bump_index = bump_index;
  spec.bump_amount = bump_amount;
  spec.tau_shift = tau_shift;
  spec.prebuilt = (maps != nullptr) ? &maps->fine : nullptr;
  switch (route.method) {
  case DiscreteDivMethod::HistoricalSpliced:
  case DiscreteDivMethod::CubicCrr:
    return ddiv_detail::run_production_lattice(S, K, T, sigma, r, q, side, dividends,
                                               route.steps, exercise, spec, buffers);
  case DiscreteDivMethod::CubicBbsRichardson: {
    const Result<LatticeOutcome> fine = ddiv_detail::run_production_lattice(
        S, K, T, sigma, r, q, side, dividends, route.steps, exercise, spec, buffers);
    if (!fine) {
      return Err(fine.error());
    }
    spec.prebuilt = (maps != nullptr) ? &maps->coarse : nullptr;
    const Result<LatticeOutcome> coarse = ddiv_detail::run_production_lattice(
        S, K, T, sigma, r, q, side, dividends, route.coarse_steps, exercise, spec, buffers);
    if (!coarse) {
      return Err(coarse.error());
    }
    const double n = static_cast<double>(route.steps);
    const double m = static_cast<double>(route.coarse_steps);
    LatticeOutcome out = *fine;
    const double floor = (exercise == ExerciseStyle::American) ? intrinsic(side, S, K) : 0.0;
    out.price = std::max((n * fine->price - m * coarse->price) / (n - m), floor);
    return Ok(out);
  }
  }
  return Err(ErrorCode::InvalidArgument, "unknown production lattice method");
}

// A route the caller handed in: shape checks (a route is data, it can be stale or
// hand-built) and the workspace admission.
[[nodiscard]] Status admit_route(const DiscreteDivLatticeRoute &route,
                                 const DiscreteDivWorkspace &workspace) {
  const bool historical = (route.method == DiscreteDivMethod::HistoricalSpliced);
  const bool refined = (route.method == DiscreteDivMethod::CubicBbsRichardson);
  const bool shape = route.steps >= 4 && route.steps <= kDiscreteDivMaxSteps &&
                     route.pre_steps >= 0 && route.pre_steps <= kDiscreteDivMaxPreSteps &&
                     route.pre_steps % 2 == 0 && (!historical || route.pre_steps == 0) &&
                     (!refined || (route.coarse_steps >= 4 && route.coarse_steps < route.steps));
  if (!shape) {
    return Err(ErrorCode::InvalidArgument, "production route: malformed route");
  }
  const bool sized = historical ? workspace.admits(route.steps)
                                : workspace.admits_production(route.steps, route.pre_steps);
  if (!sized) {
    return Err(ErrorCode::InvalidArgument,
               "discrete-dividend workspace: not sized for this production route "
               "(steps + pre-steps; build it with DiscreteDivWorkspace(max_steps, max_pre_steps))");
  }
  return Ok();
}

// The validated price of a route on a workspace (`maps`: a batch group's prebuilt
// step maps, or null).
[[nodiscard]] Result<double> price_route_on(const DiscreteDivLatticeRoute &route, double S,
                                            double K, double T, double sigma, double r, double q,
                                            Side side, std::span<const CashDividend> dividends,
                                            ExerciseStyle exercise,
                                            DiscreteDivWorkspace &workspace,
                                            const ddiv_detail::RouteMaps *maps = nullptr) {
  const Status ok = ddiv_detail::validate(S, K, T, sigma, r, q, dividends, route.steps);
  if (!ok) {
    return Err(ok.error());
  }
  const Status admitted = admit_route(route, workspace);
  if (!admitted) {
    return Err(admitted.error());
  }
  const Result<LatticeOutcome> out =
      route_outcome(route, S, K, T, sigma, r, q, side, dividends, exercise, workspace.buffers(),
                    ddiv_detail::ProductionSpec::kNoBump, 0.0, 0.0, maps);
  if (!out) {
    return Err(out.error());
  }
  return Ok(out->price);
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
  if (scheme == DiscreteDivScheme::SplicedCubic) {
    DiscreteDivProductionOpts opts;
    opts.base_steps = steps;
    const Result<DiscreteDivLatticeRoute> route = build_route(S, T, dividends, opts, true, 0.0);
    if (!route) {
      return Err(route.error());
    }
    return price_route_on(*route, S, K, T, sigma, r, q, side, dividends, exercise, workspace);
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

DiscreteDivWorkspace::DiscreteDivWorkspace(int max_steps, int max_pre_steps)
    : buffers_(std::make_unique<LatticeBuffers>()),
      max_steps_(std::clamp(max_steps, 1, kDiscreteDivMaxSteps)),
      max_pre_steps_(std::clamp(max_pre_steps, 0, kDiscreteDivMaxPreSteps)) {
  const std::size_t n = nodes_for(max_steps_);
  const std::size_t nodes = n + static_cast<std::size_t>(max_pre_steps_);
  buffers_->step_amount.reserve(n);
  buffers_->step_shift.reserve(n);
  buffers_->level.reserve(nodes);
  buffers_->value.reserve(nodes);
  buffers_->scratch.reserve(nodes);
}

DiscreteDivWorkspace::~DiscreteDivWorkspace() = default;

DiscreteDivWorkspace::DiscreteDivWorkspace(DiscreteDivWorkspace &&other) noexcept
    : buffers_(std::move(other.buffers_)), max_steps_(std::exchange(other.max_steps_, 0)),
      max_pre_steps_(std::exchange(other.max_pre_steps_, -1)) {}

DiscreteDivWorkspace &DiscreteDivWorkspace::operator=(DiscreteDivWorkspace &&other) noexcept {
  if (this != &other) {
    buffers_ = std::move(other.buffers_);
    max_steps_ = std::exchange(other.max_steps_, 0);
    max_pre_steps_ = std::exchange(other.max_pre_steps_, -1);
  }
  return *this;
}

std::size_t DiscreteDivWorkspace::reserved_bytes() const noexcept {
  if (buffers_ == nullptr) {
    return 0;
  }
  const std::size_t doubles = buffers_->step_amount.capacity() + buffers_->level.capacity() +
                              buffers_->value.capacity() + buffers_->scratch.capacity() +
                              buffers_->step_shift.capacity();
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
  // The historical tiers: the accurate (steps, scheme) lattice, the Screen scheme.
  Inverter(double price, double S, double K, double T, double r, double q, Side side,
           std::span<const CashDividend> dividends, int steps, DiscreteDivScheme scheme,
           const DiscreteDivIvOpts &opts, DiscreteDivWorkspace &workspace) noexcept
      : price_(price), S_(S), K_(K), T_(T), r_(r), q_(q), side_(side), dividends_(dividends),
        steps_(steps), scheme_(scheme), opts_(opts), ws_(workspace) {}
  // The D1 production tiers (lane D1): the accurate route, and a Screen route or
  // nullptr. Both routes must outlive the Inverter.
  Inverter(double price, double S, double K, double T, double r, double q, Side side,
           std::span<const CashDividend> dividends, const DiscreteDivLatticeRoute &route,
           const DiscreteDivLatticeRoute *screen_route, const DiscreteDivIvOpts &opts,
           DiscreteDivWorkspace &workspace, const ddiv_detail::RouteMaps *maps = nullptr,
           const ddiv_detail::RouteMaps *screen_maps = nullptr) noexcept
      : price_(price), S_(S), K_(K), T_(T), r_(r), q_(q), side_(side), dividends_(dividends),
        steps_(route.steps), scheme_(DiscreteDivScheme::SplicedCubic), opts_(opts),
        ws_(workspace), route_(&route), screen_route_(screen_route), maps_(maps),
        screen_maps_(screen_maps) {}

  [[nodiscard]] Result<DiscreteDivIvResult> run();

private:
  // A residual on a tier; the typed refusal of the lattice propagates unchanged.
  [[nodiscard]] Result<double> acc(double sigma) {
    ++out_.accurate_lattices;
    const Result<double> p =
        (route_ != nullptr)
            ? price_route_on(*route_, S_, K_, T_, sigma, r_, q_, side_, dividends_,
                             ExerciseStyle::American, ws_, maps_)
            : price_on(S_, K_, T_, sigma, r_, q_, side_, dividends_, steps_,
                       ExerciseStyle::American, scheme_, ws_);
    if (!p) {
      return Err(p.error());
    }
    return Ok(*p - price_);
  }
  [[nodiscard]] Result<double> scr(double sigma) {
    ++out_.screen_lattices;
    const Result<double> p =
        (screen_route_ != nullptr)
            ? price_route_on(*screen_route_, S_, K_, T_, sigma, r_, q_, side_, dividends_,
                             ExerciseStyle::American, ws_, screen_maps_)
            : price_on(S_, K_, T_, sigma, r_, q_, side_, dividends_, screen_steps_,
                       ExerciseStyle::American, kDiscreteDivScreenScheme, ws_);
    if (!p) {
      return Err(p.error());
    }
    return Ok(*p - price_);
  }
  // The setup of the two tiers (validation, lower endpoints, Screen admission).
  [[nodiscard]] Status prepare_historical(bool &screen);
  [[nodiscard]] Status prepare_production(bool &screen);
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
  const DiscreteDivLatticeRoute *route_ = nullptr;        // production: the accurate route
  const DiscreteDivLatticeRoute *screen_route_ = nullptr; // production: the Screen route
  const ddiv_detail::RouteMaps *maps_ = nullptr;          // a batch group's prebuilt maps
  const ddiv_detail::RouteMaps *screen_maps_ = nullptr;

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
  bool screen = false;
  const Status prepared =
      (route_ != nullptr) ? prepare_production(screen) : prepare_historical(screen);
  if (!prepared) {
    return Err(prepared.error());
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

Status Inverter::prepare_historical(bool &screen) {
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
  return Ok();
}

// The production tiers (lane D1). The Screen route needs no first-step refusal:
// its pre-steps give an early ex-date the nodes it needs.
Status Inverter::prepare_production(bool &screen) {
  const Status ok = ddiv_detail::validate(S_, K_, T_, 1.0, r_, q_, dividends_, route_->steps);
  if (!ok) {
    return Err(ok.error());
  }
  const Status admitted = admit_route(*route_, ws_);
  if (!admitted) {
    return Err(admitted.error());
  }
  const int coarsest = (route_->coarse_steps > 0) ? route_->coarse_steps : route_->steps;
  lo_acc_ = lattice_sigma_floor(T_, r_, q_, coarsest);
  if (!(lo_acc_ < kIvMax)) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: lattice has no admissible volatility range");
  }
  if (opts_.tier == DiscreteDivTier::Screen && screen_route_ != nullptr &&
      screen_route_->steps < route_->steps && admit_route(*screen_route_, ws_)) {
    screen_steps_ = screen_route_->steps;
    lo_scr_ = std::max(lo_acc_, lattice_sigma_floor(T_, r_, q_, screen_steps_));
    screen = lo_scr_ < kIvMax;
  }
  return Ok();
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

// ── 4. The D1 production path ───────────────────────────────────────────────

Result<double> ddiv_detail::spliced_cubic_price(double S, double K, double T, double sigma,
                                                double r, double q, Side side,
                                                std::span<const CashDividend> dividends,
                                                int steps, ExerciseStyle exercise,
                                                LatticeBuffers &buffers) {
  DiscreteDivProductionOpts opts;
  opts.base_steps = steps;
  const Result<DiscreteDivLatticeRoute> route = build_route(S, T, dividends, opts, true, 0.0);
  if (!route) {
    return Err(route.error());
  }
  const Result<LatticeOutcome> out =
      route_outcome(*route, S, K, T, sigma, r, q, side, dividends, exercise, buffers);
  if (!out) {
    return Err(out.error());
  }
  return Ok(out->price);
}

const char *to_string(DiscreteDivMethod value) noexcept {
  switch (value) {
  case DiscreteDivMethod::HistoricalSpliced:
    return "historical-spliced";
  case DiscreteDivMethod::CubicCrr:
    return "cubic-crr";
  case DiscreteDivMethod::CubicBbsRichardson:
    return "cubic-bbs-richardson";
  }
  return "unknown";
}

Result<DiscreteDivLatticeRoute> discrete_div_lattice_route(double S, double T,
                                                           std::span<const CashDividend> dividends,
                                                           const DiscreteDivProductionOpts &opts) {
  return build_route(S, T, dividends, opts, true, 0.0);
}

Result<double> american_discrete_div_route_price(const DiscreteDivLatticeRoute &route, double S,
                                                 double K, double T, double sigma, double r,
                                                 double q, Side side,
                                                 std::span<const CashDividend> dividends,
                                                 ExerciseStyle exercise,
                                                 DiscreteDivWorkspace &workspace) noexcept {
  return price_route_on(route, S, K, T, sigma, r, q, side, dividends, exercise, workspace);
}

Result<DiscreteDivProductionPrice> american_discrete_div_production_price(
    double S, double K, double T, double sigma, double r, double q, Side side,
    std::span<const CashDividend> dividends, ExerciseStyle exercise,
    const DiscreteDivProductionOpts &opts, DiscreteDivWorkspace &workspace) {
  const Result<DiscreteDivLatticeRoute> route = build_route(S, T, dividends, opts, true, 0.0);
  if (!route) {
    return Err(route.error());
  }
  const Result<double> price =
      price_route_on(*route, S, K, T, sigma, r, q, side, dividends, exercise, workspace);
  if (!price) {
    return Err(price.error());
  }
  DiscreteDivProductionPrice out;
  out.price = *price;
  out.route = *route;
  out.convention = discrete_div_convention(route->method == DiscreteDivMethod::HistoricalSpliced
                                               ? DiscreteDivScheme::SplicedCrr
                                               : DiscreteDivScheme::SplicedCubic);
  return Ok(out);
}

Result<DiscreteDivProductionIv> american_discrete_div_production_iv(
    double price, double S, double K, double T, double r, double q, Side side,
    std::span<const CashDividend> dividends, const DiscreteDivProductionOpts &opts,
    const DiscreteDivIvOpts &iv_opts, DiscreteDivWorkspace &workspace) {
  const Result<DiscreteDivLatticeRoute> route = build_route(S, T, dividends, opts, true, 0.0);
  if (!route) {
    return Err(route.error());
  }
  DiscreteDivProductionIv out;
  out.route = *route;
  if (route->method == DiscreteDivMethod::HistoricalSpliced) {
    // The kill switch: section 3 on the historical lattice, call for call.
    const Result<DiscreteDivIvResult> iv =
        american_discrete_div_implied_vol(price, S, K, T, r, q, side, dividends, route->steps,
                                          DiscreteDivScheme::SplicedCrr, iv_opts, workspace);
    if (!iv) {
      return Err(iv.error());
    }
    out.iv = *iv;
    return Ok(out);
  }
  // The Screen route: the Standard lattice at the screen count, no per-event raise.
  bool has_screen = false;
  if (iv_opts.screen_steps >= 4) {
    const Result<DiscreteDivLatticeRoute> screen =
        ddiv_detail::screen_route(S, T, dividends, iv_opts.screen_steps);
    if (screen) {
      out.screen = *screen;
      has_screen = true;
    }
  }
  Inverter inverter(price, S, K, T, r, q, side, dividends, out.route,
                    has_screen ? &out.screen : nullptr, iv_opts, workspace);
  const Result<DiscreteDivIvResult> iv = inverter.run();
  if (!iv) {
    return Err(iv.error());
  }
  out.iv = *iv;
  out.iv.convention = discrete_div_convention(DiscreteDivScheme::SplicedCubic);
  return Ok(out);
}

Result<double> ddiv_detail::route_price_prebuilt(const DiscreteDivLatticeRoute &route,
                                                 const RouteMaps *maps, double S, double K,
                                                 double T, double sigma, double r, double q,
                                                 Side side,
                                                 std::span<const CashDividend> dividends,
                                                 ExerciseStyle exercise,
                                                 DiscreteDivWorkspace &workspace) {
  return price_route_on(route, S, K, T, sigma, r, q, side, dividends, exercise, workspace, maps);
}

Result<DiscreteDivIvResult>
ddiv_detail::route_iv_prebuilt(double price, const DiscreteDivLatticeRoute &route,
                               const RouteMaps *maps, const DiscreteDivLatticeRoute *screen,
                               const RouteMaps *screen_maps, double S, double K, double T, double r,
                               double q, Side side, std::span<const CashDividend> dividends,
                               const DiscreteDivIvOpts &opts, DiscreteDivWorkspace &workspace) {
  Inverter inverter(price, S, K, T, r, q, side, dividends, route, screen, opts, workspace, maps,
                    screen_maps);
  Result<DiscreteDivIvResult> out = inverter.run();
  if (out) {
    out->convention = discrete_div_convention(route.method == DiscreteDivMethod::HistoricalSpliced
                                                  ? DiscreteDivScheme::SplicedCrr
                                                  : DiscreteDivScheme::SplicedCubic);
  }
  return out;
}

Result<DiscreteDivLatticeRoute> ddiv_detail::screen_route(double S, double T,
                                                          std::span<const CashDividend> dividends,
                                                          int screen_steps) {
  DiscreteDivProductionOpts screen_opts;
  screen_opts.base_steps = screen_steps;
  return build_route(S, T, dividends, screen_opts, false, 0.0);
}

std::size_t ddiv_detail::route_map_doubles(const DiscreteDivLatticeRoute &route) noexcept {
  const auto layers = [](int steps) {
    return (steps > 0) ? 2U * (static_cast<std::size_t>(steps) + 1U) : std::size_t{0};
  };
  const bool refined = (route.method == DiscreteDivMethod::CubicBbsRichardson);
  return layers(route.steps) + (refined ? layers(route.coarse_steps) : std::size_t{0});
}

ddiv_detail::RouteMaps ddiv_detail::build_route_maps(const DiscreteDivLatticeRoute &route,
                                                     double T, double r, double q,
                                                     std::span<const CashDividend> dividends,
                                                     std::span<double> arena) noexcept {
  RouteMaps maps;
  if (route.steps <= 0 || arena.size() < route_map_doubles(route)) {
    return maps; // empty maps: the rollback refuses them by size
  }
  const ProductionSpec spec = route_spec(route);
  std::size_t used = 0;
  const auto build = [&](int steps) {
    StepMap map;
    const std::size_t n = static_cast<std::size_t>(steps) + 1U;
    const std::span<double> amount = arena.subspan(used, n);
    const std::span<double> shift = arena.subspan(used + n, n);
    used += 2U * n;
    map.any = build_production_step_map(T, r, q, dividends, steps, spec, amount, shift);
    map.amount = amount;
    map.shift = shift;
    return map;
  };
  maps.fine = build(route.steps);
  if (route.method == DiscreteDivMethod::CubicBbsRichardson) {
    maps.coarse = build(route.coarse_steps);
  }
  return maps;
}

double ddiv_detail::route_sigma_floor(const DiscreteDivLatticeRoute &route, double T, double r,
                                      double q) noexcept {
  const int coarsest = (route.coarse_steps > 0) ? route.coarse_steps : route.steps;
  return lattice_sigma_floor(T, r, q, std::max(coarsest, 1));
}

namespace {

constexpr std::uint16_t greek_bit(DiscreteDivGreek greek) noexcept {
  return static_cast<std::uint16_t>(1U << static_cast<unsigned>(greek));
}

// The bumped price of one route (ProductionSpec hooks: a dividend amount, a
// calendar shift). Validated inputs, admitted route.
[[nodiscard]] Result<double>
bumped_price(const DiscreteDivLatticeRoute &route, double S, double K, double T, double sigma,
             double r, double q, Side side, std::span<const CashDividend> dividends,
             ExerciseStyle exercise, LatticeBuffers &buffers,
             std::size_t bump_index = ddiv_detail::ProductionSpec::kNoBump,
             double bump_amount = 0.0, const ddiv_detail::RouteMaps *maps = nullptr) {
  const Result<LatticeOutcome> out =
      route_outcome(route, S, K, T, sigma, r, q, side, dividends, exercise, buffers, bump_index,
                    bump_amount, 0.0, maps);
  if (!out) {
    return Err(out.error());
  }
  return Ok(out->price);
}

// The calendar roll of header section 4: cash in (0, horizon] comes off the stock,
// later ex-dates move closer, the contract is re-routed as seen at t + horizon.
[[nodiscard]] Result<DiscreteDivTimeRoll>
time_roll(double price, double S, double K, double T, double sigma, double r, double q, Side side,
          std::span<const CashDividend> dividends, ExerciseStyle exercise, double horizon,
          const DiscreteDivProductionOpts &opts, DiscreteDivWorkspace &workspace) {
  DiscreteDivTimeRoll roll;
  roll.horizon = horizon;
  for (const CashDividend &dv : dividends) {
    if (in_window(dv, T, 0.0) && dv.tau <= horizon) {
      ++roll.events_crossed;
      roll.cash_crossed += dv.amount;
    }
  }
  const double rolled_spot = S - roll.cash_crossed;
  if (!(rolled_spot > 0.0)) {
    return Err(ErrorCode::OutOfRange, "time roll: the cash crossed exhausts the stock");
  }
  const double rolled_T = T - horizon;
  if (!(rolled_T > 0.0)) {
    // A roll through expiry is an event too: the option is its payoff.
    roll.rolled_price = intrinsic(side, rolled_spot, K);
  } else {
    const Result<DiscreteDivLatticeRoute> rolled =
        build_route(rolled_spot, rolled_T, dividends, opts, true, horizon);
    if (!rolled) {
      return Err(rolled.error());
    }
    const Status admitted = admit_route(*rolled, workspace);
    if (!admitted) {
      return Err(admitted.error());
    }
    const Result<LatticeOutcome> out =
        route_outcome(*rolled, rolled_spot, K, rolled_T, sigma, r, q, side, dividends, exercise,
                      workspace.buffers(), ddiv_detail::ProductionSpec::kNoBump, 0.0, horizon);
    if (!out) {
      return Err(out.error());
    }
    roll.rolled_price = out->price;
  }
  roll.roll = roll.rolled_price - price;
  return Ok(roll);
}

} // namespace

Result<DiscreteDivProductionGreeks> ddiv_detail::route_greeks_prebuilt(
    const DiscreteDivLatticeRoute &route_in, const RouteMaps *maps, double S, double K, double T,
    double sigma, double r, double q, Side side, std::span<const CashDividend> dividends,
    ExerciseStyle exercise, std::span<double> dP_dDiv_out, double roll_horizon,
    const DiscreteDivProductionOpts &opts, DiscreteDivWorkspace &workspace) {
  if (!dP_dDiv_out.empty() && dP_dDiv_out.size() != dividends.size()) {
    return Err(ErrorCode::InvalidArgument,
               "dP_dDiv_out must be empty or carry exactly one slot per dividend event");
  }
  if (!std::isfinite(roll_horizon)) {
    return Err(ErrorCode::InvalidArgument, "roll horizon must be finite");
  }
  const DiscreteDivLatticeRoute *route = &route_in;
  const Status ok = ddiv_detail::validate(S, K, T, sigma, r, q, dividends, route->steps);
  if (!ok) {
    return Err(ok.error());
  }
  const Status admitted = admit_route(*route, workspace);
  if (!admitted) {
    return Err(admitted.error());
  }
  LatticeBuffers &buffers = workspace.buffers();
  // The vega legs share the group's (r, q) and so its maps; the rate and yield legs
  // move the forward scale of every ex-date and rebuild theirs.
  const auto price_at = [&](double s, double rr, double qq) {
    const RouteMaps *legs = (rr == r && qq == q) ? maps : nullptr;
    return bumped_price(*route, S, K, T, s, rr, qq, side, dividends, exercise, buffers,
                        ProductionSpec::kNoBump, 0.0, legs);
  };

  DiscreteDivProductionGreeks out;
  out.route = *route;
  const Result<LatticeOutcome> base =
      route_outcome(*route, S, K, T, sigma, r, q, side, dividends, exercise, buffers,
                    ProductionSpec::kNoBump, 0.0, 0.0, maps);
  if (!base) {
    return Err(base.error());
  }
  out.price = base->price;
  out.delta = base->delta;
  out.gamma = base->gamma;
  // The two-step stencils difference across steps 0..2: an ex-date there makes
  // them a difference across an EVENT, which is not a derivative.
  if (route->first_ex_step >= 0 && route->first_ex_step <= 2) {
    out.theta = kNaN;
    out.charm = kNaN;
    out.unsupported |= greek_bit(DiscreteDivGreek::Theta) | greek_bit(DiscreteDivGreek::Charm);
  } else {
    out.theta = base->theta;
    out.charm = base->charm;
  }

  const double hv = discrete_div_sigma_bump(sigma);
  const Result<double> vol_up = price_at(sigma + hv, r, q);
  const Result<double> vol_dn = price_at(sigma - hv, r, q);
  const Result<double> rate_up = price_at(sigma, r + kDiscreteDivRateBump, q);
  const Result<double> rate_dn = price_at(sigma, r - kDiscreteDivRateBump, q);
  const Result<double> yield_up = price_at(sigma, r, q + kDiscreteDivYieldBump);
  const Result<double> yield_dn = price_at(sigma, r, q - kDiscreteDivYieldBump);
  for (const Result<double> *leg : {&vol_up, &vol_dn, &rate_up, &rate_dn, &yield_up, &yield_dn}) {
    if (!*leg) {
      return Err(leg->error());
    }
  }
  out.vega = (*vol_up - *vol_dn) / (2.0 * hv);
  out.rho = (*rate_up - *rate_dn) / (2.0 * kDiscreteDivRateBump);
  out.phi = (*yield_up - *yield_dn) / (2.0 * kDiscreteDivYieldBump);

  if (dP_dDiv_out.empty()) {
    out.unsupported |= greek_bit(DiscreteDivGreek::DividendAmount); // not computed
  }
  for (std::size_t i = 0; i < dP_dDiv_out.size(); ++i) {
    const CashDividend &dv = dividends[i];
    // The lattice's (0, T] window on the ex-date alone: a zero amount inside it
    // still has a (one-sided) sensitivity; an event outside it moves nothing.
    if (!(dv.tau > 0.0) || dv.tau > T * (1.0 + kTauAtExpiryRelTol)) {
      dP_dDiv_out[i] = 0.0;
      continue;
    }
    const double h = discrete_div_amount_bump(dv.amount);
    const double lo = std::max(dv.amount - h, 0.0);
    const double hi = dv.amount + h;
    const Result<double> up = bumped_price(*route, S, K, T, sigma, r, q, side, dividends,
                                           exercise, buffers, i, hi);
    const Result<double> dn = bumped_price(*route, S, K, T, sigma, r, q, side, dividends,
                                           exercise, buffers, i, lo);
    if (!up || !dn) {
      return Err(!up ? up.error() : dn.error());
    }
    dP_dDiv_out[i] = (*up - *dn) / (hi - lo);
  }

  if (roll_horizon > 0.0) {
    const Result<DiscreteDivTimeRoll> roll = time_roll(out.price, S, K, T, sigma, r, q, side,
                                                       dividends, exercise, roll_horizon, opts,
                                                       workspace);
    if (roll) {
      out.roll = *roll;
    } else {
      out.roll.horizon = roll_horizon;
      out.roll.rolled_price = kNaN;
      out.roll.roll = kNaN;
      out.unsupported |= greek_bit(DiscreteDivGreek::Roll);
    }
  } else {
    out.roll.rolled_price = kNaN;
    out.roll.roll = kNaN;
    out.unsupported |= greek_bit(DiscreteDivGreek::Roll);
  }
  return Ok(out);
}

Result<DiscreteDivProductionGreeks> american_discrete_div_production_greeks(
    double S, double K, double T, double sigma, double r, double q, Side side,
    std::span<const CashDividend> dividends, ExerciseStyle exercise,
    std::span<double> dP_dDiv_out, double roll_horizon, const DiscreteDivProductionOpts &opts,
    DiscreteDivWorkspace &workspace) {
  const Result<DiscreteDivLatticeRoute> route = build_route(S, T, dividends, opts, true, 0.0);
  if (!route) {
    return Err(route.error());
  }
  return ddiv_detail::route_greeks_prebuilt(*route, nullptr, S, K, T, sigma, r, q, side,
                                            dividends, exercise, dP_dDiv_out, roll_horizon, opts,
                                            workspace);
}

} // namespace atx::vol
