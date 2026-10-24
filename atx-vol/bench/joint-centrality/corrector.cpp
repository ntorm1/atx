#include "corrector.hpp"

#include <algorithm>
#include <cmath>

namespace atx::vol::research {
namespace {

bool finite(std::span<const double> values) {
  return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
}

bool positive(std::span<const double> values) {
  return std::all_of(values.begin(), values.end(), [](double v) { return v > 0. && std::isfinite(v); });
}

double step(std::span<const double> values, std::span<const double> delta) {
  double result = 1.;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (delta[i] < 0.) result = std::min(result, -values[i] / delta[i]);
  }
  return result;
}

bool valid(std::span<const double> s, std::span<const double> z,
           CentralityDirection d, CentralityScratch scratch, HomogeneousSolve solve) {
  const auto m = s.size(), n = d.x.size();
  return m > 0 && n > 0 && z.size() == m && d.s.size() == m && d.z.size() == m &&
      scratch.residual.size() == m && scratch.direction.x.size() == n &&
      scratch.direction.s.size() == m && scratch.direction.z.size() == m &&
      solve != nullptr && positive(s) && positive(z) && finite(d.x) && finite(d.s) && finite(d.z);
}

bool residual(std::span<const double> s, std::span<const double> z,
              CentralityDirection d, double ap, double ad, double low, double high,
              std::span<double> rc, bool &nonzero) {
  nonzero = false;
  const double trial_p = std::min(ap + .1, 1.), trial_d = std::min(ad + .1, 1.);
  for (std::size_t i = 0; i < s.size(); ++i) {
    const double primal = s[i] + trial_p * d.s[i];
    const double dual = z[i] + trial_d * d.z[i];
    const double product = primal * dual;
    if (!std::isfinite(primal) || !std::isfinite(dual) || !std::isfinite(product)) return false;
    rc[i] = product - std::clamp(product, low, high); // rc=-t for the existing Newton equations.
    if (!std::isfinite(rc[i])) return false;
    nonzero = nonzero || rc[i] != 0.;
  }
  return true;
}

bool combine(std::span<const double> original, std::span<double> correction) {
  for (std::size_t i = 0; i < original.size(); ++i) {
    correction[i] += original[i];
    if (!std::isfinite(correction[i])) return false;
  }
  return true;
}

} // namespace

bool improve_centrality(std::span<const double> s, std::span<const double> z, double mu_target,
    CentralityDirection direction, CentralityScratch scratch, HomogeneousSolve solve,
    void *context, CentralityWork &work) {
  work = {};
  if (!valid(s, z, direction, scratch, solve)) return false;
  const double low = .1 * mu_target, high = 10. * mu_target;
  if (!(mu_target > 0.) || !std::isfinite(mu_target) || !(low > 0.) ||
      !std::isfinite(low) || !std::isfinite(high)) {
    ++work.invalid_target;
    return true;
  }
  for (std::size_t attempt = 0; attempt < 3; ++attempt) {
    const double ap = step(s, direction.s), ad = step(z, direction.z);
    if (ap == 1. && ad == 1.) break;
    ++work.attempts;
    bool nonzero = false;
    if (!residual(s, z, direction, ap, ad, low, high, scratch.residual, nonzero)) {
      ++work.nonfinite;
      break;
    }
    if (!nonzero) { ++work.zero_target; break; }
    ++work.solves;
    if (!solve(context, scratch.residual, scratch.direction)) { ++work.solve_refused; break; }
    const auto next = scratch.direction;
    if (!combine(direction.x, next.x) || !combine(direction.s, next.s) ||
        !combine(direction.z, next.z)) { ++work.nonfinite; break; }
    const double next_p = step(s, next.s), next_d = step(z, next.z);
    if (next_p < std::min(1., ap + .01) || next_d < std::min(1., ad + .01) ||
        !(next_p > ap || next_d > ad)) {
      ++work.nonimproving;
      break;
    }
    std::copy(next.x.begin(), next.x.end(), direction.x.begin());
    std::copy(next.s.begin(), next.s.end(), direction.s.begin());
    std::copy(next.z.begin(), next.z.end(), direction.z.begin());
    ++work.accepted;
  }
  return true;
}

} // namespace atx::vol::research
