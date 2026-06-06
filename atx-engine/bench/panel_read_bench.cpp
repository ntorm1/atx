// atx::engine::alpha — panel read throughput: zero-copy shm Panel vs an owned
// Panel built by copying the same field blocks out of the mapping. Reports
// items/s over cells so the ratio is the copy cost the zero-copy path avoids.
//
// Both benchmarks attach (open + mmap + validate) the SAME on-disk segment
// per-iteration, so that fixed cost cancels out. The measured delta is then
// purely: borrow-spans + O(cells) universe materialization (zero-copy, no f64
// copy) vs a full f64 memcpy of every field block + owned Panel::create.

#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp"

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {

// Build a dense F=5 OHLCV segment of `dates` x `instruments` once, return path.
std::string build_seg(atx::usize dates, atx::usize instruments) {
  const std::vector<std::string> fnames{"close", "open", "high", "low", "volume"};
  std::vector<std::string> syms(instruments);
  for (atx::usize i = 0; i < instruments; ++i) {
    syms[i] = "S" + std::to_string(i);
  }
  std::vector<atx::i64> axis(dates);
  for (atx::usize d = 0; d < dates; ++d) {
    axis[d] = static_cast<atx::i64>((d + 1) * 100);
  }
  atx::tsdb::SegmentBuilder b(fnames, syms, axis);
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      for (atx::u32 f = 0; f < 5; ++f) {
        b.set(f, d, static_cast<atx::u32>(i), static_cast<atx::f64>(d + i + f + 1));
      }
    }
  }
  // A fixed path in the temp dir (bench is single-process; overwrite each run).
  const std::string path = std::string("atx_panel_bench.atxseg");
  (void)b.write(path, 0);
  return path;
}

void BM_AttachZeroCopy(benchmark::State &state) {
  const auto dates = static_cast<atx::usize>(state.range(0));
  const auto inst = static_cast<atx::usize>(state.range(1));
  const std::string path = build_seg(dates, inst); // build once, outside the hot loop
  for (auto _ : state) {
    auto mp = atx::engine::alpha::attach_segment_panel(path);
    benchmark::DoNotOptimize(mp->panel().field_all(0).data());
  }
  state.SetItemsProcessed(static_cast<atx::i64>(state.iterations() * dates * inst * 5));
}

void BM_MemcpyOwned(benchmark::State &state) {
  const auto dates = static_cast<atx::usize>(state.range(0));
  const auto inst = static_cast<atx::usize>(state.range(1));
  const std::string path = build_seg(dates, inst); // build once, outside the hot loop
  for (auto _ : state) {
    auto reader =
        atx::tsdb::SegmentReader::attach(path); // attach per-iter (same as zero-copy path)
    std::vector<std::vector<atx::f64>> cols(reader->field_count());
    for (atx::u32 f = 0; f < reader->field_count(); ++f) {
      const auto block = reader->field_block_view(f);
      cols[f].assign(block.begin(), block.end()); // the copy the zero-copy path avoids
    }
    auto p = atx::engine::alpha::Panel::create(
        dates, inst, {"close", "open", "high", "low", "volume"}, std::move(cols), {});
    benchmark::DoNotOptimize(p->field_all(0).data());
  }
  state.SetItemsProcessed(static_cast<atx::i64>(state.iterations() * dates * inst * 5));
}

} // namespace

BENCHMARK(BM_AttachZeroCopy)->Args({2520, 500})->Args({5040, 3000});
BENCHMARK(BM_MemcpyOwned)->Args({2520, 500})->Args({5040, 3000});
