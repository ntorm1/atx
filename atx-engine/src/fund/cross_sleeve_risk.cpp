// cross_sleeve_risk.cpp — P2-S2-3 private implementation (USER DIRECTIVE: the unit's
// numeric kernels live in the .cpp, not the header; .agents/cpp §6).
//
// Defines fund_risk + sleeve_return_cov, plus the anonymous-namespace helpers the
// algorithm decomposes into: net_book (W = Σ c_s w_s), dot_diag (WᵀDW), agg_exposure
// (Xᵀ W), quad_form (cᵀΩc via Ωc), and euler_component_risk (RC_s over Ω). All
// reductions are ascending-index order-fixed; no RNG, no clock, no unordered containers
// (R1). sleeve_return_cov consumes only its input window — structurally PIT-safe (R2).

#include "atx/engine/fund/cross_sleeve_risk.hpp"

#include <algorithm> // std::max (factor_var FP-cancellation floor)
#include <cmath>     // std::sqrt, std::isnan
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include <Eigen/Dense> // Eigen::Index

#include "atx/engine/combine/correlation.hpp" // combine::pairwise_complete_corr (shared NaN policy)

namespace atx::engine::fund {

namespace {

// ---------------------------------------------------------------------------
//  net_book — W[i] = Σ_s c[s]·sleeve_books[s][i] (ascending sleeve then name, R1).
//
//  Order-fixed: the OUTER loop is the sleeve (ascending s), the inner is the name
//  (ascending i), so the accumulation order is fixed for byte-reproducibility. Length M.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<atx::f64>
net_book(std::span<const std::span<const atx::f64>> sleeve_books, std::span<const atx::f64> c,
         atx::usize m) {
  std::vector<atx::f64> w(m, 0.0);
  for (atx::usize s = 0U; s < c.size(); ++s) {
    const std::span<const atx::f64> ws = sleeve_books[s];
    const atx::f64 cs = c[s];
    for (atx::usize i = 0U; i < m; ++i) {
      w[i] += cs * ws[i];
    }
  }
  return w;
}

// ---------------------------------------------------------------------------
//  dot_diag — WᵀDW = Σ_i D_i·W_i² (the specific variance; D the floored diagonal).
//
//  Order-fixed ascending i. D is FactorModel::specific_var() (floored > 0); this is the
//  SAME Σ D_i w_i² the model's risk() accumulates, recovered here so factor_var can be
//  formed by subtraction WITHOUT a dense V (§0.4).
// ---------------------------------------------------------------------------
[[nodiscard]] atx::f64 dot_diag(const atx::core::linalg::VecX& d, const std::vector<atx::f64>& w) {
  atx::f64 acc = 0.0;
  for (atx::usize i = 0U; i < w.size(); ++i) {
    const atx::f64 wi = w[i];
    acc += d[static_cast<Eigen::Index>(i)] * wi * wi;
  }
  return acc;
}

// ---------------------------------------------------------------------------
//  agg_exposure — b_fund = Xᵀ W : b[k] = Σ_i X(i,k)·W[i] (length K, order-fixed).
//
//  Equivalently Σ_s c_s Xᵀw_s, but XᵀW (over the already-netted W) is cheaper and
//  identical. The outer loop is the factor k, the inner the name i (ascending both).
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<atx::f64> agg_exposure(const atx::core::linalg::MatX& x,
                                                 const std::vector<atx::f64>& w) {
  const auto k = static_cast<atx::usize>(x.cols());
  std::vector<atx::f64> b(k, 0.0);
  for (atx::usize col = 0U; col < k; ++col) {
    atx::f64 acc = 0.0;
    for (atx::usize i = 0U; i < w.size(); ++i) {
      acc += x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(col)) * w[i];
    }
    b[col] = acc;
  }
  return b;
}

// ---------------------------------------------------------------------------
//  omega_times_c — (Ωc)_s = Σ_t Omega(s,t)·c[t] (length S, order-fixed ascending t).
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<atx::f64> omega_times_c(const atx::core::linalg::MatX& omega,
                                                  std::span<const atx::f64> c) {
  const atx::usize s = c.size();
  std::vector<atx::f64> oc(s, 0.0);
  for (atx::usize i = 0U; i < s; ++i) {
    atx::f64 acc = 0.0;
    for (atx::usize t = 0U; t < s; ++t) {
      acc += omega(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(t)) * c[t];
    }
    oc[i] = acc;
  }
  return oc;
}

// ---------------------------------------------------------------------------
//  quad_form — cᵀΩc = Σ_s c[s]·(Ωc)_s (the Ω-based sleeve-portfolio variance).
//
//  Takes the precomputed (Ωc) so the Euler RC reuses it (Ωc is needed per-component).
//  Order-fixed ascending s.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::f64 quad_form(std::span<const atx::f64> c, const std::vector<atx::f64>& oc) {
  atx::f64 q = 0.0;
  for (atx::usize s = 0U; s < c.size(); ++s) {
    q += c[s] * oc[s];
  }
  return q;
}

// ---------------------------------------------------------------------------
//  euler_component_risk — RC_s = c[s]·(Ωc)_s / sigma_sleeve OVER Ω (R4).
//
//  sigma_sleeve = sqrt(max(cᵀΩc, 0)) is the Ω-based sleeve-portfolio vol; the Euler
//  identity Σ_s RC_s = (cᵀΩc)/sqrt(cᵀΩc) = sigma_sleeve holds EXACTLY only with THIS
//  divisor (NOT the V-based sigma_fund). A non-positive cᵀΩc (all-zero c / degenerate)
//  ⇒ sigma_sleeve = 0 ⇒ the guard returns every RC_s = 0 (no div-by-zero). Length S.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<atx::f64> euler_component_risk(std::span<const atx::f64> c,
                                                         const std::vector<atx::f64>& oc,
                                                         atx::f64 cqc) {
  const atx::usize s = c.size();
  std::vector<atx::f64> rc(s, 0.0);
  const atx::f64 sigma_sleeve = (cqc > 0.0) ? std::sqrt(cqc) : 0.0;
  if (!(sigma_sleeve > 0.0)) {
    return rc; // all-zero c / degenerate Ω ⇒ sigma_sleeve = 0 ⇒ all RC 0 (guarded)
  }
  for (atx::usize i = 0U; i < s; ++i) {
    rc[i] = c[i] * oc[i] / sigma_sleeve;
  }
  return rc;
}

// ---------------------------------------------------------------------------
//  pop_variance_nan — population variance of a series over its NON-NaN entries
//  (order-fixed two-pass mean then sum-of-squares). <2 valid samples ⇒ 0.0.
//
//  Mirrors risk/factor_model.hpp detail::pop_variance but adds the listwise NaN drop
//  so an all-NaN (or thin) sleeve P&L yields σ²=0 — a finite, zero row/col of Ω. This
//  is the diagonal Ω(s,s); it is consistent with corr(s,s)=1 (Ω(s,s)=corr·σ·σ=σ²).
// ---------------------------------------------------------------------------
[[nodiscard]] atx::f64 pop_variance_nan(std::span<const atx::f64> xs) {
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (const atx::f64 v : xs) {
    if (!std::isnan(v)) {
      sum += v;
      ++n;
    }
  }
  if (n < 2U) {
    return 0.0;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (const atx::f64 v : xs) {
    if (!std::isnan(v)) {
      const atx::f64 dv = v - mean;
      ss += dv * dv;
    }
  }
  return ss / static_cast<atx::f64>(n);
}

} // namespace

// ---------------------------------------------------------------------------
//  fund_risk — the cross-sleeve risk report (the public entry point).
// ---------------------------------------------------------------------------
atx::core::Result<FundRisk> fund_risk(std::span<const std::span<const atx::f64>> sleeve_books,
                                      std::span<const atx::f64> c, const risk::FactorModel& V,
                                      const atx::core::linalg::MatX& Omega) {
  const atx::usize s = c.size();
  const atx::usize m = V.n_instruments();

  // --- boundary validation (shape) ---
  if (sleeve_books.size() != s) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fund_risk: sleeve_books.size() must equal c.size() (S)");
  }
  for (atx::usize i = 0U; i < s; ++i) {
    if (sleeve_books[i].size() != m) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "fund_risk: each sleeve book length must equal M (V.n_instruments())");
    }
  }
  if (static_cast<atx::usize>(Omega.rows()) != s ||
      static_cast<atx::usize>(Omega.cols()) != s) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fund_risk: Omega must be S×S (S == c.size())");
  }

  // --- net the fund book W = Σ_s c_s w_s (order-fixed; length M) ---
  const std::vector<atx::f64> w = net_book(sleeve_books, c, m);

  FundRisk out;

  // --- V-based BOOK risk: var_total = WᵀVW (factored, NEVER densified) ---
  const atx::f64 var_total = V.risk(std::span<const atx::f64>(w));
  // factor / specific split via the identity factor_var = risk(W) − WᵀDW (§0.4).
  out.specific_var = dot_diag(V.specific_var(), w); // WᵀDW
  // = b_fundᵀ F b_fund ≥ 0 (no F accessor). Floor at 0: when W is nearly in the
  // specific-only subspace the subtraction can yield a tiny-negative FP artifact,
  // which would NaN a downstream sqrt(factor_var) in the S2-5 report.
  out.factor_var = std::max(var_total - out.specific_var, 0.0);
  out.sigma_fund = (var_total > 0.0) ? std::sqrt(var_total) : 0.0;

  // --- aggregate factor exposure b_fund = Xᵀ W (length K) ---
  out.b_fund = agg_exposure(V.exposures(), w);

  // --- per-sleeve Euler component risk OVER Ω (R4): RC_s = c_s·(Ωc)_s / sqrt(cᵀΩc) ---
  const std::vector<atx::f64> oc = omega_times_c(Omega, c);
  const atx::f64 cqc = quad_form(c, oc);
  out.risk_contrib = euler_component_risk(c, oc, cqc);

  return atx::core::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
//  sleeve_return_cov — Ω (S×S) from trailing per-sleeve P&L.
//
//  Ω(s,s) = σ_s² (pop-variance over non-NaN entries); Ω(s,t) = corr·σ_s·σ_t for s≠t,
//  REUSING combine::pairwise_complete_corr (no pairwise_complete_cov exists). Symmetric
//  (compute s≤t, mirror); ascending s,t (R1). PURE function of its input window (R2).
// ---------------------------------------------------------------------------
atx::core::Result<atx::core::linalg::MatX>
sleeve_return_cov(std::span<const std::span<const atx::f64>> sleeve_pnl) {
  const atx::usize s = sleeve_pnl.size();
  if (s == 0U) {
    return atx::core::Ok(atx::core::linalg::MatX(0, 0)); // empty panel ⇒ 0×0 (§0.8)
  }

  // All series MUST be the same length — pairwise_complete_corr asserts equal length, so
  // validate at the boundary here to fail with a Result (not a debug assert) on misuse.
  const atx::usize len = sleeve_pnl[0].size();
  for (atx::usize i = 1U; i < s; ++i) {
    if (sleeve_pnl[i].size() != len) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "sleeve_return_cov: all P&L series must be the same length");
    }
  }

  // Per-sleeve σ_s = sqrt(pop-variance over non-NaN entries). A degenerate (all-NaN /
  // <2-sample) sleeve ⇒ σ_s = 0 ⇒ a zero, finite row/col.
  std::vector<atx::f64> sigma(s, 0.0);
  for (atx::usize i = 0U; i < s; ++i) {
    sigma[i] = std::sqrt(pop_variance_nan(sleeve_pnl[i]));
  }

  atx::core::linalg::MatX omega(static_cast<Eigen::Index>(s), static_cast<Eigen::Index>(s));
  for (atx::usize i = 0U; i < s; ++i) {
    // Diagonal = σ_i² (= corr(i,i)·σ_i·σ_i since corr(i,i)=1 exactly). Order-fixed.
    omega(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = sigma[i] * sigma[i];
    for (atx::usize j = i + 1U; j < s; ++j) {
      // Off-diagonal Ω(i,j) = corr·σ_i·σ_j (no pairwise_complete_cov — build from corr).
      const atx::f64 rho = combine::pairwise_complete_corr(sleeve_pnl[i], sleeve_pnl[j]);
      const atx::f64 cov = rho * sigma[i] * sigma[j];
      omega(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = cov;
      omega(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = cov; // symmetric mirror
    }
  }
  return atx::core::Ok(std::move(omega));
}

} // namespace atx::engine::fund
