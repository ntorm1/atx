#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/dispersion.hpp"
#include "atx/vol/opra_batch.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

struct RunSpec {
  std::string label{"SPY listed-options dispersion proxy"};
  std::string date_lo{};
  std::string date_hi{};
  std::string snapshot_suffix{"T19:55:00Z"};
  std::filesystem::path opra_root{};
  std::string path_template{"{symbol}/{date}.parquet"};
  std::filesystem::path universe_path{};
  std::filesystem::path definitions_path{};
  std::filesystem::path occ_ess_root{};
  double flat_rate{0.0};
  std::size_t min_names{10};
  double min_weight_coverage{0.8};
  double target_dte_days{30.0};
  double min_dte_days{21.0};
  double max_dte_days{60.0};
  double roll_dte_days{7.0};
  double gross_index_vega{10000.0};
  double delta_band{0.0};
  unsigned fit_workers{0};
  bool core_mode{false};
};

struct UniverseRow {
  std::string effective_date{};
  std::string symbol{};
  double raw_weight{0.0};
  std::string source{};
  std::string as_of{};
};

[[nodiscard]] Result<RunSpec> read_run_spec(const std::filesystem::path &path);
[[nodiscard]] Status write_resolved_spec(const std::filesystem::path &path, const RunSpec &spec);
[[nodiscard]] Result<std::vector<UniverseRow>> read_universe(const std::filesystem::path &path);
[[nodiscard]] std::vector<std::string> all_symbols(std::span<const UniverseRow> rows);
[[nodiscard]] Result<DispersionUniverse> universe_at(std::span<const UniverseRow> rows,
                                                     std::string_view date);
[[nodiscard]] OpraBatchSpec batch_spec(const RunSpec &spec, std::span<const std::string> symbols,
                                       std::string_view date_lo, std::string_view date_hi);

} // namespace atx::vol
