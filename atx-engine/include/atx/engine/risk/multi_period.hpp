#pragma once

// atx::engine::risk — MultiPeriodOptimizer: receding-horizon multi-period DRIVER
// over the as-built single-period risk::PortfolioOptimizer::solve (S7-1).
//
// ===========================================================================
//  What this unit is (and is NOT)
// ===========================================================================
//  This is NOT a new inner solver. It is a receding-horizon DRIVER that walks an
//  ascending rebalance schedule and, at each as-of period, calls the EXISTING
//  PortfolioOptimizer::solve VERBATIM. Its only jobs around that reused solve are:
//
//    * thread w_prev from the prior period's REALIZED book, so turnover is measured
//      across the REAL schedule (not from flat each period);
//    * set the turnover penalty κ to the CALIBRATED book::CostInputs.kappa, so the
//      single-period prox shrinks trading by the calibrated amount;
//    * apply a Gârleanu-Pedersen partial trade-rate toward the solved target
//      (book = w_prev + rate·(target − w_prev)); rate == 1 ⇒ full step (book ==
//      target bit-for-bit);
//    * capacity-bound the gross: clip single.gross_leverage at the capacity ceiling
//      (book::CostInputs.capacity_gross) before constructing the inner optimizer.
//
//  Because the inner solve is reused VERBATIM and is itself FIXED-iteration and
//  RNG-free, the whole multi-period book chain inherits bit-determinism for free:
//  same inputs ⇒ byte-identical books (the R1 determinism pin).
//
// ===========================================================================
//  Determinism + allocation
// ===========================================================================
//  NO map / clock / RNG. All reductions (l1_diff, blend) run in canonical ascending
//  index order. The only allocations are the result vectors (books / turnover /
//  cost_bps) plus the per-period target/book the inner solve already produces —
//  documented, at rebalance cadence, never on a hot tick path.
//
// ===========================================================================
//  Recorded residual (L7)
// ===========================================================================
//  The fwd.hpp docstring records that a TRUE OSQP-style ADMM multi-period QP is the
//  atx-core L7 lift. This driver intentionally ships on the as-built projected/
//  proximal PortfolioOptimizer loop (Pattern-B: reuse the existing engine solver,
//  defer the dedicated QP kernel to atx-core).

#include <algorithm> // std::min
#include <cmath>     // std::fabs
#include <functional> // std::function (alpha_at / model_at callbacks)
#include <optional>   // std::optional (the optional augmented ConstraintSet — S8.4)
#include <span>       // std::span
#include <utility>    // std::move
#include <vector>     // std::vector (result books)

#include "atx/core/error.hpp" // Result, Ok, ATX_TRY
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/book/fwd.hpp"          // book::CostInputs (forward decl; full def below)
#include "atx/engine/risk/constraints.hpp"  // ConstraintSet (the augmented-dispatch set — S8.4)
#include "atx/engine/risk/factor_model.hpp" // FactorModel (the trailing-fit risk model)
#include "atx/engine/risk/optimizer.hpp"    // OptimizerConfig, PortfolioOptimizer (reused verbatim)
#include "atx/engine/risk/qp_solver.hpp"      // QpConfig (ADMM knobs for the augmented dispatch — S8.4)
#include "atx/engine/risk/reference_data.hpp" // CapacityRef (%ADV / %shares box inputs — S8.4)

// ===========================================================================
//  book::CostInputs — the calibrated scalar cost adapter (CANONICAL definition).
//
//  book/fwd.hpp forward-declares this type (a clean scalar-adapter seam so optimizer
//  code never takes a direct cost::CalibratedCost dependency). A forward decl + this
//  later full definition is legal; the full definition lives HERE because this is the
//  first consumer that needs the complete type. Three fields, all calibrated upstream
//  by the merged S6 cost:: API (cost_aware_knobs / capacity_point).
// ===========================================================================
namespace atx::engine::book {

struct CostInputs {
  atx::f64 kappa = 0.0;               // calibrated turnover penalty (cost_aware_knobs().kappa)
  atx::f64 round_trip_cost_bps = 0.0; // calibrated round-trip cost in bps (charged per unit turnover)
  atx::f64 capacity_gross = 1e9;      // capacity ceiling on gross leverage (cost::capacity_point-derived)
};

} // namespace atx::engine::book

namespace atx::engine::risk {

// ===========================================================================
//  RebalanceSchedule — the ascending as-of period indices (PIT).
// ===========================================================================
struct RebalanceSchedule {
  std::vector<atx::usize> periods; // ascending point-in-time as-of period indices
};

// ===========================================================================
//  MultiPeriodConfig — the driver knobs.
// ===========================================================================
struct MultiPeriodConfig {
  OptimizerConfig single;            // λ, κ, L, name_cap, dollar_neutral, max_iters — REUSED verbatim
                                     // (κ is overridden to cost.kappa by run()).
  atx::f64 trade_rate = 1.0;         // Gârleanu-Pedersen partial step toward target ∈ (0,1]; 1 ⇒ full
  bool capacity_bound_gross = true;  // clip single.gross_leverage at the capacity ceiling

  // ---- S8.4 augmented-dispatch fields (§0.5) ----------------------------------
  // An OPTIONAL full constraint set threaded into the inner PortfolioOptimizer. ABSENT
  // or MINIMAL (GrossNet + optional PositionCap) ⇒ the inner solve takes the as-built
  // fast path, BYTE-IDENTICAL to the pre-S8.4 driver (the single-solve pin holds). Any
  // extra row routes the inner solve through the ConstrainedQpSolver. Default empty ⇒
  // the as-built behavior, so every existing caller / pin is untouched.
  std::optional<ConstraintSet> constraints;
  QpConfig qp{};     // ADMM knobs forwarded to the inner solve's augmented path
  CapacityRef ref{}; // %ADV / %shares reference panel forwarded to the augmented path
};

// ===========================================================================
//  MultiPeriodResult — the realized book chain + per-period turnover/cost.
// ===========================================================================
struct MultiPeriodResult {
  std::vector<std::vector<atx::f64>> books;    // one weight vector per schedule period (books[s][i])
  std::vector<atx::f64> turnover;              // per-period Σ_i |book[s] − book[s-1]| (one-sided from flat at s=0)
  std::vector<atx::f64> cost_bps;              // per-period calibrated cost charged on that turnover
};

// ===========================================================================
//  MultiPeriodOptimizer — the receding-horizon driver.
// ===========================================================================
class MultiPeriodOptimizer {
public:
  MultiPeriodConfig cfg;

  // Walk the ascending schedule, threading w_prev from the prior REALIZED book. For
  // each as-of period the inner PortfolioOptimizer::solve (reused VERBATIM) yields the
  // target book; a Gârleanu-Pedersen trade-rate partial-steps toward it; turnover is
  // the L1 move from the prior book (from flat at s=0) and the calibrated round-trip
  // cost is charged on it. κ is set to the calibrated cost.kappa and the gross is
  // capacity-bounded before the inner optimizer is constructed. `[[nodiscard]] const`.
  [[nodiscard]] atx::core::Result<MultiPeriodResult>
  run(const RebalanceSchedule &sched,
      const std::function<std::span<const atx::f64>(atx::usize s)> &alpha_at,
      const std::function<const FactorModel &(atx::usize s)> &model_at,
      const book::CostInputs &cost) const {
    // Validate at the boundary: the Gârleanu-Pedersen trade-rate domain is (0,1].
    if (cfg.trade_rate <= 0.0 || cfg.trade_rate > 1.0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "MultiPeriodOptimizer::run: trade_rate must be in (0, 1]");
    }
    MultiPeriodResult out;
    out.books.reserve(sched.periods.size());
    out.turnover.reserve(sched.periods.size());
    out.cost_bps.reserve(sched.periods.size());

    // The inner config: REUSE cfg.single verbatim, but set κ to the calibrated value
    // and capacity-bound the gross at the cost ceiling. Constructed ONCE — the same
    // optimizer is reused across every schedule period.
    OptimizerConfig oc = cfg.single;
    oc.turnover_penalty = cost.kappa;
    if (cfg.capacity_bound_gross) {
      // NOTE: this capacity_bound_gross clip governs ONLY the fast path — it lands in
      // `oc.gross_leverage`, which the inner solve reads when NO set is attached, and
      // which an attached MINIMAL set's translation re-derives from set.gross (so the
      // clip does NOT reach a minimal set's gross). On the AUGMENTED path the QP
      // materializes the constraint set's gross independently of `oc`, so the clip is
      // not applied there: the augmented path honors whatever gross the caller set in
      // cfg.constraints.gross (mirroring MultiHorizonOptimizer::solve_augmented's
      // docstring — the capacity-clip is the minimal/fast-path concern only).
      oc.gross_leverage = std::min(oc.gross_leverage, cost.capacity_gross);
    }
    // Thread the optional augmented constraint set + QP knobs + capacity reference into
    // the inner solve (S8.4 §0.5). When absent/minimal the inner solve takes the
    // as-built fast path off `oc`, byte-identical to the pre-S8.4 driver.
    PortfolioOptimizer opt{oc};
    opt.constraints = cfg.constraints;
    opt.qp = cfg.qp;
    opt.ref = cfg.ref;

    std::vector<atx::f64> w_prev; // EMPTY at s=0 ⇒ a flat all-zero previous book.
    for (atx::usize s = 0; s < sched.periods.size(); ++s) {
      const auto a = alpha_at(sched.periods[s]);
      const FactorModel &V = model_at(sched.periods[s]);
      ATX_TRY(std::vector<atx::f64> target, opt.solve(a, V, w_prev));

      std::vector<atx::f64> book = blend_toward(w_prev, target, cfg.trade_rate);
      out.turnover.push_back(l1_diff(book, w_prev));
      out.cost_bps.push_back(out.turnover.back() * cost.round_trip_cost_bps);
      out.books.push_back(book);
      w_prev = std::move(book);
    }
    return atx::core::Ok(std::move(out));
  }

private:
  // Gârleanu-Pedersen partial step: book[i] = w_prev[i] + rate·(target[i] − w_prev[i]),
  // ascending i. An empty w_prev is treated as zeros of target.size() (the s=0 flat
  // book), and a NaN-target name — which solve already 0s — stays 0 (0 + rate·(0 − 0) = 0).
  // The rate == 1.0 full step is SPECIAL-CASED to assign target[i] VERBATIM: the algebraic
  // form 0.0 + 1.0·(target[i] − 0.0) would flush a −0.0 target weight to +0.0 (IEEE:
  // 0.0 + −0.0 == +0.0), and solve's dollar-neutral demean can emit −0.0, so the verbatim
  // assignment is what makes a full step byte-identical to the solver output (the test #1
  // bit_cast<u64> single-solve pin — signed zero preserved).
  [[nodiscard]] static std::vector<atx::f64> blend_toward(std::span<const atx::f64> w_prev,
                                                          std::span<const atx::f64> target,
                                                          atx::f64 rate) {
    std::vector<atx::f64> book(target.size(), 0.0);
    for (atx::usize i = 0; i < target.size(); ++i) {
      const atx::f64 p = i < w_prev.size() ? w_prev[i] : 0.0;
      book[i] = (rate == 1.0) ? target[i] : (p + rate * (target[i] - p));
    }
    return book;
  }

  // Σ_i |a[i] − b[i]| in ascending i; an empty b is the flat all-zero book ⇒ Σ_i |a[i]|.
  [[nodiscard]] static atx::f64 l1_diff(std::span<const atx::f64> a,
                                        std::span<const atx::f64> b) noexcept {
    atx::f64 s = 0.0;
    for (atx::usize i = 0; i < a.size(); ++i) {
      const atx::f64 bi = i < b.size() ? b[i] : 0.0;
      s += std::fabs(a[i] - bi);
    }
    return s;
  }
};

} // namespace atx::engine::risk
