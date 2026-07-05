// opra_dbn_to_parquet.cpp — OFFLINE converter: cached OPRA cbbo-1m (NBBO) DBN -> Parquet.
//
// Reads a Databento OPRA cbbo-1m (consolidated BBO subsampled once per minute) DBN file
// that was ALREADY downloaded to disk (e.g. by databento_xom_bbo) and re-writes it as a
// Parquet file in the EXACT schema produced by
// `atx::external::databento::pull_opra_cbbo_1m_to_parquet`. This lets downstream tools
// consume real option-chain NBBO data with ZERO new API spend: no network calls, no cost.
//
// It is a pure local transform — DbnFileStore replays the cached records, the metadata's
// PitSymbolMap resolves instrument_id -> OSI/OCC symbol, and the rows are handed to
// atx::core::io::write_parquet. The output schema matches the paid pull byte-for-byte so
// the two are interchangeable to consumers.
//
// Usage: opra_dbn_to_parquet [IN.dbn.zst] [OUT.parquet] [INDEX_DATE=YYYY-MM-DD]
//   defaults: data/xom_opra_cbbo1m_2026-06-05T1955Z.dbn.zst
//             data/xom_opra_cbbo1m_2026-06-05T1955Z.parquet
//             2026-06-05
//   INDEX_DATE is the UTC date used to build the instrument_id->symbol map for the file.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>   // printf, fprintf
#include <exception>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <date/date.h>  // year_month_day for the symbol map

#include <databento/constants.hpp>       // kUndefPrice
#include <databento/datetime.hpp>        // UnixNanos
#include <databento/dbn.hpp>             // Metadata
#include <databento/dbn_file_store.hpp>  // DbnFileStore (alias of DbnStore)
#include <databento/enums.hpp>           // ToString
#include <databento/record.hpp>          // Record, CbboMsg, ConsolidatedBidAskPair
#include <databento/symbol_map.hpp>      // PitSymbolMap

#include "atx/core/datetime.hpp"           // time::Timestamp
#include "atx/core/io/parquet_writer.hpp"  // WriteColumn, write_parquet

namespace {

using Timestamp = atx::core::time::Timestamp;

// Unset fixed-point price sentinel, matching pull_opra_cbbo_1m_to_parquet's px_or_unset.
constexpr std::int64_t kUnsetPx = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] std::int64_t px_or_unset(std::int64_t px) {
  return px == databento::kUndefPrice ? kUnsetPx : px;
}

// OSI root = leading capital letters of an option symbol,
// e.g. "XOM   260619C00110000" -> "XOM". Replicated verbatim from the pull.
[[nodiscard]] std::string osi_root(std::string_view osi) {
  std::size_t i = 0;
  while (i < osi.size() && osi[i] >= 'A' && osi[i] <= 'Z') {
    ++i;
  }
  return std::string{osi.substr(0, i)};
}

}  // namespace

int main(int argc, char** argv) {
  using namespace databento;

  const std::filesystem::path in =
      argc > 1 ? std::filesystem::path{argv[1]}
               : std::filesystem::path{"data/xom_opra_cbbo1m_2026-06-05T1955Z.dbn.zst"};
  const std::filesystem::path out =
      argc > 2 ? std::filesystem::path{argv[2]}
               : std::filesystem::path{"data/xom_opra_cbbo1m_2026-06-05T1955Z.parquet"};
  const std::string index_date = argc > 3 ? argv[3] : "2026-06-05";

  int y = 0;
  int mo = 0;
  int d = 0;
  {
    const std::string_view sv{index_date};
    const bool ok =
        sv.size() == 10 && sv[4] == '-' && sv[7] == '-' &&
        std::from_chars(sv.data(), sv.data() + 4, y).ec == std::errc{} &&
        std::from_chars(sv.data() + 5, sv.data() + 7, mo).ec == std::errc{} &&
        std::from_chars(sv.data() + 8, sv.data() + 10, d).ec == std::errc{};
    if (!ok) {
      std::fprintf(stderr, "bad INDEX_DATE '%s' (expected YYYY-MM-DD)\n", index_date.c_str());
      return 1;
    }
  }
  const date::year_month_day day = date::year{y} / mo / d;

  try {
    std::printf("Reading %s  (index date %s, OPRA cbbo-1m NBBO)\n", in.string().c_str(),
                index_date.c_str());

    // ---- Open the cached DBN file directly (no download, no network) ----
    DbnFileStore store{in};

    // ---- Build instrument_id -> OSI symbol map for the UTC index date ----
    const auto& md = store.GetMetadata();
    PitSymbolMap syms = md.CreateSymbolMapForDate(day);

    // ---- Parallel column buffers (borrowed by WriteColumn spans below) ----
    std::vector<Timestamp> ts;
    std::vector<std::string> underlying;
    std::vector<std::string> symbol;
    std::vector<std::int64_t> bid_px;
    std::vector<std::int64_t> ask_px;
    std::vector<std::int64_t> bid_sz;
    std::vector<std::int64_t> ask_sz;
    std::unordered_set<std::string> underlyings_seen;

    // ---- Replay records, mapping instrument_id -> option symbol ----
    for (const Record* rec = store.NextRecord(); rec != nullptr; rec = store.NextRecord()) {
      if (!rec->Holds<CbboMsg>()) {
        continue;
      }
      const auto& q = rec->Get<CbboMsg>();
      const ConsolidatedBidAskPair& l = q.levels[0];

      const auto it = syms.Find(q.hd.instrument_id);
      const bool mapped = it != syms.Map().end();
      const std::string sym = mapped ? it->second : std::to_string(q.hd.instrument_id);
      const std::string root = mapped ? osi_root(sym) : sym;

      ts.push_back(Timestamp::from_unix_nanos(
          static_cast<std::int64_t>(q.ts_recv.time_since_epoch().count())));
      underlying.push_back(root);
      symbol.push_back(sym);
      bid_px.push_back(px_or_unset(l.bid_px));
      ask_px.push_back(px_or_unset(l.ask_px));
      bid_sz.push_back(static_cast<std::int64_t>(l.bid_sz));
      ask_sz.push_back(static_cast<std::int64_t>(l.ask_sz));
      underlyings_seen.insert(root);
    }

    const std::uint64_t rows = static_cast<std::uint64_t>(ts.size());
    if (rows == 0) {
      std::fprintf(stderr, "No CbboMsg records found in %s\n", in.string().c_str());
      return 2;
    }

    // ---- Write Parquet in the exact pull_opra_cbbo_1m_to_parquet schema ----
    if (out.has_parent_path()) {
      std::filesystem::create_directories(out.parent_path());
    }
    const std::vector<atx::core::io::WriteColumn> cols{
        {"ts", std::span<const Timestamp>(ts)},
        {"underlying", std::span<const std::string>(underlying)},
        {"symbol", std::span<const std::string>(symbol)},
        {"bid_px", std::span<const std::int64_t>(bid_px)},
        {"ask_px", std::span<const std::int64_t>(ask_px)},
        {"bid_sz", std::span<const std::int64_t>(bid_sz)},
        {"ask_sz", std::span<const std::int64_t>(ask_sz)},
    };
    const std::string out_str = out.string();
    auto w = atx::core::io::write_parquet(cols, out_str);
    if (!w.has_value()) {
      std::fprintf(stderr, "write_parquet failed: %s\n", w.error().to_string().c_str());
      return 1;
    }

    std::error_code ec;
    const auto sz = std::filesystem::file_size(out, ec);
    std::printf(
        "Wrote %llu rows across %llu underlyings.\nFile: %s (%llu bytes)\n",
        static_cast<unsigned long long>(rows),
        static_cast<unsigned long long>(underlyings_seen.size()), out.string().c_str(),
        static_cast<unsigned long long>(ec ? 0 : sz));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "opra_dbn_to_parquet error: %s\n", e.what());
    return 1;
  }
}
