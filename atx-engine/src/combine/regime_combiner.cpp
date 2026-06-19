// atx::engine::combine — RegimeCombiner implementation (S10-3).
//
// The heavy bodies (the masked sub-pool build + the AlphaCombiner refits) live
// here so the public header drags neither the linalg combiner internals nor the
// fit machinery into every includer. See combine/regime_combiner.hpp for the full
// contract; this file only realizes it.

#include "atx/engine/combine/regime_combiner.hpp"

#include <cmath>  // std::isfinite (the blend finite-posterior guard)
#include <vector> // std::vector (compacted PnL / dummy positions scratch)

#include "atx/core/macro.hpp" // ATX_ASSERT (blend programmer-error preconditions)
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/combine/metrics.hpp" // AlphaMetrics (zero-init for the sub-pool rows)

namespace atx::engine::combine {

std::vector<atx::f64> RegimeCombiner::blend(std::span<const atx::f64> posterior) const {
  // Programmer-error preconditions (abort in debug): a misshaped posterior or an
  // empty/ragged per_regime is a caller bug, not a recoverable runtime condition.
  ATX_ASSERT(!per_regime.empty());
  ATX_ASSERT(posterior.size() == per_regime.size());
  const atx::usize n_alpha = per_regime.front().weights.size();
  for (const Combination &c : per_regime) {
    ATX_ASSERT(c.weights.size() == n_alpha); // equal weight length across regimes
  }
  for (const atx::f64 p : posterior) {
    ATX_ASSERT(std::isfinite(p)); // a NaN/inf posterior would poison every weight
  }

  // out[i] = Σ_r posterior[r] * per_regime[r].weights[i]. Order-fixed: ascending
  // regime r (outer), then ascending alpha i (inner) — byte-identical run to run.
  std::vector<atx::f64> out(n_alpha, 0.0);
  for (atx::usize r = 0U; r < per_regime.size(); ++r) {
    const atx::f64 w_r = posterior[r];
    const std::vector<atx::f64> &wr = per_regime[r].weights;
    for (atx::usize i = 0U; i < n_alpha; ++i) {
      out[i] += w_r * wr[i];
    }
  }
  return out;
}

namespace {

// Fit the GLOBAL combo over the full [fit_begin, fit_end) window — the fallback a
// regime falls back to when it has < 2 periods (AlphaCombiner requires T >= 2).
// Shared by the under-populated-regime path so the fallback is computed ONE way.
[[nodiscard]] atx::core::Result<Combination> fit_global(const AlphaStore &pool,
                                                        atx::usize fit_begin, atx::usize fit_end,
                                                        const CombinerConfig &cfg) {
  return AlphaCombiner{cfg}.fit(pool, fit_begin, fit_end);
}

// Build the per-regime masked sub-pool and fit AlphaCombiner over it. `idxs` is the
// ascending list of periods (in the fit window) labelled this regime; it has been
// checked to hold >= 2 entries by the caller. Each alpha's PnL is COMPACTED to the
// idxs rows (a contiguous sub-history), so AlphaCombiner::fit over [0, idxs.size())
// reads exactly this regime's rows — the §3.1 fit firewall is inherited unchanged.
//
// positions are NEVER read by fit (verified in combiner.hpp: every method consumes
// only pnl(id).subspan(...)), so the sub-pool's positions are an all-zero dummy of
// the right shape — present only to satisfy AlphaStore::insert's coherent-shape
// contract (positions_flat.size() == n_periods * n_instruments).
[[nodiscard]] atx::core::Result<Combination>
fit_one_regime(const AlphaStore &pool, const std::vector<atx::usize> &idxs,
               const CombinerConfig &cfg) {
  const atx::usize n_alpha = pool.size();
  const atx::usize sub_periods = idxs.size();
  // n_instruments == 0 (a position-less test pool) would make insert's required
  // positions length 0; use a width of 1 so the dummy stream is non-degenerate yet
  // still all-zero. fit ignores it either way.
  const atx::usize n_inst = (pool.n_instruments() > 0U) ? pool.n_instruments() : 1U;
  const std::vector<atx::f64> dummy_positions(sub_periods * n_inst, 0.0);

  AlphaStore sub_pool;
  std::vector<atx::f64> compacted(sub_periods, 0.0);
  for (atx::usize a = 0U; a < n_alpha; ++a) {
    const AlphaId id{static_cast<atx::u32>(a)};
    const std::span<const atx::f64> full_pnl = pool.pnl(id);
    for (atx::usize k = 0U; k < sub_periods; ++k) {
      compacted[k] = full_pnl[idxs[k]]; // gather this regime's rows in ascending order
    }
    // Carry the source's metrics verbatim; the source handle is irrelevant to a
    // pure fit (fit never re-evaluates), so pass nullptr.
    ATX_TRY(AlphaId inserted,
            sub_pool.insert(nullptr, compacted, dummy_positions, pool.get(id).metrics));
    (void)inserted;
  }
  return AlphaCombiner{cfg}.fit(sub_pool, 0U, sub_periods);
}

} // namespace

atx::core::Result<RegimeCombiner> fit_regime_combiner(const AlphaStore &pool,
                                                      std::span<const atx::u32> regime_labels,
                                                      atx::usize n_regimes, atx::usize fit_begin,
                                                      atx::usize fit_end,
                                                      const CombinerConfig &cfg) {
  // Fail-closed input validation (runtime conditions -> Err, never abort).
  if (pool.size() == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_regime_combiner: empty pool");
  }
  if (regime_labels.size() != pool.n_periods()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_regime_combiner: regime_labels length != pool.n_periods()");
  }
  if (n_regimes == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_regime_combiner: n_regimes must be >= 1");
  }
  if (fit_end <= fit_begin || fit_end > pool.n_periods()) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "fit_regime_combiner: require fit_begin < fit_end <= pool.n_periods()");
  }
  // Every label in the window must name a real regime (a stray label would silently
  // drop periods from every regime's mask).
  for (atx::usize p = fit_begin; p < fit_end; ++p) {
    if (static_cast<atx::usize>(regime_labels[p]) >= n_regimes) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                            "fit_regime_combiner: regime label >= n_regimes");
    }
  }

  RegimeCombiner rc;
  rc.per_regime.reserve(n_regimes);
  for (atx::usize r = 0U; r < n_regimes; ++r) {
    // Ascending list of in-window periods assigned to regime r (the mask).
    std::vector<atx::usize> idxs;
    for (atx::usize p = fit_begin; p < fit_end; ++p) {
      if (static_cast<atx::usize>(regime_labels[p]) == r) {
        idxs.push_back(p);
      }
    }
    if (idxs.size() < 2U) {
      // Under-populated regime: AlphaCombiner needs T >= 2. Fall back to the global
      // combo over the full window so the slot is still a valid, blendable vector.
      ATX_TRY(Combination global, fit_global(pool, fit_begin, fit_end, cfg));
      rc.per_regime.push_back(std::move(global));
      continue;
    }
    ATX_TRY(Combination combo, fit_one_regime(pool, idxs, cfg));
    rc.per_regime.push_back(std::move(combo));
  }
  return atx::core::Ok(std::move(rc));
}

} // namespace atx::engine::combine
