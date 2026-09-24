#pragma once

// atx::engine::data — history panel helpers (legacy ORATS API names).
//
// orats_total_return_close: canonical total-return adjusted close for a single
// symbol from the historical archive. Computed pointwise as close*cumulReturnFactor;
// the source factor already includes corporate actions, including dividends.
// Canonical research open/high/low use the same factor. Invalid prices or factors
// are gaps (NaN), never silently replaced with zero or a factor of one.
//
// build_history_panel (S3-5): orchestrator that assembles a deterministic,
// digest-pinned alpha::Panel from the on-disk ORATS per-date partition.
// Multi-segment attach -> raw-price universe screen -> adjusted OHLC -> lineage/digest.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp"  // alpha::TimeWindow
#include "atx/engine/data/universe.hpp"         // UniverseConfig

namespace atx::engine::data {

// =========================================================================
//  Level basis — panel metadata for the level-basis lint (W0-D0, D-01).
// =========================================================================
//
// Every assembled field carries one of three level bases:
//   Raw           — an as-traded or as-published value (raw close, raw volume, raw
//                   dollar volume, market cap, counts, category codes). Its level on
//                   date t uses only information available on t, so comparing levels
//                   across instruments on one date is point-in-time clean.
//   AdjustedLevel — a price level multiplied by a snapshot backward adjustment factor
//                   (the TRI close and the research open/high/low/vwap). The factor at
//                   t already contains every split and dividend up to the snapshot
//                   date, so the LEVEL carries future corporate actions. Ratios of the
//                   same instrument's own series (returns) are clean; a cross-sectional
//                   operation on the level is a look-ahead and is a lint error (W2-A3).
//   Ratio         — a dimensionless quantity (returns, implied vols, short-interest
//                   ratios). Cross-sectional use is clean.
enum class LevelBasis : std::uint8_t {
  Raw = 1,
  AdjustedLevel = 2,
  Ratio = 3,
};

// Stable lower-case name of a basis ("raw" / "adjusted_level" / "ratio").
[[nodiscard]] constexpr std::string_view level_basis_name(LevelBasis basis) noexcept {
  switch (basis) {
  case LevelBasis::Raw:
    return "raw";
  case LevelBasis::AdjustedLevel:
    return "adjusted_level";
  case LevelBasis::Ratio:
    return "ratio";
  }
  return "unknown";
}

// Level basis of a history-panel field by NAME. Covers the twelve kHistField*
// fields, the Alpha101 augmentation (`returns`, `cap`, `IndClass.*`,
// `dollar_volume`, `vwap`, `adv{d}`), the opt-in IV/liquidity families, the FINRA
// short-interest fields and `regime_*` overlays. `dollar_volume`/`adv{d}` are Raw
// because with_alpha101_fields builds them from raw_close x volume by default
// (alpha::DollarVolumeBasis::RawCloseV2); `vwap` is the typical price on the close
// basis, so it is AdjustedLevel. Unknown names return nullopt — a lint must treat an
// untagged field as unknown, never as clean.
[[nodiscard]] std::optional<LevelBasis> history_field_level_basis(std::string_view name) noexcept;

// =========================================================================
//  Canonical assembled-Panel field order (digest hashes fields in THIS order).
// =========================================================================
inline constexpr std::string_view kHistFieldClose     = "close";       // = TRI (close*cumret)
inline constexpr std::string_view kHistFieldRawClose  = "raw_close";   // raw as-traded close
inline constexpr std::string_view kHistFieldVolume    = "volume";      // raw traded shares
inline constexpr std::string_view kHistFieldHigh      = "high";        // adjusted like close
inline constexpr std::string_view kHistFieldLow       = "low";         // adjusted like close
inline constexpr std::string_view kHistFieldOpen      = "open";        // adjusted like close
inline constexpr std::string_view kHistFieldMarketCap = "market_cap";
inline constexpr std::string_view kHistFieldSector    = "sector";
inline constexpr std::string_view kHistFieldEarnFlag  = "earnFlag";      // earnings-day flag
inline constexpr std::string_view kHistFieldAtmIv21   = "atmCenI_21d";   // ATM implied move, 21d
inline constexpr std::string_view kHistFieldAtmIv126  = "atmCenI_126d";  // ATM implied move, 126d
inline constexpr std::string_view kHistFieldEarnCnt5  = "nEarnCnt_5d";   // earnings count, 5d window

// =========================================================================
//  Configuration
// =========================================================================
struct HistoryDataConfig {
  std::string seg_dir;          // data/orats_history_1d
  alpha::TimeWindow window{};   // [start,end) trading dates (unix-nanos)
  UniverseConfig universe{};    // market-cap / ADV / sector / top-N screen
  // When true, drop instrument columns that are NEVER in-universe over the whole
  // window (after the universe screen) — a lossless tightening: those columns are
  // all-NaN-masked at eval anyway, so dropping them shrinks the panel (memory +
  // eval cost) without changing any in-universe signal. false ⇒ keep every column
  // (legacy; preserves the digest).
  bool compact_to_universe = false;
  // Point-in-time membership allow-list. When non-empty, instrument columns whose
  // securityID is absent from this set are marked never-in-universe before
  // compaction (a RESTRICTION only — it can never admit a name the screen rejected);
  // empty ⇒ off, and every code path is byte-identical to a build without it.
  // Order and duplicates are irrelevant: the build sorts and dedupes a copy.
  std::vector<atx::i64> allow_ids{};
};

// =========================================================================
//  Result
// =========================================================================
struct HistoryPanel {
  alpha::Panel panel;
  atx::u64 digest{};
  std::vector<std::string> lineage;
  // Exact stored axes for build_history_panel; session keys are source timestamps
  // (daily session labels), not data availability or execution instants.
  std::vector<atx::i64> session_keys{};
  std::vector<std::string> instrument_ids{}; // canonical positive i64 securityID strings
  // Each output column's index in the uncompacted first-seen source axis.
  std::vector<atx::usize> original_instrument_indices{};
  std::vector<std::string> source_segment_paths{}; // selected readers, lexical path order
  // Columns the HistoryDataConfig::allow_ids restriction forced out of the universe
  // on every date; 0 whenever the allow-list is empty (the restriction is off).
  atx::usize allow_list_excluded_columns{};
  // Level basis of each panel field, parallel to panel's FieldIds (field_basis[f]
  // tags field f). Filled by build_history_panel; empty only on the legacy numeric
  // no-op append path, whose callers use history_field_level_basis(name) instead.
  std::vector<LevelBasis> field_basis{};
};

// =========================================================================
//  orats_total_return_close (S3-3)
// =========================================================================

// Canonical total-return adjusted close = close * cum_return_factor pointwise.
// Dividends already enter the source factor; no extra cash dividend is added.
// Price, factor, and product must each be finite and strictly positive. Invalid
// cells become NaN; later valid cells resume independently, matching the adjustment
// layer's gap convention. Unequal input lengths return an empty vector.
[[nodiscard]] std::vector<atx::f64>
orats_total_return_close(std::span<const atx::f64> close,
                         std::span<const atx::f64> cum_return_factor);

// =========================================================================
//  build_history_panel (S3-5)
// =========================================================================

// Assemble a deterministic, digest-pinned real-data Panel from the on-disk ORATS
// partition: multi-segment attach -> S1 raw-price universe screen (market_cap =
// shares*raw_close, causal ADV, GICS sector, in_universe mask) -> adjusted OHLC
// (price*cumulReturnFactor) -> Catalog lineage -> final 12-field Panel in kHistField* order
// -> digest. Fields 0..7: close, raw_close, volume, high, low, open, market_cap,
// sector. Fields 8..11: earnFlag, atmCenI_21d, atmCenI_126d, nEarnCnt_5d (raw
// passthrough — options/earnings axis, orthogonal to price/volume).
// Raw source segments are unchanged; raw_close and raw volume remain in the panel.
// Shares*raw_close, raw-price floors, and raw dollar-ADV still drive the universe.
// Do not divide volume by the total-return factor: it includes cash distributions.
// Research OHLC are not execution prices; use the raw segments for execution.
// Result axes stay aligned through compaction. Invalid/noncanonical source IDs
// fail with InvalidArgument; source numeric IDs are never narrowed to InstKey.
//
// Replay limitation: shares and other archive fields have no individual original
// publication/revision timestamps here. Trading-date rows and kNoDate filing
// sentinels do not establish PIT availability. Callers must enforce an explicit
// archive-availability convention; this assembly does not prove it.
//
// Digest migration: OHLC now share one basis; close uses direct products instead
// of a rounded return chain. Existing panel digests/caches need a full rebuild.
// with_alpha101_fields derives dollar_volume/adv{d} from raw_close x raw volume
// (D-01): an adjusted-close product scales liquidity by the snapshot factor, which
// contains future splits and dividends.
// Every field is tagged in HistoryPanel::field_basis (see LevelBasis).
//
// Point-in-time contract (DataHistoryPanelFuturePerturb_* pins it): with
// compact_to_universe == false, every output row <= t — every field, the mask and
// the field-basis tags — is bit-identical when any source row dated after t is
// changed (prices, volume, shares, sector, factors, options fields). Column
// compaction is a whole-window decision, so a compacted panel keeps this property
// only for the columns both builds retain.
// Two calls with identical inputs return an identical digest. Err on: missing
// partition, an empty window, or a shape mismatch (propagated).
[[nodiscard]] atx::core::Result<HistoryPanel> build_history_panel(const HistoryDataConfig &cfg);

// =========================================================================
//  append_history_panel (S6-1) — incremental panel extension
// =========================================================================
//
// CONTRACT — byte-identical to a full rebuild over the combined date range.
//   Given an already-built `existing` Panel (loaded from panel.bin) and a
//   directory of NEW per-date .seg files for dates STRICTLY AFTER the existing
//   panel's last date, return an extended HistoryPanel whose serialized bytes
//   (and therefore its fnv1a64 panel.bin trailer) are IDENTICAL to
//   build_history_panel() run over the whole combined range. This identity — not
//   an assertion — is the central correctness gate (S6-1 byte-identity test).
//
//   `combined_cfg` describes the COMBINED build: combined_cfg.seg_dir MUST be a
//   directory holding BOTH the existing and the new per-date segments (the normal
//   data-loop layout — a new day's .seg is dropped into the same partition), and
//   combined_cfg.window MUST span the combined [first_existing, last_new] range
//   (an unset/default window selects all segments, which is the common case).
//   `new_seg_dir` is the directory of ONLY the new-date segments; it is read to
//   (a) detect the no-op case and (b) enforce the strictly-after ordering — it is
//   NOT a second data source. (`new_seg_dir` may equal combined_cfg.seg_dir only
//   when the partition contains exclusively new dates, which is not the append
//   case; pass a distinct new-only directory.)
//
// DETERMINISM — why this is byte-identical by construction.
//   Every history-panel field at date t depends only on input rows at dates <= t:
//   adjusted OHLC (raw*cumret) is per-row, market_cap (shares*raw_close) is per-cell,
//   sector is per-cell, and ADV is the engine's CAUSAL trailing mean (window
//   [t-w+1, t], no look-ahead). Appending later dates therefore cannot perturb
//   any earlier row. The instrument axis is the first-seen union in ascending
//   date order, so the existing instruments keep their column positions and any
//   brand-new symbol lands at the tail — exactly as a full rebuild orders them.
//   This describes deterministic replay of the supplied archive, not proof that
//   the archive itself preserves original point-in-time publication/revisions.
//
// PRECONDITIONS / ERRORS (fail closed):
//   * new_seg_dir contains no in-window dates  -> Ok(existing unchanged) (no-op).
//     This legacy numeric-only no-op has empty identity vectors: a bare Panel
//     cannot establish its original axes. Identified callers must validate and
//     carry existing identity separately, never infer it from dimensions.
//   * any new-seg date <= existing panel's last date (i.e. the combined build
//     would not extend strictly past `existing`) -> Err(InvalidArgument).
//   * combined_cfg.compact_to_universe == true -> Err(InvalidArgument): column
//     compaction is a whole-window decision that has no stable incremental
//     meaning here; the caller must append on an uncompacted panel.
//   * any error from the underlying full build is propagated.
//
// IMPLEMENTATION NOTE (honest limitation): alpha::Panel carries neither a symbol
//   axis nor a date axis, so the new rows cannot be spliced onto `existing`
//   purely in memory without re-deriving identity from the segments. This unit
//   therefore re-reads the combined partition through build_history_panel and
//   VERIFIES that the rebuild's first existing.dates() rows are byte-identical to
//   `existing` (turning any latent divergence into a loud error rather than a
//   silent one). A zero-re-read in-memory splice is deferred (it needs a panel
//   symbol/date axis — out of S6 scope).
[[nodiscard]] atx::core::Result<HistoryPanel>
append_history_panel(const alpha::Panel &existing, const std::string &new_seg_dir,
                     const HistoryDataConfig &combined_cfg);

} // namespace atx::engine::data
