// Smoke test for the official Databento C++ client (third-party/databento-cpp).
//
// Pulls AAPL 1-minute OHLCV bars for 2026-06-05 from the Historical API and prints
// them. The API key is read from the DATABENTO_API_KEY environment variable (see the
// repo .env). Prices are Databento native 1e-9 fixed-point, scaled to dollars here.
//
// WARNING: TimeseriesGetRange hits the live API and incurs cost.

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include <databento/constants.hpp>   // dataset::kXnasItch
#include <databento/datetime.hpp>    // DateTimeRange, ToIso8601
#include <databento/enums.hpp>       // Schema
#include <databento/historical.hpp>  // Historical
#include <databento/record.hpp>      // Record, OhlcvMsg
#include <databento/timeseries.hpp>  // KeepGoing, RecordCallback

int main() {
  using namespace databento;
  try {
    auto client = Historical::Builder().SetKeyFromEnv().Build();

    constexpr double kScale = 1e-9;  // fixed-point -> dollars
    std::uint64_t bars = 0;

    // end is exclusive, so [2026-06-05, 2026-06-06) == the full UTC session day.
    client.TimeseriesGetRange(
        dataset::kXnasItch,
        DateTimeRange<std::string>{"2026-06-05", "2026-06-06"}, {"AAPL"},
        Schema::Ohlcv1M, [&](const Record& rec) {
          if (rec.Holds<OhlcvMsg>()) {
            const auto& bar = rec.Get<OhlcvMsg>();
            std::printf("%s  O=%.2f H=%.2f L=%.2f C=%.2f V=%llu\n",
                        ToIso8601(bar.hd.ts_event).c_str(),
                        static_cast<double>(bar.open) * kScale,
                        static_cast<double>(bar.high) * kScale,
                        static_cast<double>(bar.low) * kScale,
                        static_cast<double>(bar.close) * kScale,
                        static_cast<unsigned long long>(bar.volume));
            ++bars;
          }
          return KeepGoing::Continue;
        });

    std::printf("AAPL ohlcv-1m 2026-06-05 (XNAS.ITCH): %llu bars\n",
                static_cast<unsigned long long>(bars));
    return bars > 0 ? 0 : 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "databento error: %s\n", e.what());
    return 1;
  }
}
