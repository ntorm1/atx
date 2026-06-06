#pragma once

// atx::external::databento — ingest a Databento EQUS.SUMMARY batch .zip into a
// date-partitioned Parquet hive on disk.
//
// The zip holds zstd-compressed DBN files (*.dbn.zst). This loader unzips,
// zstd-decompresses, decodes the DBN OHLCV records (see atx::external::dbn), and
// writes <dest_dir>/date=YYYY-MM-DD/data.parquet via atx::core::io. Self-contained:
// no network, no Python, no JSON dependency.
//
// Output schema per file: ts:timestamp[ns], symbol:string, open/high/low/close:i64
// (units of 1e-9), volume:i64. The partition column "date" is encoded in the path.
// Prices use Databento's native 1e-9 fixed-point (divide by 1e9 for dollars).

#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::external::databento {

using atx::core::Result;

struct LoadStats {
  i64 files_processed{0};    // .dbn.zst entries decoded
  i64 records_decoded{0};    // OHLCV rows written
  i64 partitions_written{0}; // distinct date= partitions
  i64 records_skipped{0};    // records of unsupported rtype
};

// Decode `zip_path` into <dest_dir>/date=YYYY-MM-DD/data.parquet. Err on missing/
// malformed input or if zero OHLCV records are found.
[[nodiscard]] Result<LoadStats> load_equs_summary_zip(std::string_view zip_path,
                                                      std::string_view dest_dir);

} // namespace atx::external::databento
