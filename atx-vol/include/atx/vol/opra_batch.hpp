#pragma once

// P2-4 date-range batch OPRA loader + the term-curve -> MarketEnv bridge.
//
// A multi-symbol x multi-date driver over OPRA cbbo-1m Parquet files ALREADY on
// disk. It does not fetch: it enumerates every calendar date in an inclusive
// [date_lo, date_hi] range, substitutes a path template per (symbol, date), and
// dispatches each existing file through the P2-2 single-symbol loader
// `load_opra_cbbo_parquet` (so every file is validated as one underlier and, when
// term pillars are supplied, gets the P2-3 per-expiry rate curve).
//
// Missing files (weekends, holidays, un-pulled days) are NON-fatal: each yields an
// `Err(ErrorCode::NotFound)` entry and bumps `n_missing`. A file that exists but
// fails to load bumps `n_error`. The whole batch is `Ok` unless the SPEC itself is
// malformed (empty symbols, reversed/date-order, unparseable dates, mismatched
// pillar arrays) — those are the only top-level `Err`s.
//
// ## The term-curve -> MarketEnv bridge (`market_env_from_frame`)
//
// A loaded `QuoteFrame` carries the date's yield-curve pillars (real term pillars
// when the caller supplied them, else the flat {T=1, r} pillar). `market_env_from_frame`
// lowers a frame to the `MarketEnv` a corpus fit (P3) hands to
// `OptionChain::from_frame(frame, env)`, so each date's chain picks up its OWN
// short rate at the front expiry instead of a hardcoded flat 1y rate — the
// multi-date front-rate corruption P2-3 targets. See the function contract below.
//
// ## Ownership / thread-safety
//
// Pure functions over by-value RAII aggregates (Rule of Zero). `load_opra_daterange`
// borrows the parquet files under `spec.root_dir` for the duration of the call and
// returns an owning result. The per-(symbol, date) reads fan out over
// `spec.n_threads` workers (W4.3): each cell writes only its own pre-sized result
// slot, and the parquet read path holds no shared mutable state (distinct files,
// a fresh per-call Arrow reader), so the result is identical for any thread count.
// The optional progress callback is invoked synchronously on the calling thread in
// a serial post-join pass (monotonic `done`), so callers still see one in-order
// call per cell.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"       // CorpusBoard
#include "atx/vol/data.hpp"         // QuoteFrame
#include "atx/vol/market_env.hpp"   // MarketEnv
#include "atx/vol/opra_panel.hpp"   // OpraPanel, OpraLoadSpec
#include "atx/vol/types.hpp"        // Result

namespace atx::vol {

enum class MissingMarketInputPolicy : std::uint8_t {
  UseFallback = 0,
  Quarantine = 1,
  Error = 2,
};

// Point-in-time external inputs for exactly one canonical (date, symbol) cell.
// The table constructor canonicalizes/sorts keys, validates term pillars and
// rejects future as-of tags before any OPRA file is opened.
struct CorpusMarketInputCell {
  std::string date{};
  std::string symbol{};
  std::optional<double> spot_override{};
  std::vector<double> yc_pillar_t{};
  std::vector<double> yc_pillar_r{};
  std::vector<DividendEvent> cash_divs{};
  FitContext fit_context{};
  OpraMarketInputProvenance provenance{};
};

class CorpusMarketInputTable {
public:
  CorpusMarketInputTable() = default;

  [[nodiscard]] static Result<CorpusMarketInputTable>
  create(std::vector<CorpusMarketInputCell> cells);

  [[nodiscard]] const CorpusMarketInputCell *find(std::string_view date,
                                                  std::string_view symbol) const;
  [[nodiscard]] std::span<const CorpusMarketInputCell> cells() const noexcept { return cells_; }
  [[nodiscard]] std::uint64_t fingerprint() const noexcept { return fingerprint_; }

private:
  std::vector<CorpusMarketInputCell> cells_{};
  std::uint64_t fingerprint_{0};
};

// Spec for `load_opra_daterange`.
struct OpraBatchSpec {
  std::vector<std::string> symbols;   // underliers to load (one file per symbol/date)
  std::string date_lo;                // "YYYY-MM-DD" inclusive lower bound
  std::string date_hi;                // "YYYY-MM-DD" inclusive upper bound
  std::string root_dir;               // base dir holding the parquet tree

  // Relative path under `root_dir`, with "{symbol}" and "{date}" substituted
  // (date = "YYYY-MM-DD"). E.g. "XOM/2026-06-01.parquet".
  std::string path_template = "{symbol}/{date}.parquet";

  // The per-file snapshot stamp is `date + snapshot_suffix`, passed verbatim as
  // `OpraLoadSpec.snapshot_iso` (drives the frame's snapshot_iso and the
  // year-fractions the loader computes). Default = the 19:55Z pre-close minute.
  std::string snapshot_suffix = "T19:55:00Z";

  double r = 0.0;                     // fallback flat continuously-compounded rate

  // Optional term-structure pillars applied to EVERY file (P2-3): two or more
  // (strictly ascending) year-fraction pillars build a `YieldCurve` queried at
  // each expiry's own maturity. Empty (default) or one pillar => flat `r`. The
  // two arrays must be equal length (a mismatch is a malformed-spec top-level Err).
  std::vector<double> yc_pillar_t;
  std::vector<double> yc_pillar_r;
  CorpusMarketInputTable market_inputs{};
  MissingMarketInputPolicy missing_market_inputs{MissingMarketInputPolicy::UseFallback};
  OpraProvenanceMode provenance_mode{OpraProvenanceMode::Compatibility};

  // Per-file load fan-out (W4.3). 0 = auto (hardware concurrency), 1 = serial.
  // The (symbol, date) cells are loaded over a dynamic worker queue; each cell
  // writes only its own pre-sized result slot (disjoint) after pure reads of the
  // shared spec, so the batch result is IDENTICAL for any value (each read hits a
  // distinct parquet file, and read_parquet / LazyParquet hold no shared mutable
  // state). Progress + counters are aggregated deterministically in a serial
  // post-join pass, so `progress`'s monotonic `done` contract holds.
  unsigned n_threads{0};
};

// One (symbol, date) cell of a batch. `panel` is `Ok` on a successful load,
// `Err(ErrorCode::NotFound)` when the file is absent, or `Err(...)` (the loader's
// own error) when it exists but fails to load.
struct OpraBatchEntry {
  std::string symbol;                 // the requested underlier
  std::string date;                   // "YYYY-MM-DD"
  std::string path;                   // resolved parquet path (root_dir + template)

  // Epoch-ns of `date + snapshot_suffix`, from the memoized `iso_to_ns` parse the
  // batch shares across the symbols of a date (see load_opra_daterange). Populated
  // even for a missing file (there is no frame to read it from otherwise).
  std::int64_t snapshot_ts_ns = 0;
  bool used_market_input_fallback{false};
  std::uint64_t market_input_fingerprint{0};

  // This cell is a COVERAGE HOLE, not a defect: the date's file is present and
  // readable, and this symbol is simply absent from it (a sparse universe, not a
  // corrupt hive). Set only by `load_opra_hive`, which classifies it structurally
  // from the file's own distinct-underlying set — `load_opra_daterange` (the v1
  // per-(symbol,date) layout) has no such case and never sets it. The entry's
  // `panel` still carries the same zero-match `Err` it always did, so this is a
  // pure ANNOTATION: it changes no error, code, or message.
  bool coverage_hole{false};

  Result<OpraPanel> panel;            // Ok | Err(NotFound) | Err(load failure)
};

// Aggregate result of a date-range batch. `entries` are ordered date-major then
// symbol-major (every date's row of symbols in ascending calendar order). The
// counters partition `entries`: n_loaded + n_missing + n_error == n_total.
struct OpraBatchResult {
  std::vector<OpraBatchEntry> entries;
  std::size_t n_total = 0;            // symbols.size() * (#dates in range)
  std::size_t n_loaded = 0;          // panel.has_value()
  std::size_t n_missing = 0;         // Err(NotFound) — file absent (non-fatal)
  std::size_t n_error = 0;           // Err(...) — file present but load failed
  // How many of `n_error` are `entry.coverage_hole` — a present, readable date
  // file that simply does not carry that symbol. A SUB-COUNT of `n_error`, never
  // a fourth bucket: the partition n_loaded + n_missing + n_error == n_total
  // still holds, and `n_error - n_coverage_holes` is the count of cells that
  // failed for a real defect (unreadable/unparseable file, wrong schema,
  // quarantined market inputs). Only `load_opra_hive` ever raises it.
  std::size_t n_coverage_holes = 0;
};

// Progress sink: called once per (symbol, date) AFTER its entry is appended, with
// the running `done` (1..total, monotonic) and a reference to the just-added entry.
using OpraBatchProgress =
    std::function<void(std::size_t done, std::size_t total, const OpraBatchEntry& entry)>;

// Load every (symbol, date) in `spec` over the inclusive [date_lo, date_hi] range.
//
// Behavior:
//   * Dates are enumerated with a self-contained civil-date kernel (days-from-civil
//     / Howard-Hinnant), so no external date library is pulled in.
//   * Each existing file is loaded single-symbol via `load_opra_cbbo_parquet` with
//     `OpraLoadSpec{ .path, .underlying=symbol, .snapshot_iso=date+snapshot_suffix,
//     .r, .yc_pillar_t, .yc_pillar_r }` — so P2-2 multi-symbol validation and the
//     P2-3 term curve apply per file.
//   * A missing file is non-fatal: `Err(NotFound)` + `n_missing`. A present file
//     that fails to load: `Err(...)` + `n_error`. A success: `Ok` + `n_loaded`.
//   * `progress` (if set) fires after each file.
//
// Parse caching: the `date+snapshot_suffix -> ts_ns` (`iso_to_ns`) parse is memoized
// in a local map, so the M distinct snapshot stamps are each parsed once rather than
// once per (symbol, date). The per-contract OSI symbol parse stays INSIDE the
// per-file loader (its signature is deliberately unchanged); this batch layer adds
// no contract-level caching.
//
// @return the full result (Ok even when some/all files are missing) unless the spec
//         is malformed: `InvalidArgument` for empty `symbols`, a `date_hi` before
//         `date_lo`, an unparseable `date_lo`/`date_hi`, or a `yc_pillar_t`/`_r`
//         length mismatch.
[[nodiscard]] Result<OpraBatchResult>
load_opra_daterange(const OpraBatchSpec& spec, const OpraBatchProgress& progress = {});

// Build the `MarketEnv` a fit consumes from a loaded `QuoteFrame` — the bridge that
// makes the frame's term pillars non-hollow.
//
// Decision:
//   * >= 2 pillars (equal-length, and `YieldCurve::create` succeeds) => carry a term
//     `YieldCurve`; `env.rate_at(T)` then interpolates the real short rate for this
//     date at each maturity. `flat_rate` is set to the first pillar as the T<=0
//     fallback.
//   * otherwise (0 or 1 pillar, or a create failure) => a FLAT env at
//     `yc_pillar_r.front()` (or 0 when there are no pillars). A single pillar does
//     NOT interpolate flat, so — matching the loader — it is treated as the flat
//     rate rather than built into a curve.
// `spot`, `now_ns`, and `cash_divs` are copied straight from the frame.
//
// Scope: this fixes FRONT-RATE SELECTION — the fitter still lowers the env with one
// representative rate at the front expiry (`chain.r_repr_`), so the per-expiry rate
// curve threaded all the way into the carry remains a documented enhancement. What
// this bridge fixes is that the representative rate is now each date's real
// `rate_at(front_T)` instead of a hardcoded flat 1y rate — the multi-date corruption
// P2-3 targets.
[[nodiscard]] MarketEnv market_env_from_frame(const QuoteFrame& frame);

// Lossless bridge into corpus fitting: the frame supplies spot/rates/dividends,
// the panel supplies FitContext and strict source-provenance state.
[[nodiscard]] CorpusBoard corpus_board_from_opra(std::string date, std::string symbol,
                                                 OpraPanel panel);

} // namespace atx::vol
