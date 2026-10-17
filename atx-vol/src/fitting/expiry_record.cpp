// Per-expiry (log-moneyness, sigma) calibration record. See
// api/fitting/expiry_record.hpp for the representation and its contract.

#include "atx/vol/api/fitting/expiry_record.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "atx/core/math.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/greeks.hpp"
#include "fitting/correction_detail.hpp" // cheb_node / cheb_dct2 / cheb_clenshaw1d / unit maps
#include "pricing/american_boundary.hpp" // amer:: boundary seam

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::uint16_t kNk = ExpiryRecord::kNk;
constexpr std::uint16_t kNs = ExpiryRecord::kNs;

// The continuation domain of a sigma row is never narrower than this fraction of
// the moneyness box (a row whose edge sits at the box's far end), nor does it run
// further than this many box widths past the box (a row whose edge is far out of
// the box). Inside those limits the domain endpoint IS the unclamped edge, so it
// stays a smooth function of sigma; a clamp only engages in corners the
// certification then reports.
constexpr double kMinDomainFrac = 0.05;
constexpr double kMaxDomainOvershoot = 1.0;

// Clenshaw for p(x) = c0 + sum_{k>=1} c_k T_k(x) and its first x-derivative
// (differentiated recurrence; same full-c0 form as detail::cheb_clenshaw1d).
struct ChebD1 {
  double v = 0.0, d = 0.0;
};
[[nodiscard]] ChebD1 clenshaw_d1(const double *c, std::uint16_t n, double x) noexcept {
  double b1 = 0.0, b2 = 0.0, d1 = 0.0, d2 = 0.0;
  const double two_x = 2.0 * x;
  for (int k = static_cast<int>(n) - 1; k >= 1; --k) {
    const double b = c[k] + two_x * b1 - b2;
    const double d = 2.0 * b1 + two_x * d1 - d2;
    b2 = b1;
    b1 = b;
    d2 = d1;
    d1 = d;
  }
  return ChebD1{c[0] + x * b1 - b2, b1 + x * d1 - d2};
}

// The kNs sigma rows of the record plane, each a kNk-term moneyness series,
// evaluated at one u in LOCKSTEP (serve-path lane item 4). The plane is stored
// moneyness-major (coefs[i * kNs + j], sigma row j contiguous per term i), so
// each Clenshaw step reads one contiguous kNs-double row for every sigma row and
// the row loop vectorizes. Row j's recurrence is exactly
// detail::cheb_clenshaw1d's (Order 0), clenshaw_d1's (Order 1) or clenshaw_d2's
// (Order 2) — the same operations in the same order — so every output is
// bit-identical to evaluating that row on its own; interleaving only turns kNs
// serial dependency chains into one throughput-bound loop. `vu` / `vuu` may be
// null below the order that writes them.
template <int Order>
void eval_rows(const double *coefs, double u, double *v, double *vu, double *vuu) noexcept {
  static_assert(Order >= 0 && Order <= 2, "value, first or second u-derivative");
  std::array<double, kNs> b1{};
  std::array<double, kNs> b2{};
  std::array<double, kNs> d1{};
  std::array<double, kNs> d2{};
  std::array<double, kNs> e1{};
  std::array<double, kNs> e2{};
  const double two_u = 2.0 * u;
  for (int k = static_cast<int>(kNk) - 1; k >= 1; --k) {
    const double *ck = coefs + static_cast<std::size_t>(k) * kNs;
    for (std::size_t j = 0; j < kNs; ++j) {
      const double b = ck[j] + two_u * b1[j] - b2[j];
      if constexpr (Order >= 1) {
        const double d = 2.0 * b1[j] + two_u * d1[j] - d2[j];
        if constexpr (Order >= 2) {
          const double e = 4.0 * d1[j] + two_u * e1[j] - e2[j];
          e2[j] = e1[j];
          e1[j] = e;
        }
        d2[j] = d1[j];
        d1[j] = d;
      }
      b2[j] = b1[j];
      b1[j] = b;
    }
  }
  for (std::size_t j = 0; j < kNs; ++j) {
    v[j] = coefs[j] + u * b1[j] - b2[j];
    if constexpr (Order >= 1) {
      vu[j] = b1[j] + u * d1[j] - d2[j];
    }
    if constexpr (Order >= 2) {
      vuu[j] = 2.0 * d1[j] + u * e1[j] - e2[j];
    }
  }
}

// One AL boundary + workspace (~45 KB): heap-held for the duration of a build so
// a build does not put two of them on the stack.
struct BoundaryWork {
  amer::AlBoundary bnd;
  amer::AlWorkspace ws;
};

// The internal-put view of a side. A put is itself; a call is the McDonald-
// Schroder put P(K, S, q, r), so the boundary machinery is shared.
struct InternalPut {
  bool put = true;
  double rp = 0.0; // internal-put rate
  double qp = 0.0; // internal-put yield
};

// Solve the boundary of the side at sigma with F normalized to 1 (spot S1 =
// e^{-(r-q)T}); returns the unclamped exercise edge k* in ln(K/F) terms, or NaN
// when the boundary cannot be solved. Put: reference strike 1, exercised iff
// S1 <= e^k b*(sigma) <=> k >= ln S1 - ln b*. Call: internal-put strike S1,
// exercised iff the internal spot e^k <= B_int <=> k <= ln B_int.
[[nodiscard]] double solve_edge(BoundaryWork &w, const InternalPut &ip, double T, double sigma,
                                double S1, const amer::AlScheme &sch) noexcept {
  const double k_ref = ip.put ? 1.0 : S1;
  if (amer::al_solve_put_boundary(k_ref, T, sigma, ip.rp, ip.qp, sch, w.bnd, w.ws) !=
      amer::AlSolveStatus::Ok) {
    return kNaN;
  }
  const double b_now = amer::al_boundary_now(w.bnd);
  if (!(b_now > 0.0) || !std::isfinite(b_now)) {
    return kNaN;
  }
  return ip.put ? (std::log(S1) - std::log(b_now / k_ref)) : std::log(b_now);
}

// Forward-normalized premium at log-moneyness k on the boundary solved by
// solve_edge (F = 1). Put: rescale the K-independent boundary state to strike e^k
// (andersen_lake_put_slice's homogeneity reuse). Call: the internal put prices at
// spot e^k against the fixed internal strike S1.
[[nodiscard]] double premium_at(BoundaryWork &w, const InternalPut &ip, double k, double sigma,
                                double S1) noexcept {
  const double K = std::exp(k);
  if (ip.put) {
    w.bnd.K = K;
    w.bnd.xmax = amer::al_xmax_put(K, ip.rp, ip.qp);
    return amer::al_put_premium_from_boundary(w.bnd, w.ws, S1, sigma, ip.rp, ip.qp);
  }
  return amer::al_put_premium_from_boundary(w.bnd, w.ws, K, sigma, ip.rp, ip.qp);
}

// Cold accurate price (F = 1) at (k, sigma) on a boundary solved by solve_edge
// with the accurate scheme — the certification reference.
[[nodiscard]] double cold_price_at(BoundaryWork &w, const InternalPut &ip, double k, double sigma,
                                   double T, double S1) noexcept {
  const double K = std::exp(k);
  if (ip.put) {
    w.bnd.K = K;
    w.bnd.xmax = amer::al_xmax_put(K, ip.rp, ip.qp);
    return amer::al_put_price_from_boundary(w.bnd, w.ws, S1, K, T, sigma, ip.rp, ip.qp);
  }
  return amer::al_put_price_from_boundary(w.bnd, w.ws, /*S=*/K, /*K=*/S1, T, sigma, ip.rp, ip.qp);
}

// 2-D DCT of the sampled plane: `vals` is sigma-row-major (vals[j * kNk + i]),
// `coefs` is written moneyness-major (coefs[i * kNs + j]) for eval_rows.
void dct2d(const std::array<double, static_cast<std::size_t>(kNk) * kNs> &vals,
           std::array<double, static_cast<std::size_t>(kNk) * kNs> &coefs) noexcept {
  std::array<double, kNk> in{};
  std::array<double, kNk> out{};
  std::array<double, static_cast<std::size_t>(kNk) * kNs> tmp{};
  for (std::uint16_t j = 0; j < kNs; ++j) {
    for (std::uint16_t i = 0; i < kNk; ++i) {
      in[i] = vals[static_cast<std::size_t>(j) * kNk + i];
    }
    detail::cheb_dct2(in.data(), out.data(), kNk);
    for (std::uint16_t i = 0; i < kNk; ++i) {
      tmp[static_cast<std::size_t>(j) * kNk + i] = out[i];
    }
  }
  std::array<double, kNs> col{};
  std::array<double, kNs> col_out{};
  for (std::uint16_t i = 0; i < kNk; ++i) {
    for (std::uint16_t j = 0; j < kNs; ++j) {
      col[j] = tmp[static_cast<std::size_t>(j) * kNk + i];
    }
    detail::cheb_dct2(col.data(), col_out.data(), kNs);
    for (std::uint16_t j = 0; j < kNs; ++j) {
      coefs[static_cast<std::size_t>(i) * kNs + j] = col_out[j];
    }
  }
}

[[nodiscard]] bool finite_all(std::initializer_list<double> xs) noexcept {
  for (const double x : xs) {
    if (!std::isfinite(x)) {
      return false;
    }
  }
  return true;
}

} // namespace

// ── build ────────────────────────────────────────────────────────────────────

Result<ExpiryRecord> ExpiryRecord::build(const ExpiryRecordSpec &spec) {
  const double T = spec.expiry;
  const double r = spec.carry.rate;
  const double q = spec.carry.carry;
  if (!finite_all({T, r, q, spec.k_min, spec.k_max, spec.sigma_min, spec.sigma_max}) ||
      !(T > 1.0e-10) || !(spec.k_max > spec.k_min) || !(spec.sigma_min > 0.0) ||
      !(spec.sigma_max > spec.sigma_min)) {
    return Err(ErrorCode::InvalidArgument,
               "ExpiryRecord::build: need finite T > 0, k_min < k_max, 0 < sigma_min < "
               "sigma_max and finite carry");
  }
  if (!spec.carry.dividends.empty()) {
    return Err(ErrorCode::NotImplemented,
               "ExpiryRecord::build: dated cash dividends are not supported yet (the record "
               "needs a dividend-to-spot axis and a Vellekoop-Nieuwenhuis reference)");
  }
  const bool put = (spec.side == Side::Put);
  const InternalPut ip{put, put ? r : q, put ? q : r};

  ExpiryRecord rec;
  rec.T_ = T;
  rec.r_ = r;
  rec.q_ = q;
  rec.growth_ = std::exp((r - q) * T);
  rec.df_ = std::exp(-r * T);
  rec.k_min_ = spec.k_min;
  rec.k_max_ = spec.k_max;
  rec.s_min_ = spec.sigma_min;
  rec.s_max_ = spec.sigma_max;
  rec.side_ = spec.side;
  rec.cert_err_ = kNaN;
  rec.cert_pts_ = 0;

  switch (detail::classify_regime(ip.rp, ip.qp)) {
  case detail::ExerciseRegime::Unsupported:
    return Err(ErrorCode::NotImplemented,
               "ExpiryRecord::build: double-continuation regime (no single exercise boundary)");
  case detail::ExerciseRegime::European:
    rec.american_ = false;
    rec.has_edge_ = false;
    break;
  case detail::ExerciseRegime::American:
    rec.american_ = true;
    break;
  }

  const double S1 = std::exp(-(r - q) * T); // spot that puts the forward at 1
  const double width = spec.k_max - spec.k_min;
  std::array<double, kNs> s_nodes{};
  for (std::uint16_t j = 0; j < kNs; ++j) {
    s_nodes[j] = detail::cheb_from_unit(detail::cheb_node(j, kNs), spec.sigma_min, spec.sigma_max);
  }

  if (rec.american_) {
    const amer::AlScheme sch = amer::scheme_from_opts(spec.sample_opts);
    // One retained boundary per sigma node (~45 KB each, build-time heap only):
    // the edge pass and the sampling pass share them instead of re-solving.
    const auto rows = std::make_unique<BoundaryWork[]>(kNs);
    const auto work = std::make_unique<BoundaryWork>();
    std::array<double, kNs> kstar{};
    for (std::uint16_t j = 0; j < kNs; ++j) {
      kstar[j] = solve_edge(rows[j], ip, T, s_nodes[j], S1, sch);
      if (!std::isfinite(kstar[j])) {
        return Err(ErrorCode::NotImplemented,
                   "ExpiryRecord::build: exercise boundary unsolvable at a sigma node");
      }
    }
    // The edge is monotone in sigma, so its extent over the box is set by the two
    // box ends; Chebyshev nodes are interior, hence the two extra solves.
    const double e_lo = solve_edge(*work, ip, T, spec.sigma_min, S1, sch);
    const double e_hi = solve_edge(*work, ip, T, spec.sigma_max, S1, sch);
    double e_min = std::min(e_lo, e_hi);
    double e_max = std::max(e_lo, e_hi);
    for (const double e : kstar) {
      e_min = std::min(e_min, e);
      e_max = std::max(e_max, e);
    }
    rec.has_edge_ = put ? (e_min < spec.k_max) : (e_max > spec.k_min);

    std::array<double, kNs> dom{};
    for (std::uint16_t j = 0; j < kNs; ++j) {
      if (!rec.has_edge_) {
        dom[j] = put ? spec.k_max : spec.k_min;
      } else if (put) {
        dom[j] = std::clamp(kstar[j], spec.k_min + kMinDomainFrac * width,
                            spec.k_max + kMaxDomainOvershoot * width);
      } else {
        dom[j] = std::clamp(kstar[j], spec.k_min - kMaxDomainOvershoot * width,
                            spec.k_max - kMinDomainFrac * width);
      }
    }
    detail::cheb_dct2(kstar.data(), rec.edge_.data(), kNs);
    detail::cheb_dct2(dom.data(), rec.domain_.data(), kNs);

    std::array<double, static_cast<std::size_t>(kNk) * kNs> vals{};
    for (std::uint16_t j = 0; j < kNs; ++j) {
      const double lo = put ? spec.k_min : dom[j];
      const double hi = put ? dom[j] : spec.k_max;
      for (std::uint16_t i = 0; i < kNk; ++i) {
        const double k = detail::cheb_from_unit(detail::cheb_node(i, kNk), lo, hi);
        vals[static_cast<std::size_t>(j) * kNk + i] = premium_at(rows[j], ip, k, s_nodes[j], S1);
      }
    }
    dct2d(vals, rec.coefs_);
  }
  rec.populated_ = true;

  if (spec.certify) {
    // σ checks: both box ends plus every midpoint between adjacent (sorted) nodes.
    std::array<double, kNs + 1> s_check{};
    std::array<double, kNs> s_sorted = s_nodes;
    std::sort(s_sorted.begin(), s_sorted.end());
    s_check[0] = spec.sigma_min;
    for (std::uint16_t j = 0; j + 1 < kNs; ++j) {
      s_check[j + 1] = 0.5 * (s_sorted[j] + s_sorted[j + 1]);
    }
    s_check[kNs] = spec.sigma_max;
    std::array<double, kNk> u_sorted{};
    for (std::uint16_t i = 0; i < kNk; ++i) {
      u_sorted[i] = detail::cheb_node(i, kNk);
    }
    std::sort(u_sorted.begin(), u_sorted.end());

    const amer::AlScheme acc = amer::scheme_from_opts(std::nullopt);
    const auto work = std::make_unique<BoundaryWork>();
    double worst = 0.0;
    std::uint32_t pts = 0;
    std::uint32_t refused_rows = 0;
    const auto check = [&](double k, double sigma, bool cold_ok) {
      const double K = std::exp(k);
      double cold = 0.0;
      if (!rec.american_) {
        cold = black76_price(1.0, K, T, sigma, rec.df_, spec.side);
      } else if (cold_ok) {
        cold = cold_price_at(*work, ip, k, sigma, T, S1);
      } else {
        return;
      }
      const double got = rec.price(S1, K, sigma);
      const double err = std::abs(got - cold);
      worst = std::max(worst, std::isfinite(err) ? err : kInf);
      ++pts;
    };
    for (const double sc : s_check) {
      bool cold_ok = true;
      if (rec.american_) {
        cold_ok = std::isfinite(solve_edge(*work, ip, T, sc, S1, acc));
      }
      if (!cold_ok) {
        // No reference at this sigma: the grid would have a hole the
        // certificate cannot vouch for, so the record is refused below.
        ++refused_rows;
        continue;
      }
      const double edge = rec.exercise_edge(sc);
      // Continuation-side moneyness checks: the u-node midpoints of this sigma's
      // domain, clipped to the box, plus the two box ends.
      const double xs = detail::cheb_to_unit(sc, spec.sigma_min, spec.sigma_max);
      const double d = rec.has_edge_ ? detail::cheb_clenshaw1d(rec.domain_.data(), kNs, xs)
                                     : (put ? spec.k_max : spec.k_min);
      const double lo = put ? spec.k_min : d;
      const double hi = put ? d : spec.k_max;
      for (std::uint16_t i = 0; i + 1 < kNk; ++i) {
        const double u = 0.5 * (u_sorted[i] + u_sorted[i + 1]);
        const double k = detail::cheb_from_unit(u, lo, hi);
        if (k >= spec.k_min && k <= spec.k_max) {
          check(k, sc, cold_ok);
        }
      }
      check(spec.k_min, sc, cold_ok);
      check(spec.k_max, sc, cold_ok);
      // Exercise-side checks (intrinsic on both sides when the edge is right).
      if (rec.has_edge_) {
        const double a = put ? std::max(edge, spec.k_min) : spec.k_min;
        const double b = put ? spec.k_max : std::min(edge, spec.k_max);
        if (b > a) {
          for (int m = 1; m <= 4; ++m) {
            check(a + (b - a) * static_cast<double>(m) / 5.0, sc, cold_ok);
          }
        }
      }
    }
    rec.cert_err_ = (refused_rows == 0u) ? worst : kNaN;
    rec.cert_pts_ = pts;
    rec.cert_refused_rows_ = refused_rows;
  }
  return Ok(rec);
}

// ── queries ──────────────────────────────────────────────────────────────────

bool ExpiryRecord::contains(double k_log, double sigma) const noexcept {
  return populated_ && std::isfinite(k_log) && std::isfinite(sigma) && k_log >= k_min_ &&
         k_log <= k_max_ && sigma >= s_min_ && sigma <= s_max_;
}

double ExpiryRecord::exercise_edge(double sigma) const noexcept {
  if (!populated_) {
    return kNaN;
  }
  if (!american_) {
    return (side_ == Side::Put) ? kInf : -kInf;
  }
  const double s = atx::core::clamp(sigma, s_min_, s_max_);
  return detail::cheb_clenshaw1d(edge_.data(), kNs, detail::cheb_to_unit(s, s_min_, s_max_));
}

bool ExpiryRecord::in_exercise_region(double k_log, double sigma) const noexcept {
  if (!has_edge_) {
    return false;
  }
  const double e = exercise_edge(sigma);
  return (side_ == Side::Put) ? (k_log >= e) : (k_log <= e);
}

ExpiryRecord::Jet ExpiryRecord::eval_jet(double k_log, double sigma, bool want_k) const noexcept {
  Jet out;
  if (!american_) {
    return out;
  }
  const bool put = (side_ == Side::Put);
  const bool s_in = sigma >= s_min_ && sigma <= s_max_;
  const double s = atx::core::clamp(sigma, s_min_, s_max_);
  const double xs = detail::cheb_to_unit(s, s_min_, s_max_);
  const double dxs = 2.0 / (s_max_ - s_min_);
  ChebD1 dom{put ? k_max_ : k_min_, 0.0};
  if (has_edge_) {
    dom = clenshaw_d1(domain_.data(), kNs, xs);
  }
  const double lo = put ? k_min_ : dom.v;
  const double hi = put ? dom.v : k_max_;
  const double span = hi - lo;
  const double kc = atx::core::clamp(k_log, lo, hi);
  const bool k_in = (kc == k_log);
  const double u = (2.0 * kc - (lo + hi)) / span;

  std::array<double, kNs> v{};
  std::array<double, kNs> vu{};
  std::array<double, kNs> vuu{};
  if (want_k) {
    eval_rows<2>(coefs_.data(), u, v.data(), vu.data(), vuu.data());
  } else {
    eval_rows<1>(coefs_.data(), u, v.data(), vu.data(), nullptr);
  }
  const ChebD1 cs = clenshaw_d1(v.data(), kNs, xs);
  const double c_u = detail::cheb_clenshaw1d(vu.data(), kNs, xs);
  out.c = cs.v;
  if (!(out.c > 0.0)) {
    out.c = 0.0; // served max(0, polynomial): the clamped branch has zero slope
    return out;
  }
  // d u / d sigma at fixed k through the sigma-dependent domain endpoint.
  const double du_dsigma =
      has_edge_ ? (put ? -(u + 1.0) / span : -(1.0 - u) / span) * dom.d * dxs : 0.0;
  out.c_s = s_in ? (cs.d * dxs + (k_in ? c_u * du_dsigma : 0.0)) : 0.0;
  if (want_k && k_in) {
    const double a = 2.0 / span;
    out.c_k = c_u * a;
    out.c_kk = detail::cheb_clenshaw1d(vuu.data(), kNs, xs) * a * a;
  }
  return out;
}

double ExpiryRecord::premium_fwd(double k_log, double sigma) const noexcept {
  if (!populated_) {
    return kNaN;
  }
  if (!american_ || in_exercise_region(k_log, sigma)) {
    return 0.0;
  }
  const bool put = (side_ == Side::Put);
  const double s = atx::core::clamp(sigma, s_min_, s_max_);
  const double xs = detail::cheb_to_unit(s, s_min_, s_max_);
  const double d = has_edge_ ? detail::cheb_clenshaw1d(domain_.data(), kNs, xs)
                             : (put ? k_max_ : k_min_);
  const double lo = put ? k_min_ : d;
  const double hi = put ? d : k_max_;
  const double u = detail::cheb_to_unit(atx::core::clamp(k_log, lo, hi), lo, hi);
  std::array<double, kNs> v{};
  eval_rows<0>(coefs_.data(), u, v.data(), nullptr, nullptr);
  const double c = detail::cheb_clenshaw1d(v.data(), kNs, xs);
  return (c > 0.0) ? c : 0.0;
}

double ExpiryRecord::price(double S, double K, double sigma) const noexcept {
  if (!populated_ || !(S > 0.0) || !(K > 0.0) || !(sigma > 0.0)) {
    return kNaN;
  }
  const double F = S * growth_;
  const double k = std::log(K / F);
  const double intr = (side_ == Side::Put) ? (K - S) : (S - K);
  if (in_exercise_region(k, sigma)) {
    return (intr > 0.0) ? intr : 0.0;
  }
  const double euro = black76_price(F, K, T_, sigma, df_, side_);
  double px = euro + F * premium_fwd(k, sigma);
  if (intr > px) {
    px = intr;
  }
  if (euro > px) {
    px = euro;
  }
  return (px > 0.0) ? px : 0.0;
}

double ExpiryRecord::price_and_vega(double S, double K, double sigma,
                                    double *vega_out) const noexcept {
  if (!populated_ || !(S > 0.0) || !(K > 0.0) || !(sigma > 0.0)) {
    if (vega_out != nullptr) {
      *vega_out = kNaN;
    }
    return kNaN;
  }
  const double F = S * growth_;
  const double k = std::log(K / F);
  const double intr = (side_ == Side::Put) ? (K - S) : (S - K);
  return value_and_vega_at(F, K, k, intr, sigma, std::sqrt(T_), vega_out);
}

double ExpiryRecord::value_and_vega_at(double F, double K, double k, double intr, double sigma,
                                       double sqrt_t, double *vega_out) const noexcept {
  if (in_exercise_region(k, sigma)) {
    if (vega_out != nullptr) {
      *vega_out = 0.0;
    }
    return (intr > 0.0) ? intr : 0.0;
  }
  // sqrt_t == std::sqrt(T_), the value black76_value_and_vega computes itself
  // when it is not passed, so the result is bit-identical either way.
  const Black76ValueVega bv = black76_value_and_vega(F, K, T_, sigma, df_, side_, sqrt_t);
  const Jet j = eval_jet(k, sigma, /*want_k=*/false);
  const double cont = bv.price + F * j.c;
  double px = cont;
  if (intr > px) {
    px = intr;
  }
  if (bv.price > px) {
    px = bv.price;
  }
  if (vega_out != nullptr) {
    // A price floored at intrinsic is flat in sigma there: its vega is 0, the
    // same as greeks() reports for it.
    *vega_out = (intr > 0.0 && !(cont > intr)) ? 0.0 : bv.vega + F * j.c_s;
  }
  return (px > 0.0) ? px : 0.0;
}

RecordGreeks ExpiryRecord::greeks(double S, double K, double sigma) const noexcept {
  RecordGreeks g;
  if (!populated_ || !(S > 0.0) || !(K > 0.0) || !(sigma > 0.0)) {
    g.price = g.delta = g.gamma = g.vega = g.theta = kNaN;
    g.in_box = false;
    return g;
  }
  const bool put = (side_ == Side::Put);
  const double m = growth_;
  const double F = S * m;
  const double k = std::log(K / F);
  g.in_box = contains(k, sigma);
  const double intr = put ? (K - S) : (S - K);
  if (in_exercise_region(k, sigma)) {
    g.exercised = true;
    g.price = (intr > 0.0) ? intr : 0.0;
    g.delta = put ? -1.0 : 1.0;
    return g; // gamma = vega = theta = 0 (no time value in the exercise region)
  }
  const Black76Greeks bg = black76_greeks(F, K, T_, sigma, r_, df_, side_);
  const Jet j = eval_jet(k, sigma, /*want_k=*/true);
  const double cont = bg.price + F * j.c;
  if (intr > 0.0 && !(cont > intr)) {
    // The continuation value fell to intrinsic on the continuation side of the
    // recorded edge: serve intrinsic with the exercise Greeks, so the Greeks
    // belong to the served price (tick-path review nit 12).
    g.floored = true;
    g.exercised = true;
    g.price = intr;
    g.delta = put ? -1.0 : 1.0;
    return g; // gamma = vega = theta = 0
  }
  // c >= 0 (eval_jet's served max(0, polynomial)) keeps cont >= Black-76 >= 0, so
  // neither the European nor the zero floor can bind here.
  g.price = (cont > 0.0) ? cont : 0.0;
  const double D = bg.greeks.delta + j.c - j.c_k; // dP/dF
  g.delta = m * D;
  g.gamma = m * m * (bg.greeks.gamma + (j.c_kk - j.c_k) / F);
  g.vega = bg.greeks.vega + F * j.c_s;
  g.theta = r_ * g.price - (r_ - q_) * S * g.delta - 0.5 * sigma * sigma * S * S * g.gamma;
  return g;
}

// ── implied vol against the record (audit item 3) ────────────────────────────

RecordIv ExpiryRecord::implied_vol(double S, double K, double target_price, double sigma_seed,
                                   double price_tol, double vega_floor) const noexcept {
  RecordIv out;
  out.sigma = kNaN;
  if (!populated_ || !(S > 0.0) || !(K > 0.0) || !std::isfinite(target_price) ||
      !(price_tol > 0.0) || !(vega_floor >= 0.0)) {
    out.status = RecordIvStatus::Invalid;
    return out;
  }
  const double tol = price_tol * std::max(1.0, target_price);
  const double floor_v = vega_floor * K;
  const double intr_raw = (side_ == Side::Put) ? (K - S) : (S - K);
  const double intr = (intr_raw > 0.0) ? intr_raw : 0.0;
  // Hoisted out of the Newton loop: everything price_and_vega derives from (S, K)
  // alone (the forward, the log-moneyness and sqrt T), computed exactly as it
  // computes them, so each evaluation is bit-identical to price_and_vega(S, K,
  // sigma) without repeating the log and the square root (serve-path item 4).
  const double F = S * growth_;
  const double k_log = std::log(K / F);
  const double sqrt_t = std::sqrt(T_);
  if (target_price < intr - tol) {
    out.status = RecordIvStatus::BelowIntrinsic;
    return out;
  }
  // An in-the-money target AT intrinsic is reproduced by every sigma up to the
  // exercise edge (the price is flat there), so it identifies no vol at all.
  if (intr > 0.0 && target_price <= intr + tol) {
    out.status = RecordIvStatus::VegaFloor;
    return out;
  }
  // Price is non-decreasing in sigma, so [lo, hi] always brackets the root once
  // each evaluation moves the matching end; the box ends are tested only when
  // Newton aims past them.
  double lo = s_min_;
  double hi = s_max_;
  double sig = (std::isfinite(sigma_seed) && sigma_seed > 0.0)
                   ? atx::core::clamp(sigma_seed, s_min_, s_max_)
                   : 0.5 * (s_min_ + s_max_);
  double last_vega = 0.0;
  constexpr int kMaxEvaluations = 16;
  for (int it = 0; it < kMaxEvaluations; ++it) {
    double vega = 0.0;
    const double p = value_and_vega_at(F, K, k_log, intr_raw, sig, sqrt_t, &vega);
    out.evaluations = static_cast<std::uint8_t>(it + 1);
    last_vega = vega;
    const double f = p - target_price;
    if (std::abs(f) <= tol) {
      // A match where the price is flat in sigma (exercise region, deep ITM)
      // identifies no vol: report it rather than return the seed as an answer.
      out.sigma = sig;
      out.status = (vega > floor_v) ? RecordIvStatus::Ok : RecordIvStatus::VegaFloor;
      return out;
    }
    if (sig == s_min_ && f > 0.0) {
      out.sigma = s_min_;
      out.status = RecordIvStatus::BelowBox;
      return out;
    }
    if (sig == s_max_ && f < 0.0) {
      out.sigma = s_max_;
      out.status = RecordIvStatus::AboveBox;
      return out;
    }
    if (f > 0.0) {
      hi = sig;
    } else {
      lo = sig;
    }
    double next = 0.5 * (lo + hi);
    if (vega > floor_v) {
      const double newton = sig - f / vega;
      if (newton > lo && newton < hi) {
        next = newton;
      } else if (newton >= hi && hi == s_max_) {
        next = s_max_; // test the box end itself: AboveBox if still short
      } else if (newton <= lo && lo == s_min_) {
        next = s_min_;
      }
    }
    if (!(hi - lo > 1.0e-14 * s_max_) || next == sig) {
      break;
    }
    sig = next;
  }
  out.sigma = sig;
  out.status = (last_vega > floor_v) ? RecordIvStatus::NoConvergence : RecordIvStatus::VegaFloor;
  return out;
}

// ── bank ─────────────────────────────────────────────────────────────────────

ExpiryRecordBank::ExpiryRecordBank(std::size_t capacity) : capacity_(capacity) {
  records_.reserve(capacity);
}

Result<std::size_t> ExpiryRecordBank::insert(const ExpiryRecord &record) {
  if (!record.populated()) {
    return Err(ErrorCode::InvalidArgument, "ExpiryRecordBank::insert: unpopulated record");
  }
  if (records_.size() >= capacity_) {
    return Err(ErrorCode::OutOfRange, "ExpiryRecordBank::insert: bank is at capacity");
  }
  records_.push_back(record);
  return Ok(records_.size() - 1u);
}

const ExpiryRecord *ExpiryRecordBank::at(std::size_t i) const noexcept {
  return (i < records_.size()) ? &records_[i] : nullptr;
}

const ExpiryRecord *ExpiryRecordBank::find(double T, Side side) const noexcept {
  for (const ExpiryRecord &r : records_) {
    if (r.expiry() == T && r.side() == side) {
      return &r;
    }
  }
  return nullptr;
}

} // namespace atx::vol
