#pragma once

// Incremental production builder for BacktestDb.
//
// The source of truth is a SurfaceDb. Each requested (template, symbol) cell is
// run through the theoretical contract-projection strategy and persisted as one
// binary BacktestDb partition. An unchanged cell is not priced again. When the
// source list is an exact prefix of the current SurfaceDb list, the stored engine
// checkpoint resumes from the previous final close and only newly arrived dates
// are evaluated. A changed historical source identity rebuilds that cell from
// inception; a caller-provided date range that would drop stored coverage is
// refused rather than silently truncating history.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "backtest/backtest_template.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

enum class BacktestDbCellBuildMode : std::uint8_t {
  Full = 0,
  Extended = 1,
  Rebuilt = 2,
  Unchanged = 3,
  Failed = 4,
};

struct BacktestDbCellBuildReport {
  std::string template_id{};
  std::string symbol{};
  BacktestDbCellBuildMode mode{BacktestDbCellBuildMode::Failed};
  std::size_t source_dates{0};
  std::size_t rows_before{0};
  std::size_t rows_after{0};
  std::size_t rows_computed{0};
  std::size_t rows_added{0};
  std::string detail{};
};

struct BacktestDbBuildSpec {
  std::string surface_db_root{};
  std::string backtest_db_root{};
  std::vector<BacktestStrategyTemplate> templates{};
  // Empty means every symbol registered in the SurfaceDb manifest.
  std::vector<std::string> symbols{};
  // Inclusive ISO-date bounds. Empty means unbounded on that side.
  std::string date_lo{};
  std::string date_hi{};
  // 0 delegates to the library pricing worker policy.
  unsigned price_threads{0};
};

struct BacktestDbBuildReport {
  std::vector<BacktestDbCellBuildReport> cells{};
  std::size_t n_full{0};
  std::size_t n_extended{0};
  std::size_t n_rebuilt{0};
  std::size_t n_unchanged{0};
  std::size_t n_failed{0};
  std::size_t rows_computed{0};
  std::size_t rows_added{0};
};

// Create-or-open the destination database, register templates idempotently, and
// build or extend every requested cell. Expected per-symbol market/projection
// failures are recorded in `report.cells` and do not prevent independent cells
// from completing. Structural source/store errors return Err.
//
// Source consistency: the builder pins the SurfaceDb manifest generation before
// loading, reopens and validates the current manifest after loading,
// immediately before every series publication, and once after all cells
// (including an all-Unchanged run). A generation change returns Unavailable
// with retry guidance. No cell is published after drift is observed; cells
// committed before a later drift remain identity-attested and are safely
// reconciled on the retry.
[[nodiscard]] Result<BacktestDbBuildReport> build_backtest_db(const BacktestDbBuildSpec &spec);

} // namespace atx::vol
