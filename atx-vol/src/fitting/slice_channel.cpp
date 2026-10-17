#include "fitting/slice_channel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <Eigen/Cholesky>
#include <Eigen/Core>

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

constexpr std::size_t kMaxKnots = kSrMoneynessGrid.size();
using KnotMatrix =
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::ColMajor, kMaxKnots, kMaxKnots>;
using KnotVector = Eigen::Matrix<double, Eigen::Dynamic, 1, Eigen::ColMajor, kMaxKnots, 1>;

[[nodiscard]] bool positive(double value) noexcept { return std::isfinite(value) && value > 0.0; }
[[nodiscard]] bool nonnegative(double value) noexcept {
  return std::isfinite(value) && value >= 0.0;
}

struct Prepared {
  double k;
  double lo;
  double hi;  // may be +inf
  double mid; // NaN when absent
  double mid_scale;
  double h;   // floored half-width used to normalize this contract's terms
};

struct Knots {
  std::array<double, kMaxKnots> z{};
  std::size_t n{};
};

struct PassResult {
  Knots knots;
  KnotVector v;
  double sigma_ref{};
  double z_lo{};
  double z_hi{};
  double objective{};
  std::size_t iterations{};
  std::size_t violations{};
  double max_violation{};
  bool converged{false};
};

[[nodiscard]] Status validate_config(const ChannelFitConfig &c) {
  if (!nonnegative(c.channel_weight) || !nonnegative(c.mid_weight) ||
      !nonnegative(c.default_shape_weight) || !positive(c.prior_scale) ||
      !nonnegative(c.roughness) || !positive(c.min_half_width) || !positive(c.mult_floor) ||
      !positive(c.mult_ceil) || !(c.mult_ceil > c.mult_floor) || c.max_iterations == 0 ||
      c.min_observations == 0) {
    return Err(ErrorCode::InvalidArgument, "slice channel: invalid configuration");
  }
  return Ok();
}

[[nodiscard]] Status validate_prior(const ChannelPrior &prior) {
  if (!positive(prior.atm_vol) || !nonnegative(prior.strength) || prior.z.empty() ||
      prior.z.size() != prior.multiples.size()) {
    return Err(ErrorCode::InvalidArgument, "slice channel: invalid prior");
  }
  for (std::size_t i = 0; i < prior.z.size(); ++i) {
    if (!std::isfinite(prior.z[i]) || !positive(prior.multiples[i]) ||
        (i > 0 && !(prior.z[i] > prior.z[i - 1]))) {
      return Err(ErrorCode::InvalidArgument, "slice channel: invalid prior shape");
    }
  }
  return Ok();
}

[[nodiscard]] std::vector<Prepared> prepare(std::span<const ChannelObservation> observations,
                                            const ChannelFitConfig &config) {
  std::vector<Prepared> out;
  out.reserve(observations.size());
  for (const ChannelObservation &o : observations) {
    const bool upper = positive(o.sigma_hi);
    if (!std::isfinite(o.k) || !nonnegative(o.sigma_lo) || !nonnegative(o.mid_scale) ||
        !(o.sigma_hi > o.sigma_lo) || (!upper && !(o.sigma_lo > 0.0))) {
      continue; // malformed, or carries no volatility information at all
    }
    const double width = upper ? 0.5 * (o.sigma_hi - o.sigma_lo) : 0.5 * o.sigma_lo;
    const bool mid = std::isfinite(o.sigma_mid) && o.sigma_mid >= o.sigma_lo &&
                     o.sigma_mid <= o.sigma_hi;
    out.push_back({o.k, o.sigma_lo, o.sigma_hi,
                   mid ? o.sigma_mid : std::numeric_limits<double>::quiet_NaN(), o.mid_scale,
                   std::max(width, config.min_half_width)});
  }
  return out;
}

// Weighted median of channel centres (weights 1/h^2), refined toward the money
// with a standard-normal kernel in the median's standardized moneyness.
[[nodiscard]] double seed_atm(const std::vector<Prepared> &observations, double variance_years) {
  std::vector<std::pair<double, double>> centres;
  centres.reserve(observations.size());
  double total = 0.0;
  for (const Prepared &o : observations) {
    const double centre = std::isfinite(o.mid) ? o.mid
                          : std::isfinite(o.hi) ? 0.5 * (o.lo + o.hi)
                                                : o.lo;
    const double weight = 1.0 / (o.h * o.h);
    centres.emplace_back(centre, weight);
    total += weight;
  }
  std::sort(centres.begin(), centres.end());
  double cumulative = 0.0;
  double median = centres.back().first;
  for (const auto &[centre, weight] : centres) {
    cumulative += weight;
    if (cumulative >= 0.5 * total) {
      median = centre;
      break;
    }
  }
  if (!positive(median)) {
    return median;
  }
  const double axis = median * std::sqrt(variance_years);
  double numerator = 0.0;
  double denominator = 0.0;
  for (std::size_t i = 0; i < observations.size(); ++i) {
    const double z = observations[i].k / axis;
    const double weight = std::exp(-0.5 * z * z) / (observations[i].h * observations[i].h);
    const double centre = std::isfinite(observations[i].mid) ? observations[i].mid
                          : std::isfinite(observations[i].hi)
                              ? 0.5 * (observations[i].lo + observations[i].hi)
                              : observations[i].lo;
    numerator += weight * centre;
    denominator += weight;
  }
  const double refined = denominator > 0.0 ? numerator / denominator : median;
  return positive(refined) ? refined : median;
}

// Every grid knot within one standardized unit of the observed range, always
// including the nine central knots on [-1, 1].
[[nodiscard]] Knots select_knots(double z_lo, double z_hi) noexcept {
  const double lo = std::min(z_lo - 1.0, -1.0);
  const double hi = std::max(z_hi + 1.0, 1.0);
  Knots knots;
  for (const double z : kSrMoneynessGrid) {
    if (z >= lo && z <= hi) {
      knots.z[knots.n++] = z;
    }
  }
  return knots;
}

// Column j holds the natural-spline second derivatives for the unit ordinate e_j
// (Thomas algorithm on the strictly diagonally dominant interior system).
void cardinal_second_derivatives(const Knots &knots, KnotMatrix &second) {
  const std::size_t n = knots.n;
  second.setZero(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
  if (n < 3) {
    return;
  }
  const std::size_t interior = n - 2;
  std::array<double, kMaxKnots> h{};
  for (std::size_t i = 0; i + 1 < n; ++i) {
    h[i] = knots.z[i + 1] - knots.z[i];
  }
  std::array<double, kMaxKnots> cp{};
  std::array<double, kMaxKnots> denominators{};
  denominators[0] = 2.0 * (h[0] + h[1]);
  cp[0] = interior > 1 ? h[1] / denominators[0] : 0.0;
  for (std::size_t t = 1; t < interior; ++t) {
    denominators[t] = 2.0 * (h[t] + h[t + 1]) - h[t] * cp[t - 1];
    cp[t] = t + 1 < interior ? h[t + 1] / denominators[t] : 0.0;
  }
  for (std::size_t j = 0; j < n; ++j) {
    std::array<double, kMaxKnots> d{};
    for (std::size_t t = 0; t < interior; ++t) {
      const std::size_t i = t + 1;
      const double y_prev = i - 1 == j ? 1.0 : 0.0;
      const double y_here = i == j ? 1.0 : 0.0;
      const double y_next = i + 1 == j ? 1.0 : 0.0;
      d[t] = 6.0 * ((y_next - y_here) / h[t + 1] - (y_here - y_prev) / h[t]);
    }
    std::array<double, kMaxKnots> dp{};
    dp[0] = d[0] / denominators[0];
    for (std::size_t t = 1; t < interior; ++t) {
      dp[t] = (d[t] - h[t] * dp[t - 1]) / denominators[t];
    }
    std::array<double, kMaxKnots> y{};
    y[interior - 1] = dp[interior - 1];
    for (std::size_t t = interior - 1; t > 0; --t) {
      y[t - 1] = dp[t - 1] - cp[t - 1] * y[t];
    }
    for (std::size_t t = 0; t < interior; ++t) {
      second(static_cast<Eigen::Index>(t + 1), static_cast<Eigen::Index>(j)) = y[t];
    }
  }
}

// Cardinal basis row at z (clamped into the knot span: flat wings), matching the
// served SplineVolCurve evaluation formula.
void basis_row(const Knots &knots, const KnotMatrix &second, double z, double *row) noexcept {
  const std::size_t n = knots.n;
  std::fill(row, row + n, 0.0);
  z = std::clamp(z, knots.z[0], knots.z[n - 1]);
  std::size_t idx = static_cast<std::size_t>(
      std::upper_bound(knots.z.begin(), knots.z.begin() + static_cast<std::ptrdiff_t>(n), z) -
      knots.z.begin());
  idx = std::clamp<std::size_t>(idx, 1, n - 1) - 1;
  const double h = knots.z[idx + 1] - knots.z[idx];
  const double a = (knots.z[idx + 1] - z) / h;
  const double b = (z - knots.z[idx]) / h;
  const double ca = (a * a * a - a) * (h * h) / 6.0;
  const double cb = (b * b * b - b) * (h * h) / 6.0;
  for (std::size_t j = 0; j < n; ++j) {
    const auto col = static_cast<Eigen::Index>(j);
    row[j] = ca * second(static_cast<Eigen::Index>(idx), col) +
             cb * second(static_cast<Eigen::Index>(idx + 1), col);
  }
  row[idx] += a;
  row[idx + 1] += b;
}

[[nodiscard]] double prior_value(const ChannelPrior &prior, double z) noexcept {
  const auto zs = prior.z;
  const auto ms = prior.multiples;
  if (z <= zs.front()) return prior.atm_vol * ms.front();
  if (z >= zs.back()) return prior.atm_vol * ms.back();
  const auto upper = std::upper_bound(zs.begin(), zs.end(), z);
  const auto i = static_cast<std::size_t>(upper - zs.begin());
  const double fraction = (z - zs[i - 1]) / (zs[i] - zs[i - 1]);
  return prior.atm_vol * std::lerp(ms[i - 1], ms[i], fraction);
}

class Problem {
public:
  Problem(const std::vector<Prepared> &observations, double variance_years, double sigma_ref,
          const ChannelPrior *prior, const ChannelFitConfig &config)
      : obs_(observations), config_(config), sigma_ref_(sigma_ref),
        channel_scale_(config.channel_weight /
                       (config.min_half_width * config.min_half_width)) {
    const double axis = sigma_ref * std::sqrt(variance_years);
    z_lo_ = std::numeric_limits<double>::infinity();
    z_hi_ = -z_lo_;
    for (const Prepared &o : obs_) {
      z_lo_ = std::min(z_lo_, o.k / axis);
      z_hi_ = std::max(z_hi_, o.k / axis);
    }
    knots_ = select_knots(z_lo_, z_hi_);
    n_ = knots_.n;
    const auto size = static_cast<Eigen::Index>(n_);
    KnotMatrix second;
    cardinal_second_derivatives(knots_, second);
    basis_.resize(obs_.size() * n_);
    for (std::size_t i = 0; i < obs_.size(); ++i) {
      basis_row(knots_, second, obs_[i].k / axis, &basis_[i * n_]);
    }
    target_.resize(size);
    weight_.resize(size);
    const double strength = prior ? prior->strength : config.default_shape_weight;
    for (std::size_t j = 0; j < n_; ++j) {
      const double value =
          prior ? prior_value(*prior, knots_.z[j] * sigma_ref / prior->atm_vol) : sigma_ref;
      const double scale = config.prior_scale * value;
      target_(static_cast<Eigen::Index>(j)) = value;
      weight_(static_cast<Eigen::Index>(j)) = strength / (scale * scale);
    }
    roughness_.setZero(size, size);
    const double coefficient = config.roughness / (sigma_ref * sigma_ref);
    for (std::size_t r = 0; r + 2 < n_; ++r) {
      const std::array<double, 3> d{1.0, -2.0, 1.0};
      for (std::size_t a = 0; a < 3; ++a) {
        for (std::size_t b = 0; b < 3; ++b) {
          roughness_(static_cast<Eigen::Index>(r + a), static_cast<Eigen::Index>(r + b)) +=
              coefficient * d[a] * d[b];
        }
      }
    }
  }

  [[nodiscard]] double sigma(std::size_t i, const KnotVector &v) const noexcept {
    double value = 0.0;
    const double *row = &basis_[i * n_];
    for (std::size_t j = 0; j < n_; ++j) {
      value += row[j] * v(static_cast<Eigen::Index>(j));
    }
    return value;
  }

  // Exact objective; records each observation's active hinge (-1 low, +1 high).
  [[nodiscard]] double objective(const KnotVector &v, std::vector<std::int8_t> &active) const {
    double total = 0.0;
    for (std::size_t i = 0; i < obs_.size(); ++i) {
      const Prepared &o = obs_[i];
      const double s = sigma(i, v);
      const double inverse = 1.0 / (o.h * o.h);
      if (std::isfinite(o.mid)) {
        const double e = s - o.mid;
        total += config_.mid_weight * o.mid_scale * e * e * inverse;
      }
      active[i] = 0;
      if (s < o.lo) {
        active[i] = -1;
        total += channel_scale_ * (o.lo - s) * (o.lo - s);
      } else if (s > o.hi) {
        active[i] = 1;
        total += channel_scale_ * (s - o.hi) * (s - o.hi);
      }
    }
    const KnotVector deviation = v - target_;
    total += deviation.cwiseProduct(deviation).dot(weight_);
    total += v.dot(roughness_ * v);
    return total;
  }

  [[nodiscard]] bool solve(const std::vector<std::int8_t> &active, KnotVector &out) const {
    const auto size = static_cast<Eigen::Index>(n_);
    KnotMatrix hessian = roughness_;
    hessian.diagonal() += weight_;
    // Tiny relative ridge keeps an unregularized, unidentified knot solvable.
    const double ridge = 1.0e-12 * std::max(1.0, hessian.diagonal().cwiseAbs().maxCoeff());
    hessian.diagonal().array() += ridge;
    KnotVector rhs = weight_.cwiseProduct(target_);
    for (std::size_t i = 0; i < obs_.size(); ++i) {
      const Prepared &o = obs_[i];
      const double inverse = 1.0 / (o.h * o.h);
      double curvature = 0.0;
      double force = 0.0;
      if (std::isfinite(o.mid)) {
        const double a = config_.mid_weight * o.mid_scale * inverse;
        curvature += a;
        force += a * o.mid;
      }
      if (active[i] != 0) {
        const double a = channel_scale_;
        curvature += a;
        force += a * (active[i] < 0 ? o.lo : o.hi);
      }
      if (!(curvature > 0.0)) continue;
      const double *row = &basis_[i * n_];
      for (std::size_t a = 0; a < n_; ++a) {
        const auto ia = static_cast<Eigen::Index>(a);
        rhs(ia) += force * row[a];
        for (std::size_t b = 0; b < n_; ++b) {
          hessian(ia, static_cast<Eigen::Index>(b)) += curvature * row[a] * row[b];
        }
      }
    }
    const Eigen::LLT<KnotMatrix> factor(hessian);
    if (factor.info() != Eigen::Success) {
      return false;
    }
    out = factor.solve(rhs);
    return out.size() == size && out.allFinite();
  }

  [[nodiscard]] Result<PassResult> run() const {
    PassResult result;
    result.knots = knots_;
    result.sigma_ref = sigma_ref_;
    result.z_lo = z_lo_;
    result.z_hi = z_hi_;
    KnotVector v = target_;
    std::vector<std::int8_t> active(obs_.size(), 0);
    std::vector<std::int8_t> next(obs_.size(), 0);
    double value = objective(v, active);
    KnotVector solution;
    for (std::uint16_t iteration = 0; iteration < config_.max_iterations; ++iteration) {
      if (!solve(active, solution)) {
        return Err(ErrorCode::Internal, "slice channel: knot system is not positive definite");
      }
      KnotVector candidate = solution;
      double candidate_value = objective(candidate, next);
      double step = 1.0;
      for (int backtrack = 0; backtrack < 30 && !(candidate_value <= value); ++backtrack) {
        step *= 0.5;
        candidate = v + step * (solution - v);
        candidate_value = objective(candidate, next);
      }
      ++result.iterations;
      if (!(candidate_value <= value)) {
        result.converged = true; // no descent direction remains at this precision
        break;
      }
      const bool stable = next == active;
      v = candidate;
      value = candidate_value;
      active.swap(next);
      if (stable && step == 1.0) {
        // A full Newton step whose hinge pattern is unchanged is the exact
        // minimizer of the convex piece containing it, hence global.
        result.converged = true;
        break;
      }
    }
    const double floor = config_.mult_floor * sigma_ref_;
    for (Eigen::Index j = 0; j < v.size(); ++j) {
      v(j) = std::max(v(j), floor);
    }
    result.objective = objective(v, active);
    for (std::size_t i = 0; i < obs_.size(); ++i) {
      const double s = sigma(i, v);
      const double excess = std::max(obs_[i].lo - s, s - obs_[i].hi);
      if (excess > 1.0e-9) {
        ++result.violations;
        result.max_violation = std::max(result.max_violation, excess / obs_[i].h);
      }
    }
    result.v = v;
    return Ok(std::move(result));
  }

  [[nodiscard]] double atm(const KnotVector &v) const noexcept {
    KnotMatrix second;
    cardinal_second_derivatives(knots_, second);
    std::array<double, kMaxKnots> row{};
    basis_row(knots_, second, 0.0, row.data());
    double value = 0.0;
    for (std::size_t j = 0; j < n_; ++j) {
      value += row[j] * v(static_cast<Eigen::Index>(j));
    }
    return value;
  }

private:
  const std::vector<Prepared> &obs_;
  const ChannelFitConfig &config_;
  double sigma_ref_{};
  // The channel is a near-hard constraint: violations are measured in a FIXED
  // volatility unit (min_half_width), not in the quote's own half-width, so a
  // wide or bidless channel is as binding as a tight one.
  double channel_scale_{};
  double z_lo_{};
  double z_hi_{};
  Knots knots_;
  std::size_t n_{};
  std::vector<double> basis_;
  KnotVector target_;
  KnotVector weight_;
  KnotMatrix roughness_;
};

} // namespace

Result<ChannelFit> fit_slice_channel(std::span<const ChannelObservation> observations,
                                     double variance_years, const ChannelPrior *prior,
                                     const ChannelFitConfig &config) {
  ATX_TRY_VOID(validate_config(config));
  if (!positive(variance_years)) {
    return Err(ErrorCode::InvalidArgument, "slice channel: invalid variance time");
  }
  if (prior != nullptr) {
    ATX_TRY_VOID(validate_prior(*prior));
  }
  const std::vector<Prepared> prepared = prepare(observations, config);
  if (prepared.size() < config.min_observations) {
    return Err(ErrorCode::Unavailable, "slice channel: too few usable channel observations");
  }
  const bool use_prior = prior != nullptr && prior->strength > 0.0;
  const ChannelPrior *applied = use_prior ? prior : nullptr;
  double sigma_ref = use_prior ? prior->atm_vol : seed_atm(prepared, variance_years);
  if (!positive(sigma_ref)) {
    return Err(ErrorCode::Unavailable, "slice channel: no ATM volatility can be seeded");
  }
  std::size_t iterations = 0;
  const Problem first(prepared, variance_years, sigma_ref, applied, config);
  ATX_TRY(PassResult pass, first.run());
  iterations += pass.iterations;
  // Re-standardize once at the fitted ATM volatility (two solves in total).
  const double fitted_atm = first.atm(pass.v);
  if (positive(fitted_atm) && std::abs(fitted_atm / sigma_ref - 1.0) > 1.0e-6) {
    sigma_ref = fitted_atm;
    const Problem second(prepared, variance_years, sigma_ref, applied, config);
    ATX_TRY(PassResult refined, second.run());
    iterations += refined.iterations;
    pass = std::move(refined);
  }
  ChannelFit out;
  auto &params = out.params;
  params.atm_vol = pass.sigma_ref;
  params.z.assign(pass.knots.z.begin(),
                  pass.knots.z.begin() + static_cast<std::ptrdiff_t>(pass.knots.n));
  params.mult.resize(pass.knots.n);
  for (std::size_t j = 0; j < pass.knots.n; ++j) {
    params.mult[j] = pass.v(static_cast<Eigen::Index>(j)) / pass.sigma_ref;
  }
  params.z_lo_valid = pass.z_lo;
  params.z_hi_valid = pass.z_hi;
  params.mult_cap = config.mult_ceil;
  auto &diag = out.diagnostics;
  diag.observations = prepared.size();
  diag.mid_observations = static_cast<std::size_t>(std::count_if(
      prepared.begin(), prepared.end(), [](const Prepared &o) { return std::isfinite(o.mid); }));
  diag.knots = pass.knots.n;
  diag.iterations = iterations;
  diag.violations = pass.violations;
  diag.max_violation_half_widths = pass.max_violation;
  diag.objective = pass.objective;
  diag.prior_strength = use_prior ? prior->strength : config.default_shape_weight;
  diag.used_term_prior = use_prior;
  diag.converged = pass.converged;
  return Ok(std::move(out));
}

namespace {

// Quadratic terms at one log-moneyness add: the merged target is the weighted
// mean with the summed weight (and the tighter unit).
void merge_targets(std::vector<ChannelTarget> &targets) {
  std::sort(targets.begin(), targets.end(),
            [](const ChannelTarget &a, const ChannelTarget &b) { return a.k < b.k; });
  std::size_t kept = 0;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const ChannelTarget t = targets[i];
    if (kept > 0 &&
        std::abs(t.k - targets[kept - 1].k) <= 1.0e-12 * std::max(1.0, std::abs(t.k))) {
      ChannelTarget &merged = targets[kept - 1];
      const double weight = merged.weight + t.weight;
      merged.sigma = (merged.weight * merged.sigma + t.weight * t.sigma) / weight;
      merged.weight = weight;
      merged.unit = std::min(merged.unit, t.unit);
    } else {
      targets[kept++] = t;
    }
  }
  targets.resize(kept);
}

} // namespace

Result<ChannelFamilyFit>
fit_slice_channel_family(std::span<const ChannelObservation> observations, double variance_years,
                         const ChannelPrior *prior, const ChannelFitConfig &config,
                         std::uint16_t max_steps, const ChannelFamilyStep &step) {
  ATX_TRY_VOID(validate_config(config));
  if (!positive(variance_years)) {
    return Err(ErrorCode::InvalidArgument, "slice channel: invalid variance time");
  }
  if (max_steps == 0 || !step) {
    return Err(ErrorCode::InvalidArgument, "slice channel: invalid family step");
  }
  if (prior != nullptr) {
    ATX_TRY_VOID(validate_prior(*prior));
  }
  const std::vector<Prepared> prepared = prepare(observations, config);
  if (prepared.size() < config.min_observations) {
    return Err(ErrorCode::Unavailable, "slice channel: too few usable channel observations");
  }
  const bool use_prior = prior != nullptr && prior->strength > 0.0;
  const double sigma_ref = use_prior ? prior->atm_vol : seed_atm(prepared, variance_years);
  if (!positive(sigma_ref)) {
    return Err(ErrorCode::Unavailable, "slice channel: no ATM volatility can be seeded");
  }
  const double axis = sigma_ref * std::sqrt(variance_years);
  double z_lo = std::numeric_limits<double>::infinity();
  double z_hi = -z_lo;
  for (const Prepared &o : prepared) {
    z_lo = std::min(z_lo, o.k / axis);
    z_hi = std::max(z_hi, o.k / axis);
  }
  // The prior as point targets on the knots the knot spline would use.
  const Knots knots = select_knots(z_lo, z_hi);
  const double strength = use_prior ? prior->strength : config.default_shape_weight;
  std::vector<ChannelTarget> anchors;
  anchors.reserve(knots.n);
  for (std::size_t j = 0; j < knots.n; ++j) {
    const double value =
        use_prior ? prior_value(*prior, knots.z[j] * sigma_ref / prior->atm_vol) : sigma_ref;
    const double unit = config.prior_scale * value;
    anchors.push_back({knots.z[j] * axis, value, strength / (unit * unit), unit});
  }
  const std::size_t n = prepared.size();
  std::vector<double> k_eval(n + anchors.size());
  for (std::size_t i = 0; i < n; ++i) k_eval[i] = prepared[i].k;
  for (std::size_t j = 0; j < anchors.size(); ++j) k_eval[n + j] = anchors[j].k;
  std::vector<double> sigma(k_eval.size());
  const double channel_scale =
      config.channel_weight / (config.min_half_width * config.min_half_width);
  const auto mid_weight = [&config](const Prepared &o) {
    return std::isfinite(o.mid) ? config.mid_weight * o.mid_scale / (o.h * o.h) : 0.0;
  };
  std::vector<std::int8_t> active(n, 0);
  std::vector<std::int8_t> next(n, 0);
  std::vector<ChannelTarget> targets;
  targets.reserve(n + anchors.size());
  ChannelFamilyFit out;
  out.sigma_ref = sigma_ref;
  auto &diag = out.diagnostics;
  double best = std::numeric_limits<double>::infinity();
  std::size_t steps = 0;
  for (std::uint16_t iteration = 0; iteration < max_steps; ++iteration) {
    targets.clear();
    for (std::size_t i = 0; i < n; ++i) {
      const Prepared &o = prepared[i];
      const double a = mid_weight(o);
      const double b = active[i] != 0 ? channel_scale : 0.0;
      if (!(a + b > 0.0)) continue;
      const double bound = active[i] < 0 ? o.lo : o.hi;
      const double target = b == 0.0 ? o.mid : a == 0.0 ? bound : (a * o.mid + b * bound) / (a + b);
      targets.push_back({o.k, target, a + b, 2.0 * o.h});
    }
    if (strength > 0.0) targets.insert(targets.end(), anchors.begin(), anchors.end());
    merge_targets(targets);
    const Status fitted = step(sigma_ref, targets, k_eval, sigma);
    const bool finite = fitted && std::all_of(sigma.begin(), sigma.begin() +
                                                  static_cast<std::ptrdiff_t>(n),
                                              [](double s) { return positive(s); });
    if (!finite) {
      if (steps > 0) break; // keep the best step so far
      if (!fitted) return Err(fitted.error());
      return Err(ErrorCode::Internal, "slice channel: family curve is not finite on the quotes");
    }
    double value = 0.0;
    std::size_t violations = 0;
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const Prepared &o = prepared[i];
      const double s = sigma[i];
      if (std::isfinite(o.mid)) value += mid_weight(o) * (s - o.mid) * (s - o.mid);
      next[i] = 0;
      if (s < o.lo) {
        next[i] = -1;
        value += channel_scale * (o.lo - s) * (o.lo - s);
      } else if (s > o.hi) {
        next[i] = 1;
        value += channel_scale * (s - o.hi) * (s - o.hi);
      }
      const double excess = std::max(o.lo - s, s - o.hi);
      if (excess > 1.0e-9) {
        ++violations;
        worst = std::max(worst, excess / o.h);
      }
    }
    for (std::size_t j = 0; j < anchors.size(); ++j) {
      const double s = sigma[n + j];
      if (!std::isfinite(s)) continue; // outside the family's domain: no prior term
      const double d = s - anchors[j].sigma;
      value += anchors[j].weight * d * d;
    }
    if (value < best) {
      best = value;
      out.step = steps;
      diag.violations = violations;
      diag.max_violation_half_widths = worst;
      diag.objective = value;
    }
    ++steps;
    if (next == active) {
      diag.converged = true; // the next step would refit the same targets
      break;
    }
    active.swap(next);
  }
  diag.observations = n;
  diag.mid_observations = static_cast<std::size_t>(std::count_if(
      prepared.begin(), prepared.end(), [](const Prepared &o) { return std::isfinite(o.mid); }));
  diag.knots = anchors.size();
  diag.iterations = steps;
  diag.prior_strength = strength;
  diag.used_term_prior = use_prior;
  return Ok(std::move(out));
}

} // namespace atx::vol
