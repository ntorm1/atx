#pragma once

// Corpus — a whole-panel archive builder: fit MANY (date, symbol) boards in
// parallel and lay the fitted surfaces out as ONE `SurfaceArchive` per date,
// indexed by a single deterministic manifest.
//
// A research/production universe is not one board — it is a grid of (date,
// symbol) volatility surfaces (a "corpus"). Each board is fit through the ONE
// blessed atx-vol path (OptionChain::from_frame -> PricerFitter::fit ->
// VolaSession::to_priced_surface), the curve family AUTO-SELECTED per board when
// the fit template leaves `PricerConfig::curve` unset (a penny-dense index board
// picks ConvexDense; a sparse single-name board picks the parsimonious eSSVI
// backbone — see curve_selector.hpp). The fits fan out ACROSS boards (each board
// itself fit single-threaded); the surfaces of one date are packed into that
// date's `SurfaceArchive` file, and a manifest indexes the whole corpus.
//
// ## Determinism
//
// The build is deterministic by construction, independent of the worker count:
// a single-threaded pre-pass fixes a stable board order, each worker owns its
// own fit scratch and writes a DISJOINT result slot (never a shared index), and
// the manifest entries are sorted (date asc, symbol asc) before any output. Two
// runs — at any thread count — produce the same manifest and the same per-date
// surfaces (fitting is a pure function of the board), so `map_symbol` reloads a
// surface that reprices bit-for-bit.
//
// ## Ownership / thread-safety
//
// `build_corpus` owns every intermediate (the move-only `PricedSurface`s live in
// a stable container for the duration of each per-date archive write). It is a
// self-contained call: pass boards + an output directory, receive the in-memory
// manifest and the on-disk archives. The manifest value types are plain
// aggregates (Rule of Zero); serialize / parse are pure functions of their input.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/data.hpp"             // QuoteFrame
#include "atx/vol/market_env.hpp"       // MarketEnv
#include "atx/vol/pricer_fitter.hpp"    // PricerConfig
#include "atx/vol/surface_archive.hpp"  // SurfaceArchiveWriteOpts
#include "atx/vol/types.hpp"            // Result, Status, ErrorCode
#include "atx/vol/vol_curve.hpp"        // VolCurveKind

namespace atx::vol {

// ── One board to fit ────────────────────────────────────────────────────────
//
// A single underlier's quote board tagged by valuation date + symbol. `date`
// groups boards into per-date archives (its exact string is the archive-file
// stem); `symbol` is the archive key (canonicalized inside the archive:
// ASCII-upper-cased, truncated to 32). `frame` + `env` are exactly what
// `OptionChain::from_frame` consumes.
struct CorpusBoard {
  std::string date;    // e.g. "2026-06-19"; groups boards into per-date archives
  std::string symbol;  // archive key (canonicalized inside the archive)
  QuoteFrame frame;    // the board's quotes
  MarketEnv env;       // spot / rate-curve / divs / valuation time for from_frame
  // Per-board curve override. std::nullopt (the default) => this board uses the
  // config's `fit_template.curve` policy (auto-select when that is also unset).
  // Set it to PIN a specific curve family for this board (e.g. the ConvexDense
  // index recipe), independent of the rest of the corpus.
  std::optional<CurveConfig> curve{};
};

// Per-board fit outcome, mirroring calib_pool.hpp's FitStatus but kept local so
// this header carries no calibrator-pool dependency (the corpus never drives
// calibrate_pool — see corpus.cpp for why).
enum class CorpusFitStatus : std::uint8_t {
  Ok = 0,       // fit + snapshot succeeded; the surface is in its date's archive
  Failed = 1,   // chain build / fit / snapshot failed (see `error_code`)
  Skipped = 2,  // the board had nothing fittable (empty frame)
};

[[nodiscard]] const char* to_string(CorpusFitStatus status) noexcept;

// ── Per-board manifest record ───────────────────────────────────────────────
//
// One row of the corpus index. `chosen_kind` / `n_slices` / `oos_in_band` are
// meaningful iff `status == Ok`; `error_code` iff `status == Failed`.
// `archive_path` names the per-date archive this surface was written to (empty
// for a non-Ok board that was not archived).
struct CorpusEntry {
  std::string date{};
  std::string symbol{};
  CorpusFitStatus status{CorpusFitStatus::Skipped};
  VolCurveKind chosen_kind{VolCurveKind::ConvexDense};  // iff Ok
  std::uint32_t n_slices{0};
  double oos_in_band{0.0};  // chosen candidate's OOS in-band (0 if curve pinned)
  ErrorCode error_code{ErrorCode::Unknown};  // iff Failed
  std::string archive_path{};                // the per-date archive (iff archived)

  [[nodiscard]] bool operator==(const CorpusEntry&) const = default;
};

// ── Whole-corpus index ──────────────────────────────────────────────────────
//
// `dates` are unique + ascending; `entries` are sorted (date asc, symbol asc) —
// fully deterministic. The aggregate counts sum over `entries`.
struct CorpusManifest {
  std::vector<std::string> dates{};    // ascending, unique
  std::vector<CorpusEntry> entries{};  // sorted (date asc, symbol asc)
  std::uint32_t n_boards{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_skipped{0};

  [[nodiscard]] bool operator==(const CorpusManifest&) const = default;
};

// ── Build policy ────────────────────────────────────────────────────────────
struct CorpusConfig {
  // The per-board fit template. `fit_template.curve` left std::nullopt (the
  // default) => the CurveSelector picks the family per board. `n_threads` on the
  // template is IGNORED — each board is fit single-threaded; parallelism is
  // across boards via `CorpusConfig::n_threads`.
  PricerConfig fit_template{};
  // Worker count for the ACROSS-board fan-out. 0 => hardware_concurrency (>= 1),
  // clamped to the board count.
  unsigned n_threads{0};
  // Options forwarded verbatim to every per-date archive write.
  SurfaceArchiveWriteOpts write_opts{};
};

// ── Driver ──────────────────────────────────────────────────────────────────

// Fit every board (parallel across boards, each board single-threaded), group
// by `date`, write ONE `SurfaceArchive` file per date into `out_dir`
// (`out_dir/<date>.atxvsa`), write the manifest (`out_dir/manifest.tsv`), and
// return the in-memory manifest. A date with zero Ok boards writes no archive
// (the archive writer rejects an empty item list); those boards are still
// recorded (Failed / Skipped). One failing board never sinks the corpus.
//
// Deterministic: identical manifest + surfaces across runs and thread counts.
//
// @return InvalidArgument if `boards` is empty or `out_dir` is empty; IoError
//         propagated from a per-date archive write or the manifest write; an
//         archive AlreadyExists (a duplicate canonical symbol within one date)
//         propagated.
[[nodiscard]] Result<CorpusManifest> build_corpus(std::span<const CorpusBoard> boards,
                                                  std::string_view out_dir,
                                                  const CorpusConfig& cfg = {});

// ── Manifest serialization (deterministic TSV) ──────────────────────────────

// Serialize `m` as deterministic TSV: a magic line, an aggregate-counts line, a
// dates line, a column-header line, then one tab-separated row per entry (in
// `entries` order). `oos_in_band` is written at full round-trip precision;
// enums as their integer value. `parse(serialize(m)) == m`.
[[nodiscard]] std::string serialize_manifest(const CorpusManifest& m);

// Parse the TSV produced by `serialize_manifest`.
//
// @return ParseError on a malformed document (bad magic, short line, a
//         non-numeric numeric field, or an unknown enum value).
[[nodiscard]] Result<CorpusManifest> parse_manifest(std::string_view tsv);

// Persist / load the manifest TSV. `write_manifest_file` creates the parent
// directory if missing (atomic temp-then-rename). Both add IoError on a
// filesystem failure; `read_manifest_file` adds NotFound for a missing file.
[[nodiscard]] Status write_manifest_file(std::string_view path, const CorpusManifest& m);
[[nodiscard]] Result<CorpusManifest> read_manifest_file(std::string_view path);

}  // namespace atx::vol
