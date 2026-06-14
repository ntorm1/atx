#include "mining_shim.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxpy {

namespace alpha = atx::engine::alpha;
namespace factory = atx::engine::factory;
namespace combine = atx::engine::combine;
namespace exec = atx::engine::exec;

MineResult mine_alphas(const MineParams &p) {
  auto panel_r = alpha::Panel::create(p.dates, p.instruments, p.field_names, p.field_columns,
                                      std::vector<std::uint8_t>{});
  if (!panel_r) {
    throw std::runtime_error("mine panel: " + panel_r.error().to_string());
  }
  const alpha::Panel panel = std::move(*panel_r);
  const alpha::Library lib;
  const exec::ExecutionSimulator sim; // default cost model
  const atx::engine::WeightPolicy policy;

  factory::Factory f{lib, panel, sim, policy};

  factory::FactoryConfig cfg;
  cfg.search.master_seed = p.master_seed;
  cfg.search.population = p.population;
  cfg.search.generations = p.generations;
  cfg.search.elites = p.elites;
  cfg.search.k_tournament = p.k_tournament;
  cfg.search.p_cross = p.p_cross;
  cfg.search.novelty_w = p.novelty_w;
  cfg.search.max_lookback = p.max_lookback;
  cfg.search.fitness.trial_count = p.trial_count;
  cfg.search.fitness.book_size = p.book_size;
  cfg.seed_exprs = p.seed_exprs;
  cfg.panel_fields = p.panel_fields;
  cfg.min_dsr = p.min_dsr;
  cfg.book_size = p.book_size;

  combine::GateConfig gcfg;
  gcfg.min_sharpe = p.min_sharpe;
  gcfg.min_fitness = p.min_fitness;
  gcfg.max_turnover = p.max_turnover;
  gcfg.max_pool_corr = p.max_pool_corr;
  const combine::AlphaGate gate{gcfg};

  MineResult out;
  out.report = f.mine(cfg, out.pool, gate); // fills out.pool in place
  return out;
}

} // namespace atxpy
