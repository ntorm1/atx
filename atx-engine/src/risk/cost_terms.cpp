// atx::engine::risk — cost_terms out-of-line definitions (Lane 6 trade-cost augmentation).
#include "atx/engine/risk/cost_terms.hpp"

#include <cmath>   // std::isfinite, std::isinf, std::sqrt, std::pow, std::fabs
#include <string>  // std::string (Err messages)
#include <utility> // std::move
#include <vector>  // std::vector (triplets, per-name index lists)

#include <Eigen/SparseCore>

#include "atx/core/linalg/linalg.hpp" // VecX

namespace atx::engine::risk {
namespace {

namespace co = atx::core;
namespace cl = atx::core::linalg;
using Trip = Eigen::Triplet<atx::f64>;
using SpMat = Eigen::SparseMatrix<atx::f64>;

// The rotated-cone constant for s² ≤ u/4 written as 2·u·b ≥ s² (b = 1/8).
constexpr atx::f64 kRotB = 0.125;

[[nodiscard]] bool positive_at(std::span<const atx::f64> v, atx::usize i) noexcept {
  return !v.empty() && v[i] > 0.0;
}

[[nodiscard]] bool finite_cap_at(std::span<const atx::f64> v, atx::usize i) noexcept {
  return !v.empty() && !std::isinf(v[i]);
}

[[nodiscard]] atx::f64 prev_at(std::span<const atx::f64> w_prev, atx::usize i) noexcept {
  return w_prev.empty() ? 0.0 : w_prev[i];
}

[[nodiscard]] co::Status check_span(std::span<const atx::f64> v, atx::usize m, const char *name,
                                    bool allow_neg, bool allow_pos_inf) {
  if (v.empty()) {
    return co::Ok();
  }
  if (v.size() != m) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   std::string("TradeCostTerms: ") + name + " length must equal M");
  }
  for (const atx::f64 x : v) {
    const bool ok_inf = allow_pos_inf && std::isinf(x) && x > 0.0;
    if (!ok_inf && !std::isfinite(x)) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     std::string("TradeCostTerms: ") + name + " must be finite");
    }
    if (!allow_neg && x < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     std::string("TradeCostTerms: ") + name + " must be >= 0");
    }
  }
  return co::Ok();
}

// Copy every stored entry of a CSC matrix into triplets (column-major traversal, R1).
void append_triplets(const SpMat &a, std::vector<Trip> &out) {
  for (int c = 0; c < a.outerSize(); ++c) {
    for (SpMat::InnerIterator it(a, c); it; ++it) {
      out.emplace_back(it.row(), c, it.value());
    }
  }
}

// Growing row/bound builder for the appended block.
struct RowSink {
  std::vector<Trip> trips;
  std::vector<atx::f64> lo;
  std::vector<atx::f64> hi;
  atx::usize base_rows = 0;

  [[nodiscard]] int next() const noexcept { return static_cast<int>(base_rows + lo.size()); }
  void close(atx::f64 l, atx::f64 u) {
    lo.push_back(l);
    hi.push_back(u);
  }
};

// u ≥ |w_i − w0| as two rows:  w_i − u ≤ w0  and  −w_i − u ≤ −w0.
void emit_abs_rows(RowSink &rs, atx::usize w_col, atx::usize u_col, atx::f64 w0) {
  const int wc = static_cast<int>(w_col);
  const int uc = static_cast<int>(u_col);
  rs.trips.emplace_back(rs.next(), wc, 1.0);
  rs.trips.emplace_back(rs.next(), uc, -1.0);
  rs.close(-kAugInf, w0);
  rs.trips.emplace_back(rs.next(), wc, -1.0);
  rs.trips.emplace_back(rs.next(), uc, -1.0);
  rs.close(-kAugInf, -w0);
}

// Rotated cone 2·a·b ≥ c² as the variable-apex SOC ‖(c, (a−b)/√2)‖ ≤ (a+b)/√2, where
// a/b/c are each (column, constant) affine terms. Emits 3 contiguous rows (apex first).
struct Affine {
  int col = -1;          // < 0 ⇒ pure constant
  atx::f64 coef = 0.0;   // multiplier on the column
  atx::f64 constant = 0.0;
};

void emit_rotated_cone(RowSink &rs, std::vector<SocBlock> &cones, Affine a, Affine b, Affine c) {
  const atx::f64 inv_sqrt2 = 1.0 / std::sqrt(2.0);
  SocBlock blk;
  blk.row_start = static_cast<atx::usize>(rs.next());
  blk.dim = 3U;
  blk.radius = 0.0;
  blk.variable_apex = true;
  blk.offset = cl::VecX::Zero(3);
  // Row j of the block = sign_a·a/√2 + sign_b·b/√2 (or c for the middle row).
  const auto emit = [&](Eigen::Index j, atx::f64 sa, atx::f64 sb, bool is_c) {
    const int row = rs.next();
    atx::f64 off = 0.0;
    if (is_c) {
      if (c.col >= 0) {
        rs.trips.emplace_back(row, c.col, c.coef);
      }
      off = c.constant;
    } else {
      // a and b never share a column in our cones, so no duplicate triplets arise.
      if (a.col >= 0) {
        rs.trips.emplace_back(row, a.col, sa * a.coef * inv_sqrt2);
      }
      if (b.col >= 0) {
        rs.trips.emplace_back(row, b.col, sb * b.coef * inv_sqrt2);
      }
      off = (sa * a.constant + sb * b.constant) * inv_sqrt2;
    }
    blk.offset[j] = off;
    rs.close(-kAugInf, kAugInf); // inert box band — the SOC projection owns the row
  };
  emit(0, 1.0, 1.0, false);  // apex (a+b)/√2
  emit(1, 0.0, 0.0, true);   // c
  emit(2, 1.0, -1.0, false); // (a−b)/√2
  cones.push_back(std::move(blk));
}

} // namespace

bool TradeCostTerms::active() const noexcept {
  const auto any_pos = [](std::span<const atx::f64> v) {
    for (const atx::f64 x : v) {
      if (x > 0.0) {
        return true;
      }
    }
    return false;
  };
  bool finite_cap = false;
  for (const atx::f64 x : locate_cap) {
    finite_cap = finite_cap || !std::isinf(x);
  }
  return any_pos(kappa_lin) || any_pos(c_three_halves) || any_pos(borrow_fee) || finite_cap;
}

atx::core::Status validate_cost_terms(const TradeCostTerms &terms, atx::usize m) {
  ATX_TRY_VOID(check_span(terms.kappa_lin, m, "kappa_lin", false, false));
  ATX_TRY_VOID(check_span(terms.c_three_halves, m, "c_three_halves", false, false));
  ATX_TRY_VOID(check_span(terms.borrow_fee, m, "borrow_fee", false, false));
  ATX_TRY_VOID(check_span(terms.locate_cap, m, "locate_cap", false, true));
  ATX_TRY_VOID(check_span(terms.w_prev, m, "w_prev", true, false));
  return co::Ok();
}

atx::core::Result<AugmentedQp> append_cost_terms(AugmentedQp base, const TradeCostTerms &terms) {
  const atx::usize m = base.n_w;
  ATX_TRY_VOID(validate_cost_terms(terms, m));
  if (!terms.active()) {
    return co::Ok(std::move(base)); // R10: verbatim base ⇒ byte-identical downstream solve
  }

  const atx::usize n0 = base.n_w + base.n_y + base.n_aux;
  const auto r0 = static_cast<atx::usize>(base.A_tilde.rows());

  // Column plan (ascending name order within each block): κ | (u,s,τ) | b.
  std::vector<atx::usize> kappa_names;
  std::vector<atx::usize> impact_names;
  std::vector<atx::usize> short_names;
  for (atx::usize i = 0; i < m; ++i) {
    if (positive_at(terms.kappa_lin, i)) {
      kappa_names.push_back(i);
    }
    if (positive_at(terms.c_three_halves, i)) {
      impact_names.push_back(i);
    }
    if (positive_at(terms.borrow_fee, i) || finite_cap_at(terms.locate_cap, i)) {
      short_names.push_back(i);
    }
  }
  const atx::usize kappa_off = n0;
  const atx::usize impact_off = kappa_off + kappa_names.size();
  const atx::usize short_off = impact_off + 3U * impact_names.size();
  const atx::usize n1 = short_off + short_names.size();

  RowSink rs;
  rs.base_rows = r0;
  std::vector<SocBlock> new_cones;
  cl::VecX q = cl::VecX::Zero(static_cast<Eigen::Index>(n1));
  q.head(static_cast<Eigen::Index>(n0)) = base.q_aug;

  for (atx::usize k = 0; k < kappa_names.size(); ++k) { // (1) linear cost
    const atx::usize i = kappa_names[k];
    emit_abs_rows(rs, i, kappa_off + k, prev_at(terms.w_prev, i));
    q[static_cast<Eigen::Index>(kappa_off + k)] = terms.kappa_lin[i];
  }
  for (atx::usize k = 0; k < impact_names.size(); ++k) { // (2) |Δ| rows for the impact
    const atx::usize i = impact_names[k];
    const atx::usize u_col = impact_off + 3U * k;
    emit_abs_rows(rs, i, u_col, prev_at(terms.w_prev, i));
    q[static_cast<Eigen::Index>(u_col + 2U)] = terms.c_three_halves[i];
  }
  for (atx::usize k = 0; k < short_names.size(); ++k) { // (3) borrow split + locate box
    const atx::usize i = short_names[k];
    const int b_col = static_cast<int>(short_off + k);
    rs.trips.emplace_back(rs.next(), static_cast<int>(i), 1.0);
    rs.trips.emplace_back(rs.next(), b_col, 1.0);
    rs.close(0.0, kAugInf); // w_i + b_i ≥ 0
    const atx::f64 cap = finite_cap_at(terms.locate_cap, i) ? terms.locate_cap[i] : kAugInf;
    rs.trips.emplace_back(rs.next(), b_col, 1.0);
    rs.close(0.0, cap); // 0 ≤ b_i ≤ cap_i
    q[static_cast<Eigen::Index>(b_col)] =
        terms.borrow_fee.empty() ? 0.0 : terms.borrow_fee[i];
  }
  for (atx::usize k = 0; k < impact_names.size(); ++k) { // (2) the two rotated cones
    const int u_col = static_cast<int>(impact_off + 3U * k);
    const Affine u{u_col, 1.0, 0.0};
    const Affine s{u_col + 1, 1.0, 0.0};
    const Affine tau{u_col + 2, 1.0, 0.0};
    emit_rotated_cone(rs, new_cones, s, tau, u);                  // 2 s τ ≥ u²
    emit_rotated_cone(rs, new_cones, u, Affine{-1, 0.0, kRotB}, s); // 2 u (1/8) ≥ s²
  }

  const atx::usize r1 = r0 + rs.lo.size();
  AugmentedQp out;
  out.n_w = base.n_w;
  out.n_y = base.n_y;
  out.n_aux = base.n_aux + (n1 - n0);

  // P: base block, zero on the new columns (costs are linear in the epigraph variables).
  std::vector<Trip> p_trips;
  p_trips.reserve(static_cast<atx::usize>(base.P.nonZeros()));
  append_triplets(base.P, p_trips);
  out.P.resize(static_cast<int>(n1), static_cast<int>(n1));
  out.P.setFromTriplets(p_trips.begin(), p_trips.end());
  out.P.makeCompressed();

  std::vector<Trip> a_trips;
  a_trips.reserve(static_cast<atx::usize>(base.A_tilde.nonZeros()) + rs.trips.size());
  append_triplets(base.A_tilde, a_trips);
  a_trips.insert(a_trips.end(), rs.trips.begin(), rs.trips.end());
  out.A_tilde.resize(static_cast<int>(r1), static_cast<int>(n1));
  out.A_tilde.setFromTriplets(a_trips.begin(), a_trips.end());
  out.A_tilde.makeCompressed();

  out.q_aug = std::move(q);
  out.l = cl::VecX(static_cast<Eigen::Index>(r1));
  out.u = cl::VecX(static_cast<Eigen::Index>(r1));
  out.l.head(static_cast<Eigen::Index>(r0)) = base.l;
  out.u.head(static_cast<Eigen::Index>(r0)) = base.u;
  for (atx::usize j = 0; j < rs.lo.size(); ++j) {
    out.l[static_cast<Eigen::Index>(r0 + j)] = rs.lo[j];
    out.u[static_cast<Eigen::Index>(r0 + j)] = rs.hi[j];
  }
  out.cones = std::move(base.cones);
  for (SocBlock &blk : new_cones) {
    out.cones.push_back(std::move(blk));
  }
  return co::Ok(std::move(out));
}

atx::core::Result<TradeCostBreakdown> evaluate_trade_costs(const TradeCostTerms &terms,
                                                           std::span<const atx::f64> w) {
  const atx::usize m = w.size();
  ATX_TRY_VOID(validate_cost_terms(terms, m));
  TradeCostBreakdown out;
  for (atx::usize i = 0; i < m; ++i) {
    const atx::f64 dz = std::fabs(w[i] - prev_at(terms.w_prev, i));
    if (!terms.kappa_lin.empty()) {
      out.linear += terms.kappa_lin[i] * dz;
    }
    if (!terms.c_three_halves.empty() && terms.c_three_halves[i] > 0.0) {
      out.impact += terms.c_three_halves[i] * dz * std::sqrt(dz);
    }
    if (!terms.borrow_fee.empty() && w[i] < 0.0) {
      out.borrow += terms.borrow_fee[i] * (-w[i]);
    }
  }
  return co::Ok(out);
}

atx::core::Result<QpResult> solve_with_costs(const ConstrainedQpSolver &solver,
                                             const QpProblem &p, const TradeCostTerms &terms) {
  ATX_TRY_VOID(ConstrainedQpSolver::check_problem(p));
  ATX_TRY_VOID(validate_cost_terms(terms, p.V.n_instruments()));
  AugmentedQp base = build_augmented(p.V, p.risk_aversion, p.q, p.C);
  ATX_TRY(AugmentedQp aug, append_cost_terms(std::move(base), terms));
  return solver.solve_augmented_form(aug, p);
}

} // namespace atx::engine::risk
