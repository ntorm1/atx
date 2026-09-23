#pragma once

// atx::engine::risk — discretize: the deterministic POST-SOLVE pass that turns a continuous
// optimizer book into a tradable one (Lane 6). Three rules, applied in a fixed order:
//
//   (1) MAX-NAMES  — keep at most `max_names` held names: the largest |w_i| of the
//       continuous book (ties → lower index); every other name is pinned at 0.
//   (2) MIN-TRADE  — a kept name whose trade |w_i − w0_i| is below `min_trade` is pinned at
//       its previous weight w0_i (no trade).
//   (3) RE-SOLVE   — when (1)/(2) pinned anything, the QP is solved again with each pinned
//       name as an equality row w_i = target_i, so the free names re-optimize around the
//       pins (risk, neutrality and budgets are re-balanced instead of silently broken).
//   (4) ROUND LOTS — each free name's trade is rounded to the nearest multiple of its lot
//       (weight units); a rounded trade below `min_trade` is dropped.
//
//  The pass is heuristic (cardinality is non-convex) but deterministic: a pure function of
//  (problem, w0, continuous book, cfg), one re-solve at most, no RNG. Lot rounding can move
//  a linear constraint by up to Σ lot/2, so the result carries the realized worst violation
//  of the ORIGINAL constraint set for the caller to gate on.

#include <algorithm> // std::stable_sort, std::max
#include <cmath>     // std::fabs, std::round, std::isfinite
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err
#include "atx/core/types.hpp" // f64, u8, u32, usize

#include "atx/engine/risk/constraints.hpp" // MaterializedConstraints
#include "atx/engine/risk/qp_solver.hpp"   // ConstrainedQpSolver, QpProblem

namespace atx::engine::risk {

struct DiscretizeCfg {
  atx::f64 min_trade = 0.0;       // |Δw| below this ⇒ no trade (weight units); 0 ⇒ off
  std::span<const atx::f64> lot;  // per-name lot size in weight units; empty / ≤ 0 ⇒ continuous
  atx::u32 max_names = 0;         // cap on held names; 0 ⇒ no cap
  atx::f64 hold_eps = 1e-10;      // |w| ≤ this counts as "not held"
};

struct DiscretizedBook {
  std::vector<atx::f64> book;   // length M
  std::vector<atx::u8> pinned;  // 1 ⇒ pinned by rule (1) or (2)
  atx::usize n_names = 0;       // |w_i| > hold_eps
  atx::usize n_trades = 0;      // w_i != w0_i
  atx::f64 max_violation = 0.0; // worst violation of the original rows / L1 budgets (≥ 0)
  bool resolved = false;        // rule (3) ran
};

namespace detail {

// Worst violation of `w` against the linear rows and the gross / turnover L1 budgets.
[[nodiscard]] inline atx::f64 book_violation(const MaterializedConstraints &c,
                                             std::span<const atx::f64> w) {
  atx::f64 worst = 0.0;
  for (Eigen::Index r = 0; r < c.A.rows(); ++r) {
    atx::f64 a = 0.0;
    for (Eigen::Index j = 0; j < c.A.cols(); ++j) {
      a += c.A(r, j) * w[static_cast<atx::usize>(j)];
    }
    worst = std::max(worst, std::max(a - c.u[r], c.l[r] - a));
  }
  if (c.gross_l1_budget >= 0.0) {
    atx::f64 g = 0.0;
    for (const atx::f64 v : w) {
      g += std::fabs(v);
    }
    worst = std::max(worst, g - c.gross_l1_budget);
  }
  if (c.has_turnover && c.turnover_ref.size() == w.size()) {
    atx::f64 t = 0.0;
    for (atx::usize i = 0; i < w.size(); ++i) {
      t += std::fabs(w[i] - c.turnover_ref[i]);
    }
    worst = std::max(worst, t - c.turnover_budget);
  }
  return worst;
}

// Copy of `c` with one equality row w_i = target_i appended per pinned name.
[[nodiscard]] inline MaterializedConstraints with_pins(const MaterializedConstraints &c,
                                                       std::span<const atx::u8> pinned,
                                                       std::span<const atx::f64> target) {
  const auto m = static_cast<Eigen::Index>(pinned.size());
  Eigen::Index n_pins = 0;
  for (const atx::u8 p : pinned) {
    n_pins += (p != 0U) ? 1 : 0;
  }
  MaterializedConstraints out = c;
  const Eigen::Index r0 = c.A.rows();
  out.A = atx::core::linalg::MatX::Zero(r0 + n_pins, m);
  out.l = atx::core::linalg::VecX::Zero(r0 + n_pins);
  out.u = atx::core::linalg::VecX::Zero(r0 + n_pins);
  if (r0 > 0) {
    out.A.topRows(r0) = c.A;
    out.l.head(r0) = c.l;
    out.u.head(r0) = c.u;
  }
  Eigen::Index row = r0;
  for (Eigen::Index i = 0; i < m; ++i) {
    if (pinned[static_cast<atx::usize>(i)] != 0U) {
      out.A(row, i) = 1.0;
      out.l[row] = target[static_cast<atx::usize>(i)];
      out.u[row] = target[static_cast<atx::usize>(i)];
      ++row;
    }
  }
  return out;
}

} // namespace detail

// Run the discretization pass. `w_prev` and `w_cont` are length M (= p.V.n_instruments()).
// Err(InvalidArgument) on a length / finiteness mismatch; a failing re-solve (the pins made
// the set infeasible for the solver's budget) propagates the solver's Err unchanged.
[[nodiscard]] inline atx::core::Result<DiscretizedBook>
discretize_and_resolve(const ConstrainedQpSolver &solver, const QpProblem &p,
                       std::span<const atx::f64> w_prev, std::span<const atx::f64> w_cont,
                       const DiscretizeCfg &cfg) {
  namespace co = atx::core;
  const atx::usize m = p.V.n_instruments();
  if (w_prev.size() != m || w_cont.size() != m) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "discretize_and_resolve: w_prev and w_cont must have length M");
  }
  if (!cfg.lot.empty() && cfg.lot.size() != m) {
    return co::Err(co::ErrorCode::InvalidArgument, "discretize_and_resolve: lot must be M");
  }
  if (!std::isfinite(cfg.min_trade) || cfg.min_trade < 0.0) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "discretize_and_resolve: min_trade must be finite and >= 0");
  }
  for (atx::usize i = 0; i < m; ++i) {
    if (!std::isfinite(w_prev[i]) || !std::isfinite(w_cont[i])) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "discretize_and_resolve: books must be finite");
    }
  }

  DiscretizedBook out;
  out.book.assign(w_cont.begin(), w_cont.end());
  out.pinned.assign(m, 0U);
  std::vector<atx::f64> target(m, 0.0);

  // (1) max-names: rank held names by |w| descending, ties by index (stable sort).
  std::vector<atx::u8> kept(m, 1U);
  if (cfg.max_names > 0U) {
    std::vector<atx::usize> held;
    for (atx::usize i = 0; i < m; ++i) {
      if (std::fabs(w_cont[i]) > cfg.hold_eps) {
        held.push_back(i);
      }
    }
    if (held.size() > cfg.max_names) {
      std::stable_sort(held.begin(), held.end(), [&](atx::usize a, atx::usize b) {
        return std::fabs(w_cont[a]) > std::fabs(w_cont[b]);
      });
      for (atx::usize k = cfg.max_names; k < held.size(); ++k) {
        kept[held[k]] = 0U;
      }
      for (atx::usize i = 0; i < m; ++i) {
        if (kept[i] == 0U || std::fabs(w_cont[i]) <= cfg.hold_eps) {
          out.pinned[i] = 1U; // excluded or not held ⇒ pinned flat
          target[i] = 0.0;
        }
      }
    }
  }
  // (2) min-trade on the kept, un-pinned names.
  if (cfg.min_trade > 0.0) {
    for (atx::usize i = 0; i < m; ++i) {
      if (out.pinned[i] == 0U && std::fabs(w_cont[i] - w_prev[i]) < cfg.min_trade) {
        out.pinned[i] = 1U;
        target[i] = w_prev[i];
      }
    }
  }
  // (3) one re-solve around the pins.
  atx::usize n_pinned = 0;
  for (const atx::u8 v : out.pinned) {
    n_pinned += (v != 0U) ? 1U : 0U;
  }
  if (n_pinned == m) {
    out.book = target; // every name pinned ⇒ the pins ARE the book (no solve needed)
  } else if (n_pinned > 0U) {
    // The pins are equality rows: the scheduled ADMM (equality-row rho boost) converges on
    // them far faster than the fixed-rho loop.
    const MaterializedConstraints pinned_c = detail::with_pins(p.C, out.pinned, target);
    const QpProblem rp{p.V, p.risk_aversion, p.q, pinned_c};
    ATX_TRY(QpResult r, solver.solve_with_cert(rp, AdmmSchedule{}));
    out.book = std::move(r.book);
    out.resolved = true;
    for (atx::usize i = 0; i < m; ++i) {
      if (out.pinned[i] != 0U) {
        out.book[i] = target[i]; // exact pin (the ADMM honours it only to feas_tol)
      }
    }
  }
  // (4) lot rounding of the free names' trades, then the min-trade drop.
  for (atx::usize i = 0; i < m; ++i) {
    if (out.pinned[i] != 0U) {
      continue;
    }
    atx::f64 trade = out.book[i] - w_prev[i];
    if (!cfg.lot.empty() && cfg.lot[i] > 0.0) {
      trade = cfg.lot[i] * std::round(trade / cfg.lot[i]);
    }
    if (std::fabs(trade) < cfg.min_trade) {
      trade = 0.0;
    }
    if ((!cfg.lot.empty() && cfg.lot[i] > 0.0) || cfg.min_trade > 0.0) {
      out.book[i] = w_prev[i] + trade;
    }
  }
  for (atx::usize i = 0; i < m; ++i) {
    out.n_names += std::fabs(out.book[i]) > cfg.hold_eps ? 1U : 0U;
    out.n_trades += out.book[i] != w_prev[i] ? 1U : 0U;
  }
  out.max_violation = std::max(0.0, detail::book_violation(p.C, out.book));
  return co::Ok(std::move(out));
}

} // namespace atx::engine::risk
