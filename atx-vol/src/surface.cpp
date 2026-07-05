#include "atx/vol/surface.hpp"

#include <cmath>
#include <limits>
#include <type_traits>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

// ── Raw SVI ──────────────────────────────────────────────────────────────

double svi_w(const SviSlice& s, double k_log) noexcept {
  const double dk = k_log - s.m;
  const double r = std::sqrt(dk * dk + s.sigma * s.sigma);
  return s.a + s.b * (s.rho * dk + r);
}

// ── eSSVI ────────────────────────────────────────────────────────────────

double essvi_w(const EssviSlice& s, double k_log) noexcept {
  const double pk = s.phi * k_log;
  const double a = pk + s.rho;
  const double inner = a * a + (1.0 - s.rho * s.rho);
  return 0.5 * s.theta * (1.0 + s.rho * pk + std::sqrt(inner));
}

EssviGrad essvi_w_grad(const EssviSlice& s, double k_log) noexcept {
  const double pk = s.phi * k_log;
  const double a = pk + s.rho;
  const double inner = a * a + (1.0 - s.rho * s.rho);
  const double r = std::sqrt(inner);
  // Chain rule (mirrors the C's ats_vol_essvi_w_grad exactly):
  //   dr/dphi = a*k/r          dw/dphi = (theta/2)*(rho*k + a*k/r)
  //   dr/drho = (a-rho)/r      dw/drho = (theta/2)*(pk + (a-rho)/r)
  const double dwdth = 0.5 * (1.0 + s.rho * pk + r);
  const double dwdphi = 0.5 * s.theta * (s.rho * k_log + (a * k_log) / r);
  const double dwdrho = 0.5 * s.theta * (pk + (a - s.rho) / r);
  return EssviGrad{dwdth, dwdphi, dwdrho};
}

// ── Surface<Slice> ───────────────────────────────────────────────────────

template <class Slice>
double Surface<Slice>::eval_w(const Slice& slice, double k_log) noexcept {
  if constexpr (std::is_same_v<Slice, SviSlice>) {
    return svi_w(slice, k_log);
  } else {
    static_assert(std::is_same_v<Slice, EssviSlice>,
                  "Surface<Slice> only supports SviSlice / EssviSlice");
    return essvi_w(slice, k_log);
  }
}

template <class Slice>
Status Surface<Slice>::set_slice(std::size_t idx, const Slice& slice) {
  if (idx >= slices_.size()) {
    return Err(ErrorCode::OutOfRange,
               "Surface::set_slice: idx exceeds capacity");
  }
  slices_[idx] = slice;
  if (idx >= n_slices_) {
    n_slices_ = idx + 1;
  }
  return Ok();
}

template <class Slice>
double Surface<Slice>::w(double k_log, double T) const noexcept {
  if (n_slices_ == 0) {
    return kNaN;
  }
  if (T < kTMinEval) {
    T = kTMinEval;
  }

  const std::size_t n = n_slices_;
  const double T0 = slices_[0].T;
  if (T <= T0) {
    // Sprint-26 short-T extrapolation guard (see file header): refuse to
    // extrapolate when the query sits materially below the first slice.
    if (T < 0.5 * T0) {
      return kNaN;
    }
    return eval_w(slices_[0], k_log);
  }
  // Exact-pillar hit at the longest slice must evaluate that slice, not
  // fall through to the "T > last" extrapolation-forbidden branch.
  const double T_last = slices_[n - 1].T;
  if (T == T_last) {
    return eval_w(slices_[n - 1], k_log);
  }
  if (T > T_last) {
    return kNaN;  // Extrapolation past the longest slice is never allowed.
  }

  // Binary search for the bracketing pair [lo, hi] with T_lo <= T < T_hi.
  std::size_t lo = 0;
  std::size_t hi = n - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (slices_[mid].T <= T) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double T_lo = slices_[lo].T;
  const double T_hi = slices_[hi].T;
  const double w_lo = eval_w(slices_[lo], k_log);
  const double w_hi = eval_w(slices_[hi], k_log);
  const double alpha = (T - T_lo) / (T_hi - T_lo);
  return w_lo + alpha * (w_hi - w_lo);
}

template <class Slice>
double Surface<Slice>::iv(double k_log, double T) const noexcept {
  const double wv = w(k_log, T);
  if (!std::isfinite(wv) || wv <= 0.0) {
    return kNaN;
  }
  // Deliberately divides by the caller's original T, not the internally
  // floored value w() uses for bracketing — matches the C's
  // ats_vol_surface_iv exactly (see the header-doc note on this function).
  return std::sqrt(wv / T);
}

// Only these two parametrizations are supported by this port (see the
// header's scope note); explicit instantiation keeps the template body out
// of the public header.
template class Surface<SviSlice>;
template class Surface<EssviSlice>;

}  // namespace atx::vol
