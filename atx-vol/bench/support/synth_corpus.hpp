#pragma once

// Data-free synthetic ARCHIVE CORPUS + Clock for the backtest-engine benchmarks.
//
// WHY THIS EXISTS (fixture-reuse note, same rationale as synth_book.hpp):
// `synth_book.hpp` builds an in-memory market — surfaces and a SurfaceSet — which
// is all the portfolio pricer needs. The BACKTEST engine needs something else: a
// `Clock` over per-date ATXVSA2 archives ON DISK, because `run_backtest` loads each
// step's snapshot through `MarketSnapshot::load`. The only builder of that shape
// lived inline in backtest_throughput_bench.cpp, so a second backtest benchmark had
// to either duplicate ~120 lines of archive-writing or grow inside that file. This
// header is that builder, lifted VERBATIM and parameterized.
//
// The alternative — `build_corpus` from corpus.cpp — is unusable here for the same
// reason synth_book.hpp gives: it de-Americanizes and FITS real boards through a
// VolaSession, needing data on disk and a full solve per board.
//
// CORPUS-COMPATIBILITY IS A CONTRACT OF THIS HEADER. backtest_throughput_bench.cpp
// has a committed baseline (bench/baselines/*-backtest-throughput.json), and that
// baseline is only comparable if the corpus under the benchmark is unchanged. The
// surface construction, spot/vol-bump progression, symbol naming ("U<i>"), date
// formatting ("2027-MM-DD"), manifest shape, and uid assignment below are therefore
// exactly what that file used before the extraction. Change any of them and you have
// silently invalidated a baseline, not refactored a fixture.
//
// HOW THAT WAS VERIFIED, and how it CANNOT be: hashing the generated .atxvsa files
// does not work. Two runs of the SAME binary produce different archive bytes — the
// header stamps `created_ts_ns` at write time, and excluding the 464-byte header
// still differs because blob padding to kArchiveBlobAlign is not zero-filled. So a
// before/after byte comparison here would be measuring the clock, not the fixture.
// The extraction was instead verified TEXTUALLY: each function body below was
// checked to be character-identical to the one it replaced after applying only the
// renames (make_surface -> synth_surface, write_archive -> write_synth_archive,
// make_manifest -> make_synth_manifest, build_corpus_impl -> build_synth_corpus,
// bench_fatal -> corpus_fatal, kR/kBaseNow/kDayNs -> kCorpus*), comments and
// whitespace normalized. The benchmark's own reported invariant (final_open_lots
// = 160 on the straddle case) is unchanged across the move.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"        // Clock
#include "atx/vol/corpus.hpp"          // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/surface_archive.hpp" // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Status
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

namespace atx::vol::bench {

inline constexpr double kCorpusRate = 0.043;
inline constexpr std::int64_t kCorpusBaseNow = 1700000000000000000LL;
inline constexpr std::int64_t kCorpusDayNs = 86400LL * 1000000000LL;

// Setup failures happen once, outside any timed region (static fixture init), so an
// abort is the honest response — a benchmark of an error return measures nothing.
[[noreturn]] inline void corpus_fatal(const std::string& msg) {
  std::fprintf(stderr, "FATAL(atx-vol bench synth_corpus): %s\n", msg.c_str());
  std::abort();
}

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.00]. Mirrors backtest_test's make_surface.
//
// THE SLICE GRID BOUNDS EVERY CALLER'S TENOR: a strategy whose clips age outside
// [0.05, 1.00] is extrapolating off the ends of this surface, so a benchmark must
// keep its tenor (and the residual maturity the oldest live clip reaches) inside
// that span or it stops measuring the intended path.
[[nodiscard]] inline PricedSurface synth_surface(std::uint32_t uid, double S, double fwd,
                                                 std::int64_t now_ts, double vol_bump = 0.0) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = fwd;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kCorpusRate * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kCorpusRate;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  if (!ps.has_value()) {
    corpus_fatal(ps.error().to_string());
  }
  return std::move(*ps);
}

// Write `items` (symbol -> surface) as one date's archive; return its path. ATXVSA2
// (v2) writer: MarketSnapshot::load is v2-only after the WS-S S4 clean break, so the
// backtest under test only accepts a v2 archive.
[[nodiscard]] inline std::string
write_synth_archive(const std::filesystem::path& dir, const std::string& date,
                    const std::vector<std::pair<std::string, const PricedSurface*>>& items) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  std::vector<SurfaceArchiveItem> its;
  its.reserve(items.size());
  for (const auto& [sym, ps] : items) {
    its.push_back(SurfaceArchiveItem{sym, ps});
  }
  const Status st = write_surface_archive_v2_file(path, its);
  if (!st.has_value()) {
    corpus_fatal(st.error().to_string());
  }
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows.
[[nodiscard]] inline CorpusManifest
make_synth_manifest(const std::vector<std::pair<std::string, std::string>>& date_paths) {
  CorpusManifest m;
  for (const auto& [date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "U0"; // first-Ok archive per date is all the clock needs
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

// Build `n_dates` per-date archives (each holding `n_names` distinct-uid synthetic
// surfaces) under a process-lifetime temp dir, and return the Clock over them.
//
// Call ONCE per fixture (a function-local static, the bench-suite convention): the
// archives are written to disk and re-read by every timed `run_backtest`, so the
// write must stay outside the timed loop. Give each fixture its OWN `dir_name` and
// `uid_base` so concurrent fixtures in one binary never collide.
[[nodiscard]] inline Clock build_synth_corpus(int n_dates, int n_names, std::uint32_t uid_base,
                                              const char* dir_name) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / dir_name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  std::vector<std::pair<std::string, std::string>> dp;
  dp.reserve(static_cast<std::size_t>(n_dates));
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kCorpusBaseNow + static_cast<std::int64_t>(d) * kCorpusDayNs;
    // Surfaces are kept alive in `owned` for the duration of the archive write.
    std::vector<PricedSurface> owned;
    owned.reserve(static_cast<std::size_t>(n_names));
    std::vector<std::pair<std::string, const PricedSurface*>> items;
    items.reserve(static_cast<std::size_t>(n_names));
    for (int u = 0; u < n_names; ++u) {
      const double S =
          (100.0 + 10.0 * static_cast<double>(u)) * (1.0 + 0.003 * static_cast<double>(d));
      const double vb = 0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(u);
      owned.push_back(synth_surface(uid_base + static_cast<std::uint32_t>(u), S, S, now, vb));
    }
    std::vector<std::string> syms;
    syms.reserve(static_cast<std::size_t>(n_names));
    for (int u = 0; u < n_names; ++u) {
      syms.push_back("U" + std::to_string(u));
      items.emplace_back(syms.back(), &owned[static_cast<std::size_t>(u)]);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "2027-%02d-%02d", 1 + d / 28, 1 + d % 28);
    dp.emplace_back(buf, write_synth_archive(dir, buf, items));
  }

  auto clock = Clock::from_manifest(make_synth_manifest(dp));
  if (!clock.has_value()) {
    corpus_fatal(clock.error().to_string());
  }
  if (clock->size() != static_cast<std::size_t>(n_dates)) {
    corpus_fatal("clock size mismatch");
  }
  return std::move(*clock);
}

} // namespace atx::vol::bench
