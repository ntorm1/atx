// databento_cost.cpp — price a historical pull BEFORE downloading any data.
//
// Every call here is a Metadata endpoint, which is FREE: no outbound data bytes,
// so no usage charge and no draw on your data credits (unlike TimeseriesGetRange /
// BatchSubmitJob, which the client header explicitly warns "will incur a cost").
//
// Use it to verify whether a pull is absorbed by your subscription/credits ($0 or
// within credit) or whether it falls into pay-as-you-go usage cost.
//
// Usage: databento_cost [START] [END] [DATASET] [SYMBOL...]
//   defaults: 2025-06-05 2026-06-05 XNAS.ITCH AAPL   (schema = ohlcv-1m)
// Key from the DATABENTO_API_KEY environment variable (see .env).

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include <databento/datetime.hpp>    // DateTimeRange
#include <databento/enums.hpp>       // Schema, FeedMode, ToString
#include <databento/historical.hpp>  // Historical
#include <databento/metadata.hpp>    // DatasetRange, UnitPricesForMode

int main(int argc, char** argv) {
  using namespace databento;

  const std::string start = argc > 1 ? argv[1] : "2025-06-05";
  const std::string end = argc > 2 ? argv[2] : "2026-06-05";
  const std::string dataset = argc > 3 ? argv[3] : "XNAS.ITCH";
  std::vector<std::string> symbols;
  for (int i = 4; i < argc; ++i) {
    symbols.emplace_back(argv[i]);
  }
  if (symbols.empty()) {
    symbols = {"AAPL"};
  }
  constexpr Schema kSchema = Schema::Ohlcv1M;

  try {
    auto client = Historical::Builder().SetKeyFromEnv().Build();
    const DateTimeRange<std::string> range{start, end};

    std::printf("Query: %s %s  [%s, %s)  symbols=", dataset.c_str(),
                ToString(kSchema), start.c_str(), end.c_str());
    for (const auto& s : symbols) {
      std::printf("%s ", s.c_str());
    }
    std::printf("\n\n");

    // Available history window for this schema — confirms whether your range falls
    // inside the subscription's included history.
    const auto dr = client.MetadataGetDatasetRange(dataset);
    const auto rng = dr.range_by_schema.find(kSchema);
    if (rng != dr.range_by_schema.end()) {
      std::printf("Available %s history: %s .. %s\n", ToString(kSchema),
                  rng->second.start.c_str(), rng->second.end.c_str());
    } else {
      std::printf("Dataset available: %s .. %s\n", dr.start.c_str(), dr.end.c_str());
    }

    const auto records =
        client.MetadataGetRecordCount(dataset, range, symbols, kSchema);
    const auto bytes =
        client.MetadataGetBillableSize(dataset, range, symbols, kSchema);
    const auto cost = client.MetadataGetCost(dataset, range, symbols, kSchema);

    std::printf("Records:        %llu\n", static_cast<unsigned long long>(records));
    std::printf("Billable bytes: %llu (%.3f MB)\n",
                static_cast<unsigned long long>(bytes),
                static_cast<double>(bytes) / 1e6);
    std::printf("Usage cost:     $%.6f\n\n", cost);

    std::printf("Unit prices ($/GB) for %s by mode:\n", ToString(kSchema));
    for (const auto& upm : client.MetadataListUnitPrices(dataset)) {
      const auto p = upm.unit_prices.find(kSchema);
      if (p != upm.unit_prices.end()) {
        std::printf("  %-20s $%.4f\n", ToString(upm.mode), p->second);
      }
    }

    std::printf(
        "\nUsage cost = metered pay-as-you-go price for this exact pull.\n"
        "  $0      -> free / within subscription scope.\n"
        "  > $0    -> draws your data credits first, then bills as overage.\n"
        "Cross-check remaining credits + plan inclusions in the billing portal\n"
        "(databento.com -> Portal -> Billing). Metadata calls above were free.\n");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "databento error: %s\n", e.what());
    return 1;
  }
}
