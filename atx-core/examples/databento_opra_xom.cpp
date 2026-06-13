// databento_opra_xom.cpp — pull the full XOM OPRA option-chain 1-minute OHLCV bars
// for a single minute and store them to disk as DBN.
//
// Full chain via parent symbology: symbol "XOM.OPT" with SType::Parent expands to
// every listed XOM option contract on OPRA. Output is written by the client straight
// to a zstd-compressed DBN file, then replayed back to print a sample.
//
// COST-GATED: OPRA historical is billed per symbol-record, so this first calls the
// FREE Metadata cost endpoints and REFUSES the paid download if the estimate exceeds
// a cap (default $2, override with the 4th arg). Only TimeseriesGetRangeToFile incurs
// a charge; everything before it is free.
//
// Usage: databento_opra_xom [START_UTC] [END_UTC] [OUT_FILE] [COST_CAP_USD]
//   defaults: 2026-06-05T19:55:00  2026-06-05T19:56:00
//             data/xom_opra_ohlcv1m_2026-06-05T1955Z.dbn.zst   2.0
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

#include <databento/constants.hpp>   // dataset::kOpraPillar
#include <databento/datetime.hpp>    // DateTimeRange
#include <databento/dbn.hpp>         // Metadata
#include <databento/dbn_store.hpp>   // DbnStore
#include <databento/enums.hpp>       // Schema, SType, ToString
#include <databento/historical.hpp>  // Historical
#include <databento/record.hpp>      // Record, OhlcvMsg
#include <databento/symbol_map.hpp>  // PitSymbolMap

int main(int argc, char** argv) {
  using namespace databento;

  const std::string start = argc > 1 ? argv[1] : "2026-06-05T19:55:00";
  const std::string end = argc > 2 ? argv[2] : "2026-06-05T19:56:00";
  const std::filesystem::path out =
      argc > 3 ? std::filesystem::path{argv[3]}
               : std::filesystem::path{"data/xom_opra_ohlcv1m_2026-06-05T1955Z.dbn.zst"};
  const double cap = argc > 4 ? std::atof(argv[4]) : 2.0;

  const std::string dataset = dataset::kOpraPillar;
  const std::vector<std::string> symbols{"XOM.OPT"};
  constexpr Schema kSchema = Schema::Ohlcv1M;
  constexpr SType kStypeIn = SType::Parent;
  constexpr SType kStypeOut = SType::InstrumentId;
  constexpr double kScale = 1e-9;  // fixed-point -> dollars

  try {
    auto client = Historical::Builder().SetKeyFromEnv().Build();
    const DateTimeRange<std::string> range{start, end};

    std::printf("Query: %s %s  [%s, %s)  parent=%s\n\n", dataset.c_str(),
                ToString(kSchema), start.c_str(), end.c_str(), symbols.front().c_str());

    // ---- FREE preflight (Metadata endpoints; no egress, no charge) ----
    const auto records =
        client.MetadataGetRecordCount(dataset, range, symbols, kSchema, kStypeIn, 0);
    const auto bytes =
        client.MetadataGetBillableSize(dataset, range, symbols, kSchema, kStypeIn, 0);
    const auto cost =
        client.MetadataGetCost(dataset, range, symbols, kSchema, kStypeIn, 0);
    std::printf("Preflight (free): records=%llu  billable=%.3f MB  usage_cost=$%.6f\n",
                static_cast<unsigned long long>(records),
                static_cast<double>(bytes) / 1e6, cost);

    if (cost > cap) {
      std::printf(
          "ABORT: estimated cost $%.6f exceeds cap $%.2f. No data pulled.\n"
          "Re-run with a higher cap as the 4th arg to force the download.\n",
          cost, cap);
      return 3;
    }

    // ---- PAID download straight to disk ----
    if (out.has_parent_path()) {
      std::filesystem::create_directories(out.parent_path());
    }
    std::printf("Cost within cap. Downloading to %s ...\n", out.string().c_str());
    auto store = client.TimeseriesGetRangeToFile(dataset, range, symbols, kSchema,
                                                 kStypeIn, kStypeOut, 0, out);

    // ---- Replay from the stored file, mapping instrument_id -> option symbol ----
    const auto& md = store.GetMetadata();
    const date::year_month_day day = date::year{2026} / 6 / 5;  // UTC index date
    PitSymbolMap syms = md.CreateSymbolMapForDate(day);

    std::printf("\nXOM chain OHLCV bars (sample):\n");
    std::uint64_t bars = 0;
    for (const Record* rec = store.NextRecord(); rec != nullptr;
         rec = store.NextRecord()) {
      if (!rec->Holds<OhlcvMsg>()) {
        continue;
      }
      const auto& b = rec->Get<OhlcvMsg>();
      const auto it = syms.Find(b.hd.instrument_id);
      const std::string sym = it != syms.Map().end()
                                  ? it->second
                                  : std::to_string(b.hd.instrument_id);
      if (bars < 25) {
        std::printf("  %-26s O=%.2f H=%.2f L=%.2f C=%.2f V=%llu\n", sym.c_str(),
                    static_cast<double>(b.open) * kScale,
                    static_cast<double>(b.high) * kScale,
                    static_cast<double>(b.low) * kScale,
                    static_cast<double>(b.close) * kScale,
                    static_cast<unsigned long long>(b.volume));
      }
      ++bars;
    }

    std::error_code ec;
    const auto sz = std::filesystem::file_size(out, ec);
    std::printf("\nStored %llu OHLCV bars across the XOM chain.\nFile: %s (%llu bytes)\n",
                static_cast<unsigned long long>(bars), out.string().c_str(),
                static_cast<unsigned long long>(ec ? 0 : sz));
    return bars > 0 ? 0 : 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "databento error: %s\n", e.what());
    return 1;
  }
}
