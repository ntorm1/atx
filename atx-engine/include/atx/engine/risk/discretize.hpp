#pragma once

// atx::engine::risk — discretize: the deterministic POST-SOLVE pass that turns a continuous
// optimizer book into a tradable one (Lane 6). Three rules, applied in a fixed order:
//
//   (1) MAX-NAMES  — keep at most `max_names` held names: the largest |w_i| of the
//       continuous book (ties → lower index). EVERY other name, including one the
//       continuous book does not hold at all, is pinned at 0, so no later rule (min-trade
//       at a nonzero w_prev, the re-solve, lot rounding) can hand weight to it: the cap is
//       an invariant of the result, not only of the continuous book.
//   (2) MIN-TRADE  — a kept name whose trade |w_i − w0_i| is below `min_trade` is pinned at
//       its previous weight w0_i (no trade). Excluded names are never re-pinned at w0_i.
//   (3) RE-SOLVE   — when (1)/(2) pinned a name AWAY from its continuous weight (by more
//       than hold_eps), the QP is solved again with each pinned name as an equality row
//       w_i = target_i, so the free names re-optimize around the pins (risk, neutrality
//       and budgets are re-balanced instead of silently broken). Pins that already
//       coincide with the continuous book are snapped exactly and need no re-solve. The
//       re-solve runs cfg.schedule and, when cfg.warm is set, is warm-started from the
//       continuous solve (its primal, its duals re-laid-out around the pin rows, its ρ).
//       With solver.cfg.factor_space the re-solve takes the factor-space path, where each
//       pin is a single-name equality folded into the box block (exact after the polish).
//   (4) ROUND LOTS — each free name's trade is rounded to the nearest multiple of its lot
//       (weight units); a rounded trade below `min_trade` is dropped.
//
//  The pass is heuristic (cardinality is non-convex) but deterministic: a pure function of
//  (problem, w0, continuous book, cfg), one re-solve at most, no RNG. Lot rounding can move
//  a linear constraint by up to Σ lot/2, so the result carries the realized worst violation
//  of the ORIGINAL constraint set for the caller to gate on. With max_names > 0 the result
//  is checked: n_names > max_names is Err(Internal) (unreachable by construction; it guards
//  the invariant against future edits).

#include <algorithm> // std::stable_sort, std::max, std::min
#include <cstddef>   // std::ptrdiff_t
#include <cmath>     // std::fabs, std::round, std::isfinite
#include <limits>
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err
#include "atx/core/types.hpp" // f64, u8, u32, usize

#include "atx/engine/risk/admm_schedule.hpp" // AdmmSchedule, WarmStart
#include "atx/engine/risk/constraints.hpp" // MaterializedConstraints
#include "atx/engine/risk/qp_augment.hpp"  // detail::aug_total_rows (pin dual re-layout)
#include "atx/engine/risk/qp_solver.hpp"   // ConstrainedQpSolver, QpProblem

namespace atx::engine::risk {

struct DiscretizeCfg {
  atx::f64 min_trade = 0.0;       // |Δw| below this ⇒ no trade (weight units); 0 ⇒ off
  std::span<const atx::f64> lot;  // per-name lot size in weight units; empty / ≤ 0 ⇒ continuous
  atx::u32 max_names = 0;         // cap on held names; 0 ⇒ no cap
  atx::f64 hold_eps = 1e-10;      // |w| ≤ this counts as "not held"
  // The pin re-solve's ADMM schedule. At production scale set early_exit (with the solver's
  // cfg.iters as a cap): the default fixed 300-iteration schedule can miss the 1e-6 gate.
  AdmmSchedule schedule{};
  // Optional: the CONTINUOUS solve's result for the same problem p. Its x_full seeds the
  // re-solve's primal, its y_full is re-laid-out around the appended pin rows (pin duals
  // seeded 0) and cert.rho_final seeds ρ. Must outlive the call. nullptr ⇒ cold re-solve.
  const QpResult *warm = nullptr;
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
  for (atx::usize row = 0; row < c.row_count(); ++row) {
    const auto r = static_cast<Eigen::Index>(row);
    atx::f64 a = 0.0;
    if (c.sparse()) {
      c.visit_row(row, [&](atx::usize j, atx::f64 value) { a += value * w[j]; });
    } else {
      for (Eigen::Index j = 0; j < c.A.cols(); ++j)
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
[[nodiscard]] inline atx::core::Result<MaterializedConstraints> with_pins(const MaterializedConstraints &c,
                                                       std::span<const atx::u8> pinned,
                                                       std::span<const atx::f64> target) {
  const auto m = static_cast<Eigen::Index>(pinned.size());
  Eigen::Index n_pins = 0;
  for (const atx::u8 p : pinned) {
    n_pins += (p != 0U) ? 1 : 0;
  }
  ATX_TRY_VOID(c.validate_layout(pinned.size()));
  if (target.size() != pinned.size())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "discretize: pin target shape mismatch");
  if (c.sparse() && static_cast<atx::usize>(n_pins) > c.storage.max_nnz - c.stored_nonzeros())
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "discretize: pin rows exceed nnz budget");
  if (c.sparse()) {
    // validate_layout bounds all three dimensions by int_max/4, so these u64
    // resource calculations cannot overflow, including before the copy below.
    const auto pins = static_cast<atx::usize>(n_pins);
    const auto bytes = 256ULL * (c.row_count() + pinned.size() + pins) +
        32ULL * (c.csr.values.size() + pins) + sizeof(MaterializedConstraints);
    if (c.row_count() + pins > static_cast<atx::usize>(std::numeric_limits<int>::max()) / 4 ||
        bytes > c.storage.max_materialization_bytes)
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "discretize: pin workspace exceeds budget");
  }
  MaterializedConstraints out = c;
  const Eigen::Index r0 = static_cast<Eigen::Index>(c.row_count());
  if (!c.sparse()) out.A = atx::core::linalg::MatX::Zero(r0 + n_pins, m);
  out.l = atx::core::linalg::VecX::Zero(r0 + n_pins);
  out.u = atx::core::linalg::VecX::Zero(r0 + n_pins);
  if (r0 > 0) {
    if (!c.sparse()) out.A.topRows(r0) = c.A;
    out.l.head(r0) = c.l;
    out.u.head(r0) = c.u;
  }
  Eigen::Index row = r0;
  for (Eigen::Index i = 0; i < m; ++i) {
    if (pinned[static_cast<atx::usize>(i)] != 0U) {
      if (c.sparse()) {
        out.csr.column_indices.push_back(static_cast<atx::usize>(i));
        out.csr.values.push_back(1.0);
        out.csr.row_offsets.push_back(out.csr.values.size());
      } else {
        out.A(row, i) = 1.0;
      }
      out.l[row] = target[static_cast<atx::usize>(i)];
      out.u[row] = target[static_cast<atx::usize>(i)];
      ++row;
    }
  }
  ATX_TRY_VOID(out.validate_layout(pinned.size()));
  return atx::core::Ok(std::move(out));
}

// Re-lay-out a continuous solve's augmented dual around the n_pins equality rows that
// with_pins appends after C.A's rows: augmented rows are [K factor rows | C.A rows | rest],
// so the pin rows land at K + C.A.rows(). Pin duals seed 0. Returns empty (⇒ the solver's
// zero dual seed) when y_full does not have the un-pinned problem's augmented length.
[[nodiscard]] inline std::vector<atx::f64> pin_dual_seed(const QpProblem &p,
                                                         std::span<const atx::f64> y_full,
                                                         atx::usize n_pins) {
  const atx::usize k = p.V.n_factors();
  const bool has_gross = p.C.gross_l1_budget >= 0.0;
  const bool has_turn = p.C.has_turnover || p.C.turnover_penalty > 0.0;
  const atx::usize r = aug_total_rows(p.V.n_instruments(), k, p.C, has_gross, has_turn);
  if (y_full.size() != r) {
    return {};
  }
  const auto split = static_cast<std::ptrdiff_t>(k + p.C.row_count());
  std::vector<atx::f64> out;
  out.reserve(r + n_pins);
  out.insert(out.end(), y_full.begin(), y_full.begin() + split);
  out.insert(out.end(), n_pins, 0.0);
  out.insert(out.end(), y_full.begin() + split, y_full.end());
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

  // (1) max-names: rank held names by |w| descending, ties by index (stable sort). Every
  //     name outside the kept set is pinned flat, also when held.size() <= max_names, so
  //     rule (2) cannot re-pin an excluded name at a nonzero w_prev (review case: M=2, cap
  //     1, w_cont=[0.3,0], w_prev=[0.3,0.05], min_trade 0.1 used to return 2 names).
  if (cfg.max_names > 0U) {
    std::vector<atx::usize> held;
    for (atx::usize i = 0; i < m; ++i) {
      if (std::fabs(w_cont[i]) > cfg.hold_eps) {
        held.push_back(i);
      }
    }
    std::stable_sort(held.begin(), held.end(), [&](atx::usize a, atx::usize b) {
      return std::fabs(w_cont[a]) > std::fabs(w_cont[b]);
    });
    std::vector<atx::u8> kept(m, 0U);
    const atx::usize n_keep = std::min<atx::usize>(cfg.max_names, held.size());
    for (atx::usize k = 0; k < n_keep; ++k) {
      kept[held[k]] = 1U;
    }
    for (atx::usize i = 0; i < m; ++i) {
      if (kept[i] == 0U) {
        out.pinned[i] = 1U; // excluded or not held ⇒ pinned flat
        target[i] = 0.0;
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
  // (3) one re-solve around the pins, only when a pin actually moves a name.
  atx::usize n_pinned = 0;
  bool pin_moves = false;
  for (atx::usize i = 0; i < m; ++i) {
    if (out.pinned[i] != 0U) {
      ++n_pinned;
      pin_moves = pin_moves || std::fabs(target[i] - w_cont[i]) > cfg.hold_eps;
    }
  }
  if (n_pinned == m) {
    out.book = target; // every name pinned ⇒ the pins ARE the book (no solve needed)
  } else if (pin_moves) {
    // The pins are equality rows: the scheduled ADMM (equality-row rho boost) converges on
    // them far faster than the fixed-rho loop.
    ATX_TRY(const MaterializedConstraints pinned_c, detail::with_pins(p.C, out.pinned, target));
    const QpProblem rp{p.V, p.risk_aversion, p.q, pinned_c};
    std::vector<atx::f64> y_seed;
    WarmStart ws;
    const WarmStart *wsp = nullptr;
    if (cfg.warm != nullptr) {
      // Factor-space layout: a pin is a single-name row folded into the box block, so the
      // dual layout [y_box ; y_dense (; y_turn)] is unchanged by the pins.
      y_seed = cfg.warm->cert.factor_space
                   ? cfg.warm->y_full
                   : detail::pin_dual_seed(p, cfg.warm->y_full, n_pinned);
      ws.x0 = cfg.warm->x_full;
      ws.y0 = y_seed;
      ws.rho = cfg.warm->cert.rho_final;
      wsp = &ws;
    }
    ATX_TRY(QpResult r, solver.solve_with_cert(rp, cfg.schedule, wsp));
    out.book = std::move(r.book);
    out.resolved = true;
    for (atx::usize i = 0; i < m; ++i) {
      if (out.pinned[i] != 0U) {
        out.book[i] = target[i]; // exact pin (the ADMM honours it only to feas_tol)
      }
    }
  } else {
    for (atx::usize i = 0; i < m; ++i) {
      if (out.pinned[i] != 0U) {
        out.book[i] = target[i]; // the pin coincides with w_cont to hold_eps: snap exactly
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
  if (cfg.max_names > 0U && out.n_names > cfg.max_names) {
    return co::Err(co::ErrorCode::Internal,
                   "discretize_and_resolve: result holds more than max_names names");
  }
  return co::Ok(std::move(out));
}

} // namespace atx::engine::risk
