# Iteration 15: point-in-time universe builder over the 2013–2019 archive

Design only. No implementation, build, test or native run was performed for this note. All
paths are relative to the isolated worktree `C:\atx\.worktrees\equity-platform`, branch
`feat/equity-platform-20260920`. Authored by a read-only design subagent.

Adopted scope: `.superpowers/sdd/equity-platform-parent-goal/progress.md`, entries "cp15 research"
and "RULINGS cp15 (R15-1..R15-16)"; every ruling is binding and cited by number where applied.
Research: `research-cp15-universe.md` (same directory), cited as "research §X".

**Revision 3 (frozen for implementation) + §15 amendment, 2026-09-20.** Revised in place after the independent
design review (`cp15-design-review.md`: 6 Critical, 12 Important, 9 Minor, 12 rules-without-tests)
and its scoped re-verification (N-1..N-3), under the binding parent rulings **DR15-1..DR15-16
(§14)**. Where a ruling and the reviewer differ, the ruling wins and §14 says so. Per R15-16 the
note is FROZEN: its SHA-256 is embedded in the stage (§5.8); any later edit is a new pre-registration.

## 1. Purpose and non-goals

### 1.1 Purpose

Construct, from the per-session archive segments alone, a **point-in-time** equity universe over
2013-01-02..2019-12-31: 84 monthly rebalances, each ranked on data at or before its rank date and
effective from the next session, emitted for three sizes and two bands side by side, with churn,
per-year coverage and union sizes, an inferred delisting table and a quantified survivorship
caveat — the input cp16 needs to decide per-year versus single-block contexts against
`kMaxIcInstruments = 4096` (`atx-engine/include/atx/engine/eval/cross_section_ic.hpp:96`; R15-7).

The gap: the only existing membership rule is `build_universe`'s dense mask
(`atx-engine/src/data/universe.cpp:195-224`), needing a materialised panel plus a corp-action
`Dataset`; the dense path cannot build 2013–2019 under 3 GB (research §C.1, ≈ 20 GB), and
`top_n_by_median_notional` (`universe.hpp:158-181`) is future-informed (bounded design :14).

### 1.2 Scope, frozen

- One new engine unit `atx-engine/include/atx/engine/data/point_in_time_universe.hpp` +
  `atx-engine/src/data/point_in_time_universe.cpp` + one test file (§3, R15-12).
- One new `atx-impl` subcommand `equity-universe` (§5) with `stage_equity_universe.{hpp,cpp}`,
  `kSubcommands` 13 → 14 append-only (`atx-impl/src/config.hpp:17-19`).
- Trial-ledger validator extension: `trial_count_declared == 0` accepted iff `purpose` is in an
  explicit non-trial allow-list (§5.7, R15-3).
- Six prepared+ingested windows 2014..2019 (R15-9) via the existing `prepare_tickerhistory.py` / `atx-impl load` path (done, §2.1).
- Stdlib oracle + native comparator (§8, R15-15).

### 1.3 Non-goals — explicitly out of scope

1. **No IC, no forecast, no alpha claim of any kind.** The output is membership lists and counts.
2. **The frozen 2013 context (`context.bin`, `ec572b82…`) is a regression fixture only.** Its
   21-mean/$20M/$5 rule is NOT re-emitted (R15-1); only its 2013 segments are re-read as inputs.
3. **Sealed and validation calendars unchanged.** `kValidationBeginNs` (2020-01-01) and
   `kSealedBeginNs` (2023-01-01) at `cross_section_ic.hpp:714-715` are not edited; the stage
   refuses (never skips) any segment dated ≥ 2020-01-01 (R15-13, §5.4).
4. **No market-cap ranking** (R15-2): vendor `shares` × raw close is a reported column only.
5. **No float, security-type, exchange or common-stock eligibility** (R15-6): the archive
   carries none (research §A.3); "unknown" is accepted; members with missing `gics` are counted.
6. **No edit** to `universe.{hpp,cpp}`, `history_panel.cpp`, `segment_panel.cpp`,
   `stage_equity_ic.cpp`, `cross_section_ic.{hpp,cpp}`, `panel_artifact.*`, or the `load` stage.
7. **`warehouse.duckdb` is not a parent** (R15-11). No live trading, no broker action, no order.

## 2. Data contract

### 2.1 Inputs

Per-session sealed segments `<dir>/YYYY-MM-DD.seg` written by `load_orats_history`
(`atx-engine/include/atx/engine/data/orats_history.hpp:61-72`), one directory per prepared
window:

All seven windows are ingested (receipt `build-equity/audits/iteration15-ingest-20260920-attempt2.json`,
status complete, ZIP sha `7d2b7a61…` identical before/after every year, `rows_malformed` 0). The
**real-run `--segments-dirs` list, in date order**, is pinned as:
`C:/atx/data/tickerhistory_training_native_20260919/segments` (2012-03-26..2013-12-31), then
`C:/atx/data/tickerhistory_training_native_{Y}_20260920/segments` for Y = 2014, 2015, 2016, 2017,
2018, 2019; `--preparation-manifests` = `C:/atx/data/tickerhistory_training_20120326_20131231_20260919/manifest.json`
then `C:/atx/data/tickerhistory_training_{Y}0101_{Y}1231_20260920/manifest.json`, same order.

| Window | accepted_rows | rejected_rows | duplicate_positive_keys | dates_written | distinct_securities |
| --- | --- | --- | --- | --- | --- |
| 2012-03-26..2013-12-31 (20260919, policy `tickerhistory-qa-v1`, loader exe `9415a6ab…`) | 2,883,147 | 27,586 | 4 | 445 | 7,650 |
| 2014 (20260920, loader exe `ac3ab17f…`, R15-9) | 1,702,593 | 17,144 | 75 | 252 | 7,546 |
| 2015 | 1,751,122 | 6,807 | 3 | 252 | 7,754 |
| 2016 | 1,868,815 | 113,928 | 28 | 252 | 9,008 |
| 2017 | 1,928,461 | 110,959 | 0 | 251 | 9,146 |
| 2018 | 1,980,746 | 68,873 | 86 | 251 | 9,382 |
| 2019 | 2,079,730 | 51,230 | 0 | 252 | 9,512 |

Every dir carries `_ingestion.manifest.json` (`atx-ingestion-v1`, per-segment SHA-256,
`preparation.manifest_sha256`). 2016/2017 reject ≈ 5.5 % of selected rows (reason breakdown in
each preparation manifest); §4.3 surfaces `rejected_rows`/`duplicate_positive_keys` per year. No re-ingestion.

Session key = the segment's own time axis entry `SegmentReader::times()[0]`
(`atx-tsdb/include/atx/tsdb/segment_reader.hpp:34-36`), which `load_orats_history` stamps as
midnight-UTC unix nanos of `tradingDate` (`orats_history.hpp:75-77` `date_to_nanos`). The stage
requires exactly one time entry per file and that it equals the filename date
(bounded design line 46: "File names are candidates, not trusted dates"). Instrument identity =
`symbol_name(j)` = canonical decimal positive i64 `securityID` (`orats_history.hpp:62-63`),
validated with the `history_panel.cpp:117-125` rule (`from_chars`, `> 0`, round-trip string
equality). `todayTicker` and `_symbology.parquet` are never read.

Available sessions: 1,955 over 2012-03-26..2019-12-31 (193/252/252/252/252/251/251/252 for
2012..2019; input profile `date_rows`). Rank date 2012-12-31 is session ordinal 192 (0-based):
193 sessions at or before it ≥ the 63-session warmup (R15-8).

### 2.2 Fields read, and which close is "raw"

Exactly four of the 16 `kOratsFields` (`orats_history.hpp:21-24`) are read per segment:

| Segment field | Use | Basis |
| --- | --- | --- |
| `close` | `raw_close` — the price floor, the dollar-volume factor, the reported market cap | **This is the raw as-traded close.** `history_panel.cpp:132` resolves `raw.field_id("close")`, `:145` binds it as `rc`, and `:231-233` lands `rc` as the panel field `raw_close` ("raw as-traded close"); `universe.cpp:256` reads that panel field as `raw_close` for `market_cap_field` (:264), `adv_field` (:265) and the price floor (:211). `closePr` / `closeUnadjPr` are the adjusted/unadjusted **prior-day** closes (`2026-09-19-tbltickerhistory-input-audit.md:43`) and are NOT used. (The task brief's guess `closeUnadjPr` is wrong; corrected here.) |
| `volume` | raw reported volume | `history_panel.cpp:235-239` "Raw traded shares: the total-return factor is not a pure split factor" |
| `shares` | vendor shares outstanding → `vendor_market_cap_usd = shares × close`, reported only (R15-2) | `history_panel.cpp:163-170`: "no per-observation filing, publication, or revision time" |
| `gics` | reported per member; NaN ⇒ missing (R15-6) | `history_panel.cpp:172-177` (NaN → `kNoSector`) |

Dollar volume on session `s` for ID `i`: `dv(i,s) = close(i,s) * volume(i,s)` in binary64
(`universe.cpp:127` `dvol[i] = raw_close[i] * volume[i]`, same operand order).

### 2.3 Bar validity and missing-bar semantics

A bar `(i,s)` is **valid** iff the ID is present in the segment (`present(0,j)`), `close` is
finite and `> 0`, and `volume` is finite and `≥ 0`. Zero volume is valid and yields `dv = 0`
(bounded design line 50). Anything else — ID absent from the segment, non-finite or
non-positive close, negative or non-finite volume — is a **missing** bar: `dv = NaN` for that
session. A missing bar is never filled, forward or backward, and a later bar never repairs an
earlier missing slot. `shares`/`gics` validity does not affect bar validity: `shares ≤ 0` or
NaN ⇒ `vendor_market_cap_usd` empty; `gics` NaN ⇒ `gics` empty.

### 2.4 Duplicate-key quarantine

Three layers already refuse duplicates; the builder adds a fourth:

1. Preparation (`tickerhistory-qa-v1`): "All positive duplicate date/ID rows quarantined,
   including identical duplicates" (prep manifest `policy`); `daily_counts[].{duplicate_positive_keys,
   rejected}` per date feed `dates_with_quarantined_duplicates`, `duplicate_positive_keys`,
   `rejected_rows` in §4.3.
2. Loader: duplicate positive `(tradingDate, securityID)` fails closed (`orats_history.hpp:67-69`).
3. Segment: duplicate symbol name inside one file rejected at attach
   (`atx-engine/src/alpha/segment_panel.cpp:160-167`).
4. Builder: a repeated ID inside one `observe_session` call → `Err(InvalidArgument)` (§3.4).

Consequence accepted by R15-9: on the 83 dates of 2018 (86 quarantined rows) and the 2/2/7/3/10/0
dates of 2012–2017 (research §A.2; 2012 = 2 verified in the prep manifest) the affected IDs have
a missing bar; that thins those sessions and is counted, not repaired.

### 2.5 Known caveats carried into every output

- **ID reuse**: securityID 77622 (MIL/TTT) collides across time (audit :100-107); one ID = one instrument for life. Reported, not resolved.
- **Vendor `shares` lag splits** (AAPL 861,381,000 on 2014-06-06 and 06-09, audit :134-137), zeros
  (3,705 accepted zero-share rows in 2012–13): `vendor_market_cap_usd` is `no-filing-vintage`, never a rank key (R15-2).
- **Archive backfill policy unknown** (`historical_availability: unknown-archive-snapshot`): every
  survivorship number is a lower bound on bias (R15-10, §11). **`todayTicker` never used.**

## 3. Engine unit `data::PitUniverseBuilder`

Layer `data/` beside `universe.hpp` (R15-12): a data-selection transform, not a statistic. Test
target `atx-engine-data-tests` globs `atx-engine/tests/data/*_test.cpp` (`tests/CMakeLists.txt:53-54`),
so only `src/data/point_in_time_universe.cpp` is added to `atx-engine/CMakeLists.txt` after :79.

Aliases: `atx::f64/i64/i32/u8/u32/u64/usize` (`atx-core/include/atx/core/types.hpp:17-33`);
errors travel in `atx::core::Result` / `Status` / `Err(ErrorCode::…)` as `universe.cpp` uses them.

### 3.1 Public API (exact)

```cpp
namespace atx::engine::data {

// Frozen defaults (§6). The stage passes these; tests may pass smaller cuts.
inline constexpr atx::usize kPitAdvWindow            = 63;
inline constexpr atx::usize kPitMinValidObservations = 57;   // ceil(0.9 * 63)
inline constexpr atx::f64   kPitMinRawPriceExclusive = 1.0;  // R15-5, raw close > 1.0
inline constexpr atx::f64   kPitMinAdvUsd            = 0.0;  // R15-5
inline constexpr atx::usize kPitMaxCuts              = 8;    // |top_n| * |band| <= 8
inline constexpr atx::usize kPitMaxTopNValues        = 4;
inline constexpr atx::usize kPitMaxBandValues        = 2;
inline constexpr atx::usize kPitMaxSourceIds         = 32768; // R15-12
inline constexpr atx::usize kPitMaxRebalances        = 4096;  // R15-12
inline constexpr atx::usize kPitMaxSessions          = 8192;  // R15-12
inline constexpr atx::u32   kPitNoBar                = 0xFFFFFFFFu;
inline constexpr atx::i64   kPitHashEmptyKey         = 0;   // IDs are > 0 (DR15-1)
// 2020-01-01T00:00:00Z; the data layer restates the literal, never includes eval/. A test
// asserts equality with eval::kValidationBeginNs (cross_section_ic.hpp:714,717).
inline constexpr atx::i64   kPitSessionKeyEndExclusive = 1'577'836'800'000'000'000;

struct PitUniverseConfig {
  atx::usize adv_window{kPitAdvWindow};
  atx::usize min_valid_observations{kPitMinValidObservations};
  atx::f64   min_raw_price_exclusive{kPitMinRawPriceExclusive};
  atx::f64   min_adv_usd{kPitMinAdvUsd};
  std::array<atx::usize, kPitMaxTopNValues> top_n{};   // ascending, distinct, > 0
  atx::usize top_n_count{};
  std::array<atx::u32, kPitMaxBandValues> band_bp{};   // basis points, ascending, distinct
  atx::usize band_count{};
  atx::usize max_source_ids{kPitMaxSourceIds};         // hash capacity = next pow2 >= 4*this (DR15-1)
  atx::usize max_rebalances{kPitMaxRebalances};        // stage passes the exact count
  atx::usize max_sessions{kPitMaxSessions};
  atx::i64   session_key_end_exclusive{kPitSessionKeyEndExclusive};
};

enum class PitMemberStatus : atx::u8 { Add = 0, Keep = 1 };
enum class PitDropKind     : atx::u8 { Rank = 0, LastBar = 1 };
// DR15-14: exit_kind integers pinned; enum order, §4.5 text order, F9 and delisting.csv follow it.
enum class PitExitKind     : atx::u8 { RankDrop = 0, LastBarWithinWindow = 1, WindowEnd = 2 };

struct PitRankedRow {          // one eligible ID at one rank session, valid until next call
  atx::u32 slot;               // first-seen slot (§3.3)
  atx::i64 security_id;
  atx::u32 rank;               // 1-based, key desc then slot asc
  atx::f64 adv63_usd;          // §3.5 median
  atx::u32 valid_observations; // 57..63
  atx::f64 raw_close;          // close on the rank session
  atx::f64 vendor_market_cap_usd; // shares*close, NaN when shares NaN or <= 0
  atx::f64 gics;               // NaN when missing
};
struct PitMemberRow  { atx::u32 slot; atx::u32 rank; PitMemberStatus status; };
struct PitDropRow    { atx::u32 slot; PitDropKind kind; };
struct PitChurn      { atx::u32 adds, drops_rank, drops_last_bar, kept, members; };

// Everything the stage writes for one rebalance. Spans alias builder scratch that ONLY
// `rebalance` rewrites, so the view stays valid across later `observe_session` calls until
// the next `rebalance` (DR15-3: rows are written once the effective session is observed).
struct PitRebalanceView {
  atx::usize index;            // r
  atx::i64 rank_session_key;
  atx::usize ids_seen, ids_with_valid_bar, eligible;
  std::span<const PitRankedRow> ranked;            // eligible IDs in rank order
  // per cut c = top_n_index * band_count + band_index:
  std::span<const std::span<const PitMemberRow>> members;   // rank ascending (DR15-2)
  std::span<const std::span<const PitDropRow>>   drops;     // slot ascending (M-3)
  std::span<const PitChurn> churn;
  std::span<const atx::u32> gics_missing_members;      // per cut
  std::span<const atx::u64> valid_observations_total;  // per cut, Σ valid_count over members (DR15-4)
};

// Aggregate rows (§4): PitCoverageYear{year,sessions,ids_seen,ids_with_valid_bar_median,
// rebalances,eligible_median, per-cut arrays members_median/nonmissing_fraction_median/
// gics_missing_members_median}; PitUnionYear{year, per-cut distinct, cumulative};
// PitSurvivorship{per-cut ever_members, ended_before_window_end, censored; per-cut per-year
// year_start_members, year_exits (array<…,16>), years}; PitExitRecord{security_id, first_seen,
// first_bar, last_bar (session ordinals), first_member_rebalance, last_member_rebalance, PitExitKind exit_kind}.

// Pure, allocation-free monthly rank-session selection (DR15-8): session_keys ascending;
// out[k] = every session whose UTC calendar month differs from the NEXT attached session's
// month (the month's last session BY DATA), restricted to start_key <= key <= end_inclusive_key;
// the last attached session is never selected. Err(OutOfRange) if out is too small.
[[nodiscard]] atx::core::Status select_monthly_rank_sessions(std::span<const atx::i64> session_keys,
    atx::i64 start_key, atx::i64 end_inclusive_key, std::span<atx::i64> out, atx::usize& n);
// §4.7 codec (DR15-6). encode allocates its output string — post-run, outside the no-heap paths.
[[nodiscard]] atx::core::Result<std::string> encode_membership_bin(const PitUniverseBuilder&);
struct PitMembershipImage { /* header fields, per-rebalance keys, per-cut sorted ids + ranks */ };
[[nodiscard]] atx::core::Result<PitMembershipImage> decode_membership_bin(std::string_view bytes);

class PitUniverseBuilder {
public:
  // Validates config against every kPitMax*, checks every product with overflow helpers,
  // performs ALL allocation here. Err(InvalidArgument|OutOfRange), never partial.
  [[nodiscard]] static atx::core::Result<PitUniverseBuilder> create(const PitUniverseConfig&);
  // One session; parallel spans (shares/gics may be empty => NaN). No allocation. Validates
  // EVERYTHING before mutating anything (DR15-9): an Err leaves the builder exactly as before.
  // Err on: key <= previous, key >= end_exclusive, id <= 0, id repeated in the call,
  // > max_source_ids distinct, > max_sessions, span size mismatch. Any positive i64 id is accepted.
  [[nodiscard]] atx::core::Status observe_session(atx::i64 session_key,
      std::span<const atx::i64> ids, std::span<const atx::f64> raw_close,
      std::span<const atx::f64> volume, std::span<const atx::f64> shares,
      std::span<const atx::f64> gics);
  // rank_session_key MUST equal the last observed key (a rank date with no data is rejected,
  // never interpolated). No allocation.
  [[nodiscard]] atx::core::Result<PitRebalanceView> rebalance(atx::i64 rank_session_key);
  [[nodiscard]] atx::usize sessions() const noexcept;   rebalances(); source_ids();
  [[nodiscard]] atx::i64 session_key(atx::usize ordinal) const noexcept;
  [[nodiscard]] atx::i64 security_id(atx::u32 slot) const noexcept;  first_seen(slot), first_bar(slot), last_bar(slot) -> u32;
  [[nodiscard]] std::span<const atx::u32> members(atx::usize r, atx::usize c) const noexcept; // slot asc
  [[nodiscard]] atx::i64 rank_key(atx::usize r) const noexcept;  effective_key(r) (0 if never observed);
  // Aggregates over what has been observed (§4), allocation-free, caller-sized spans:
  [[nodiscard]] atx::core::Status coverage_by_year(std::span<PitCoverageYear> out, atx::usize& n) const;
  [[nodiscard]] atx::core::Status union_by_year(std::span<PitUnionYear> out, atx::usize& n) const;
  [[nodiscard]] atx::core::Result<PitSurvivorship> survivorship() const;
  [[nodiscard]] atx::core::Status exits(atx::usize cut, std::span<PitExitRecord> out, atx::usize& n) const;
};
} // namespace atx::engine::data
```

Calendar-year of a session key: `year(k) = civil_from_days(k / 86'400'000'000'000)` (Howard Hinnant's
algorithm, integer only), exposed as `pit_year_of(i64)`; the oracle restates the same algorithm.

### 3.2 Bounded storage, all sized in `create`

Let `U = max_source_ids`, `W = adv_window`, `R = max_rebalances`, `C = top_n_count *
band_count`, `N = top_n[top_n_count-1]`, `S = max_sessions`, `H` = hash capacity = the smallest
power of two `≥ 4*U` (131,072 at `U = 32768`).

| Buffer | Bytes |
| --- | --- |
| `ring` (dollar volume, NaN-initialised, `U*W` f64) | `8*U*W` |
| `id_table` (DR15-1): open-addressing, linear probing, `H` entries of `{i64 key, u32 slot}` (16 B padded), keys `kPitHashEmptyKey` = 0 empty; hash `h(id) = (u64(id) * 0x9E3779B97F4A7C15) >> (64 - log2 H)`; built in `create`; load ≤ 1/4 so a probe is bounded; slot numbers are assigned first-seen, so results never depend on `h` | `16*H` |
| per-slot `id` i64, `first_seen` u32 (session ordinal of first appearance, DR15-11), `first_bar` u32 (first VALID bar, I-7), `last_bar` u32, `last_close`/`last_shares`/`last_gics` f64, `member_bits` u8 (bit c = member of cut c at the last rebalance), `ever_bits` u8, `last_year_seen` u16; per (slot,cut) `first_member_r` u32, `last_member_r` u32, `exit_kind` u8; per-cut union bitmap `U` bytes (I-10) | `48*U + 9*C*U + C*U` |
| `retained` (`R*C*N` × {u32 slot, u32 rank}, DR15-2); `rank_keys`/`effective_keys` (`R` i64); `churn` (`R*C` × 20 B); per-rebalance `ids_seen`/`ids_valid`/`eligible` u32 + per-cut `gics_missing` u32, `valid_total` u64; per-session `session_keys` i64, `ids_with_valid_bar` u32 | `8*R*C*N + 16*R + 20*R*C + R*(12+12*C) + 12*S` |
| scratch: `ranked` rows (`U` × 56 B), `order` u32 `U`, `seen_this_call` u8 `U`, `median_scratch` f64 `W`, `members_scratch` `C*N` × 12 B, `drops_scratch` `C*N` × 8 B (M-2) | `61*U + 8*W + 20*C*N` |

Every product is computed with checked `multiply`/`add` helpers modelled on
`stage_equity_ic.cpp:164-176` before any `std::vector::resize`; a failure is
`Err(OutOfRange)` with the offending product named. `observe_session` and `rebalance` allocate
nothing: `std::sort` on the pre-sized `order` scratch is the only sort (R15-12 — introsort is
in-place; `std::stable_sort` is NOT used because it may allocate a temporary buffer).

### 3.3 `observe_session` — step by step

**Pass 1 — validate, mutate nothing (DR15-9).** (a) `session_key > last key` (else
`InvalidArgument` "session out of order"), `< session_key_end_exclusive` (else `PermissionDenied`
"session at/after validation boundary"), `sessions() < max_sessions`; span sizes equal (shares/gics
may be empty). (b) For each `j`: `ids[j] > 0` (else `OutOfRange`); probe `id_table`; a known
id's slot is checked against `seen_this_call` (duplicate → `InvalidArgument` "duplicate id in
session"); an UNKNOWN id is **inserted provisionally** (DR15-13): if `source_ids() + provisional
== max_source_ids` → `OutOfRange`, else it gets slot `source_ids() + provisional`, its table
entry is written, and the slot is pushed on a bounded `provisional_slots` list (capacity `U`,
pre-sized) and marked in `seen_this_call` — so a repeated unknown id in the same call is caught by
the very same lookup. (c) **Rollback on any failure:** every `provisional_slots` entry's table
cell is reset to `kPitHashEmptyKey` (linear-probing deletion is safe here because provisional
entries are the newest and are removed newest-first), the slot counter is restored, and
`seen_this_call` is cleared before returning — no state mutation survives a rejected call. On
success the provisional slots become real (counter advanced) and pass 2 runs.

**Pass 2 — apply.** (1) `s = sessions()`, `k = s % W`. **Sweep**: for every slot `u <
source_ids()`, `ring[u*W + k] = NaN` (absent IDs read NaN for this session; entries older than
`s-W+1` are overwritten exactly once). (2) For each `j`: look up or insert the slot (insertion
order = position in the span, which the stage feeds in segment column order, matching
`segment_panel.cpp:187-196` "first-seen across rows in ascending date order"); a new slot gets
`id`, `first_seen = s`, `first_bar = last_bar = kPitNoBar`. Bar validity per §2.3: valid ⇒
`ring[slot*W + k] = raw_close[j] * volume[j]` (one binary64 product, stored before use — no FMA
shape, DR15-5), `last_bar = s`, `first_bar = min(first_bar, s)`, `last_close = raw_close[j]`,
`last_shares = shares.empty() ? NaN : shares[j]`, `last_gics = gics.empty() ? NaN : gics[j]`;
invalid ⇒ nothing written. (3) `session_keys[s] = session_key`, `ids_with_valid_bar[s]`, per-slot
`last_year_seen`; if rebalance `r` is pending (`effective_keys[r] == 0`, `rank_keys[r]` = the
previous key) set `effective_keys[r] = session_key` (R15-4) — the stage then writes that
rebalance's rows from the still-valid view (§3.7).

### 3.4 `rebalance(rank_session_key)` — step by step

Precondition: `rank_session_key == session_keys[sessions()-1]` (else `InvalidArgument` "rank
session has no data"); `rebalances() < max_rebalances`; if `rebalances() > 0`, key `>` previous
rank key. Let `R_s = sessions()-1`, `r = rebalances()`.

1. **Eligibility and key**, per slot `u`:
   `bar_on_rank = (last_bar[u] == R_s)`; `valid_count` = number of finite entries in
   `ring[u*W .. u*W+W)`; `adv = median(valid entries)` (§3.5) when `valid_count > 0`, else NaN.
   `eligible(u) = bar_on_rank && last_close[u] > min_raw_price_exclusive && valid_count >=
   min_valid_observations && adv >= min_adv_usd` (NaN compares false, as `universe.cpp:207-211`).
   Eligible slots are appended to `ranked` with `slot, security_id, adv63_usd, valid_count,
   raw_close = last_close, vendor_market_cap_usd = (last_shares > 0 ? last_shares * last_close
   : NaN), gics = last_gics`. `ids_seen = source_ids()`, `ids_with_valid_bar =
   ids_with_valid_bar[R_s]`, `eligible = |ranked|`.
2. **Order**: `order[0..E)` = indices into `ranked`; `std::sort(order, cmp)` with the total
   order `cmp(a,b) = adv[a] > adv[b] || (adv[a] == adv[b] && slot[a] < slot[b])`
   (`universe.cpp:180-184` rule: "Descending by ADV; canonical-id ascending tie-break"; here
   canonical id = first-seen slot, R15-12). `rank = position + 1`.
3. **Per cut** `c = ti * band_count + bi`, `n = top_n[ti]`, `K_band = n + (n * band_bp[bi]) /
   10000` (integer arithmetic; `n * band_bp` ≤ 3000·10000 fits u64):
   a. `keep`: walk `order` in rank order; a row whose slot has `member_bits & (1<<c)`
      (incumbent, i.e. member of THIS cut at rebalance `r-1`) and `rank <= K_band` is
      appended to `members_scratch[c]` with status `Keep` — stop appending once
      `|members| == n` (**if incumbents-within-band alone exceed `n`, the best-ranked `n` of
      them are kept**).
   b. `fill`: walk `order` again in rank order; a row not yet in `members_scratch[c]` is
      appended until `|members| == n` or the list is exhausted; status = `Keep` if it was an
      incumbent, else `Add`. (The "incumbent beyond `K_band` re-admitted" branch is
      unreachable — ≥ `n` rows precede it — and is NOT modelled by the oracle; M-1.)
   c. `drops`: every slot with `member_bits & (1<<c)` set that is not in the new list, in slot
      ascending order (M-3): **`kind = (last_bar[slot] < R_s) ? LastBar : Rank` — formula only**
      (DR15-10): `LastBar` iff the ID has no VALID bar (§2.3) on the rank session (absent, or
      present with a non-finite/non-positive close or invalid volume); `Rank` iff it has a valid
      bar on the rank session and is ineligible or out-ranked.
   d. `churn[r][c] = {adds, drops_rank, drops_last_bar, kept, members = |list|}`;
      `one_way_turnover` is computed by the writer (§4.2), not stored.
   e. `gics_missing_members[c]` = members with `isnan(gics)`; `valid_observations_total[c]` =
      `Σ valid_count` over members, an integer (DR15-4); no fraction lives in the engine.
   f. **Order (DR15-2).** The membership SET is the keep-then-fill construction above; the
      OUTPUT order is rank ascending: `std::sort(members_scratch[c], by rank)` (ranks are
      distinct, so a total order). Three orders, pinned once:

      | Where | Order |
      | --- | --- |
      | `PitRebalanceView.members[c]`, `membership.csv` rows, F4/F7 `members[]` | rank ascending (kept and added alike; `rank` = position in the eligible ranked list) |
      | `retained` / `members(r,c)` | slot ascending (`std::sort` on the scratch copy) |
      | `membership.bin` | `security_id` ascending with a parallel u32 `rank` array (§4.7) |

      Then update `member_bits`, `ever_bits`, `first_member_r`/`last_member_r`/`exit_kind`.
   Note: `r == 0` has no incumbents; every member is `Add`, `drops = 0`.
4. `rank_keys[r] = rank_session_key`, `effective_keys[r] = 0` (pending); return the view, which
   stays valid until the next `rebalance` (§3.1).

At `r = 0` with `E < n` the list is short: `members < top_n` is legal and reported.

### 3.5 Median rule (pinned for bit-for-bit oracle parity)

`valid` = the finite ring entries in ascending ring index order (not chronological; order is
irrelevant after sorting). `std::sort` ascending on `median_scratch[0..m)`, `m = valid_count`.
`median = (m % 2 == 1) ? v[m/2] : (v[m/2 - 1] + v[m/2]) * 0.5` in binary64 (DR15-5; identical
in value to `universe.hpp:147-154`'s `0.5 * (a + b)` — multiplication is commutative and both
round once). The oracle reproduces the double arithmetic exactly: `dv = close * volume` as one
Python `float` product stored before use (no FMA shape), `(a + b) * 0.5` as two `float` ops,
everything else in exact integers/`Fraction`. Comparison is `Fraction(float(native)) ==
Fraction(float(oracle))` (§8.2), zero tolerance — never text.

### 3.6 Determinism and truncation invariance

Every buffer is initialised in `create`; no clock, RNG or `std::` container lookup is consulted
(the pre-sized `id_table` is the only hash and slots are assigned first-seen, so no output
depends on `h`); the sort key is a total order. Feeding sessions after rank session `R_s` cannot change the rebalance at `R_s` because
`rebalance` reads only `ring` (sessions ≤ `R_s` by the sweep), `last_*` (written ≤ `R_s`) and
`member_bits` (rebalance `r-1`). Both properties are asserted by tests §9.1 (cases 12, 13).

### 3.7 Streaming emission

The engine does not retain per-member `adv/raw_close/market_cap/gics` across rebalances
(`R*C*N*32 B ≈ 48 MB` at the frozen dimensions). A rebalance is **emitted only once its effective
session has been observed** (DR15-3): the stage calls `rebalance` on a rank session, keeps the
view, calls `observe_session` for the next file, then writes that rebalance's `membership.csv`
and `churn.csv` rows (with `effective_date = effective_key(r)`) through a buffered streaming
file writer (DR15-11, §5.1) before the next `rebalance`. A rebalance still pending at end of run
is a stage error (`InvalidArgument "final rebalance has no effective session"`), never written
with 0. Retained: `{slot, rank}` per (r, c), keys, churn counters, per-slot
`first_seen/first_bar/last_bar`, per-(slot,cut) membership history — exactly what §4 needs.

## 4. Outputs and metrics

All under `--out`. Small files use the `write_text` idiom of `stage_equity_ic.cpp:202-216`
(`.partial` → hard link → remove partial, digest `sha256_hex`); `membership.csv` and `churn.csv`
are appended per rebalance to `<name>.partial` through one `std::ofstream` kept open (1 MiB
buffer), hard-link-published the same way and digested with `atx::core::sha256_file`
(`atx-core/include/atx/core/sha256.hpp:34`) — DR15-11, I-8. Reals via `std::to_chars` shortest round-trip
(`stage_equity_baseline.cpp:41-45` `number()`), dates as `YYYY-MM-DD` derived from session
keys by the §3.1 civil algorithm, LF endings, no trailing whitespace. Rows are ordered as
stated; two runs over the same inputs are byte-identical (R15-14, test §9.2).

### 4.1 `membership.csv`

Header, then one row per (rebalance, cut, member) in rebalance order, then cut order (top_n
ascending, band ascending), then rank order:

```text
rebalance_rank_date,effective_date,top_n,band,security_id,rank,adv63_usd,raw_close,vendor_market_cap_usd,gics,valid_observations,nonmissing_fraction,status
```

`band` is the literal `0.00` or `0.10`; `vendor_market_cap_usd` empty when NaN; `gics` empty
when NaN, else the integer text of the f64; `valid_observations` integer 57..63;
`nonmissing_fraction = static_cast<double>(valid_observations) / 63.0` (DR15-4; not compared by
the comparator); `status` ∈ `add|keep`. `effective_date` is never empty: rows exist only for
emitted rebalances (§3.7); for rank 2019-11-29 it is 2019-12-02 (`--rank-end` pinned, §5.2).

### 4.2 `churn.csv`

```text
rebalance_rank_date,effective_date,top_n,band,adds,drops_rank,drops_last_bar,kept,members,one_way_turnover
```

`one_way_turnover = (adds + drops_rank + drops_last_bar) / (2 * top_n)`, binary64 division,
shortest round-trip. At `r = 0`: `adds = members`, drops 0, kept 0.

### 4.3 `coverage_by_year.csv`

One row per calendar year of the observed sessions (2012..2019), per cut (cut columns are
suffixed `_t{top_n}_b{band_bp}`, e.g. `members_median_t1000_b0`):

```text
year,sessions,dates_with_quarantined_duplicates,duplicate_positive_keys,rejected_rows,ids_seen,ids_with_valid_bar_median,rebalances,eligible_median,members_median_<cut>...,nonmissing_fraction_median_<cut>...,gics_missing_members_median_<cut>...
```

- `sessions`: observed sessions with `year(session_key) == year`.
- `dates_with_quarantined_duplicates` (R15-9 name, I-11), `duplicate_positive_keys`,
  `rejected_rows`: stage-supplied from the preparation manifests' `daily_counts[]` over dates in
  the year (count of dates with `duplicate_positive_keys > 0`; Σ `duplicate_positive_keys`; Σ
  `rejected`). Expected dates 2/2/7/3/10/0/83/0 for 2012..2019 and rows per §2.1, asserted by the
  receipt, not by code.
- `ids_seen`: distinct slots with ≥ 1 valid bar in the year (per-slot u16 `last_year_seen`,
  counted on transition).
- `ids_with_valid_bar_median`: §3.5 median over the year's sessions of `ids_with_valid_bar[s]`.
- `rebalances`, `eligible_median`, `members_median`, `gics_missing_members_median`: over
  rebalances whose **rank** session is in the year (2012 carries exactly the 2012-12-31
  rebalance, 2019 carries eleven). `nonmissing_fraction_median_<cut>` = median over those
  rebalances of `static_cast<double>(valid_observations_total) / static_cast<double>(63 * members)`
  (one division, C-4; 0.0 when `members == 0`); reported, not compared by the comparator (DR15-4).

### 4.4 `union_by_year.csv` (R15-7)

```text
year,distinct_<cut>...,cumulative_<cut>...
```

`year` = calendar year of the **effective** session (2013..2019). `distinct` = number of slots
that are a member of cut `c` at ≥ 1 rebalance effective in the year; `cumulative` = distinct
over all rebalances effective in or before the year. Computed at the end from `retained` with
one pre-sized `U`-byte bitmap per cut (no allocation). cp16 compares `cumulative` at 2019 and
each `distinct` against `kMaxIcInstruments = 4096`.

### 4.5 `delisting.csv` (R15-14)

One row per (cut, slot ever a member of that cut), cut order then `security_id` ascending:

```text
top_n,band,security_id,first_bar,last_bar,first_member_effective_date,last_member_rank_date,exit_kind
```

`first_bar` = date of the slot's first VALID bar (I-7). `exit_kind` text ↔ integer (DR15-14):
`rank_drop` = 0, `last_bar_within_window` = 1, `window_end` = 2 (`PitExitKind`); the CSV carries
the text. `window_end` iff the slot is a member at the final rebalance; else the `PitDropKind`
recorded at the rebalance where it was LAST dropped (`Rank` → `rank_drop`, `LastBar` →
`last_bar_within_window`). `last_bar` is the
date of the last valid bar over ALL observed sessions (may be later than the drop: a rank-drop
that later stops trading keeps `rank_drop` here and is counted by §4.6 (a) instead).

### 4.6 `survivorship.json` (R15-10)

```json
{"schema":"atx-equity-universe-survivorship-v1","window_end":"2019-12-31",
 "cuts":[{"top_n":1000,"band":"0.00",
   "ever_members":N,"ended_before_window_end":E,"censored":N-E,"fraction_ended":E/N,
   "per_year":[{"year":2013,"members_at_first_rebalance":m,"exited_within_year":x,"fraction":x/m},...]}],
 "comparison":{"russell_3000_2019_reconstitution_deletions":157,"positions":3000,
   "deletions_per_year_fraction":0.0523,"source":"nasdaq.com 2019-06-24 (research §D.4)"},
 "caveat":"Both metrics are LOWER BOUNDS on survivorship bias: the archive's backfill policy is unknown (historical_availability: unknown-archive-snapshot), the archive has no delisting date/code/return fields, and a name absent from the archive from its first day is invisible here."}
```

(a) `ended_before_window_end` = ever-members whose `last_bar` ordinal `<` the final observed
session ordinal (archive-end guard: `last_bar == final` ⇒ censored). (b) per year `Y`:
`members_at_first_rebalance` = members of the rebalance whose effective session is the first
observed session of `Y` (rank date = last session of `Y-1`); `exited_within_year` = those whose
`last_bar` `<` the last observed session of `Y`. Fractions as binary64 shortest text. (b) is an
interpretation of R15-10's "members at the year's first rebalance" as the rebalance EFFECTIVE on
the year's first session; the alternative "first rebalance RANKED in `Y`" was not chosen (M-7).

### 4.7 `membership.bin` (R15-14) — byte layout, little-endian throughout

```text
offset  size  field
0       8     magic "ATXPITU1" (ASCII, no NUL)
8       4     version u32 = 1
12      4     adv_window u32 (63)
16      4     min_valid_observations u32 (57)
20      8     min_raw_price_exclusive f64 (1.0)
28      4     top_n_count u32 = T ; then T x u32 top_n (ascending)
..      4     band_count u32 = B ; then B x u32 band_bp (ascending)
..      4     rebalance_count u32 = R
per rebalance r = 0..R-1:
        8     rank_session_key i64
        8     effective_session_key i64 (> 0 always; encode refuses 0 with Internal, DR15-3)
        per cut c = ti*B + bi, ti outer, bi inner:
        4     member_count u32 = n_c
        8*n_c security_id i64, ascending
        4*n_c rank u32, parallel to the ids (DR15-2)
trailer 8     fnv1a64 over every preceding byte (offset 14695981039346656037, prime
              1099511628211, byte-wise), u64 LE — the serialize_panel.hpp:5-7 convention
```

`encode_membership_bin` / `decode_membership_bin` live in the ENGINE unit (§3.1, DR15-6) so the F8
marker comes from `atx-engine-data-tests`; the decoder refuses bad magic, bad version, a short
buffer and a trailer mismatch (`InvalidArgument` / `Internal`), round-trip tested (§9.1 #27).

### 4.8 `request.json`, `seal.json`, `manifest.json`

`request.json`: the §6 table as JSON, attached segments (filename, sha256, size), per-dir
ingestion- and preparation-manifest SHAs, `design_note_sha256` (§5.8), `producer_executable_sha256`,
ledger `trial_id` and pre-registration line SHA. `seal.json`: `{"policy":"RefuseAtOrAfterValidationBeginV1",
"validation_begin":"2020-01-01","sealed_begin":"2023-01-01","segments_refused":0,"latest_attached":"2019-12-31",
"statement":"no segment at/after 2020-01-01 was mapped"}`. `manifest.json` (written last): every output
file with sha256 and bytes, `runtime_seconds`, `peak_working_set_bytes` (null + `kPeakWorkingSetSource`
text, `stage_equity_ic.cpp:60-65`), config, segment SHAs, both ledger line SHAs, the §11 qualification strings.

## 5. `atx-impl equity-universe`

### 5.1 Files and wiring

- `atx-impl/src/stage_equity_universe.hpp` — `run_equity_universe(const RunConfig&)`,
  `kEquityUniverseDesignNoteSha256`; `.cpp` also holds the TU-local `StreamedCsv` helper
  (open `<name>.partial`, append rows, close, hard-link publish, `sha256_file`; DR15-11).
- `atx-impl/tests/config_equity_ic_test.cpp:65-66` (DR15-7): `13U → 14U`, `back()` re-pinned to
  `"equity-universe"`, plus an assertion that index 12 is still `"equity-ic"`; the ONLY test edit
  outside the new files.
- `atx-impl/src/config.hpp:17-19`: `std::array<std::string_view, 14>`, append
  `"equity-universe"` LAST. New `RunConfig` members: `std::string equity_segments_dirs;
  std::string equity_preparation_manifests; std::string equity_rank_start;
  std::string equity_rank_end;`.
- `atx-impl/src/config.cpp`: four new flag branches beside `:90-97` (`--segments-dirs`,
  `--preparation-manifests`, `--rank-start`, `--rank-end`, each rejecting an empty or `--`
  value exactly as `--trial-ledger` does), one usage line.
- `atx-impl/src/dispatch.cpp:142`: `if (sub == "equity-universe") return
  run_equity_universe(cfg);` after the `equity-ic` line. No entry in the config-file merge
  predicate (`:120-122`): like `equity-ic` (ruling AR-9) it accepts no `--config`.
- `atx-impl/CMakeLists.txt:34`: add `src/stage_equity_universe.cpp` after
  `src/stage_equity_ic.cpp`. Tests are globbed (`atx-impl/tests/CMakeLists.txt:1-2`).

### 5.2 Flags — the allow-list, frozen

`resolve()` mirrors `stage_equity_ic.cpp:352-375`: `allowed = {"segments-dirs",
"preparation-manifests", "out", "rank-start", "rank-end", "max-working-bytes", "trial-ledger",
"quiet", "digest-only"}`; any other set flag → `InvalidArgument "equity universe: unsupported
flag --<flag>"`; the same "no override of the frozen recipe" predicate over the legacy fields.

| Flag | Semantics |
| --- | --- |
| `--segments-dirs a;b;…` | `;`-separated segment directories (Windows-safe; a path containing `;` is rejected). Order irrelevant: the stage lists `*.seg` in every dir, parses each filename as `YYYY-MM-DD` with `date_to_nanos`, refuses a non-date name, a duplicate date across dirs, or any date ≥ 2020-01-01 (§5.4), sorts globally by date, and requires each dir's date range to be disjoint from every other's. |
| `--preparation-manifests a;b;…` | Same count as dirs, matched positionally; each must hash to the `preparation.manifest_sha256` of that dir's `_ingestion.manifest.json` (else `InvalidArgument`). Source of the three §4.3 quarantine/rejection columns. |
| `--out` | fresh directory (§5.3) |
| `--rank-start`, `--rank-end` | inclusive `YYYY-MM-DD`; rank sessions = `select_monthly_rank_sessions(all attached keys, start, end)` (DR15-8: a session is a month's rank session iff the NEXT attached session is in a different UTC month; the last attached session is never one; then the `[start,end]` filter — reading A of I-4). `end` must be `<` the last attached session date and `≤ 2019-12-31` (R15-13, C-6); ≥ 63 attached sessions at or before the first rank session (R15-8); else `InvalidArgument`. **Real run: `--rank-start 2012-12-31 --rank-end 2019-11-29`** (DR15-3), giving exactly 84. |
| `--trial-ledger` | default `atx-engine/reviews/trial-ledger.jsonl` (`stage_equity_ic.cpp:47`) |
| `--max-working-bytes` | default 3,000,000,000 (`config.hpp:457`); must exceed `kOverheadReserve` |

**No `--top-n`, no `--band`, no `--rank-key`, no `--cadence`**: the sets `{1000,2000,3000}` and
`{0.00,0.10}`, the key and the cadence are frozen in this note and embedded as constants
(R15-1, R15-3, R15-4); supplying such a flag is "unsupported flag".

### 5.3 Fresh output directory — with the M-6 diagnostic fixed locally

`.superpowers/sdd/equity-platform-parent-goal/final-branch-review.md:257` (M-6; M-5 of the cp15
review corrects the path): `stage_equity_ic.cpp:190-199` reports `AlreadyExists
"output root must not exist"` for a failed parent creation or any I/O error. The new stage's
own `reserve_directory` (TU-local, `stage_equity_ic.cpp` untouched):

```cpp
if (fs::exists(directory, ec) || ec)          return Err(AlreadyExists, "equity universe: output root must not exist");
if (!parent.empty()) { fs::create_directories(parent, ec); if (ec) return Err(IoError, "cannot create parent: " + ec.message()); }
if (!fs::create_directory(directory, ec) || ec) return Err(IoError, "cannot create output root: " + ec.message());
if (!fs::create_directory(directory / ".pending", ec) || ec) return Err(IoError, "cannot reserve publication");
```

### 5.4 Seal refusal (R15-13)

Before any file is mapped: every `*.seg` filename in every listed dir is parsed; a date
`≥ 2020-01-01` → `PermissionDenied "equity universe: segment at/after validation boundary:
<file>"` and the run stops (refuse, not skip; a dir containing such a file is unusable, which is
exactly the 2026 dirs in research §A.1). After attach, `times().size() == 1 && times()[0] ==
filename nanos` is required, and the engine independently rejects a key
`≥ kPitSessionKeyEndExclusive` (§3.3; unreachable through the stage because the axis check
precedes it — defence in depth, engine case 18). `--rank-end > 2019-12-31` or `≥` the last
attached session date is rejected at `resolve()`.

### 5.5 Run sequence

1. `resolve()` (flags, dates, ledger path, `current_executable_sha256()`, `stage_data_provenance.hpp:68`).
2. Enumerate + seal-check + sort segment files; read each dir's `_ingestion.manifest.json`
   (≤ 16 MiB, `strict_json`) and the paired preparation manifest; `sha256_file` each selected
   `.seg` (never `snapshot_panel_sources`, which attaches every segment in a directory — bounded
   design line 16). **Binding (I-12):** the digest must equal that dir's manifest
   `segments[].sha256` for the same filename; a mismatch, a selected `.seg` absent from the
   manifest, or a manifest entry in the selected date range with no file → `InvalidArgument`,
   before the ledger line. Record a `SourceFileDigest` per file.
3. Rank sessions via `select_monthly_rank_sessions` (§5.2); `R_actual` = their count (84 in the
   real run); budget preflight (§7); `reserve_directory`; `PitUniverseBuilder::create` with
   `max_rebalances = R_actual`, `max_sessions = files`, cuts from §6.
4. Ledger pre-registration line (§5.6) — **point of no return**; every later exit path,
   exceptions included, falls through to the terminal append (ruling C-1 shape,
   `stage_equity_ic.cpp:968-973`, `:1226-1295`).
5. Per file in date order: `SegmentReader::attach` (one mapping alive, no `prefetch`), resolve
   the four field indices, parallel spans over `field_block_view` with a pre-sized `ids` buffer
   (`instrument_count ≤ kPitMaxSourceIds`), `observe_session`; if a rebalance is pending, write
   its rows now (effective = this session, §3.7); if this session is a rank session, `rebalance`;
   release the mapping.
6. After the last file: a still-pending rebalance is an error (§3.7); `coverage_by_year`,
   `union_by_year`, `exits`, `survivorship`, `encode_membership_bin`; publish the streamed CSVs;
   write all other files, `manifest.json` last; terminal ledger line.

### 5.6 Ledger line (R15-3) — Option A, exact instance

`checkpoint = 15`, `purpose = "point-in-time-universe-construction"`, `trial_count_declared =
0`, `trial_id = "iteration15-point-in-time-universe-NNNN"` where `NNNN` = 1 + the number of
existing pre-registered lines with `checkpoint == 15` (new API §5.7; the cp14 ordinal rule
`declared / kTrialCountDeclared` at `stage_equity_ic.cpp:820` cannot divide by 0). Two lines per
run: `pre-registered`/`pending` before step 6, `completed`+`manifest_sha256` or
`failed`+`failure_sha256` after (`trial_ledger.hpp:206-208`, `:187-193`).

The v1 schema is cp14-shaped (`TrialLedgerRecipe`, `trial_ledger.hpp:128-145`) and
`recipe.signals` must be non-empty (`trial_ledger.cpp:334`). Pinned encoding:

```json
"checkpoint":15,"purpose":"point-in-time-universe-construction","status":"pre-registered","trial_count_declared":0,
"parents":[{"role":"segments-2012-2013","sha256":"<sha256 of that dir's _ingestion.manifest.json>"},
           {"role":"segments-2014","sha256":"…"},…,{"role":"segments-2019","sha256":"…"},
           {"role":"design-note","sha256":"<kEquityUniverseDesignNoteSha256>"}],
"recipe":{"signals":[{"name":"adv63_median_dollar_volume",
            "dsl":"median_63(close*volume) over sessions <= rank date; valid>=57; raw close>1.0 on rank date",
            "dsl_sha256":"<sha256 of that dsl string>"}],
          "restrictions":["top1000_b0","top1000_b10","top2000_b0","top2000_b10","top3000_b0","top3000_b10"],
          "alignment":"rank-at-t-effective-at-t-plus-1-session", … every other recipe key at its struct default …},
"window":{"start":"2013-01-02","end_exclusive":"2020-01-01","observations":84},
"source_exclusions":{… struct defaults, "ex34_restriction_ids":[] …},
"fit_boundary":{"fit_kind":"unfit-no-fitting-performed","fitted_observations":0},
"notes":"Checkpoint 15 universe construction; not a forecast trial (trial_count_declared 0). recipe.horizons/quantiles/bootstrap/autocorr_lags/turnover_source/forward_variants/samples/common_sample_rule/net_spread_rule/cost and source_exclusions carry atx-trial-ledger-v1 defaults and are NOT APPLICABLE to this line. The six top_n x band cuts are reported side by side (AR-7 semantics); selecting among them later is a new trial."
```

`window.observations = 84` = rebalances (must be `> 0`, `trial_ledger.cpp:338`). This v1 encoding is ACCEPTED (DR15-16, §12 Q1).

### 5.7 Validator extension in `trial_ledger.{hpp,cpp}` (R15-3)

- `trial_ledger.hpp`: `inline constexpr std::array<std::string_view, 1> kNonTrialPurposes =
  {"point-in-time-universe-construction"};` and
  `[[nodiscard]] bool is_non_trial_purpose(std::string_view purpose);`
- `trial_ledger.cpp:301` becomes: `< 0` → reject always; `== 0` → reject unless
  `is_non_trial_purpose(entry.purpose)`; `> 0` → reject **if** `is_non_trial_purpose`
  ("a non-trial purpose must declare 0").
- `trial_ledger.cpp:729-733` (`declared_trials_for_checkpoint` also rejects `<= 0` with
  `ParseError`; R15-3 named only :301): `< 0` → `ParseError` as today; `== 0` → allowed iff
  `scan_field(line, "purpose")` is a non-trial purpose, else `ParseError`; the line counts as
  `found` and adds 0, so `declared_trials_for_checkpoint(15) == 0` (`Ok(0)`, not `NotFound`).
- New: `Result<atx::u64> pre_registered_lines_for_checkpoint(const std::string& ledger_path,
  atx::i64 checkpoint)` — verified walk, count of `status == "pre-registered"` lines with that
  checkpoint; `Ok(0)` when none.
- The two cp14 lines are unaffected: `declared_trials_for_checkpoint(14) == 30` (§9.3 L4/L7).

### 5.8 Embedded design-note SHA and runner preflight

`kEquityUniverseDesignNoteRelativePath =
"atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md"` and
`kEquityUniverseDesignNoteSha256 = "<64 hex>"` in `stage_equity_universe.cpp`, mirroring
`stage_equity_ic.cpp:49-59` and ruling I-2: embedded, never read at runtime; the value is the
SHA-256 of this Revision 2 (T2 embeds it; the review addendum records it).
`iteration15_run_equity_universe.py` refuses to launch unless the on-disk note hashes to it
(`iteration14_run_equity_ic.py:60-67`); it is written into `request.json`, `manifest.json` and the
ledger `design-note` parent. **A later edit to this note is a new freeze**: re-embed, re-review, new line.

### 5.9 Manifest pins

`manifest.json.parents`: one entry per attached segment (`filename, sha256, size_bytes`), each
dir's `_ingestion.manifest.json` SHA and its `recipe.executable_sha256` — heterogeneous by
construction: `9415a6ab…` loaded the 2012–13 dir, `ac3ab17f…` the six 2014–2019 dirs (R15-9);
the manifest and the receipt state this explicitly (M-6) — each preparation manifest SHA,
`producer_executable_sha256`, `design_note_sha256`, the two ledger line SHAs. No
`PanelIdentity`/APNL is written (membership is not a panel); the FNV trailer of
`membership.bin` and its SHA-256 are both recorded.

## 6. Pre-registration block (FROZEN)

Changing any value below is a new pre-registration (new ledger line, new design SHA).

| Key | Frozen value | Ruling |
| --- | --- | --- |
| Rank key | ADV$ = §3.5 median of `close × volume` over the trailing 63 sessions ending at the rank session, inclusive | R15-1 |
| Window `W` | 63 sessions (session ordinals, not calendar days) | R15-1 |
| Minimum valid observations | 57 of 63 (`min_nonmissing_fraction` 0.9 ⇒ `ceil(56.7)`) | R15-1 |
| Missing bar | NaN slot; never filled; §2.3 validity | R15-1 |
| Price floor | raw `close` on the rank session `> 1.0`, strict | R15-5 |
| Bar on rank session | required (`last_bar == rank ordinal`) | R15-5 |
| `min_adv_usd` | 0 | R15-5 |
| Market cap | reported only: `shares × close`, label `no-filing-vintage`; NaN when `shares` NaN or `≤ 0` | R15-2 |
| Instrument type | unknown accepted; `gics_missing_members` reported | R15-6 |
| Cadence | monthly: rank session = attached session whose UTC month differs from the next attached session's month (last of the month BY DATA); the last attached session is never one; `[start,end]` filter after | R15-4, DR15-8 |
| Effective | the next observed session after the rank session | R15-4 |
| Rank dates | 2012-12-31 .. 2019-11-29, **84** rebalances (verified against the input profile: first 2012-12-31, 2013-01-31, 2013-02-28 …, last 2019-09-30, 2019-10-31, 2019-11-29); membership covers 2013-01-02 .. 2019-12-31 | R15-4 |
| Real-run flags | `--rank-start 2012-12-31 --rank-end 2019-11-29`, the seven `--segments-dirs` and `--preparation-manifests` of §2.1 in date order; the runner asserts `rebalances == 84` from `request.json` | DR15-3 |
| Member order | set = keep-then-fill; CSV/view order = rank ascending; `.bin` = id ascending + parallel rank | DR15-2 |
| Warmup | 63 sessions; 2012 data is warmup only; no 2012 membership emitted | R15-8 |
| `top_n` set | {1000, 2000, 3000}, all emitted side by side | R15-3 |
| `band` set | {0.00, 0.10} as `band_bp` {0, 1000}; `K_band = top_n + top_n*band_bp/10000` = {1000,1100,2000,2200,3000,3300} | R15-4 |
| Band rule | incumbent (member of the same cut at `r-1`) kept iff `rank ≤ K_band`; §3.4 3a–3b ordering; best-ranked `top_n` when band-kept incumbents exceed `top_n` | R15-4 |
| Tie-break | key descending, then first-seen slot ascending (`universe.cpp:180-184`) | R15-12 |
| Median | §3.5, `(a+b)*0.5` binary64; `dv = close*volume` one product | R15-1, DR15-5 |
| Drop classification | formula only: `LastBar` iff `last_bar < rank ordinal`, else `Rank` | R15-14, DR15-10 |
| Maxima | `max_source_ids` 32768, `max_rebalances` 4096, `max_sessions` 8192, `kPitMaxCuts` 8, overflow-checked; ID→slot by pre-sized open-addressing hash (capacity next pow2 ≥ 4·32768), any positive i64 ID accepted | R15-12 as revised by DR15-1 |
| Seal | refuse any segment ≥ 2020-01-01; `rank-end ≤ 2019-12-31` and `<` last attached session; constants unchanged | R15-13 |
| Inputs | seven segment dirs, policy `tickerhistory-qa-v1`, quarantine-all duplicates | R15-9 |
| Ledger | one cp15 line, purpose `point-in-time-universe-construction`, `trial_count_declared` 0 (§5.6) | R15-3 |
| Design SHA | embedded after review (§5.8) | R15-16 |

**Trial accounting.** `N_15 = 0`. "Universe cuts are restrictions to be reported side by side
(AR-7 semantics); selecting among them later is a new trial" (cp14 design §4.4 lines 939-943,
§11.2 AR-7 lines 2275-2280). `declared_trials_for_checkpoint(14)` stays 30; the two existing
ledger lines are untouched.

## 7. Memory and runtime bound

Engine state at the frozen dimensions (§3.2; `U = 32768`, `W = 63`, `H = 131072`, `R = 84`,
`C = 6`, `N = 3000`, `S = 1955`):

```text
B_engine(U,W,H,R,C,N,S) = 8*U*W (16,515,072) + 16*H (2,097,152) + 48*U + 9*C*U + C*U (3,538,944)
   + 8*R*C*N (12,096,000) + 16*R + 20*R*C + R*(12+12*C) + 12*S (41,940) + 61*U + 8*W + 20*C*N (2,359,352)
   = 36,648,460 B ≈ 36.6 MB
```

Stage additions: one mapped segment ≤ `max_segment_bytes` (2012–13 files 0.9–1.3 MB; 2019 at
8,456 rows/session ⇒ ≤ ~1.6 MB), the two streamed CSV writers (1 MiB buffer each; on disk
`membership.csv` ≤ `R*C*N = 1,512,000` rows × ~110 B ≈ 166 MB, never in memory — DR15-11),
`membership.bin` string `≈ 12*R*C*N ≈ 18.1 MB` (encode allocates once, post-run), JSON manifests
≤ 16 MiB read one at a time, `kOverheadReserve = 512,000,000` (`stage_equity_ic.cpp:45`).
Preflight: `B_engine + max_segment_bytes + 16 MiB + 2 MiB + 20 MiB + kOverheadReserve ≤
--max-working-bytes` (`OutOfRange` otherwise); ≈ 0.6 GB against the 3 GB default. The sum of attached segment bytes
(≈ 2.3 GB over 1,955 files) is **never** loaded — one mapping alive at a time, released before
the next `attach`, no `prefetch()` (bounded design line 11). Runtime: `O(S*U)` sweep ≈ 64 M
writes + `O(R*U log U)` sorts — seconds. Peak working set is measured by the runner at 100 ms
sampling as in cp14 (`stage_equity_ic.cpp:63-65`); expectation < 200 MB (cp14 peaked at
165,363,712 B on larger inputs; research §C).

## 8. Oracle and comparator (R15-15)

### 8.1 `build-equity/audits/iteration15_universe_oracle.py`

Stdlib only (`fractions`, `hashlib`, `json`, `struct`); reads only this design note; opens its
output with mode `"x"`; emits `{"schema":"atx-iteration15-universe-oracle-v1","design_note_sha256":…,
"cases":[{"case_id","family","inputs":{…},"expected":{…}}]}`. Families:

| Family | Fixture / rule pinned | Expected keys |
| --- | --- | --- |
| F1 ADV window, gaps, ties, median | 63-slot ring with 57/56 valid; even/odd valid counts; exact ties → slot order; includes non-finite/non-positive close and negative volume bars as missing | `adv63_usd` (JSON number `float(exact)` + `.hex()`), `valid_observations`, `eligible`, `rank` |
| F2 price floor, rank-session bar | close 1.0 (fail), 1.0000000000000002 (pass), absent on rank session (fail) | `eligible` |
| F3 cadence, effective session | synthetic key list with a month whose last session is mid-month and a trailing session; `select_monthly_rank_sessions` rule (DR15-8); effective = next session | `rank_session_keys[]`, `effective_session_keys[]` (integers) |
| F4 banding threshold | `top_n=2, band_bp=1000` ⇒ `K_band = 2`; `top_n=10, band_bp=1000` ⇒ 11: incumbent at rank 11 kept, at rank 12 dropped | `members[]` (rank order), `status[]`, `churn` |
| F5 drop classification | incumbent absent on rank session → `drops_last_bar`; present with `close = NaN` → `drops_last_bar`; present at close 0.5 (valid, floor-fails) → `drops_rank` (DR15-10) | `churn` |
| F6 coverage / union / survivorship | 3-year synthetic panel; per-year medians (`.5` cases) emitted as `<name>_x2` = `2·median`, an exact integer (DR15-15: medians are integer or `.5`), `distinct/cumulative`, `ended_before_window_end`, `censored`, per-year exits, `valid_observations_total`, `gics_missing_members` | exact integers only (DR15-4/DR15-15): `ids_with_valid_bar_median_x2`, `eligible_median_x2`, `members_median_x2`, `gics_missing_members_median_x2`, counts |
| F7 band retention set AND order | 3 incumbents within band, `top_n = 2`: best-ranked 2 kept, third dropped as `Rank`; and the C-2 shape (`top_n = 10`, incumbents at ranks 5 and 11 → `[1,2,3,4,5,6,7,8,9,11]`, statuses `add×4,keep,add×4,keep`) | `members[]` in rank order, `status[]`, `churn` |
| F8 `membership.bin` layout | 2 rebalances × 2 cuts encoded by the §4.7 layout in Python `struct` incl. the rank array; FNV-1a-64 trailer; native side = engine `encode_membership_bin` | `sha256_hex`, `byte_length` |
| F9 turnover and exit kind | `one_way_turnover = (adds+drops)/(2*top_n)` as `float(Fraction)`; `exit_kind` on the case-23 shapes with the DR15-14 integers 0 = `rank_drop`, 1 = `last_bar_within_window`, 2 = `window_end` | `one_way_turnover` (JSON number), `exit_kind` (0/1/2) |

### 8.2 Native marker

Engine gtests print one line per case for every family F1–F9 (all native sides live in
`atx-engine-data-tests`, DR15-6/DR15-8):

```text
POINT_IN_TIME_UNIVERSE_MEASUREMENT {"schema":"atx-point-in-time-universe-measurement-v1","case_id":"…","family":"F1",
  "inputs":{…same keys as the oracle case…},"values":{…same keys as expected…}}
```

Key list is closed: `schema, case_id, family, inputs, values`; a marker with any other top-level
key fails the comparator. **Reals (DR15-5):** the oracle writes each real as a JSON number
(`float(exact)`, integral values forced to `.0` as cp14) plus its `.hex()`; the native marker
writes `std::to_chars` shortest text as a JSON number; the comparator parses both with `float()`
and asserts `Fraction(a) == Fraction(b)` (the cp14 method, `iteration14_native_comparator.py:143`).
Integer fields are compared as integers. Every median in a marker is an integer
`<name>_x2 = 2·median` (DR15-15); no `.5` ever appears in a marker; the CSVs keep the real
median. Nothing is compared as text.

### 8.3 `build-equity/audits/iteration15_native_comparator.py`

Copied protocol from `iteration14_native_comparator.py:12-19, 27-40`: `--oracle`, `--logs`
(the `ctest -VV` log of `atx-engine-data-tests` — one log; the marker is located inside the
line), `--out` refused if it exists; re-derives F3/F4/F7/F9 rules and F8 bytes from the oracle's
declared inputs before touching native data;
de-duplicates byte-identical repeats, fails on conflicting duplicates of one `case_id`; every
family is exact (no tolerance); every oracle case with no native line is recorded as
`not_measured_natively` and the exit code is nonzero. Output schema
`atx-iteration15-universe-comparison-v1`.

## 9. Test plan

### 9.1 Engine — `atx-engine/tests/data/data_point_in_time_universe_test.cpp`, suite `DataPointInTimeUniverse`

Synthetic sessions via `observe_session`; keys = consecutive UTC midnights from 2013-01-02; `W = 63` unless stated; no file I/O.

| # | Case | Fixture → exact assertion |
| --- | --- | --- |
| 1 | `LateEntrant_ExactlySixtyThreeObservations_IsEligible` | ID enters at session 10; rank at session 72 → `valid_observations == 63`, eligible; rank at 71 → 62 valid... still ≥ 57, eligible; rank at 65 → 56 valid, **ineligible** |
| 2 | `Gap_BreaksWindowButLaterBarsCount` | 63 bars with 7 missing → `valid == 56` ineligible; with 6 missing → 57 eligible; `adv` = median of the 57 |
| 2b | `BarValidity_EveryInvalidShape_IsMissing` | one bar each with close NaN / +inf / 0 / −1, volume −1 / NaN → `valid_observations` reduced by exactly that count; volume 0 still valid (rule 1) |
| 3 | `ZeroVolume_IsValidAndZeroAdvPassesFloor` | all 63 bars volume 0 → `adv == 0.0`, eligible (`min_adv_usd == 0`), rank last among positive keys |
| 4 | `Median_EvenCount_IsHalfSum` | 58 valid values 1..58 → `adv == 0.5*(29+30) == 29.5`; 57 values → `29` |
| 5 | `Ties_BreakByFirstSeenSlotAscending` | 3 IDs identical dv, fed in order 30,10,20 → ranks 1,2,3 for slots 0,1,2 (ids 30,10,20) |
| 6 | `PriceFloor_RawCloseStrictlyAboveOne` | close 1.0 → ineligible; `std::nextafter(1.0, 2.0)` → eligible |
| 7 | `RankSession_NoBar_IsIneligibleEvenWithFullWindow` | 63 valid bars then absent on rank session → ineligible |
| 8 | `Effective_IsNextObservedSession_ViewStaysValid` | rebalance at key k; `effective_key == 0`; observe k+1 day → `effective_key(r) == k+1 day` and the view's spans still read the same bytes; observe k+3 (weekend gap) instead → `== k+3`; `encode` with a pending rebalance → `Internal` |
| 9 | `Band_KeepsIncumbentAtThresholdDropsAtPlusOne` | `top_n = 10, band_bp = 1000` → `K_band = 11`; incumbent falls to rank 11 → `Keep`; rank 12 → `drops_rank == 1`; `band_bp = 0` → dropped at rank 11 |
| 9b | `FirstRebalance_AllAddNoDrops_ShortListLegal` | `r = 0` with 3 eligible and `top_n = 5` → `members == 3`, all `Add`, `drops == 0`, `kept == 0` (rule 5) |
| 9c | `MemberOrder_IsRankAscendingNotKeepThenFill` | the C-2 fixture: incumbents at ranks 5 and 11 (`K_band = 11`) → view `members[c]` = ranks `[1..9, 11]` ascending, statuses per F7; `members(r,c)` slot ascending (DR15-2) |
| 10 | `Band_IncumbentsExceedTopN_BestRankedKept` | `top_n = 2, band 1000`: 3 incumbents at ranks 1,2,3 (`K_band = 2`), so 3 is dropped as `Rank`, list = ranks 1,2; with `top_n = 3` (`K_band = 3`) all kept, a rank-4 entrant not added |
| 11 | `Drop_KindByFormulaOnly` | incumbent absent on rank session → `drops_last_bar == 1`; present with close NaN → `drops_last_bar == 1`; present at close 0.5 → `drops_rank == 1` (DR15-10); `drops` span slot-ascending (rule 3/M-3) |
| 12 | `TruncationInvariance_LaterSessionsDoNotChangeEarlierMembership` | run A: sessions 0..99 with rebalances at 62 and 80; run B: sessions 0..80 only; `members(0,c)`, `members(1,c)`, churn, ranked rows byte-identical |
| 13 | `Determinism_TwoBuildersIdentical` | same feed twice → `memcmp` equality of every retained span and `encode`-able state |
| 14 | `Id_LargeArchiveIds_AcceptedNonPositiveRejected` | ids 4,163,749 and 1,001,001,001,070 accepted, slots 0 and 1 (first-seen), both rank normally; id 0 / −1 → `OutOfRange` (DR15-1) |
| 14b | `IdTable_FullLoad_NoProbeEscape` | `max_source_ids = 8` → `H = 32`; 8 distinct ids inserted; a 9th → `OutOfRange` "max_source_ids" with state unchanged; every id re-found |
| 15 | `Session_OutOfOrder_Rejected` | key equal to previous → `InvalidArgument`; earlier → `InvalidArgument`; builder state unchanged (next valid key still accepted) |
| 16 | `Session_DuplicateIdWithinCall_RejectedWithoutMutation` | on a FRESH builder (no prior session) ids {5,7,5} → `InvalidArgument` and `source_ids() == 0` (DR15-13: the repeated UNKNOWN id is caught by the provisional insertion); then a valid session: `source_ids`, `ids_seen`, ranking equal a fresh builder fed only the valid session (DR15-9); a second variant with a known id repeated {5,7,5} after {5} → same rejection |
| 16b | `Observe_MaxSessionsMaxSourceIdsSpanMismatch_RejectedNoMutation` | `max_sessions = 2` third session → `OutOfRange`; `max_source_ids` exceeded → `OutOfRange`; `volume.size() != ids.size()` → `InvalidArgument`; state unchanged after each (rule 2) |
| 16c | `Observe_EmptySharesAndGics_ReadNaN` | empty spans → `vendor_market_cap_usd` NaN, `gics` NaN, `gics_missing_members == members` (rule 2) |
| 17 | `Rebalance_KeyNotLastObserved_Rejected` | rebalance(key of session 3) after observing 5 → `InvalidArgument`; rebalance with no sessions → `InvalidArgument` |
| 17b | `Rebalance_SameSessionTwiceOrBeyondMax_Rejected` | second `rebalance` on the same key → `InvalidArgument`; `max_rebalances = 1` second rebalance → `OutOfRange` (rule 3) |
| 18 | `Seal_SessionAtOrAfter2020_Rejected` | key `kPitSessionKeyEndExclusive` → `PermissionDenied`; `−1 day` accepted; and `EXPECT_EQ(kPitSessionKeyEndExclusive, eval::kValidationBeginNs)` |
| 19 | `Create_OverflowingMaxima_RejectedBeforeAllocation` | `max_source_ids = kPitMaxSourceIds + 1` → `InvalidArgument`; `max_rebalances*C*N` overflowing u64 → `OutOfRange`; `top_n_count = 0` → `InvalidArgument` |
| 20 | `Coverage_ThreeYearFixture_ExactCounts` | 3 synthetic years (4 sessions each, 1 rebalance/year): `sessions`, `ids_seen`, `ids_with_valid_bar_median` (a `.5` case), `eligible_median`, `members_median`, `gics_missing_members_median` exact; the F6 marker carries `*_median_x2` integers (DR15-15) |
| 20b | `Rebalance_ValidTotalAndGicsMissing_AreIntegers` | 3 members with valid 63/60/57 and one NaN gics → `valid_observations_total == 180`, `gics_missing_members == 1` (rule 4) |
| 21 | `Union_DistinctAndCumulative_ByEffectiveYear` | rank date last session of year Y → counted in year Y+1; `cumulative` monotone; exact integers |
| 22 | `Survivorship_ArchiveEndGuard_Censors` | ID with last bar on the final session → `censored`; last bar one session earlier → `ended_before_window_end`; per-year exit fraction on the fixture exact |
| 23 | `Exits_KindFollowsLastDropNotLastBar` | rank-drop in year 1, trading stops in year 2 → `exit_kind == rank_drop`, `last_bar` in year 2; member at final rebalance → `window_end` |
| 23b | `Exits_OrderedByIdWithFirstSeenFirstBarAndMemberRebalances` | `exits(c)` returns `security_id` ascending; `first_seen` (first appearance, possibly invalid) < `first_bar` (first valid) on a fixture with an invalid first bar; `first_member_rebalance`/`last_member_rebalance` exact (rule 12) |
| 24 | `VendorMarketCap_NaNOrNonPositiveShares_IsNaN` | shares NaN / 0 / −1 → NaN; 100 × close 2.5 → 250.0; eligibility unaffected |
| 25 | `DuplicateDatesCoverage_2018Shape` | a session where two IDs are absent (simulating quarantined rows) → `ids_with_valid_bar` lower by 2 on that session only; their ring slot NaN; coverage counts them as missing, not as IDs unseen |
| 26 | `OracleMarkers_EmitAllFamilies` | prints the §8.2 marker for every F1–F9 oracle case id |
| 27 | `MembershipBin_RoundTripAndRefusals` | `encode` → `decode` equality incl. ranks; bad magic → `InvalidArgument`; version 2 → `InvalidArgument`; short buffer → `InvalidArgument`; flipped trailer byte → `Internal`; F8 marker with `sha256_hex` (rule 7, DR15-6) |
| 28 | `SelectMonthlyRankSessions_LastByDataNeverLastAttached` | keys {Jan 2, Jan 15, Feb 3, Feb 27, Mar 1}: `[start=Jan 1,end=Mar 31]` → {Jan 15, Feb 27} (Mar 1 is last attached, never selected); `end = Feb 26` → {Jan 15}; `end = Jan 15` → {Jan 15}; undersized `out` → `OutOfRange` (DR15-8) |

### 9.2 Impl — `atx-impl/tests/config_equity_universe_test.cpp` (`ConfigEquityUniverse`) and `stage_equity_universe_test.cpp` (`StageEquityUniverse`)

| # | Case | Assertion |
| --- | --- | --- |
| C1 | `Subcommand_IsFourteenthAndLast` | `kSubcommands.size() == 14`, `back() == "equity-universe"`, `[12] == "equity-ic"`; `parse_args` accepts it (mirrors the DR15-7 pin in `config_equity_ic_test.cpp`) |
| C2 | `Flags_OutsideAllowList_Rejected` | `--top-n`, `--band`, `--config`, `--panel` each → `InvalidArgument` naming the flag |
| C3 | `Flags_EmptyOrDashValues_Rejected` | each of the four new flags with `""` / `--x` value |
| C4 | `RankEnd_After2019_Rejected` | `--rank-end 2020-01-02` → `InvalidArgument` |
| S1 | `SealedSegmentInDir_RefusesNotSkips` | synthetic dir with `2019-12-31.seg` and `2020-01-02.seg` (built with `SegmentBuilder`) → `PermissionDenied`, nothing under `--out` |
| S1b | `SealedAxisBehindValidName_RejectedByAxisCheck` | `2019-12-31.seg` whose axis is 2020-01-02 → `InvalidArgument` from the axis==filename check (the engine `PermissionDenied` is unreachable through the stage; rule 9) |
| S2 | `MisnamedOrMultiDateSegment_Rejected` | file `2019-01-02.seg` whose axis is 2019-01-03, and a 2-row file → `InvalidArgument` |
| S3 | `FreshOut_ReservationDiagnostics` | existing dir → `AlreadyExists`; unwritable parent → `IoError` (the M-6 fix) |
| S4 | `PreparationManifest_MustMatchIngestionBinding` | wrong manifest → `InvalidArgument`; count mismatch → `InvalidArgument`; two correct manifests in swapped order → `InvalidArgument` (positional pairing, rule 8) |
| S5 | `Run_SyntheticThreeMonths_WritesAllFilesAndPins` | 3-month fixture (66 sessions, `--rank-end` = the second month-last session) → all 10 files exist, `manifest.json` pins every segment SHA, `churn.csv` `one_way_turnover` equals `(adds+drops)/(2*top_n)` recomputed, `coverage_by_year.csv` `dates_with_quarantined_duplicates`/`duplicate_positive_keys`/`rejected_rows` equal the fixture manifest's sums |
| S5b | `Csv_HeaderAndRowText_Pinned` | 2-name fixture: the exact header line and the first data line of `membership.csv` and `churn.csv` are string-equal to literals (`0.00`/`0.10` band, integer `gics`, empty NaN cells, `add|keep`, `nonmissing_fraction` text) and rows are in rebalance/cut/rank order (rule 6) |
| S5c | `Delisting_OrderAndDates` | rows cut-major then `security_id` ascending; `first_member_effective_date`/`last_member_rank_date` equal the fixture's dates (rule 12) |
| S6 | `Run_Deterministic_ByteIdenticalTwice` | two runs, two out dirs → byte-identical: `membership.bin`, `membership.csv`, `churn.csv`, `coverage_by_year.csv`, `union_by_year.csv`, `delisting.csv`, `survivorship.json`, `seal.json`; `request.json`/`manifest.json` differ ONLY in `trial_id`, `ledger_pre_sha256`, `ledger_terminal_sha256`, `runtime_seconds`, `appended_utc` (I-9) |
| S6b | `TrialId_IncrementsAcrossRuns` | after S6: lines `…-0001` then `…-0002` (rule 10) |
| S7 | `Ledger_PreAndTerminalLinesDeclareZero` | after S5: two new lines, `checkpoint 15`, `trial_count_declared 0`, purpose as frozen, `declared_trials_for_checkpoint(15) == 0`, `(14)` unchanged on the cp14 fixture |
| S8 | `Ledger_FailedRunKeepsBothLines` | inject a write failure after pre-registration → `failed` line with `failure_sha256`, `failure.json` present |
| S9 | `Budget_BelowReserve_Rejected` | `--max-working-bytes` = `kOverheadReserve` → `InvalidArgument`; a tiny budget above it → `OutOfRange` from preflight |
| S10 | `MembershipBin_IsEngineEncodingAndPinned` | the stage's `membership.bin` bytes equal `encode_membership_bin` of an identically fed builder; `manifest.json` records its SHA-256 and FNV trailer (DR15-6) |
| S11 | `DesignNoteSha_IsSixtyFourLowerHex` | shape check; the value itself is pinned by the runner preflight |
| S12 | `SegmentSha_MustMatchIngestionManifest` | a byte-flipped `.seg`, a `.seg` absent from the manifest, a manifest entry without a file → `InvalidArgument`, no ledger line (I-12) |
| S13 | `LargeSecurityId_FlowsThroughStage` | synthetic segment carrying symbol `1001001001070` → run succeeds, id present in `membership.csv`/`.bin` (DR15-1) |
| S14 | `SegmentsDirs_Rejections` | a dir path containing `;`, a `foo.seg` name, the same date in two dirs, overlapping dir ranges, `< 63` sessions before the first rank session, `--rank-end` equal to the last attached session → each `InvalidArgument` (rule 8, C-6) |

### 9.3 `TrialLedger` new cases (append to `atx-impl/tests/trial_ledger_test.cpp`)

| # | Case |
| --- | --- |
| L1 | `ZeroDeclared_AcceptedOnlyForNonTrialPurpose` — purpose in allow-list + 0 → serialize/append Ok; purpose `training-only-forecast-evaluation` + 0 → `InvalidArgument` |
| L2 | `PositiveDeclared_RejectedForNonTrialPurpose` — allow-listed purpose + 1 → `InvalidArgument` |
| L3 | `NegativeDeclared_AlwaysRejected` |
| L4 | `DeclaredTrials_SumsZeroLinesAndFindsThem` — one cp15 zero line → `Ok(0)`; mixed cp14/cp15 fixture → `(14) == 30`, `(15) == 0` |
| L5 | `DeclaredTrials_ZeroWithTrialPurpose_IsParseError` — hand-written line → `ParseError` |
| L6 | `PreRegisteredLines_CountsByCheckpoint` — 0 / 1 / 2 lines |
| L7 | `ExistingCp14Ledger_StillVerifies` — the committed `atx-engine/reviews/trial-ledger.jsonl` + sidecar verify unchanged (skipped if absent from the test's cwd) |
| L8 | `DeclaredTrials_PositiveWithNonTrialPurposeInWalk_IsParseError` — hand-written pre-registered line, allow-listed purpose, `trial_count_declared 1` → `declared_trials_for_checkpoint` `ParseError` (rule 11) |

### 9.4 What is not tested and must not be claimed

No gtest asserts a real-data number (receipt only); no test claims survivorship completeness, instrument-type correctness, or PIT `shares`.

## 10. Tasks (R15-16 sequencing: T1 → (T2 ∥ T3) → one end review → T4)

| Task | Files (disjoint between T2 and T3) | Acceptance |
| --- | --- | --- |
| **T1** engine unit (incl. `select_monthly_rank_sessions` and the `.bin` codec, DR15-6/DR15-8) | `atx-engine/include/atx/engine/data/point_in_time_universe.hpp`, `atx-engine/src/data/point_in_time_universe.cpp`, `atx-engine/tests/data/data_point_in_time_universe_test.cpp`, `atx-engine/CMakeLists.txt` (+1 line after :79) | `check` on the .cpp, `build atx-engine-data-tests`, `-Ctest -R DataPointInTimeUniverse` all green; §9.1 cases 1–28 |
| **T2** stage + wiring + ledger | `atx-impl/src/stage_equity_universe.{hpp,cpp}`, `atx-impl/src/config.{hpp,cpp}`, `atx-impl/src/dispatch.cpp`, `atx-impl/src/trial_ledger.{hpp,cpp}`, `atx-impl/tests/config_equity_universe_test.cpp`, `atx-impl/tests/stage_equity_universe_test.cpp`, `atx-impl/tests/trial_ledger_test.cpp`, `atx-impl/tests/config_equity_ic_test.cpp` (`:65-66` pin only, DR15-7), `atx-impl/CMakeLists.txt` (+1 line after :34) | `build atx-impl-tests`, `-Ctest -R "ConfigEquityUniverse|StageEquityUniverse|TrialLedger|ConfigEquityIc"`; byte-identity of every other subcommand's stage outputs; the only test edit outside the new files is the `config_equity_ic_test.cpp` pin |
| **T3** oracle + comparator + runner | `build-equity/audits/iteration15_universe_oracle.py`, `build-equity/audits/iteration15_native_comparator.py`, `build-equity/audits/iteration15_run_equity_universe.py` (`--dry-run`, `--attempt N` versioning of audit files and `_attemptN` data dir — `iteration14_run_equity_ic.py:10-12, 125-143`; design-SHA preflight `:60-67`; 100 ms working-set sampler `:200-331` (M-4); the pinned real-run flags of §6; asserts `rebalances == 84` from `request.json`) | oracle document written; comparator self-test; runner `--dry-run` prints the exact command |
| **T4** real run + receipt + docs (parent-only native) | `build-equity/audits/iteration15_write_validation_receipt.py`, `atx-engine/reviews/2026-09-20-iteration15-validation.json`, `atx-engine/reviews/trial-ledger.jsonl` (+2 lines, append-only), `atx-engine/docs/PLATFORM_PROGRESS.md` (insert `## Checkpoint 15: point-in-time universe over 2013–2019` after the cp14 section and BEFORE `## Next candidates, subject to measurement` at :875; revise item 1), `atx-engine/README.md`, `atx-impl/README.md`, new `atx-impl/docs/EQUITY_UNIVERSE.md` (structure of `EQUITY_IC.md:1-12`) | comparator exit 0 with zero `not_measured_natively`; receipt records the 84 rebalances, per-cut union sizes vs 4096, survivorship fractions, peak WS, wall time, the two loader executables (M-6), no `.meta.txt` reference (M-9), CMake-pin drift note (progress "cp15 prep": cp13/cp14 receipt pins drift — recorded in a cp15 addendum, receipts untouched) |

Ingestion of 2014–2019 (R15-9) is complete (§2.1) and is not a task here.

## 11. Risks and what this checkpoint does NOT establish

1. **No alpha, no forecast, no Sharpe, no capacity.** Membership lists are inputs to a future
   measurement; nothing here is evidence that anything predicts anything.
2. **No float or common-stock eligibility** (R15-6): ETFs, ADRs, preferreds and funds can rank
   into the top 3000 by dollar volume; `gics_missing_members` is reported, not applied.
3. **Survivorship fractions are lower bounds** (R15-10): backfill policy unknown; names never in
   the archive are invisible; no delisting return is imputed (Shumway −30 % / −55 % cited in §13, not applied).
4. **Vendor `shares` unreliable**: lags splits, zeros; `vendor_market_cap_usd` must not be used
   as a size screen downstream without a filing-dated source.
5. **2018 thinning**: 83 dates (86 rows) with quarantined duplicate keys reduce valid bars on those
   sessions; a name hit on ≥ 7 of its trailing 63 sessions loses eligibility on that rank date.
   Counted in `coverage_by_year.csv`; not repaired.
6. **ID reuse** (77622) makes at most a handful of histories composite.
7. **Ledger schema shape** (§5.6, accepted by DR15-16): the v1 line carries cp14 recipe defaults; readers must honour `notes`.
8. **cp16 cap**: if `cumulative_t3000_b10` at 2019 exceeds 4096, cp16 must go per-year or raise
   `kMaxIcInstruments` — this checkpoint only measures the number (R15-7).
9. **CMake pin drift**: the two one-line CMake edits drift the cp13/cp14 receipt pins as cp14 drifted cp13; recorded in the cp15 addendum, never by editing receipts.

## 12. Open questions for the parent

All four questions are closed; none remains open for implementation.
- **Q1 — ACCEPTED by parent ruling DR15-16:** the `atx-trial-ledger-v1` shape with a single
  `recipe.signals` entry (§5.6), cp14 defaults declared inapplicable in `notes`; no `v2` schema.
- **Q2 — RESOLVED** (review checklist 6: "Two sites :301 and :729-733 extended (Q2 confirmed)").
- **Q3 — ACCEPTED by parent ruling DR15-16:** `;`-separated `--segments-dirs` /
  `--preparation-manifests` (§5.2); no parser change to accumulate repeated flags.
- **Q4 — RESOLVED by DR15-6**: codec and F8 marker in the engine unit; one ctest log.

## 13. References (research §D; one line each on the rule it supports)

1. Shumway (1997) JF 52(1), https://www.tylergshumway.org/Shumway-DelistingBiasCRSP-1997.pdf — performance delistings lack returns; −30 % NYSE/AMEX convention, cited in §11.3 as NOT applied.
2. Beaver, McNichols, Price (2007), https://www.sciencedirect.com/science/article/abs/pii/S0165410106000930 — application of the −30 % rule; same status.
3. Shumway & Warther (1999) JF 54(6), https://onlinelibrary.wiley.com/doi/abs/10.1111/0022-1082.00192 — −55 % Nasdaq convention; same status.
4. FTSE Russell, https://www.lseg.com/en/insights/ftse-russell/how-can-my-company-get-into-the-russell-us-indexes and https://www.lseg.com/content/dam/ftse-russell/en_us/documents/ground-rules/russell-us-indexes-construction-and-methodology.pdf — rank-day snapshot, closing price ≥ $1.00 on rank day (R15-5's floor), banding as hysteresis (R15-4's band).
5. Nasdaq, https://www.nasdaq.com/articles/2019-russell-reconstitution-and-potential-impact-to-your-stock-2019-06-24 and https://www.lseg.com/en/media-centre/press-releases/ftse-russell/2019/russell-us-indexes-annual-reconstitution-period-begins-projected-additions-deletions-posted — 157 deletions / 166 additions of 3000 in 2019 ⇒ ≈ 5.2 % deletions/yr, the §4.6 comparison figure.
6. MSCI GIMI, https://www.msci.com/eqb/methodology/meth_docs/MSCI_GIMIMethodology_May2021.pdf — 3-month trading-history and frequency-of-trading screens ⇒ the 63-session window and the 57/63 minimum.
7. S&P U.S. Indices, https://www.spglobal.com/spdji/tc/documents/methodologies/methodology-sp-us-indices.pdf — volume-traded liquidity screens on trailing months; supports dollar volume as the liquidity key.
8. Ken French, https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/data_library/det_me_breakpoints.html and https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/f-f_portfolios.html — the first URL states month-end ME breakpoints only; the forward-application attribution (rank-at-t, effective-at-t+1, R15-4) rests on the second URL, which the reviewer did not verify (M-8). Ref 5's nasdaq.com page was unreachable at review; the LSEG press release is the primary source for 157/166. Refs 1–3, 6, 7, 9, 10 are cited from research §D and were not re-fetched.
9. https://www.luxalgo.com/blog/survivorship-bias-in-backtesting-explained/ and https://www.susanpotter.net/quant/backtest-bias-taxonomy/ — magnitude of survivorship bias; why §4.6 is mandatory and why its number is a lower bound.
10. CRSP, https://www.crsp.org/wp-content/uploads/2023/08/CRSP10-User-Guide.pdf — delisting date/code/return are distinct from price observations; the archive lacks them (§2.5), so `exit_kind` is inferred from bars, never asserted as a delisting.

## 14. Parent rulings after design review (2026-09-20, binding)

Verbatim from `progress.md` ("cp15 design review" and "cp15 design scoped re-verify" entries); the design sections changed follow each.

- **DR15-1 (C-1)** R15-12 direct-index table REVOKED — real IDs exceed 2^20 (2,199 of 7,650 IDs > 2^20 in 2013, max 4,163,749; archive ID 1,001,001,001,070): ID→slot via pre-sized open-addressing hash (linear probing, capacity = next pow2 ≥ 4·max_source_ids, int64 keys, built in ctor, no heap in loop); slot numbers assigned first-seen (determinism independent of hash); > max_source_ids distinct IDs → error. — §3.1, §3.2, §3.3, §3.6, §6, §7, §9.1 #14/#14b, §9.2 S13. *Ruling vs reviewer:* the reviewer proposed `1<<24` + an overflow map; the hash table was ruled instead. Hash function and empty-key sentinel pinned in §3.2.
- **DR15-2 (C-2)** membership SET = keep-then-fill construction; membership OUTPUT ORDER = rank ascending in membership.csv (rank = position in the eligible ranked list, for kept and added alike); membership.bin stores ids sorted ascending by security_id with a parallel u32 rank array; F7 tests both. — §3.1, §3.4 3f (three-orders table), §4.7, §6, §8.1 F7, §9.1 #9c.
- **DR15-3 (C-3/C-6)** a rebalance is EMITTED only once its effective session has been observed; rows are written at the effective observe_session (or by finalize); a rank date with no following attached session is a stage ERROR, not a silent drop; real-run `--rank-end` PINNED 2019-11-29 (last rank date), so effective_session_key is never 0 in membership.bin. — §3.1 (view validity), §3.7, §4.1, §4.7, §5.2, §5.5, §6 "Real-run flags", §9.1 #8, §9.2 S14. Option (i) of the reviewer's C-3 fix is the one taken (view kept across `observe_session`; no engine retention of per-member reals).
- **DR15-4 (C-4)** markers carry integers only (valid_count, window=63); nonmissing_fraction in CSV = static_cast<double>(valid_count)/63.0, not compared by the comparator. — §3.1, §3.4 3e, §4.1, §4.3, §8.1 F6, §9.1 #20b. The reviewer's one-division formula is kept for the per-year median column, reported only.
- **DR15-5 (C-5)** comparator compares reals as Fraction(float) exact equality (cp14 method), never text; oracle reproduces double arithmetic: dollar_volume = close*volume as one double product stored before use (no FMA shape), median of two = (a+b)*0.5 in double; text serialization `.0` forced for integral reals as cp14. — §3.3, §3.5, §6, §8.1 F1, §8.2.
- **DR15-6 (I-1)** apply Q4: membership.bin encoder+decoder in the engine unit. — §3.1, §4.7, §5.1, §8.2, §8.3, §9.1 #27, §9.2 S10, §10 T1/T2, §12 Q4.
- **DR15-7 (I-2)** T2 file list adds atx-impl/tests/config_equity_ic_test.cpp:65 (13→14). — §5.1, §9.2 C1, §10 T2.
- **DR15-8 (I-3/I-4)** engine exposes pure `select_monthly_rank_sessions(span<const i64> session_keys, ...)`: rank session = attached session whose UTC calendar month differs from the NEXT attached session's month (last session of the month by data, not by calendar); the last attached session is never a rank session; natively tested (F3). — §3.1, §5.2, §5.5, §6 "Cadence", §8.1 F3, §9.1 #28. This is the reviewer's "reading A".
- **DR15-9 (I-5)** observe_session validates fully before mutating any state. — §3.1, §3.3 (two passes), §9.1 #16/#16b.
- **DR15-10 (I-6)** drop kind by formula only: drops_last_bar iff last_observed_session(slot) < rank_session_ordinal; else drops_rank. — §3.4 3c, §6, §8.1 F5, §9.1 #11.
- **DR15-11 (I-7/I-8)** per-slot first_seen ordinal u32 stored; membership.csv written by a buffered streaming FILE writer (not write_text); §7 adds its size bound (≤ 84·6·3000 rows). — §3.1, §3.2, §3.7, §4 intro, §4.5, §5.1, §7, §9.1 #23b. Both `first_seen` (ruling) and `first_bar` (reviewer, first VALID bar) are stored; `delisting.csv.first_bar` is the latter.
- **DR15-12 (I-9/I-11 + Minors)** apply as reviewer states; coverage_by_year adds `rejected_rows`, `duplicate_positive_keys` sourced from preparation manifests (per ingest ruling). — I-9 §9.2 S6; I-10 §3.2/§7; I-11 §2.4/§4.3; I-12 §5.5/§9.2 S12; M-1 §3.4 3b; M-2 §3.2; M-3 §3.4 3c; M-4 §10 T3; M-5 §5.3; M-6 §5.9/§10 T4; M-7 §4.6; M-8 §13; M-9 §10 T4; the 12 rules-without-tests → §9.1 #2b/#9b/#16b/#16c/#17b/#20b/#23b/#27, §9.2 S1b/S4/S5b/S5c/S6b/S14, §9.3 L8; F9 added (§8.1).
- **DR15-13 (N-1)** pass 1 inserts unknown ids into id_table provisionally (bounded list of provisional slots), so a repeated unknown id is caught by lookup; on any validation failure all provisional insertions are rolled back (table entries cleared, slot counter restored) before returning — no state mutation survives a rejected call; test #16 on a fresh builder must reject {5,7,5}. — §3.3 pass 1 (b)/(c), §9.1 #16.
- **DR15-14 (N-2)** exit_kind integers pinned 0=rank_drop, 1=last_bar_within_window, 2=window_end; PitDropKind enum values and CSV text follow this order exactly. — §3.1 `PitExitKind`, §4.5, §8.1 F9.
- **DR15-15 (N-3)** every marker median is emitted as `<name>_x2` integer (2·median, exact since medians are integer or .5); no `.5` in markers; CSV keeps the real. — §8.1 F6, §8.2, §9.1 #20.
- **DR15-16** §12 Q1 and Q3 stated ACCEPTED per parent rulings (v1 ledger shape; `;`-separated dir/manifest flags). Revision 3 = these four edits only; parent verifies by grep, then FREEZE (SHA embedded as kDesignNoteSha256 by T2). — §5.6, §11 #7, §12.

## 15. Implementation rulings (T1/T3, pre-T2 freeze)

Append-only amendment (cp14 §11.x pattern); sources: `progress.md` "cp15 T3"/"cp15 T1"/"cp15 T1
measured", `cp15-task-T3-report.md` §5/§6, `cp15-task-T1-report.md`. Binding for T2, T4 and the real run; supersedes where it narrows. §§1–14 unchanged.

### 15.1 Oracle authority (T3 pin 1)

`build-equity/audits/iteration15-universe-oracle-v1.json` (sha `8b21cf57…`; 42 cases: F1 10, F2 3, F3 5,
F4 4, F5 4, F6 3, F7 3, F8 2, F9 8; producer `iteration15_universe_oracle.py` sha `7fea11ca…`) is
**authoritative** for native markers: its `case_id`s, `inputs` and `expected` key names are exactly what
`atx-engine-data-tests` emits (§9.1 #26 = these 42); native `inputs` differing from the oracle's FAIL the
case (pin 11). Its fixtures bind: F6 `W = 3`, `min_valid_observations = 2`, cuts top3/top7 band 0; F8
top_n {2,3} × band {0}, ids fed out of order (228 bytes); F4/F5/F7 two 63-session regimes + session 126.

### 15.2 T3 pins 2–13 (all ACCEPTED)

| # | Pin |
| --- | --- |
| 2 | §8.1 F7 / §9.1 #10 "3 incumbents, `top_n = 2`" is unrealisable (incumbents ≤ n). **Realisable reading:** 2 incumbents at ranks 2 and 3 with `K_band = 2` → best-ranked kept, the other dropped as `Rank`; the `top_n 3 / K_band 3` half stands. §3.4 3a's stop-at-n stays coded, unreachable. |
| 3 | `rank = 0` = "not ranked"; `adv63_usd` / `valid_observations` exported for eligible rows only. |
| 4 | Marker integers: `PitMemberStatus` Add 0 / Keep 1; `PitDropKind` Rank 0 / LastBar 1; `exit_kind` per DR15-14; `drops` exported as `[security_id, kind]` in slot order; `churn` nested. |
| 5/6 | Year attribution is by the **EFFECTIVE** session's year. A year with zero emitted rebalances gets **empty CSV fields** for every rebalance-derived column and **`null`** markers; `survivorship.per_year` omits a year whose first observed session is not an effective session. Real run: 2013..2019 all have rebalances; 2012 has none (2012-12-31 rank is effective 2013-01-02) → its row carries `sessions`, the three preparation columns, `ids_seen`, `ids_with_valid_bar_median`, `rebalances = 0`, empty medians. (Narrows §4.3's rank-session attribution.) |
| 7 | F9 `one_way_turnover` restated test-side in `atx-engine-data-tests` from the churn integers, one binary64 division; the stage writer computes the same expression. |
| 8 | Measurement JSON is always `iteration15-equity-universe-measurement-attemptN.json`; the data directory follows the cp14 rule (attempt 1 plain, `_attemptN` after). |
| 9 | `request.json` key **`rebalance_count`** (84 in the real run); the runner also requires the decoded `membership.bin` header to say 84 with the pinned first/last keys. |
| 10 | Manifest digests are read by a generic walker over objects with `sha256` + `filename|file|name|path`; T2 writes `filename` + `sha256` + `size_bytes` (§4.8). |
| 12 | F8 also exports `fnv1a64_trailer`, `rebalance_count`, `header_bytes_hex`, derived from the same image. |
| 13 | Runner stale-binary check = substring `equity-universe` in the exe bytes; ledger/manifest checks catch a build that names the subcommand without implementing it. |

### 15.3 T1 deviations 1–8 and ambiguities 1–11 (all ACCEPTED)

Deviations: (1) #10 fixture as 15.2/2; (2) ≈ 2.0 MB extra pre-sized scratch beyond §3.2 (`ranked_scratch`
U rows, `new_bits` u8·U, `query_median` f64·max(S,R), `union_bitmap` u8·U, `year_mark` u16·U) — engine
bound ≈ 38.7 MB, still no allocation on the observe/rebalance paths; (3) `kPitMaxYears = 16`, a 17th
calendar year → `OutOfRange`; (4) `select_monthly_rank_sessions` rejects non-ascending keys
(`InvalidArgument`); (5) `create` catches `std::bad_alloc` → `OutOfRange`; (6) decoder also refuses
`effective_session_key == 0`, non-ascending ids in a cut, member count beyond remaining bytes, trailing
bytes (`InvalidArgument`); encoder refuses counts not fitting u32 (`Internal`); (7) civil-date algorithm
reused from `atx/core/datetime.hpp` (`data/` never includes `eval/`); (8) native markers also force `.0`
on integral reals; comparison stays `Fraction(float)`.
Ambiguities: (1) accessors `config()`, `cuts()`, `ids_with_valid_bar(ordinal)`, `member_ranks(r,c)`,
`churn(r,c)`, `pit_civil_of` added to §3.1; (2) `nonmissing_fraction_median` divides by `adv_window * members`;
(3) a median over zero elements is `0.0`; (4) `union_by_year` skips a pending rebalance; (5) `rebalance`
checks: key == last observed → `max_rebalances` → key > previous rank key; (6) marker encodings as 15.2/4;
(7) `band_bp` unbounded u32, `K_band = n + q*b + (rem*b)/10000` saturating at `usize` max; (8) `first_year` =
year of the first observed session, arrays index `year - first_year`; (9) test #12 compares ranked rows
field-wise (`bit_cast<u64>` reals), not `memcmp`; (10) aggregate structs carry fixed `kPitMaxCuts` arrays
plus `cuts`; (11) NaN-valued and empty `shares`/`gics` spans are equivalent.

### 15.4 Decoder order (parent fix after T1 measurement) — supersedes the §4.7 wording

`decode_membership_bin` parses the body **structurally first** (short read, member count beyond the remaining
bytes, trailing bytes → `InvalidArgument`); only a well-formed body whose FNV-1a-64 trailer mismatches → `Internal` (verified last; §9.1 #27 holds).

### 15.5 Measured (T1)

`DataPointInTimeUniverse` **37/37** after the decoder fix (`atx-engine-data-tests.exe` sha `1e17b828…`;
`iteration15-data-t1fix1-tests.log/.xml`); 42 markers; comparator vs oracle v1 **42/42**, 0 failed, 0
`not_measured_natively` (`iteration15-native-comparison-t1.json`), first native run. The §14 SHA `be4b3b67…` (1,000 lines) is superseded by this amended file's SHA, which T2 embeds as `kDesignNoteSha256`.
