#pragma once

// atx::engine::data — point-in-time universe builder (checkpoint 15, task T1).
//
// Design: `atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md`
// (Revision 3, frozen). Section numbers below refer to that note.
//
// WHAT THIS IS (§1.1, §3)
//   A streaming, allocation-free-after-construction membership builder. The stage
//   feeds one session at a time (`observe_session`), asks for a rebalance on a rank
//   session (`rebalance`), and the builder ranks every eligible ID by the §3.5
//   median of the trailing `adv_window` dollar volumes, applies the §3.4 band rule
//   per cut (top_n x band), and retains only what §4 needs afterwards: `{slot,
//   rank}` per (rebalance, cut), churn counters, per-slot first/last bar ordinals
//   and per-(slot, cut) membership history.
//
// BOUNDED (§3.2, §7)
//   `create` validates the config against every `kPitMax*`, computes every buffer
//   size with overflow-checked multiplication, and performs ALL allocation. After
//   that `observe_session` and `rebalance` allocate nothing: the only sort is
//   `std::sort` on pre-sized scratch (in-place introsort; `std::stable_sort` may
//   allocate and is never used).
//
// DETERMINISM (§3.6)
//   Slots are assigned first-seen, so no output depends on the hash function; the
//   rank order is a total order (key descending, then slot ascending). A rebalance
//   at rank session R reads only sessions <= R, so later sessions cannot change it
//   (truncation invariance, §9.1 #12).
//
// ERRORS
//   Nothing throws on the observe/rebalance paths; every failure travels in
//   `atx::core::Result` / `Status` exactly as `universe.cpp` does. `observe_session`
//   validates everything before mutating anything (DR15-9): a rejected call leaves
//   the builder byte-identical to its prior state.

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

// ---------------------------------------------------------------------------
//  Frozen defaults and bounded maxima (§3.1, §6). The stage passes the defaults;
//  tests may pass smaller cuts.
// ---------------------------------------------------------------------------
inline constexpr atx::usize kPitAdvWindow = 63;
inline constexpr atx::usize kPitMinValidObservations = 57; // ceil(0.9 * 63)
inline constexpr atx::f64 kPitMinRawPriceExclusive = 1.0;  // R15-5, raw close > 1.0
inline constexpr atx::f64 kPitMinAdvUsd = 0.0;             // R15-5
inline constexpr atx::usize kPitMaxCuts = 8;               // |top_n| * |band| <= 8
inline constexpr atx::usize kPitMaxTopNValues = 4;
inline constexpr atx::usize kPitMaxBandValues = 2;
inline constexpr atx::usize kPitMaxSourceIds = 32768; // R15-12
inline constexpr atx::usize kPitMaxRebalances = 4096; // R15-12
inline constexpr atx::usize kPitMaxSessions = 8192;   // R15-12
inline constexpr atx::u32 kPitNoBar = 0xFFFFFFFFu;
inline constexpr atx::i64 kPitHashEmptyKey = 0; // IDs are > 0 (DR15-1)
// Calendar years the aggregates can carry (§4.3-§4.6 per-year rows). 2012..2019 is
// eight; sixteen leaves room without an unbounded per-year table.
inline constexpr atx::usize kPitMaxYears = 16;
// 2020-01-01T00:00:00Z. The data layer restates the literal and never includes
// eval/; §9.1 #18 asserts equality with eval::kValidationBeginNs.
inline constexpr atx::i64 kPitSessionKeyEndExclusive = 1'577'836'800'000'000'000;
inline constexpr atx::i64 kPitNanosPerDay = 86'400'000'000'000;

static_assert(kPitMaxTopNValues * kPitMaxBandValues == kPitMaxCuts,
              "every (top_n, band) pair must fit the cut bound");
static_assert(kPitSessionKeyEndExclusive == 18'262 * kPitNanosPerDay,
              "kPitSessionKeyEndExclusive must be 2020-01-01T00:00:00Z in epoch nanoseconds");
static_assert(kPitMinValidObservations <= kPitAdvWindow,
              "the minimum valid count cannot exceed the window");

// §3.1. `top_n` ascending, distinct, > 0; `band_bp` ascending, distinct (basis
// points). `max_rebalances` / `max_sessions` are passed exactly by the stage.
struct PitUniverseConfig {
  atx::usize adv_window{kPitAdvWindow};
  atx::usize min_valid_observations{kPitMinValidObservations};
  atx::f64 min_raw_price_exclusive{kPitMinRawPriceExclusive};
  atx::f64 min_adv_usd{kPitMinAdvUsd};
  std::array<atx::usize, kPitMaxTopNValues> top_n{};
  atx::usize top_n_count{};
  std::array<atx::u32, kPitMaxBandValues> band_bp{};
  atx::usize band_count{};
  atx::usize max_source_ids{kPitMaxSourceIds}; // hash capacity = next pow2 >= 4*this (DR15-1)
  atx::usize max_rebalances{kPitMaxRebalances};
  atx::usize max_sessions{kPitMaxSessions};
  atx::i64 session_key_end_exclusive{kPitSessionKeyEndExclusive};
};

enum class PitMemberStatus : atx::u8 { Add = 0, Keep = 1 };
enum class PitDropKind : atx::u8 { Rank = 0, LastBar = 1 };
// DR15-14: integers pinned; enum order, §4.5 text order, F9 and delisting.csv follow it.
enum class PitExitKind : atx::u8 { RankDrop = 0, LastBarWithinWindow = 1, WindowEnd = 2 };

// One eligible ID at one rank session (§3.4 step 1); valid until the next `rebalance`.
struct PitRankedRow {
  atx::u32 slot;                  // first-seen slot (§3.3)
  atx::i64 security_id;
  atx::u32 rank;                  // 1-based, key desc then slot asc
  atx::f64 adv63_usd;             // §3.5 median
  atx::u32 valid_observations;    // min_valid_observations..adv_window
  atx::f64 raw_close;             // close on the rank session
  atx::f64 vendor_market_cap_usd; // shares*close, NaN when shares NaN or <= 0
  atx::f64 gics;                  // NaN when missing
};
struct PitMemberRow {
  atx::u32 slot;
  atx::u32 rank;
  PitMemberStatus status;
};
struct PitDropRow {
  atx::u32 slot;
  PitDropKind kind;
};
struct PitChurn {
  atx::u32 adds;
  atx::u32 drops_rank;
  atx::u32 drops_last_bar;
  atx::u32 kept;
  atx::u32 members;
};

// Everything the stage writes for one rebalance (§3.1, §3.7). Spans alias builder
// storage that ONLY `rebalance` rewrites, so the view stays valid across later
// `observe_session` calls until the next `rebalance` (DR15-3).
struct PitRebalanceView {
  atx::usize index; // r
  atx::i64 rank_session_key;
  atx::usize ids_seen;
  atx::usize ids_with_valid_bar;
  atx::usize eligible;
  std::span<const PitRankedRow> ranked; // eligible IDs in rank order
  // per cut c = top_n_index * band_count + band_index:
  std::span<const std::span<const PitMemberRow>> members; // rank ascending (DR15-2)
  std::span<const std::span<const PitDropRow>> drops;     // slot ascending (M-3)
  std::span<const PitChurn> churn;
  std::span<const atx::u32> gics_missing_members;     // per cut
  std::span<const atx::u64> valid_observations_total; // per cut, sum of valid_count (DR15-4)
};

// §4.3 one calendar year of observed sessions. Per-cut arrays are indexed by cut.
struct PitCoverageYear {
  atx::i32 year;
  atx::usize sessions;
  atx::usize ids_seen;
  atx::f64 ids_with_valid_bar_median;
  atx::usize rebalances; // rebalances whose RANK session is in the year
  atx::f64 eligible_median;
  atx::usize cuts;
  std::array<atx::f64, kPitMaxCuts> members_median;
  std::array<atx::f64, kPitMaxCuts> nonmissing_fraction_median;
  std::array<atx::f64, kPitMaxCuts> gics_missing_members_median;
};

// §4.4 one calendar year of EFFECTIVE sessions.
struct PitUnionYear {
  atx::i32 year;
  atx::usize cuts;
  std::array<atx::usize, kPitMaxCuts> distinct;
  std::array<atx::usize, kPitMaxCuts> cumulative;
};

// §4.6 one cut. `years[k]` lists, in order, every observed calendar year whose
// FIRST observed session is the effective session of a rebalance (§4.6 (b)); the
// warmup year, having no such rebalance, gets no row.
struct PitSurvivorshipCut {
  atx::usize ever_members;
  atx::usize ended_before_window_end;
  atx::usize censored;
  atx::usize year_count;
  std::array<atx::i32, kPitMaxYears> years;
  std::array<atx::usize, kPitMaxYears> year_start_members;
  std::array<atx::usize, kPitMaxYears> year_exits;
};
struct PitSurvivorship {
  atx::usize cuts;
  std::array<PitSurvivorshipCut, kPitMaxCuts> cut;
};

// §4.5 one (cut, ever-member) row. Ordinals are session ordinals; rebalance
// indices are `r`.
struct PitExitRecord {
  atx::i64 security_id;
  atx::u32 first_seen; // first appearance, possibly with an invalid bar (DR15-11)
  atx::u32 first_bar;  // first VALID bar (I-7)
  atx::u32 last_bar;   // last VALID bar over ALL observed sessions
  atx::u32 first_member_rebalance;
  atx::u32 last_member_rebalance;
  PitExitKind exit_kind;
};

// Civil date of a session key (§3.1): Hinnant's civil_from_days over
// `key / kPitNanosPerDay` (floor division, so a pre-epoch key still resolves).
struct PitCivilDate {
  atx::i32 year;
  atx::u32 month; // 1..12
  atx::u32 day;   // 1..31
};
[[nodiscard]] PitCivilDate pit_civil_of(atx::i64 session_key) noexcept;
[[nodiscard]] atx::i32 pit_year_of(atx::i64 session_key) noexcept;

// §3.1 / DR15-8. Pure, allocation-free monthly rank-session selection.
// `session_keys` must be strictly ascending (else Err(InvalidArgument)). A session
// is selected iff its UTC calendar month differs from the NEXT attached session's
// month (the month's last session BY DATA) and `start_key <= key <=
// end_inclusive_key`; the last attached session is never selected. `n` receives
// the count; Err(OutOfRange) if `out` is too small (nothing is written then).
[[nodiscard]] atx::core::Status select_monthly_rank_sessions(std::span<const atx::i64> session_keys,
                                                             atx::i64 start_key,
                                                             atx::i64 end_inclusive_key,
                                                             std::span<atx::i64> out,
                                                             atx::usize &n);

class PitUniverseBuilder {
public:
  // §3.1, §3.2. Validates the config against every kPitMax*, checks every buffer
  // product with overflow helpers, performs ALL allocation. Err(InvalidArgument |
  // OutOfRange), never partial.
  [[nodiscard]] static atx::core::Result<PitUniverseBuilder> create(const PitUniverseConfig &cfg);

  ~PitUniverseBuilder();
  PitUniverseBuilder(PitUniverseBuilder &&) noexcept;
  PitUniverseBuilder &operator=(PitUniverseBuilder &&) noexcept;
  PitUniverseBuilder(const PitUniverseBuilder &) = delete;
  PitUniverseBuilder &operator=(const PitUniverseBuilder &) = delete;

  // §3.3. One session; parallel spans (`shares` / `gics` may be empty => NaN). No
  // allocation. Validates EVERYTHING before mutating anything (DR15-9). Err on:
  // key <= previous (InvalidArgument), key >= session_key_end_exclusive
  // (PermissionDenied), sessions() == max_sessions (OutOfRange), span size mismatch
  // (InvalidArgument), id <= 0 (OutOfRange), id repeated in the call
  // (InvalidArgument), > max_source_ids distinct ids (OutOfRange), more than
  // kPitMaxYears distinct calendar years (OutOfRange). Any positive i64 id is accepted.
  [[nodiscard]] atx::core::Status observe_session(atx::i64 session_key,
                                                  std::span<const atx::i64> ids,
                                                  std::span<const atx::f64> raw_close,
                                                  std::span<const atx::f64> volume,
                                                  std::span<const atx::f64> shares,
                                                  std::span<const atx::f64> gics);

  // §3.4. `rank_session_key` MUST equal the last observed key (InvalidArgument
  // otherwise — a rank date with no data is rejected, never interpolated);
  // rebalances() < max_rebalances (OutOfRange); key > previous rank key
  // (InvalidArgument). No allocation. The returned view stays valid until the next
  // `rebalance`.
  [[nodiscard]] atx::core::Result<PitRebalanceView> rebalance(atx::i64 rank_session_key);

  [[nodiscard]] const PitUniverseConfig &config() const noexcept;
  [[nodiscard]] atx::usize sessions() const noexcept;
  [[nodiscard]] atx::usize rebalances() const noexcept;
  [[nodiscard]] atx::usize source_ids() const noexcept;
  [[nodiscard]] atx::usize cuts() const noexcept; // top_n_count * band_count

  // Per-session (ordinal < sessions()).
  [[nodiscard]] atx::i64 session_key(atx::usize ordinal) const noexcept;
  [[nodiscard]] atx::usize ids_with_valid_bar(atx::usize ordinal) const noexcept;

  // Per-slot (slot < source_ids()). `first_bar` / `last_bar` are kPitNoBar while
  // the slot has no valid bar.
  [[nodiscard]] atx::i64 security_id(atx::u32 slot) const noexcept;
  [[nodiscard]] atx::u32 first_seen(atx::u32 slot) const noexcept;
  [[nodiscard]] atx::u32 first_bar(atx::u32 slot) const noexcept;
  [[nodiscard]] atx::u32 last_bar(atx::u32 slot) const noexcept;

  // Per-rebalance (r < rebalances(), c < cuts()). `members` are slots ascending;
  // `member_ranks` is parallel to it (§3.4 3f "retained" order). `effective_key`
  // is 0 while the rebalance is pending (R15-4).
  [[nodiscard]] std::span<const atx::u32> members(atx::usize r, atx::usize c) const noexcept;
  [[nodiscard]] std::span<const atx::u32> member_ranks(atx::usize r, atx::usize c) const noexcept;
  [[nodiscard]] const PitChurn &churn(atx::usize r, atx::usize c) const noexcept;
  [[nodiscard]] atx::i64 rank_key(atx::usize r) const noexcept;
  [[nodiscard]] atx::i64 effective_key(atx::usize r) const noexcept;

  // Aggregates over what has been observed (§4.3-§4.6), allocation-free,
  // caller-sized spans; Err(OutOfRange) when `out` is too small.
  [[nodiscard]] atx::core::Status coverage_by_year(std::span<PitCoverageYear> out,
                                                   atx::usize &n) const;
  [[nodiscard]] atx::core::Status union_by_year(std::span<PitUnionYear> out, atx::usize &n) const;
  [[nodiscard]] atx::core::Result<PitSurvivorship> survivorship() const;
  // §4.5: every slot ever a member of `cut`, security_id ascending (sorted in place
  // inside `out`).
  [[nodiscard]] atx::core::Status exits(atx::usize cut, std::span<PitExitRecord> out,
                                        atx::usize &n) const;

private:
  struct Impl;
  explicit PitUniverseBuilder(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
//  §4.7 membership.bin codec (DR15-6). `encode_membership_bin` allocates its output
//  string — post-run, outside the no-heap paths. Layout, little-endian throughout:
//    "ATXPITU1" | u32 version=1 | u32 adv_window | u32 min_valid_observations |
//    f64 min_raw_price_exclusive | u32 T, T x u32 top_n | u32 B, B x u32 band_bp |
//    u32 R | per r: i64 rank_key, i64 effective_key (> 0; encode refuses 0 with
//    Internal), per cut: u32 n_c, n_c x i64 security_id ascending, n_c x u32 rank |
//    u64 fnv1a64 trailer over every preceding byte.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kPitMembershipMagic = "ATXPITU1";
inline constexpr atx::u32 kPitMembershipVersion = 1;

struct PitMembershipCut {
  std::vector<atx::i64> security_ids; // ascending
  std::vector<atx::u32> ranks;        // parallel to security_ids
};
struct PitMembershipRebalance {
  atx::i64 rank_session_key;
  atx::i64 effective_session_key;
  std::vector<PitMembershipCut> cuts; // ti outer, bi inner
};
struct PitMembershipImage {
  atx::u32 adv_window;
  atx::u32 min_valid_observations;
  atx::f64 min_raw_price_exclusive;
  std::vector<atx::u32> top_n;
  std::vector<atx::u32> band_bp;
  std::vector<PitMembershipRebalance> rebalances;
  atx::u64 fnv1a64; // the trailer as read back
};

[[nodiscard]] atx::core::Result<std::string> encode_membership_bin(const PitUniverseBuilder &b);
// Refuses a short buffer, bad magic or bad version (InvalidArgument) and a trailer
// mismatch (Internal); an effective key of 0 is InvalidArgument.
[[nodiscard]] atx::core::Result<PitMembershipImage> decode_membership_bin(std::string_view bytes);
// The trailer convention (serialize_panel.hpp): FNV-1a-64, offset
// 14695981039346656037, prime 1099511628211, byte-wise.
[[nodiscard]] atx::u64 pit_fnv1a64(std::string_view bytes) noexcept;

} // namespace atx::engine::data
