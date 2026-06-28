#include "atx/engine/factory/pool_view.hpp"

#include <span>

namespace atx::engine::factory {

[[nodiscard]] atx::core::Result<FitnessReport>
pool_aware_fitness(const Genome &cand, const PoolView &view, const alpha::Panel &panel,
                   const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                   const FitnessCfg &cfg, const alpha::Panel *weak_panel,
                   alpha::Engine *engine, const alpha::SignalSet *signals) {
  // Steps 1, 3, 5 (pool-INDEPENDENT) — shared with the legacy overload.
  ATX_TRY(const detail::FitnessCore core,
          detail::fitness_core(cand, panel, policy, sim, cfg, weak_panel, engine, signals));

  // (2) diversification discount: MAX |corr-to-pool| of the OOS PnL, served by the
  // PoolView's chosen backing (O(N) exact AlphaStore, or O(neighbors) library).
  const atx::f64 redundancy = view.worst_corr(std::span<const atx::f64>{core.oos_pnl});

  // (4) raw = wq * diversify * robust (+ S3-0 opt-in turnover penalty), assembled
  // into the report. S4.3: the cost objective (objectives[4]) is active iff
  // target_aum > 0 (cost_bps is already in `core`, computed by fitness_core under
  // the same guard). S3-0: cfg carries slope/max_turnover_target forwarded here.
  return atx::core::Ok(detail::finish_report(core, redundancy, cfg.target_aum > 0.0, cfg));
}

} // namespace atx::engine::factory
