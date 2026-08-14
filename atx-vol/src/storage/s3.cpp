#include "atx/vol/api/storage/s3.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Golden-section constant (sqrt(5) - 1) / 2 for the seed's 1-D scale search.
constexpr double kGolden = 0.6180339887498949;
// Number of golden-section iterations: 0.618^120 shrinks the bracket to well
// below double precision — a statically-bounded loop (JPL Rule 2).
constexpr int kSeedIters = 120;

// ── S3 shape f(z) and its first two z-derivatives ────────────────────────
//
//   f(z)  = 0.5*a + sqrt(Q),   a = 1 + s2*z,   Q = 0.25*a^2 + 0.5*c2*z^2.
//   f'(z) = 0.5*s2 + Q'/(2R),        Q'  = 0.5*s2*a + c2*z,   R = sqrt(Q).
//   f''(z)= Q''/(2R) - Q'^2/(4R^3),  Q'' = 0.5*s2^2 + c2   (constant in z).
//
// Derived by differentiating R = sqrt(Q) twice (Q is quadratic in z, so
// Q''' == 0). At z = 0 this gives f(0)=1, f'(0)=s2, f''(0)=c2 exactly. For
// c2 > 0 the radicand Q >= 0.25*a^2 + 0.5*c2*z^2 > 0 everywhere, so R > 0 and
// f is C-infinity. In the c2 == 0 "takeover-for-cash" limit R = 0.5*|a| and
// f has a kink at z = -1/s2 where f' / f'' are singular (bare-evaluator
// convention: NaN in => NaN out; callers keep the density grid off the kink).
struct ShapeDeriv {
  double f{};
  double fp{};   // f'(z)
  double fpp{};  // f''(z)
};

[[nodiscard]] ShapeDeriv s3_shape(double z, double s2, double c2) noexcept {
  const double a = 1.0 + s2 * z;
  const double radicand = 0.25 * a * a + 0.5 * c2 * z * z;  // Q
  const double R = std::sqrt(radicand);
  const double Qp = 0.5 * s2 * a + c2 * z;  // Q'(z)
  const double Qpp = 0.5 * s2 * s2 + c2;    // Q''(z), constant in z
  ShapeDeriv d{};
  d.f = 0.5 * a + R;
  d.fp = 0.5 * s2 + Qp / (2.0 * R);
  d.fpp = Qpp / (2.0 * R) - (Qp * Qp) / (4.0 * R * R * R);
  return d;
}

// ATF total-vol / standard deviation sigma_hat0 = sigma0 * sqrt(T). (Named
// distinctly from the public s3_atf_max_skew's `sigma_hat0` parameter to keep
// the shadow-warning gate clean.)
[[nodiscard]] double atf_stdev(const S3Params& p, double T) noexcept {
  return p.sigma0 * std::sqrt(T);
}

// One inner solve of the seed for a fixed ATF stdev S = sigma_hat0.
//
// The S3 definition yields the EXACT identity (square f - 0.5*a and cancel):
//   f^2 - f = s2*(f*z) + c2*(0.5*z^2),   f = w/S^2,  z = k/S,  w = iv^2*T.
// That is linear in (s2, c2), so a 2x2 normal-equation solve recovers them
// with no Taylor truncation. `sse` is the reconstructed residual in
// total-variance space, the objective the outer 1-D search minimizes.
struct SeedInner {
  double sse{};
  double s2{};
  double c2{};
};

[[nodiscard]] SeedInner seed_inner(double S, std::span<const double> k_log,
                                   std::span<const double> iv,
                                   double T) noexcept {
  SeedInner out{};
  if (!(S > 0.0)) {
    out.sse = kInf;
    return out;
  }
  const double inv_s2 = 1.0 / (S * S);
  double spp = 0.0;
  double spq = 0.0;
  double sqq = 0.0;
  double spy = 0.0;
  double sqy = 0.0;
  const std::size_t n = k_log.size();
  for (std::size_t i = 0; i < n; ++i) {
    const double z = k_log[i] / S;
    const double w = iv[i] * iv[i] * T;
    const double fv = w * inv_s2;
    const double p = fv * z;
    const double q = 0.5 * z * z;
    const double y = fv * fv - fv;
    spp += p * p;
    spq += p * q;
    sqq += q * q;
    spy += p * y;
    sqy += q * y;
  }
  const double det = spp * sqq - spq * spq;
  if (!std::isfinite(det) || std::fabs(det) < 1.0e-300) {
    out.sse = kInf;
    return out;
  }
  double s2 = (spy * sqq - sqy * spq) / det;
  double c2 = (spp * sqy - spq * spy) / det;
  if (c2 < 0.0) {
    // c2 is constrained non-negative: clamp and re-fit the skew alone.
    c2 = 0.0;
    s2 = (spp > 0.0) ? (spy / spp) : 0.0;
  }
  double sse = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double z = k_log[i] / S;
    const double w = iv[i] * iv[i] * T;
    const ShapeDeriv d = s3_shape(z, s2, c2);
    const double r = S * S * d.f - w;
    sse += r * r;
  }
  out.sse = sse;
  out.s2 = s2;
  out.c2 = c2;
  return out;
}

}  // namespace

// ── Shape evaluators ──────────────────────────────────────────────────────

double s3_sigma2_of_z(double z, const S3Params& p) noexcept {
  const ShapeDeriv d = s3_shape(z, p.s2, p.c2);
  return p.sigma0 * p.sigma0 * d.f;
}

double s3_total_var(double k_log, double T, const S3Params& p) noexcept {
  const double shat0 = atf_stdev(p, T);
  if (!(shat0 > 0.0)) {
    return kNaN;
  }
  const double z = k_log / shat0;
  const ShapeDeriv d = s3_shape(z, p.s2, p.c2);
  return shat0 * shat0 * d.f;  // = T * sigma0^2 * f(z)
}

double s3_iv(double k_log, double T, const S3Params& p) noexcept {
  if (!(T > 0.0)) {
    return kNaN;
  }
  const double w = s3_total_var(k_log, T, p);
  if (!(w > 0.0)) {
    return kNaN;
  }
  return std::sqrt(w / T);
}

// ── Wings ─────────────────────────────────────────────────────────────────

S3Wings s3_wings(const S3Params& p) noexcept {
  const double root = std::sqrt(0.25 * p.s2 * p.s2 + 0.5 * p.c2);
  S3Wings w{};
  w.c_minus = root - 0.5 * p.s2;
  w.c_plus = root + 0.5 * p.s2;
  return w;
}

// ── Roper butterfly density ───────────────────────────────────────────────

double s3_density_g(double z, double T, const S3Params& p) noexcept {
  const double shat0 = atf_stdev(p, T);
  if (!(shat0 > 0.0)) {
    return kNaN;
  }
  const ShapeDeriv d = s3_shape(z, p.s2, p.c2);
  const double w = shat0 * shat0 * d.f;  // total variance w(k)
  const double wp = shat0 * d.fp;        // w'(k), since dz/dk = 1/shat0
  const double wpp = d.fpp;              // w''(k) = f''(z)
  const double k = shat0 * z;            // log-moneyness
  if (!(w > 0.0)) {
    return kNaN;
  }
  // Roper density — identical form to arb.cpp / arb_check_butterfly.
  const double term1_inner = 1.0 - 0.5 * k * wp / w;
  const double term1 = term1_inner * term1_inner;
  const double term2 = 0.25 * wp * wp * (0.25 + 1.0 / w);
  const double term3 = 0.5 * wpp;
  return term1 - term2 + term3;
}

// ── ATF no-arbitrage bound ────────────────────────────────────────────────

double s3_atf_max_skew(double c2, double sigma_hat0) noexcept {
  const double denom = 1.0 + 0.25 * sigma_hat0 * sigma_hat0;
  return std::sqrt((4.0 + 2.0 * c2) / denom);
}

bool s3_atf_arb_free(const S3Params& p, double T) noexcept {
  const double shat0 = atf_stdev(p, T);
  const double max_skew = s3_atf_max_skew(p.c2, shat0);
  return p.s2 * p.s2 <= max_skew * max_skew;
}

// ── Least-squares seed ────────────────────────────────────────────────────

Result<S3Params> s3_seed_from_ivs(std::span<const double> k_log,
                                  std::span<const double> iv,
                                  double T) noexcept {
  if (k_log.size() != iv.size()) {
    return Err(ErrorCode::InvalidArgument,
               "s3_seed_from_ivs: k_log/iv length mismatch");
  }
  if (k_log.size() < 3) {
    return Err(ErrorCode::InvalidArgument, "s3_seed_from_ivs: need >= 3 samples");
  }
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "s3_seed_from_ivs: T must be > 0");
  }

  double iv_sum = 0.0;
  double k_min = k_log[0];
  double k_max = k_log[0];
  const std::size_t n = k_log.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (!std::isfinite(iv[i]) || !(iv[i] > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "s3_seed_from_ivs: iv must be finite and positive");
    }
    if (!std::isfinite(k_log[i])) {
      return Err(ErrorCode::InvalidArgument,
                 "s3_seed_from_ivs: k_log must be finite");
    }
    iv_sum += iv[i];
    k_min = std::min(k_min, k_log[i]);
    k_max = std::max(k_max, k_log[i]);
  }
  if (!(k_max - k_min > 1.0e-12)) {
    return Err(ErrorCode::InvalidArgument,
               "s3_seed_from_ivs: k_log has no spread");
  }

  // Bracket the ATF stdev generously around the mean-IV scale, then run a
  // fixed-count golden-section minimization of the total-variance residual.
  const double s0 = (iv_sum / static_cast<double>(n)) * std::sqrt(T);
  double lo = 0.1 * s0;
  double hi = 5.0 * s0;
  double c = hi - kGolden * (hi - lo);
  double d = lo + kGolden * (hi - lo);
  SeedInner fc = seed_inner(c, k_log, iv, T);
  SeedInner fd = seed_inner(d, k_log, iv, T);
  for (int it = 0; it < kSeedIters; ++it) {
    if (fc.sse < fd.sse) {
      hi = d;
      d = c;
      fd = fc;
      c = hi - kGolden * (hi - lo);
      fc = seed_inner(c, k_log, iv, T);
    } else {
      lo = c;
      c = d;
      fc = fd;
      d = lo + kGolden * (hi - lo);
      fd = seed_inner(d, k_log, iv, T);
    }
  }

  const double s_star = 0.5 * (lo + hi);
  const SeedInner best = seed_inner(s_star, k_log, iv, T);
  if (!(s_star > 0.0) || !std::isfinite(best.sse)) {
    return Err(ErrorCode::Internal, "s3_seed_from_ivs: seed did not converge");
  }

  S3Params out{};
  out.sigma0 = s_star / std::sqrt(T);
  out.s2 = best.s2;
  out.c2 = best.c2;
  return Ok(out);
}

}  // namespace atx::vol
