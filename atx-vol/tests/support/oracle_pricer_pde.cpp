#include "oracle_pricer_pde.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace atx::vol::test {

namespace {

// Thomas algorithm: solve M·u = d for a tridiagonal M with sub-diagonal a,
// diagonal b, super-diagonal c (each length n; a[0], c[n-1] unused). Overwrites
// b and d; the solution is returned in d.
void thomas(int n, double* a, double* b, double* c, double* d) noexcept {
  for (int i = 1; i < n; ++i) {
    const double m = a[i] / b[i - 1];
    b[i] -= m * c[i - 1];
    d[i] -= m * d[i - 1];
  }
  d[n - 1] /= b[n - 1];
  for (int i = n - 2; i >= 0; --i) {
    d[i] = (d[i] - c[i] * d[i + 1]) / b[i];
  }
}

}  // namespace

double oracle_pde_american(double S, double K, double T, double sigma, double r,
                           double q, Side side, const OraclePdeOpts& opts) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return kNan;
  }

  const int n_t = (opts.n_t > 0) ? opts.n_t : 2000;
  const int n_x = (opts.n_x > 0) ? opts.n_x : 4000;

  const double s_min = K * (opts.s_min_mult > 0.0 ? opts.s_min_mult : 0.05);
  const double s_max = K * (opts.s_max_mult > 0.0 ? opts.s_max_mult : 20.0);
  const double x_min = std::log(s_min);
  const double x_max = std::log(s_max);
  if (!(x_max > x_min)) {
    return kNan;
  }

  const double dx = (x_max - x_min) / static_cast<double>(n_x - 1);
  const double dt = T / static_cast<double>(n_t);

  // Operator L V = α V_xx + β V_x + γ V.
  const double alpha = 0.5 * sigma * sigma;
  const double beta = r - q - 0.5 * sigma * sigma;
  const double gamma = -r;

  const double inv_dx2 = 1.0 / (dx * dx);
  const double A = alpha * inv_dx2 - beta * 0.5 / dx;
  const double B = -2.0 * alpha * inv_dx2 + gamma;
  const double C = alpha * inv_dx2 + beta * 0.5 / dx;

  const double aL = -0.5 * dt * A;
  const double bL = 1.0 - 0.5 * dt * B;
  const double cL = -0.5 * dt * C;
  const double aR = 0.5 * dt * A;
  const double bR = 1.0 + 0.5 * dt * B;
  const double cR = 0.5 * dt * C;

  const std::size_t nx = static_cast<std::size_t>(n_x);
  std::vector<double> V(nx);
  std::vector<double> Vnew(nx);
  std::vector<double> a(nx);
  std::vector<double> b(nx);
  std::vector<double> c(nx);

  // Precompute the spot at each node once (the C oracle recomputed exp() every
  // step; the values are identical, just cached here for speed).
  std::vector<double> Sgrid(nx);
  for (int i = 0; i < n_x; ++i) {
    Sgrid[static_cast<std::size_t>(i)] = std::exp(x_min + static_cast<double>(i) * dx);
  }

  // Terminal payoff at expiry.
  for (int i = 0; i < n_x; ++i) {
    const double Si = Sgrid[static_cast<std::size_t>(i)];
    const double v = (side == Side::Call) ? (Si - K) : (K - Si);
    V[static_cast<std::size_t>(i)] = (v > 0.0) ? v : 0.0;
  }

  // March in τ from 0 -> T.
  for (int n = 0; n < n_t; ++n) {
    const double tau_new = static_cast<double>(n + 1) * dt;

    Vnew[0] = (side == Side::Put) ? K * std::exp(-r * tau_new) : 0.0;
    double up_bc = (side == Side::Call)
                       ? Sgrid[nx - 1] * std::exp(-q * tau_new) - K * std::exp(-r * tau_new)
                       : 0.0;
    if (up_bc < 0.0) {
      up_bc = 0.0;
    }
    Vnew[nx - 1] = up_bc;

    const int Mrows = n_x - 2;
    for (int i = 0; i < Mrows; ++i) {
      const std::size_t ii = static_cast<std::size_t>(i + 1);
      a[static_cast<std::size_t>(i)] = aL;
      b[static_cast<std::size_t>(i)] = bL;
      c[static_cast<std::size_t>(i)] = cL;
      double rhs = aR * V[ii - 1] + bR * V[ii] + cR * V[ii + 1];
      if (i == 0) {
        rhs -= aL * Vnew[0];
      }
      if (i == Mrows - 1) {
        rhs -= cL * Vnew[nx - 1];
      }
      Vnew[ii] = rhs;
    }
    thomas(Mrows, a.data(), b.data(), c.data(), Vnew.data() + 1);

    // Early-exercise projection.
    for (int i = 0; i < n_x; ++i) {
      const double Si = Sgrid[static_cast<std::size_t>(i)];
      const double intr = (side == Side::Call) ? (Si - K) : (K - Si);
      double& cell = Vnew[static_cast<std::size_t>(i)];
      if (intr > cell) {
        cell = intr;
      }
    }

    V.swap(Vnew);
  }

  // Interpolate at the target spot.
  const double xS = std::log(S);
  if (xS < x_min || xS > x_max) {
    return kNan;
  }
  const double pos = (xS - x_min) / dx;
  int ilo = static_cast<int>(std::floor(pos));
  if (ilo < 0) {
    ilo = 0;
  }
  if (ilo > n_x - 2) {
    ilo = n_x - 2;
  }
  const double frac = pos - static_cast<double>(ilo);
  const std::size_t lo = static_cast<std::size_t>(ilo);
  return V[lo] + frac * (V[lo + 1] - V[lo]);
}

}  // namespace atx::vol::test
