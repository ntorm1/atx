#include "atx/engine/regime/loader.hpp"

#include <filesystem>
#include <fstream>

#include "atx/tsdb/load_parquet.hpp"   // LongColumns + build_from_long

#include "atx/engine/regime/align.hpp"
#include "atx/engine/regime/series.hpp"

namespace atx::engine::regime {

atx::core::Result<RegimeLoadStats> load_regime_history(const RegimeLoadConfig &cfg) {
  namespace fs = std::filesystem;

  // 1. Parse each raw series CSV (preserve config order for determinism).
  std::vector<NamedSeries> raw;
  raw.reserve(cfg.series.size());
  for (const SeriesSpec &spec : cfg.series) {
    const std::string path = (fs::path(cfg.staging_dir) / spec.file).string();
    ATX_TRY(auto obs, parse_series_csv(path, spec.format, spec.value_column));
    raw.push_back(NamedSeries{spec.name, std::move(obs)});
  }
  if (raw.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "load_regime_history: no series configured");
  }

  // 2. Master axis = sorted-unique union of observed dates >= floor.
  const std::vector<atx::i64> axis = build_master_axis(raw, cfg.min_date_nanos);
  if (axis.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "load_regime_history: master axis is empty (check min_date)");
  }

  // 3. Forward-fill each series onto the axis.
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  names.reserve(raw.size());
  cols.reserve(raw.size());
  for (const NamedSeries &s : raw) {
    names.push_back(s.name);
    cols.push_back(forward_fill(s.obs, axis));
  }

  // 4. Derived series (after alignment, so operands share the axis).
  for (const std::string &def : cfg.derived) {
    ATX_TRY(const DerivedSpec spec, parse_derived_spec(def));
    ATX_TRY_VOID(apply_derived(names, cols, spec));
  }

  // 5. Pivot to LongColumns: one synthetic instrument "MACRO"; one row per date.
  //    LongColumns is ROW-oriented: times[r]/symbols[r]/values[field][r] share R.
  atx::tsdb::LongColumns lc;
  lc.field_names = names;
  const atx::usize T = axis.size();
  const atx::usize F = names.size();
  lc.values.assign(F, {});
  for (atx::usize f = 0; f < F; ++f) {
    lc.values[f].reserve(T);
  }
  lc.times.reserve(T);
  lc.symbols.reserve(T);
  for (atx::usize t = 0; t < T; ++t) {
    lc.times.push_back(axis[t]);
    lc.symbols.emplace_back("MACRO");
    for (atx::usize f = 0; f < F; ++f) {
      lc.values[f].push_back(cols[f][t]);
    }
  }

  ATX_TRY_VOID(atx::tsdb::build_from_long(lc, cfg.out_path, cfg.created_at_nanos));

  // 6. Hand-written JSON manifest (no JSON lib dependency).
  {
    std::ofstream m(cfg.out_path + ".manifest.json", std::ios::binary);
    m << "{\n  \"dates\": " << T << ",\n  \"series\": [";
    for (atx::usize f = 0; f < F; ++f) {
      m << (f ? ", " : "") << "\"" << names[f] << "\"";
    }
    m << "],\n  \"first_date_nanos\": " << axis.front()
      << ",\n  \"last_date_nanos\": " << axis.back()
      << ",\n  \"created_at_nanos\": " << cfg.created_at_nanos << "\n}\n";
  }

  RegimeLoadStats stats;
  stats.series_count = static_cast<atx::i64>(F);
  stats.dates_written = static_cast<atx::i64>(T);
  stats.first_date_nanos = axis.front();
  stats.last_date_nanos = axis.back();
  return atx::core::Ok(stats);
}

}  // namespace atx::engine::regime
