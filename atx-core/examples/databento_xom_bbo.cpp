// databento_xom_bbo.cpp — full XOM OPRA option-chain L1 (top-of-book) snapshot for a
// single minute, stored to disk as DBN.
//
// Uses the cbbo-1m schema (consolidated BBO subsampled once per minute). OPRA is a
// consolidated SIP feed, so its L1 schema is CONSOLIDATED BBO (cbbo-*, not bbo-*).
// It emits top-of-book bid/ask for EVERY quoted contract regardless of whether it
// traded (unlike ohlcv-1m, which only produces a bar when a trade prints). This is the
// right schema for a full-chain quote snapshot. Full chain via parent symbology
// "XOM.OPT" / SType::Parent.
//
// COST-GATED: OPRA historical is billed per symbol-record and a full chain is hundreds
// of records, so the FREE Metadata cost endpoints are called first and the paid pull is
// REFUSED above a cap (default $2, override with the 4th arg).
//
// Usage: databento_xom_bbo [START_UTC] [END_UTC] [OUT_FILE] [COST_CAP_USD]
//   defaults: 2026-06-05T19:55:00  2026-06-05T19:56:00
//             data/xom_opra_bbo1m_2026-06-05T1955Z.dbn.zst   2.0
//   NOTE: 19:55:00Z == 15:55 America/New_York on 2026-06-05 (June -> EDT, UTC-4).
//   Key from the DATABENTO_API_KEY environment variable (see .env).

#include <cstdint>
#include <cstdio>
#include <cstdlib>  // atof
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <date/date.h>  // year_month_day for the symbol map

#include <databento/constants.hpp>   // dataset::kOpraPillar, kUndefPrice
#include <databento/datetime.hpp>    // DateTimeRange
#include <databento/dbn.hpp>         // Metadata
#include <databento/dbn_store.hpp>   // DbnStore
#include <databento/enums.hpp>       // Schema, SType, ToString
#include <databento/historical.hpp>  // Historical
#include <databento/record.hpp>      // Record, CbboMsg, ConsolidatedBidAskPair
#include <databento/symbol_map.hpp>  // PitSymbolMap

namespace {
// Format a fixed-point price (1e-9) as dollars, or "       --" if unset.
std::string Px(std::int64_t px) {
  if (px == databento::kUndefPrice) {
    return std::string("      --");
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%8.2f", static_cast<double>(px) * 1e-9);
  return std::string(buf);
}
}  // namespace

int main(int argc, char** argv) {
  using namespace databento;

  const std::string start = argc > 1 ? argv[1] : "2026-06-05T19:55:00";
  const std::string end = argc > 2 ? argv[2] : "2026-06-05T19:56:00";
  const std::filesystem::path out =
      argc > 3 ? std::filesystem::path{argv[3]}
               : std::filesystem::path{"data/xom_opra_cbbo1m_2026-06-05T1955Z.dbn.zst"};
  const double cap = argc > 4 ? std::atof(argv[4]) : 2.0;

  const std::string dataset = dataset::kOpraPillar;
  const std::vector<std::string> symbols{"XOM.OPT"};
  constexpr Schema kSchema = Schema::Cbbo1M;
  constexpr SType kStypeIn = SType::Parent;
  constexpr SType kStypeOut = SType::InstrumentId;

  try {
    auto client = Historical::Builder().SetKeyFromEnv().Build();
    const DateTimeRange<std::string> range{start, end};

    std::printf("Query: %s %s  [%s, %s)  parent=%s\n\n", dataset.c_str(),
                ToString(kSchema), start.c_str(), end.c_str(), symbols.front().c_str());

    // ---- FREE preflight ----
    const auto records =
        client.MetadataGetRecordCount(dataset, range, symbols, kSchema, kStypeIn, 0);
    const auto bytes =
        client.MetadataGetBillableSize(dataset, range, symbols, kSchema, kStypeIn, 0);
    const auto cost =
        client.MetadataGetCost(dataset, range, symbols, kSchema, kStypeIn, 0);
    std::printf("Preflight (free): records=%llu  billable=%.3f MB  usage_cost=$%.6f\n",
                static_cast<unsigned long long>(records),
                static_cast<double>(bytes) / 1e6, cost);

    if (records == 0) {
      std::printf("No BBO records in window. The bbo-1m sample may fall on the minute "
                  "boundary; try widening, e.g. END = %s + 1 min.\n", end.c_str());
      return 2;
    }
    if (cost > cap) {
      std::printf(
          "ABORT: estimated cost $%.6f exceeds cap $%.2f. No data pulled.\n"
          "Re-run with a higher cap as the 4th arg to force the download.\n",
          cost, cap);
      return 3;
    }

    // ---- PAID download to disk ----
    if (out.has_parent_path()) {
      std::filesystem::create_directories(out.parent_path());
    }
    std::printf("Cost within cap. Downloading to %s ...\n", out.string().c_str());
    auto store = client.TimeseriesGetRangeToFile(dataset, range, symbols, kSchema,
                                                 kStypeIn, kStypeOut, 0, out);

    // ---- Replay, mapping instrument_id -> option symbol ----
    const auto& md = store.GetMetadata();
    const date::year_month_day day = date::year{2026} / 6 / 5;  // UTC index date
    PitSymbolMap syms = md.CreateSymbolMapForDate(day);

    std::printf("\n%-26s %8s %8s %8s  %8s x %-8s\n", "contract", "bid", "ask", "mid",
                "bidsz", "asksz");
    std::uint64_t rows = 0;
    for (const Record* rec = store.NextRecord(); rec != nullptr;
         rec = store.NextRecord()) {
      if (!rec->Holds<CbboMsg>()) {
        continue;
      }
      const auto& q = rec->Get<CbboMsg>();
      const ConsolidatedBidAskPair& l = q.levels[0];
      const auto it = syms.Find(q.hd.instrument_id);
      const std::string sym = it != syms.Map().end()
                                  ? it->second
                                  : std::to_string(q.hd.instrument_id);
      std::string mid = "      --";
      if (l.bid_px != kUndefPrice && l.ask_px != kUndefPrice) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%8.2f",
                      (static_cast<double>(l.bid_px) + static_cast<double>(l.ask_px)) *
                          0.5 * 1e-9);
        mid = buf;
      }
      if (rows < 40) {
        std::printf("%-26s %s %s %s  %8u x %-8u\n", sym.c_str(), Px(l.bid_px).c_str(),
                    Px(l.ask_px).c_str(), mid.c_str(), l.bid_sz, l.ask_sz);
      }
      ++rows;
    }

    std::error_code ec;
    const auto sz = std::filesystem::file_size(out, ec);
    std::printf("\nStored %llu BBO quotes across the XOM chain.\nFile: %s (%llu bytes)\n",
                static_cast<unsigned long long>(rows), out.string().c_str(),
                static_cast<unsigned long long>(ec ? 0 : sz));
    return rows > 0 ? 0 : 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "databento error: %s\n", e.what());
    return 1;
  }
}
