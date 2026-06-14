#pragma once
#include <string>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/factory/factory.hpp"

namespace atxpy {

// Inputs to an evolutionary alpha-mining run. The research panel is given as
// date-major flat columns (one per field), matching alpha::Panel::create.
struct MineParams {
  atx::usize dates = 0;
  atx::usize instruments = 0;
  std::vector<std::string> field_names;
  std::vector<std::vector<atx::f64>> field_columns; // field_columns[f][date*instruments+inst]
  // search budget / geometry
  std::vector<std::string> seed_exprs;
  std::vector<std::string> panel_fields;
  atx::u64 master_seed = 0;
  atx::usize population = 16;
  atx::usize generations = 5;
  atx::usize elites = 2;
  atx::usize k_tournament = 3;
  atx::f64 p_cross = 0.5;
  atx::f64 novelty_w = 0.1;
  atx::u16 max_lookback = 250;
  atx::usize trial_count = 4;
  atx::f64 min_dsr = 0.5;
  atx::f64 book_size = 1.0;
  // admission gate
  atx::f64 min_sharpe = 1.0;
  atx::f64 min_fitness = 1.0;
  atx::f64 max_turnover = 0.70;
  atx::f64 max_pool_corr = 0.7;
};

// A completed mining run: the report telemetry plus the admitted-alpha pool.
// NOTE: the pool's per-alpha ISignalSource* handles dangle after the run (the
// factory that owned them is gone); only pnl/metrics/combine are valid here.
struct MineResult {
  atx::engine::factory::FactoryReport report;
  atx::engine::combine::AlphaStore pool;
};

[[nodiscard]] MineResult mine_alphas(const MineParams &params);

} // namespace atxpy
