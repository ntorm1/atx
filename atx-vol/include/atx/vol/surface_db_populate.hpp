#pragma once

// surface_db_populate — the "fit + store" pipeline stage: fit OPRA boards
// (CorpusBoard, corpus.hpp) into a SurfaceDb, honoring each symbol's
// SymbolFitConfig from the db manifest, and store one partition per date.
//
// This is deliberately NOT `build_corpus` pointed at a SurfaceDb: the ONLY
// fit-policy difference from a plain corpus build is that EVERY board's fit
// inputs are overlaid with the symbol's manifest config (`apply_symbol_config`)
// before the fit runs, so a per-symbol operator override (a pinned curve, a
// tighter band_k, ...) reaches the actual fit — see populate_surface_db's
// doc-comment below. The board -> PricedSurface fit itself reuses the exact
// same blessed path `build_corpus` runs (src/corpus_board_fit.{hpp,cpp});
// this header/impl adds only the per-symbol config resolution, date grouping,
// uid stamping, and SurfaceDb partition writes on top.

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"     // CorpusBoard
#include "atx/vol/run_report.hpp" // MetaKv
#include "atx/vol/surface_db.hpp" // SurfaceDb, SymbolFitConfig
#include "atx/vol/types.hpp"      // Result, Status

namespace atx::vol {

struct SurfaceDbPopulateConfig {
  // Base fit inputs (preset etc). Per-symbol SymbolFitConfig from the db
  // manifest is overlaid via apply_symbol_config; a symbol absent from the
  // manifest uses `fallback` unchanged.
  SymbolFitConfig fallback{};
  unsigned n_threads{0};          // 0 = serial; determinism must hold regardless
  bool skip_existing{true};       // date key already in db.partitions() -> skip whole date
};

struct PopulateSymbolStats {
  std::string symbol;
  std::uint32_t n_attempted{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_disabled{0};    // skipped because manifest enabled=false
  // Mean fit-quality score over successful fits, when the shared corpus fit
  // path yields one (oos_in_band from curve selection; see corpus.cpp's
  // CorpusEntry.oos_in_band recording). NaN when unavailable (e.g. the
  // pinned-curve path has no OOS score — mirrors corpus.cpp).
  double mean_oos_in_band{std::numeric_limits<double>::quiet_NaN()};
};

struct SurfaceDbPopulateStats {
  std::uint32_t n_boards{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_dates_written{0};
  std::uint32_t n_dates_skipped_existing{0};
  std::vector<PopulateSymbolStats> per_symbol;   // sorted by symbol
};

// Fit every board and store one partition per distinct board date (key =
// date). Boards are grouped by date; within a date, boards fit in symbol
// order (deterministic). A board whose symbol's manifest config has
// enabled=false is skipped (n_disabled). A board whose fit fails records
// n_failed and does NOT abort the date (document per-name failures, don't
// silently drop). A date with zero successful fits writes NO partition.
// Partition write uses SurfaceArchiveItem{symbol, &surface} with owning
// symbol-string storage kept alive across the call.
// Top-level Err only on: empty boards span, db write errors, or a date key
// the db rejects.
[[nodiscard]] Result<SurfaceDbPopulateStats>
populate_surface_db(SurfaceDb &db, std::span<const CorpusBoard> boards,
                    const SurfaceDbPopulateConfig &cfg = {});

// Stats file for the report: meta (caller's, plus n_boards/n_ok/n_failed/
// n_dates_written appended), header
// "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band",
// one row per symbol (success_rate = n_ok / max(1, n_attempted - n_disabled),
// %.10g; mean_oos_in_band prints "nan" when NaN).
[[nodiscard]] Status write_populate_stats_csv(const SurfaceDbPopulateStats &s,
                                              const MetaKv &meta,
                                              std::string_view path);

}  // namespace atx::vol
