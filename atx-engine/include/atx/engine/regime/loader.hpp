#pragma once

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/regime/source_csv.hpp"

namespace atx::engine::regime {

struct SeriesSpec {
  std::string name;          // canonical series name -> becomes a segment field
  std::string file;          // CSV filename relative to staging_dir
  CsvFormat format{CsvFormat::Fred};
  std::string value_column;  // header name of the numeric column
};

struct RegimeLoadConfig {
  std::string staging_dir;             // dir holding the staged CSVs
  std::string out_path;                // output .seg path
  std::vector<SeriesSpec> series;      // raw series to load
  std::vector<std::string> derived;    // derived defs, e.g. "t10y2y = dgs10 - dgs2"
  atx::i64 min_date_nanos{0};          // inclusive floor
  atx::i64 created_at_nanos{0};        // stamped into the segment header
};

struct RegimeLoadStats {
  atx::i64 series_count{};      // raw + derived
  atx::i64 dates_written{};     // master axis length
  atx::i64 first_date_nanos{};
  atx::i64 last_date_nanos{};
};

// Read staged CSVs -> align onto a master axis (forward-filled) -> derive
// composites -> pivot to a sealed segment at cfg.out_path + a JSON manifest at
// cfg.out_path + ".manifest.json". Deterministic: same staged snapshot ->
// byte-identical segment.
[[nodiscard]] atx::core::Result<RegimeLoadStats>
load_regime_history(const RegimeLoadConfig &cfg);

}  // namespace atx::engine::regime
