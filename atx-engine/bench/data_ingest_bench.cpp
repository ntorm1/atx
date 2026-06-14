// data_ingest_bench.cpp — S6.9: ingest / align / lowering throughput on a synthetic
// catalog. Measures the cold-path data-plane operations the S6 BYO layer performs
// once per backtest window:
//
//   * BM_Ingest         — Dataset::create from raw column arrays (the ingest step).
//   * BM_AlignOnto      — align_onto(price, feature): the PIT (date x instrument)
//                         join rail onto the canonical axis.
//   * BM_PriceToPanel   — price_to_panel(price): the raw OHLCV/close lowering into
//                         an alpha::Panel (the with_datafields augmentation path).
//   * BM_PricePanelPath — the SHIPPED product surface S6 adds: register a price
//                         Dataset in a DatasetCatalog -> DataContext::create ->
//                         price_panel(). This is what users actually call (distinct
//                         from the raw price_to_panel above), so it captures the
//                         catalog register + context build + lazy lowering as one
//                         end-to-end cost. A fresh catalog + context are built INSIDE
//                         the timed loop (price_panel is lazy+cached, so a reused
//                         context would only lower once).
//
// NOTE (repo convention): this is a Debug / clang-cl build. The absolute ns/op
// figures here are UPPER BOUNDS, NOT release numbers — do not quote them as release
// performance. The bench is a relative / regression instrument, not a perf gate; it
// does not ASSERT (it is a benchmark).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"

#include "atx/engine/data/adapt_panel.hpp"
#include "atx/engine/data/align.hpp"
#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/context.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace {

using atx::f64;
using atx::u16;
using atx::usize;
using atx::engine::alpha::Panel;
using atx::engine::data::align_onto;
using atx::engine::data::AlignedView;
using atx::engine::data::ColumnDType;
using atx::engine::data::DataContext;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetCatalog;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::price_to_panel;
using atx::engine::data::Role;

constexpr usize kDates = 512; // a representative ingest window
constexpr usize kInsts = 64;

// A fixed-seed synthetic price column (no <random>, never clocked — deterministic).
[[nodiscard]] std::vector<f64> synth_close(std::uint64_t seed) {
  std::vector<f64> close(kDates * kInsts);
  std::uint64_t st = seed | 1ULL;
  auto nx = [&st] {
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(st >> 11) / static_cast<f64>(1ULL << 53);
  };
  for (usize i = 0; i < kInsts; ++i) {
    f64 px = 100.0;
    for (usize t = 0; t < kDates; ++t) {
      px *= 1.0 + 0.01 * (nx() - 0.5);
      close[t * kInsts + i] = px;
    }
  }
  return close;
}

[[nodiscard]] std::vector<DateKey> ascending_dates() {
  std::vector<DateKey> d(kDates);
  for (usize t = 0; t < kDates; ++t) {
    d[t] = static_cast<DateKey>(t);
  }
  return d;
}

[[nodiscard]] std::vector<InstKey> instrument_axis() {
  std::vector<InstKey> insts(kInsts);
  for (usize i = 0; i < kInsts; ++i) {
    insts[i] = static_cast<InstKey>(i);
  }
  return insts;
}

// A full OHLCV price Dataset over the synthetic axis (price_to_panel needs close +
// volume, and high/low when vwap is absent).
[[nodiscard]] Dataset make_price_dataset() {
  const std::vector<f64> close = synth_close(0xA11Cu);
  DatasetSchema s;
  s.columns = {"open", "high", "low", "close", "volume"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
              ColumnDType::F64};
  s.role = Role::Price;
  std::vector<std::vector<f64>> data(5, std::vector<f64>(kDates * kInsts, 0.0));
  for (usize k = 0; k < kDates * kInsts; ++k) {
    const f64 c = close[k];
    data[0][k] = c;
    data[1][k] = c * 1.01;
    data[2][k] = c * 0.99;
    data[3][k] = c;
    data[4][k] = 1000.0;
  }
  auto r = Dataset::create(std::move(s), ascending_dates(), instrument_axis(), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"bench:prices", ""});
  return std::move(r).value(); // synthetic input is always coherent
}

// A one-column feature Dataset on the same axis (for the align bench).
[[nodiscard]] Dataset make_feature_dataset() {
  DatasetSchema s;
  s.columns = {"sentiment"};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Feature;
  std::vector<std::vector<f64>> data = {synth_close(0xBEEFu)};
  auto r = Dataset::create(std::move(s), ascending_dates(), instrument_axis(), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"bench:feature", ""});
  return std::move(r).value();
}

// ---------------------------------------------------------------------------
//  Benches.
// ---------------------------------------------------------------------------

void BM_Ingest(benchmark::State &state) {
  const std::vector<f64> close = synth_close(0xA11Cu);
  for (auto _ : state) {
    DatasetSchema s;
    s.columns = {"close"};
    s.dtypes = {ColumnDType::F64};
    s.role = Role::Price;
    std::vector<std::vector<f64>> data = {close};
    auto r = Dataset::create(std::move(s), ascending_dates(), instrument_axis(), std::move(data),
                             /*mask=*/{}, DatasetProvenance{"bench", ""});
    benchmark::DoNotOptimize(r);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kDates * kInsts));
}
BENCHMARK(BM_Ingest)->Unit(benchmark::kMicrosecond);

void BM_AlignOnto(benchmark::State &state) {
  const Dataset price = make_price_dataset();
  const Dataset feature = make_feature_dataset();
  for (auto _ : state) {
    auto r = align_onto(price, feature);
    benchmark::DoNotOptimize(r);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kDates * kInsts));
}
BENCHMARK(BM_AlignOnto)->Unit(benchmark::kMicrosecond);

void BM_PriceToPanel(benchmark::State &state) {
  const Dataset price = make_price_dataset();
  const std::vector<u16> adv{};
  for (auto _ : state) {
    auto r = price_to_panel(price, std::span<const u16>{adv});
    benchmark::DoNotOptimize(r);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kDates * kInsts));
}
BENCHMARK(BM_PriceToPanel)->Unit(benchmark::kMicrosecond);

// The SHIPPED product path: register a price Dataset -> DataContext::create ->
// price_panel(). This is what users actually call to drive the engine from a catalog
// (distinct from the raw price_to_panel above), so it captures the catalog register +
// context build + lazy lowering as one end-to-end cost. A fresh catalog + context are
// built INSIDE the timed loop because price_panel() is lazy+cached — a reused context
// would lower exactly once and the loop would then time a no-op cache hit. (Debug
// upper-bound only — see the file header.)
void BM_PricePanelPath(benchmark::State &state) {
  for (auto _ : state) {
    DatasetCatalog catalog;
    (void)catalog.register_dataset("prices", make_price_dataset());
    auto ctx = DataContext::create(catalog, "prices"); // empty adv_windows => raw lowering
    if (ctx.has_value()) {
      auto panel = ctx->price_panel(); // lazy lowering happens here (first call)
      benchmark::DoNotOptimize(panel);
    }
    benchmark::DoNotOptimize(catalog);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kDates * kInsts));
}
BENCHMARK(BM_PricePanelPath)->Unit(benchmark::kMicrosecond);

} // namespace
