#pragma once

// surface_db_build — auto-generation of a SurfaceDb's per-symbol fit MANIFEST
// from a set of loaded OPRA boards, part 1: `generate_symbol_configs`.
//
// Where `surface_db_populate.hpp` FITS boards into partitions, this stage decides
// HOW each symbol should be fit and writes that decision into the manifest's
// symbol table (`SurfaceDb::upsert_symbol`) BEFORE any populate runs. For each
// symbol it takes one representative board, classifies it through the shared
// atx-vol fit-policy seam (`select_fit_policy`, optionally the full held-out
// `select_curve` search), and stores a `SymbolFitConfig` whose curve family is
// pinned to the policy's choice. A symbol whose board cannot be selected on is
// stored DISABLED (fail closed — never silently served), the top-level call
// still succeeding so one bad board never sinks a universe build.
//
// ── Ownership / thread-safety ────────────────────────────────────────────────
//
// `generate_symbol_configs` borrows `db` (mutated via its own serialized
// `upsert_symbol`) and `boards` (read-only) for the call and returns an owning
// report. It is itself single-threaded: it walks symbols in canonical order and
// upserts one at a time, so re-running it is deterministic. Not safe to call
// concurrently with other mutators on the SAME `db` (they share the manifest
// mutex, but interleaved upserts would race the skip-existing/idempotence
// bookkeeping this stage owns).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/corpus.hpp"     // CorpusBoard
#include "atx/vol/surface_db.hpp" // SurfaceDb, SymbolFitConfig, FitPreset
#include "atx/vol/types.hpp"      // Result

namespace atx::vol {

// How to auto-generate the manifest configs. Field names/defaults are
// contractual (Task 5's build driver constructs this directly).
struct AutoConfigSpec {
  // Board date used for per-symbol selection. Empty ("") => each symbol's
  // EARLIEST available board (min date string) is used.
  std::string config_date{};
  // Base preset captured into every symbol's config (`symbol_config_from_preset`).
  // The fit-policy decision pins the curve FAMILY; this preset is the numerical
  // tier and is retained (matching `populate_universe_streaming`'s seeding).
  FitPreset preset{FitPreset::Populate};
  // The designated index leg — pinned to the dense index recipe (shared with
  // `populate_universe_streaming` via `seed_symbol_config`), bypassing per-board
  // selection. Empty => no index leg.
  std::string index_symbol{};
  // false (default) => per-symbol selection uses `select_fit_policy` board
  // features only (O(quotes), no fit). true => additionally run the full
  // held-out `select_curve` OOS search and pin its winner; a NotFound/Unavailable
  // selector outcome falls back to the fit-policy decision.
  bool deep_selection{false};
  // false (default) => a symbol already present in the manifest is left
  // UNTOUCHED (idempotent resume; operator overrides win). true => overwrite it.
  bool overwrite_existing{false};
};

// What `generate_symbol_configs` did. The three disposition counters partition
// the distinct symbols it saw: `n_configured + n_skipped_existing +
// n_disabled_failed == n_symbols`.
struct AutoConfigReport {
  std::uint32_t n_symbols{0};          // distinct symbols in `boards`
  std::uint32_t n_configured{0};       // freshly configured (or overwritten), enabled
  std::uint32_t n_skipped_existing{0}; // already present, left untouched (not overwrite)
  std::uint32_t n_disabled_failed{0};  // selection failed -> stored disabled
  std::vector<std::string> failed_symbols; // sorted; mirrored as disabled configs in the db
};

// Auto-generate one `SymbolFitConfig` per distinct symbol in `boards` and upsert
// it into `db`'s manifest.
//
// Per symbol (canonical order): pick the config board (`spec.config_date` or the
// symbol's earliest); if already configured and not `spec.overwrite_existing`,
// SKIP it (`n_skipped_existing`); if it is `spec.index_symbol`, store the dense
// index recipe (`seed_symbol_config`); otherwise build the board's `OptionChain`
// (`OptionChain::from_frame`, the corpus_board_fit path), classify it with
// `select_fit_policy`, and store `symbol_config_from_preset(spec.preset)` with
// the policy's curve pinned (`pin_curve = true`, `curve = decision.curve`). When
// `spec.deep_selection`, `select_curve`'s held-out winner is pinned instead,
// falling back to the fit-policy decision on a NotFound/Unavailable selector
// outcome. A symbol whose board fails to build a selectable underlying — or whose
// deep selection fails with a hard (non-fallback) error, or throws — is stored
// DISABLED (`symbol_config_from_preset(spec.preset)` with `enabled = false`) and
// recorded in `failed_symbols` (`n_disabled_failed`); the call still succeeds.
//
// @return the disposition report, or an Error only on a db write failure
//         (`upsert_symbol` IoError/ParseError propagated). An empty `boards`
//         span yields an all-zero report (Ok), not an error.
[[nodiscard]] Result<AutoConfigReport>
generate_symbol_configs(SurfaceDb &db, std::span<const CorpusBoard> boards,
                        const AutoConfigSpec &spec);

} // namespace atx::vol
