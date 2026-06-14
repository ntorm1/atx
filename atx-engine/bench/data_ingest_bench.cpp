// data_ingest_bench.cpp — S6.9: ingest / align / lowering throughput on a synthetic
// catalog. Measures the cold-path data-plane operations the S6 BYO layer performs
// once per backtest window:
//
//   * BM_Ingest         — Dataset::create from raw column arrays (the ingest step).
//   * BM_AlignOnto      — align_onto(price, feature): the PIT (date x instrument)
//                         join rail onto the canonical axis.
//   * BM_PriceToPanel   — price_to_panel(price): the raw OHLCV/close lowering into
//                         an alpha::Panel (the with_datafields augmentation path).
//   * BM_PriceOnlyLower  — the steady-state price-only lowering: a fixed pre-built
//                         Dataset lowered repeatedly. The lowering allocates the
//                         Panel ONCE per call (the output Panel buffers) — there is
//                         NO hidden per-call growth beyond that one output (no
//                         catalog re-walk, no intermediate align). The bench reports
//                         a fixed bytes/iter via SetBytesProcessed so a regression
//                         that adds steady-state work would show up as throughput.
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
using atx::engine::data::Dataset;
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

// The steady-state price-only lowering: a FIXED pre-built Dataset lowered each call.
// price_to_panel allocates exactly the OUTPUT Panel per call (no catalog re-walk, no
// intermediate align) — so bytes/iter is constant and a regression that adds
// steady-state per-call work shows up as a throughput drop here. (Debug upper-bound
// only — see the file header.)
void BM_PriceOnlyLower(benchmark::State &state) {
  const Dataset price = make_price_dataset();
  const std::vector<u16> adv{};
  for (auto _ : state) {
    auto r = price_to_panel(price, std::span<const u16>{adv});
    benchmark::DoNotOptimize(r);
    benchmark::ClobberMemory();
  }
  // Fixed bytes/iter: the lowered price columns (a stable per-call footprint).
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(kDates * kInsts * sizeof(f64)));
}
BENCHMARK(BM_PriceOnlyLower)->Unit(benchmark::kMicrosecond);

} // namespace
