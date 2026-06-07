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

#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct PullStats {
  i64 records{0};       // L1 rows written
  i64 symbols{0};       // distinct symbols/contracts seen
  i64 api_calls{0};     // TimeseriesGetRange calls after batch split
  double cost_usd{0.0}; // summed preflight cost actually pulled
};

// Recursively split `symbols` into batches whose estimated cost is < cap. `est` is
// any callable: (std::span<const std::string>) -> double. A single symbol whose own
// estimate is >= cap is still emitted alone (caller decides whether to pull it).
// Order-preserving; the union of batches equals the input.
template <class EstFn>
[[nodiscard]] std::vector<std::vector<std::string>>
split_under_cap(std::span<const std::string> symbols, double cap, EstFn&& est) {
  std::vector<std::vector<std::string>> out;
  if (symbols.empty()) {
    return out;
  }
  if (symbols.size() == 1 || est(symbols) < cap) {
    out.emplace_back(symbols.begin(), symbols.end());
    return out;
  }
  const std::size_t mid = symbols.size() / 2;
  auto left = split_under_cap(symbols.first(mid), cap, est);
  auto right = split_under_cap(symbols.subspan(mid), cap, est);
  out.insert(out.end(), std::make_move_iterator(left.begin()),
             std::make_move_iterator(left.end()));
  out.insert(out.end(), std::make_move_iterator(right.begin()),
             std::make_move_iterator(right.end()));
  return out;
}

// Free MetadataGetCost (no egress, no charge). schema/stype_in are databento schema
// and symbology strings, e.g. "cbbo-1m" / "raw_symbol".
[[nodiscard]] Result<double> estimate_cost(
    std::string_view api_key, std::string_view dataset,
    const std::pair<std::string, std::string>& range_utc,
    std::span<const std::string> symbols, std::string_view schema,
    std::string_view stype_in);

// Equity L1 ("bbo-1m" or "cbbo-1m") for `symbols` over [start,end), stype_in
// "raw_symbol". Writes one Parquet file at out_path with columns:
// ts, symbol, bid_px, ask_px, bid_sz, ask_sz  (px = 1e-9 fixed-point i64; unset px
// stored as INT64_MIN; sizes i64). Splits symbols so every API call's preflight
// cost < cap_usd; accumulates all batches in memory; writes the file once.
[[nodiscard]] Result<PullStats> pull_equity_l1_1m_to_parquet(
    std::string_view api_key, std::string_view dataset,
    std::span<const std::string> symbols,
    const std::pair<std::string, std::string>& range_utc, std::string_view schema,
    std::string_view out_path, double cap_usd = 2.0);

} // namespace atx::external::databento
