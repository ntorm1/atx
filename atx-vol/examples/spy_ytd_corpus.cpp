// spy_ytd_corpus.cpp — build a REAL SPY surface corpus from a pulled OPRA parquet
// hive (databento_bulk_opra output) via the blessed fit path.
//
//   load_opra_daterange (per-date QuoteFrame; weekends/holidays = NotFound, skipped)
//     -> CorpusBoard{date, "SPY", frame, market_env_from_frame(frame)}
//     -> build_corpus (OptionChain::from_frame -> PricerFitter::fit ->
//        to_priced_surface, fanned out across dates)
//     -> <out>/<date>.atxvsa  +  <out>/manifest.tsv
//
// The manifest this writes is exactly what the SPY strangle backtest consumes
// (spy_strangle_backtest <manifest>). OFF by default (ATX_BUILD_EXAMPLES).
//
//   spy_ytd_corpus [--opra DIR] [--out DIR] [--start YYYY-MM-DD] [--end YYYY-MM-DD] [--r RATE]

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/corpus.hpp"      // CorpusBoard, CorpusConfig, build_corpus, CorpusManifest
#include "atx/vol/opra_batch.hpp"  // OpraBatchSpec, load_opra_daterange, market_env_from_frame
#include "atx/vol/session.hpp"     // FitPreset
#include "atx/vol/types.hpp"       // Result
#include "atx/vol/vol_curve.hpp"   // CurveConfig, VolCurveKind (pin the curve family)

using namespace atx::vol;

int main(int argc, char** argv) {
  std::string opra_root = "data/spy_ytd/opra";
  std::string out_dir = "data/spy_ytd/archives";
  std::string start = "2026-01-02";
  std::string end = "2026-07-02";
  double r = 0.043;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--opra") {
      opra_root = nv();
    } else if (a == "--out") {
      out_dir = nv();
    } else if (a == "--start") {
      start = nv();
    } else if (a == "--end") {
      end = nv();
    } else if (a == "--r") {
      r = std::strtod(nv(), nullptr);
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }

  // ── Load the parquet hive into per-date QuoteFrames ───────────────────────
  OpraBatchSpec spec;
  spec.symbols = {"SPY"};
  spec.date_lo = start;
  spec.date_hi = end;
  spec.root_dir = opra_root;
  spec.r = r;  // snapshot_suffix + path_template keep their 19:55Z / {symbol}/{date} defaults

  const Result<OpraBatchResult> batch = load_opra_daterange(spec);
  if (!batch) {
    std::fprintf(stderr, "load_opra_daterange: %s\n", batch.error().to_string().c_str());
    return 1;
  }
  std::printf("[opra] loaded=%zu missing=%zu error=%zu of %zu (%s..%s)\n", batch->n_loaded,
              batch->n_missing, batch->n_error, batch->n_total, start.c_str(), end.c_str());

  // ── Frames -> CorpusBoards (skip missing/failed cells) ────────────────────
  std::vector<CorpusBoard> boards;
  boards.reserve(batch->n_loaded);
  for (const OpraBatchEntry& e : batch->entries) {
    if (!e.panel) {
      continue;  // NotFound (weekend/holiday) or a load failure — non-fatal
    }
    CorpusBoard b;
    b.date = e.date;
    b.symbol = "SPY";
    b.frame = e.panel->frame;
    b.env = market_env_from_frame(e.panel->frame);
    boards.push_back(std::move(b));
  }
  std::printf("[corpus] fitting %zu real SPY boards -> %s\n", boards.size(), out_dir.c_str());
  if (boards.empty()) {
    std::fprintf(stderr, "no loadable boards under %s — did the pull run?\n", opra_root.c_str());
    return 1;
  }

  // ── Fit -> ATXVSA archives + manifest (Fast preset; parallel across dates) ─
  CorpusConfig cfg;
  cfg.fit_template.preset = FitPreset::Fast;  // matches spy_real_test's headline fit
  // Pin the curve family: penny-dense SPY boards deterministically auto-select
  // ConvexDense (node_cap 40 — the CurveConfig default), so leaving `curve` unset
  // makes every board pay the selector's duplicate cold de-Americanization (two
  // candidate fits + held-out cold repricings) only to re-derive ConvexDense. Pinning
  // it drops that entire per-board pass; the served surface is unchanged (SPY was
  // already served ConvexDense). ~2x faster corpus build on this hive.
  cfg.fit_template.curve = CurveConfig{};  // {VolCurveKind::ConvexDense, node_cap 40}
  const Result<CorpusManifest> man = build_corpus(boards, out_dir, cfg);
  if (!man) {
    std::fprintf(stderr, "build_corpus: %s\n", man.error().to_string().c_str());
    return 1;
  }
  std::printf("[corpus] n_boards=%u n_ok=%u n_failed=%u n_skipped=%u\n", man->n_boards, man->n_ok,
              man->n_failed, man->n_skipped);
  std::printf("[corpus] manifest: %s/manifest.tsv  (feed to: spy_strangle_backtest %s/manifest.tsv)\n",
              out_dir.c_str(), out_dir.c_str());
  return 0;
}
