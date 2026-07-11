// corpus_build_bench.cpp — Google Benchmark throughput for build_corpus.
//
// Times build_corpus over a 20-board synthetic corpus: 10 dates x 2 symbols,
// where "SPY" pins the ConvexDense index recipe and "XOM" auto-selects the eSSVI
// backbone on its smooth truth — a genuine mix of BOTH curve families through
// the fan-out build path. Relocated from the old
// TEST(Corpus, Throughput_FitsUnderCeiling) wall-clock ceiling (corpus_test.cpp):
// a throughput number belongs in a benchmark, not a gated test.
//
// The 20 boards are generated ONCE, OUTSIDE the timed loop; only build_corpus is
// timed. build_corpus writes each per-date archive via a temp-file + rename, so
// re-running it into the same output directory across iterations overwrites
// cleanly (no per-iteration filesystem teardown inside the timed region). The
// board-generation helpers are copied from tests/corpus_test.cpp with the gtest
// assertions dropped (this TU does not link gtest).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/corpus.hpp"      // build_corpus, CorpusBoard, CorpusManifest
#include "atx/vol/data.hpp"        // iso_to_ns, year_fraction
#include "atx/vol/market_env.hpp"  // MarketEnv
#include "atx/vol/panel.hpp"       // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/s3.hpp"          // S3Params
#include "atx/vol/spy_fixture.hpp" // make_spy_synthetic_spec
#include "atx/vol/vol_curve.hpp"   // CurveConfig, VolCurveKind

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

namespace fs = std::filesystem;

// A fresh unique output directory for the bench (removed if it lingers). Reused
// across iterations: build_corpus overwrites each archive in place.
[[nodiscard]] fs::path fresh_out_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-corpus-bench-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// The ConvexDense "index recipe" pin: the arb-free dense fit. Exercises the dense
// (variable-length node blob) archive path.
[[nodiscard]] CurveConfig convex_dense_pin() {
  CurveConfig c;
  c.kind = VolCurveKind::ConvexDense;
  c.convex.node_cap = 40;
  return c;
}

// A penny-dense INDEX board: the canonical SPY fixture, rescaled to `spot` and
// re-tagged, at valuation date `snapshot`. Pinned it fits ConvexDense.
[[nodiscard]] SynthPanelSpec make_index_spec(const std::string &uid, const std::string &snapshot,
                                             double spot) {
  SynthPanelSpec s = make_spy_synthetic_spec(snapshot);
  s.uid = uid;
  const double scale = spot / s.spot;
  s.spot = spot;
  for (double &k : s.strikes) {
    k *= scale;
  }
  return s;
}

// A wider-spread, single-name-style board (moderate ladder, higher vol, wide
// two-sided markets). On the default policy it auto-selects the eSSVI backbone.
[[nodiscard]] SynthPanelSpec make_singlename_spec(const std::string &uid,
                                                  const std::string &snapshot, double spot) {
  SynthPanelSpec s;
  s.uid = uid;
  s.snapshot_iso = snapshot;
  s.spot = spot;
  s.r = 0.043;
  s.borrow = 0.0;

  struct Row {
    const char *iso;
    double sigma0;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-07-17", 0.36, -0.55, 0.6},
      {"2026-08-21", 0.33, -0.52, 0.7},
      {"2026-09-18", 0.31, -0.50, 0.8},
      {"2026-12-18", 0.29, -0.46, 0.9},
  };
  for (const Row &r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(snapshot, r.iso);
    const double s2 = 2.0 * std::sqrt(e.T) * r.skew_k;
    e.truth = S3Params{r.sigma0, s2, r.c2};
    s.expiries.push_back(e);
  }
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

// Materialize a CorpusBoard from a spec (copies the frame; builds the env). An
// optional per-board curve pin is carried onto the board. Returns std::nullopt if
// the synthetic panel build fails (a programming error for these known-good specs).
[[nodiscard]] std::optional<CorpusBoard> board_from_spec(const SynthPanelSpec &spec,
                                                         std::string date, std::string symbol,
                                                         std::optional<CurveConfig> curve = std::nullopt) {
  auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    return std::nullopt;
  }
  CorpusBoard b;
  b.date = std::move(date);
  b.symbol = std::move(symbol);
  b.frame = panel->frame;
  b.env = MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs);
  b.curve = std::move(curve);
  return b;
}

// The 20-board corpus: 2 symbols per date. "SPY" pins ConvexDense (dense index
// recipe); "XOM" auto-selects (=> eSSVI on the smooth truth). Returns an empty
// vector on any panel build failure.
[[nodiscard]] std::vector<CorpusBoard> make_mixed_boards(const std::vector<std::string> &dates) {
  std::vector<CorpusBoard> boards;
  boards.reserve(dates.size() * 2u);
  for (const std::string &d : dates) {
    std::optional<CorpusBoard> spy =
        board_from_spec(make_index_spec("SPY", d, 600.0), d, "SPY", convex_dense_pin());
    std::optional<CorpusBoard> xom =
        board_from_spec(make_singlename_spec("XOM", d, 110.0), d, "XOM");
    if (!spy.has_value() || !xom.has_value()) {
      return {};
    }
    boards.push_back(std::move(*spy));
    boards.push_back(std::move(*xom));
  }
  return boards;
}

constexpr std::size_t kBoards = 20u; // 10 dates x 2 symbols

void BM_CorpusBuild(benchmark::State &state) {
  // 10 dates x 2 symbols = 20 boards. Snapshots are all before the earliest
  // listed expiry so every year-fraction is positive.
  std::vector<std::string> dates;
  for (int day = 8; day <= 17; ++day) {
    char date[16];
    std::snprintf(date, sizeof date, "2026-06-%02d", day);
    dates.emplace_back(date);
  }
  const std::vector<CorpusBoard> boards = make_mixed_boards(dates);
  if (boards.size() != kBoards) {
    state.SkipWithError("synthetic board generation failed");
    return;
  }

  const fs::path out = fresh_out_dir("throughput");
  for (auto _ : state) {
    auto man = build_corpus(boards, out.string()); // fan-out across boards
    if (!man.has_value()) {
      state.SkipWithError("build_corpus failed");
      break;
    }
    benchmark::DoNotOptimize(man->n_ok);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBoards));
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("corpus/build_20boards", BM_CorpusBuild))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
