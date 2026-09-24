// atx::engine::data — point-in-time universe builder implementation (checkpoint 15, T1).
//
// See point_in_time_universe.hpp for the contract; section numbers refer to the
// frozen design note `2026-09-20-iteration15-point-in-time-universe-design.md`.
//
// Layout of the work: `create` (§3.2) sizes and allocates everything once;
// `observe_session` (§3.3) is validate-then-mutate with provisional hash insertion
// and full rollback; `rebalance` (§3.4) ranks, bands, records churn and retains
// `{slot, rank}`; the §4 aggregates read only retained state through pre-sized
// mutable scratch; the §4.7 codec is post-run and may allocate.

#include "atx/engine/data/point_in_time_universe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp" // atx::core::time::civil_from_days (Hinnant, §3.1)
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::u16 kNoYear = 0xFFFFu;
constexpr atx::u8 kNoDrop = 0xFFu;
constexpr atx::u64 kFibonacciHash = 0x9E3779B97F4A7C15ull; // §3.2
constexpr atx::u64 kFnvOffset = 14695981039346656037ull;   // §4.7 trailer
constexpr atx::u64 kFnvPrime = 1099511628211ull;
constexpr atx::u32 kBasisPointsPerUnit = 10000u;

// --- overflow-checked sizing (§3.2, modelled on stage_equity_ic.cpp:164-176) ---

[[nodiscard]] Result<atx::usize> checked_mul(atx::usize a, atx::usize b, const char *what) {
  if (a != 0 && b > (std::numeric_limits<atx::usize>::max)() / a) {
    return Err(ErrorCode::OutOfRange,
               std::string{"pit universe: product overflows usize: "} + what);
  }
  return Ok(a * b);
}

[[nodiscard]] Result<atx::usize> checked_add(atx::usize a, atx::usize b, const char *what) {
  if (b > (std::numeric_limits<atx::usize>::max)() - a) {
    return Err(ErrorCode::OutOfRange, std::string{"pit universe: sum overflows usize: "} + what);
  }
  return Ok(a + b);
}

// §3.5 median rule, bit-for-bit: sort ascending in place, odd -> middle, even ->
// (a + b) * 0.5 in binary64. Empty -> 0.0 (only reachable from the §4.3
// aggregates over a year with no rebalance; §3.4 never calls it with m == 0).
[[nodiscard]] atx::f64 median_in_place(std::span<atx::f64> values) noexcept {
  const atx::usize m = values.size();
  if (m == 0) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  if (m % 2 == 1) {
    return values[m / 2];
  }
  return (values[m / 2 - 1] + values[m / 2]) * 0.5;
}

// §3.4 3: K_band = n + floor(n * band_bp / 10000), evaluated exactly without an
// intermediate n * band_bp (top_n carries no maximum, so the product could wrap).
// floor((q*10000 + rem) * b / 10000) == q*b + floor(rem*b / 10000) because q*b is
// an integer. Saturates at usize max, where the list can never reach n anyway.
[[nodiscard]] atx::usize band_threshold(atx::usize n, atx::u32 band_bp) noexcept {
  constexpr atx::usize kMax = (std::numeric_limits<atx::usize>::max)();
  const atx::usize q = n / kBasisPointsPerUnit;
  const atx::usize rem = n % kBasisPointsPerUnit;
  const atx::usize bp = static_cast<atx::usize>(band_bp);
  const atx::usize part = (rem * bp) / kBasisPointsPerUnit; // rem < 10000, bp < 2^32: no wrap
  if (q != 0 && bp > kMax / q) {
    return kMax;
  }
  const atx::usize qb = q * bp;
  if (part > kMax - qb) {
    return kMax;
  }
  const atx::usize extra = qb + part;
  if (extra > kMax - n) {
    return kMax;
  }
  return n + extra;
}

[[nodiscard]] atx::i64 floor_div_days(atx::i64 key) noexcept {
  atx::i64 days = key / kPitNanosPerDay;
  if (key < 0 && key % kPitNanosPerDay != 0) {
    --days;
  }
  return days;
}

// --- little-endian byte helpers for the §4.7 codec (no reinterpret_cast) ---

void put_u32(std::string &out, atx::u32 v) {
  for (atx::usize i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
  }
}

void put_u64(std::string &out, atx::u64 v) {
  for (atx::usize i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
  }
}

void put_i64(std::string &out, atx::i64 v) { put_u64(out, std::bit_cast<atx::u64>(v)); }

void put_f64(std::string &out, atx::f64 v) { put_u64(out, std::bit_cast<atx::u64>(v)); }

class ByteReader {
public:
  explicit ByteReader(std::string_view bytes) noexcept : bytes_{bytes} {}

  [[nodiscard]] atx::usize remaining() const noexcept { return bytes_.size() - offset_; }

  [[nodiscard]] bool take_u32(atx::u32 &v) noexcept {
    atx::u64 wide = 0;
    if (!take(4, wide)) {
      return false;
    }
    v = static_cast<atx::u32>(wide);
    return true;
  }
  [[nodiscard]] bool take_u64(atx::u64 &v) noexcept { return take(8, v); }
  [[nodiscard]] bool take_i64(atx::i64 &v) noexcept {
    atx::u64 wide = 0;
    if (!take(8, wide)) {
      return false;
    }
    v = std::bit_cast<atx::i64>(wide);
    return true;
  }
  [[nodiscard]] bool take_f64(atx::f64 &v) noexcept {
    atx::u64 wide = 0;
    if (!take(8, wide)) {
      return false;
    }
    v = std::bit_cast<atx::f64>(wide);
    return true;
  }

private:
  [[nodiscard]] bool take(atx::usize width, atx::u64 &v) noexcept {
    if (remaining() < width) {
      return false;
    }
    atx::u64 acc = 0;
    for (atx::usize i = 0; i < width; ++i) {
      const auto byte = static_cast<atx::u64>(static_cast<unsigned char>(bytes_[offset_ + i]));
      acc |= byte << (8 * i);
    }
    offset_ += width;
    v = acc;
    return true;
  }

  std::string_view bytes_;
  atx::usize offset_{0};
};

} // namespace

// ===========================================================================
//  Civil calendar (§3.1)
// ===========================================================================

PitCivilDate pit_civil_of(atx::i64 session_key) noexcept {
  const atx::core::time::Date d = atx::core::time::civil_from_days(floor_div_days(session_key));
  return PitCivilDate{d.year, d.month, d.day};
}

atx::i32 pit_year_of(atx::i64 session_key) noexcept { return pit_civil_of(session_key).year; }

// ===========================================================================
//  §3.1 / DR15-8 monthly rank-session selection
// ===========================================================================

Status select_monthly_rank_sessions(std::span<const atx::i64> session_keys, atx::i64 start_key,
                                    atx::i64 end_inclusive_key, std::span<atx::i64> out,
                                    atx::usize &n) {
  n = 0;
  for (atx::usize i = 1; i < session_keys.size(); ++i) {
    if (session_keys[i] <= session_keys[i - 1]) {
      return Err(ErrorCode::InvalidArgument,
                 "pit universe: session keys must be strictly ascending");
    }
  }
  // Pass 1 counts so an undersized `out` is refused before anything is written.
  atx::usize count = 0;
  for (atx::usize i = 0; i + 1 < session_keys.size(); ++i) {
    const atx::i64 key = session_keys[i];
    if (key < start_key || key > end_inclusive_key) {
      continue;
    }
    const PitCivilDate here = pit_civil_of(key);
    const PitCivilDate next = pit_civil_of(session_keys[i + 1]);
    if (here.year != next.year || here.month != next.month) {
      ++count;
    }
  }
  if (out.size() < count) {
    return Err(ErrorCode::OutOfRange, "pit universe: rank session output span too small");
  }
  for (atx::usize i = 0; i + 1 < session_keys.size(); ++i) {
    const atx::i64 key = session_keys[i];
    if (key < start_key || key > end_inclusive_key) {
      continue;
    }
    const PitCivilDate here = pit_civil_of(key);
    const PitCivilDate next = pit_civil_of(session_keys[i + 1]);
    if (here.year != next.year || here.month != next.month) {
      out[n] = key;
      ++n;
    }
  }
  return Ok();
}

// ===========================================================================
//  Builder state (§3.2) — every buffer sized once in `create`
// ===========================================================================

struct PitUniverseBuilder::Impl {
  struct Cell {
    atx::i64 key;
    atx::u32 slot;
  };

  PitUniverseConfig cfg{};
  atx::usize U{0}; // max_source_ids
  atx::usize W{0}; // adv_window
  atx::usize R{0}; // max_rebalances
  atx::usize C{0}; // cuts
  atx::usize N{0}; // largest top_n
  atx::usize S{0}; // max_sessions
  atx::usize H{0}; // hash capacity (power of two)
  unsigned hash_shift{0};

  atx::usize sessions{0};
  atx::usize rebalances{0};
  atx::usize source_ids{0};
  atx::i32 first_year{0};

  std::vector<atx::f64> ring; // U*W, NaN-initialised
  std::vector<Cell> table;    // H

  // per slot
  std::vector<atx::i64> id;
  std::vector<atx::u32> first_seen;
  std::vector<atx::u32> first_bar;
  std::vector<atx::u32> last_bar;
  std::vector<atx::f64> last_close;
  std::vector<atx::f64> last_shares;
  std::vector<atx::f64> last_gics;
  std::vector<atx::u8> member_bits;
  std::vector<atx::u8> ever_bits;
  std::vector<atx::u16> last_year_seen;
  // per (slot, cut)
  std::vector<atx::u32> first_member_r;
  std::vector<atx::u32> last_member_r;
  std::vector<atx::u8> last_drop_kind;

  // per rebalance
  std::vector<atx::u32> retained_slots; // R*C*N, slot ascending per (r, c)
  std::vector<atx::u32> retained_ranks; // parallel
  std::vector<atx::u32> retained_count; // R*C
  std::vector<atx::i64> rank_keys;
  std::vector<atx::i64> effective_keys;
  std::vector<PitChurn> churn;          // R*C
  std::vector<atx::u32> reb_ids_seen;   // R
  std::vector<atx::u32> reb_ids_valid;  // R
  std::vector<atx::u32> reb_eligible;   // R
  std::vector<atx::u32> gics_missing;   // R*C
  std::vector<atx::u64> valid_total;    // R*C

  // per session
  std::vector<atx::i64> session_keys;         // S
  std::vector<atx::u32> ids_valid_per_session; // S

  // per year (§4.3), indexed by year - first_year
  std::array<atx::usize, kPitMaxYears> year_sessions{};
  std::array<atx::usize, kPitMaxYears> year_ids_seen{};

  // observe / rebalance scratch
  std::vector<PitRankedRow> ranked_scratch; // U, slot order
  std::vector<PitRankedRow> ranked;         // U, rank order (the view)
  std::vector<atx::u32> order;              // U
  std::vector<atx::u8> seen_this_call;      // U
  std::vector<atx::u32> provisional_cells;  // U
  std::vector<atx::u8> new_bits;            // U
  std::vector<atx::f64> median_scratch;     // W
  std::vector<PitMemberRow> members_scratch; // C*N
  std::vector<PitDropRow> drops_scratch;     // C*N
  std::array<std::span<const PitMemberRow>, kPitMaxCuts> member_spans{};
  std::array<std::span<const PitDropRow>, kPitMaxCuts> drop_spans{};

  // §4 query scratch, pre-sized; the const aggregates use it (I-10)
  mutable std::vector<atx::f64> query_median; // max(S, R)
  mutable std::vector<atx::u8> union_bitmap;  // U
  mutable std::vector<atx::u16> year_mark;    // U

  // Linear-probe lookup (§3.2). Returns true with `slot` on a hit; on a miss
  // `cell` is the first empty cell (or H when the table is full, which the load
  // bound makes unreachable). Bounded by H probes.
  [[nodiscard]] bool find(atx::i64 key, atx::u32 &slot, atx::usize &cell) const noexcept {
    atx::usize i =
        static_cast<atx::usize>((static_cast<atx::u64>(key) * kFibonacciHash) >> hash_shift);
    for (atx::usize probes = 0; probes < H; ++probes) {
      const Cell &c = table[i];
      if (c.key == kPitHashEmptyKey) {
        cell = i;
        return false;
      }
      if (c.key == key) {
        slot = c.slot;
        cell = i;
        return true;
      }
      i = (i + 1) & (H - 1);
    }
    cell = H;
    return false;
  }

  [[nodiscard]] atx::usize year_index(atx::i32 year) const noexcept {
    return static_cast<atx::usize>(year - first_year);
  }
};

// ===========================================================================
//  §3.2 create
// ===========================================================================

namespace {

[[nodiscard]] Status validate_config(const PitUniverseConfig &cfg) {
  if (cfg.adv_window == 0) {
    return Err(ErrorCode::InvalidArgument, "pit universe: adv_window must be > 0");
  }
  if (cfg.min_valid_observations == 0 || cfg.min_valid_observations > cfg.adv_window) {
    return Err(ErrorCode::InvalidArgument,
               "pit universe: min_valid_observations must be in [1, adv_window]");
  }
  if (std::isnan(cfg.min_raw_price_exclusive) || std::isnan(cfg.min_adv_usd)) {
    return Err(ErrorCode::InvalidArgument, "pit universe: floors must not be NaN");
  }
  if (cfg.top_n_count == 0 || cfg.top_n_count > kPitMaxTopNValues) {
    return Err(ErrorCode::InvalidArgument, "pit universe: top_n_count must be in [1, " +
                                               std::to_string(kPitMaxTopNValues) + "]");
  }
  if (cfg.band_count == 0 || cfg.band_count > kPitMaxBandValues) {
    return Err(ErrorCode::InvalidArgument, "pit universe: band_count must be in [1, " +
                                               std::to_string(kPitMaxBandValues) + "]");
  }
  for (atx::usize i = 0; i < cfg.top_n_count; ++i) {
    if (cfg.top_n[i] == 0) {
      return Err(ErrorCode::InvalidArgument, "pit universe: top_n values must be > 0");
    }
    if (i > 0 && cfg.top_n[i] <= cfg.top_n[i - 1]) {
      return Err(ErrorCode::InvalidArgument, "pit universe: top_n must be ascending and distinct");
    }
  }
  for (atx::usize i = 1; i < cfg.band_count; ++i) {
    if (cfg.band_bp[i] <= cfg.band_bp[i - 1]) {
      return Err(ErrorCode::InvalidArgument,
                 "pit universe: band_bp must be ascending and distinct");
    }
  }
  if (cfg.max_source_ids == 0 || cfg.max_source_ids > kPitMaxSourceIds) {
    return Err(ErrorCode::InvalidArgument, "pit universe: max_source_ids must be in [1, " +
                                               std::to_string(kPitMaxSourceIds) + "]");
  }
  if (cfg.max_rebalances == 0 || cfg.max_rebalances > kPitMaxRebalances) {
    return Err(ErrorCode::InvalidArgument, "pit universe: max_rebalances must be in [1, " +
                                               std::to_string(kPitMaxRebalances) + "]");
  }
  if (cfg.max_sessions == 0 || cfg.max_sessions > kPitMaxSessions) {
    return Err(ErrorCode::InvalidArgument, "pit universe: max_sessions must be in [1, " +
                                               std::to_string(kPitMaxSessions) + "]");
  }
  return Ok();
}

// Smallest power of two >= 4 * max_source_ids (DR15-1). 4 * kPitMaxSourceIds is
// 2^17, so the loop is bounded by 17 doublings.
[[nodiscard]] Result<atx::usize> hash_capacity(atx::usize max_source_ids) {
  ATX_TRY(const atx::usize four, checked_mul(max_source_ids, 4, "4 * max_source_ids"));
  atx::usize h = 1;
  while (h < four) {
    h *= 2;
  }
  return Ok(h);
}

} // namespace

Result<PitUniverseBuilder> PitUniverseBuilder::create(const PitUniverseConfig &cfg) {
  ATX_TRY_VOID(validate_config(cfg));
  const atx::usize U = cfg.max_source_ids;
  const atx::usize W = cfg.adv_window;
  const atx::usize R = cfg.max_rebalances;
  const atx::usize C = cfg.top_n_count * cfg.band_count; // <= kPitMaxCuts by the bounds above
  const atx::usize N = cfg.top_n[cfg.top_n_count - 1];
  const atx::usize S = cfg.max_sessions;
  ATX_TRY(const atx::usize H, hash_capacity(U));
  ATX_TRY(const atx::usize ring_cells, checked_mul(U, W, "max_source_ids * adv_window"));
  ATX_TRY(const atx::usize slot_cut, checked_mul(U, C, "max_source_ids * cuts"));
  ATX_TRY(const atx::usize reb_cut, checked_mul(R, C, "max_rebalances * cuts"));
  ATX_TRY(const atx::usize retained, checked_mul(reb_cut, N, "max_rebalances * cuts * top_n"));
  ATX_TRY(const atx::usize cut_n, checked_mul(C, N, "cuts * top_n"));
  // Byte-level products, so a resize can never be asked for more than usize bytes.
  ATX_TRY_VOID(checked_mul(ring_cells, sizeof(atx::f64), "ring bytes"));
  ATX_TRY_VOID(checked_mul(retained, sizeof(atx::u32), "retained bytes"));
  ATX_TRY_VOID(checked_mul(cut_n, sizeof(PitMemberRow), "members scratch bytes"));
  ATX_TRY_VOID(checked_mul(H, sizeof(Impl::Cell), "id table bytes"));
  ATX_TRY_VOID(checked_add(S, R, "max_sessions + max_rebalances"));

  std::unique_ptr<Impl> impl;
  try {
    impl = std::make_unique<Impl>();
    Impl &m = *impl;
    m.cfg = cfg;
    m.U = U;
    m.W = W;
    m.R = R;
    m.C = C;
    m.N = N;
    m.S = S;
    m.H = H;
    m.hash_shift = 64u - static_cast<unsigned>(std::countr_zero(H));

    m.ring.assign(ring_cells, kNaN);
    m.table.assign(H, Impl::Cell{kPitHashEmptyKey, 0u});

    m.id.assign(U, 0);
    m.first_seen.assign(U, kPitNoBar);
    m.first_bar.assign(U, kPitNoBar);
    m.last_bar.assign(U, kPitNoBar);
    m.last_close.assign(U, kNaN);
    m.last_shares.assign(U, kNaN);
    m.last_gics.assign(U, kNaN);
    m.member_bits.assign(U, 0u);
    m.ever_bits.assign(U, 0u);
    m.last_year_seen.assign(U, kNoYear);
    m.first_member_r.assign(slot_cut, kPitNoBar);
    m.last_member_r.assign(slot_cut, kPitNoBar);
    m.last_drop_kind.assign(slot_cut, kNoDrop);

    m.retained_slots.assign(retained, 0u);
    m.retained_ranks.assign(retained, 0u);
    m.retained_count.assign(reb_cut, 0u);
    m.rank_keys.assign(R, 0);
    m.effective_keys.assign(R, 0);
    m.churn.assign(reb_cut, PitChurn{0u, 0u, 0u, 0u, 0u});
    m.reb_ids_seen.assign(R, 0u);
    m.reb_ids_valid.assign(R, 0u);
    m.reb_eligible.assign(R, 0u);
    m.gics_missing.assign(reb_cut, 0u);
    m.valid_total.assign(reb_cut, 0u);

    m.session_keys.assign(S, 0);
    m.ids_valid_per_session.assign(S, 0u);

    const PitRankedRow empty_row{0u, 0, 0u, kNaN, 0u, kNaN, kNaN, kNaN};
    m.ranked_scratch.assign(U, empty_row);
    m.ranked.assign(U, empty_row);
    m.order.assign(U, 0u);
    m.seen_this_call.assign(U, 0u);
    m.provisional_cells.assign(U, 0u);
    m.new_bits.assign(U, 0u);
    m.median_scratch.assign(W, kNaN);
    m.members_scratch.assign(cut_n, PitMemberRow{0u, 0u, PitMemberStatus::Add});
    m.drops_scratch.assign(cut_n, PitDropRow{0u, PitDropKind::Rank});

    m.query_median.assign(S > R ? S : R, kNaN);
    m.union_bitmap.assign(U, 0u);
    m.year_mark.assign(U, 0u);
  } catch (const std::bad_alloc &) {
    // A config whose products fit usize can still exceed the machine; the unit's
    // contract is Err, never partial and never a throw past this boundary.
    return Err(ErrorCode::OutOfRange, "pit universe: allocation failed for the requested maxima");
  }
  return Ok(PitUniverseBuilder{std::move(impl)});
}

PitUniverseBuilder::PitUniverseBuilder(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
PitUniverseBuilder::~PitUniverseBuilder() = default;
PitUniverseBuilder::PitUniverseBuilder(PitUniverseBuilder &&) noexcept = default;
PitUniverseBuilder &PitUniverseBuilder::operator=(PitUniverseBuilder &&) noexcept = default;

// ===========================================================================
//  §3.3 observe_session — pass 1 validates (provisional insertion + rollback,
//  DR15-9 / DR15-13), pass 2 applies.
// ===========================================================================

Status PitUniverseBuilder::observe_session(atx::i64 session_key, std::span<const atx::i64> ids,
                                           std::span<const atx::f64> raw_close,
                                           std::span<const atx::f64> volume,
                                           std::span<const atx::f64> shares,
                                           std::span<const atx::f64> gics) {
  Impl &m = *impl_;
  // --- pass 1 (a): session-level checks, nothing mutated -------------------
  if (m.sessions > 0 && session_key <= m.session_keys[m.sessions - 1]) {
    return Err(ErrorCode::InvalidArgument, "pit universe: session out of order");
  }
  if (session_key >= m.cfg.session_key_end_exclusive) {
    return Err(ErrorCode::PermissionDenied,
               "pit universe: session at/after validation boundary");
  }
  if (m.sessions >= m.S) {
    return Err(ErrorCode::OutOfRange, "pit universe: max_sessions exceeded");
  }
  const atx::usize count = ids.size();
  if (raw_close.size() != count || volume.size() != count ||
      (!shares.empty() && shares.size() != count) || (!gics.empty() && gics.size() != count)) {
    return Err(ErrorCode::InvalidArgument, "pit universe: span size mismatch");
  }
  const atx::i32 year = pit_year_of(session_key);
  const atx::i32 first_year = (m.sessions == 0) ? year : m.first_year;
  if (year < first_year || static_cast<atx::usize>(year - first_year) >= kPitMaxYears) {
    return Err(ErrorCode::OutOfRange, "pit universe: kPitMaxYears exceeded");
  }

  // --- pass 1 (b): per-id checks with provisional insertion -----------------
  atx::usize provisional = 0;
  atx::usize processed = 0;
  atx::core::Error failure{};
  bool failed = false;
  for (atx::usize j = 0; j < count; ++j) {
    const atx::i64 id = ids[j];
    if (id <= 0) {
      failure = atx::core::Error{ErrorCode::OutOfRange, "pit universe: security id must be > 0"};
      failed = true;
      break;
    }
    atx::u32 slot = 0;
    atx::usize cell = 0;
    if (m.find(id, slot, cell)) {
      if (m.seen_this_call[slot] != 0) {
        failure = atx::core::Error{ErrorCode::InvalidArgument,
                                   "pit universe: duplicate id in session"};
        failed = true;
        break;
      }
      m.seen_this_call[slot] = 1u;
    } else {
      if (cell >= m.H || m.source_ids + provisional >= m.U) {
        failure = atx::core::Error{ErrorCode::OutOfRange,
                                   "pit universe: max_source_ids exceeded"};
        failed = true;
        break;
      }
      slot = static_cast<atx::u32>(m.source_ids + provisional);
      m.table[cell] = Impl::Cell{id, slot};
      m.provisional_cells[provisional] = static_cast<atx::u32>(cell);
      ++provisional;
      m.seen_this_call[slot] = 1u;
    }
    processed = j + 1;
  }
  if (failed) {
    // --- pass 1 (c): rollback. Clear `seen` while every processed id is still
    // findable, then remove provisional cells newest-first (safe for linear
    // probing: nothing was inserted after the newest, so no probe passes it).
    for (atx::usize j = 0; j < processed; ++j) {
      atx::u32 slot = 0;
      atx::usize cell = 0;
      if (m.find(ids[j], slot, cell)) {
        m.seen_this_call[slot] = 0u;
      }
    }
    while (provisional > 0) {
      --provisional;
      m.table[m.provisional_cells[provisional]] = Impl::Cell{kPitHashEmptyKey, 0u};
    }
    return Err(std::move(failure));
  }

  // --- pass 2: apply -----------------------------------------------------------
  const atx::usize old_count = m.source_ids;
  m.source_ids += provisional;
  const atx::usize s = m.sessions;
  const atx::usize k = s % m.W;
  for (atx::usize u = 0; u < m.source_ids; ++u) {
    m.ring[u * m.W + k] = kNaN; // absent this session unless written below
  }
  const atx::u16 year16 = static_cast<atx::u16>(year);
  const atx::usize yidx = static_cast<atx::usize>(year - first_year);
  atx::u32 valid_bars = 0;
  for (atx::usize j = 0; j < count; ++j) {
    atx::u32 slot = 0;
    atx::usize cell = 0;
    if (!m.find(ids[j], slot, cell)) {
      return Err(ErrorCode::Internal, "pit universe: id vanished between passes");
    }
    if (static_cast<atx::usize>(slot) >= old_count) {
      m.id[slot] = ids[j];
      m.first_seen[slot] = static_cast<atx::u32>(s);
    }
    const atx::f64 close = raw_close[j];
    const atx::f64 vol = volume[j];
    const bool valid = std::isfinite(close) && close > 0.0 && std::isfinite(vol) && vol >= 0.0;
    if (valid) {
      const atx::f64 dv = close * vol; // one binary64 product, stored before use (DR15-5)
      m.ring[static_cast<atx::usize>(slot) * m.W + k] = dv;
      m.last_bar[slot] = static_cast<atx::u32>(s);
      if (m.first_bar[slot] == kPitNoBar) {
        m.first_bar[slot] = static_cast<atx::u32>(s);
      }
      m.last_close[slot] = close;
      m.last_shares[slot] = shares.empty() ? kNaN : shares[j];
      m.last_gics[slot] = gics.empty() ? kNaN : gics[j];
      ++valid_bars;
      if (m.last_year_seen[slot] != year16) {
        m.last_year_seen[slot] = year16;
        ++m.year_ids_seen[yidx];
      }
    }
    m.seen_this_call[slot] = 0u;
  }
  if (s == 0) {
    m.first_year = year;
  }
  m.session_keys[s] = session_key;
  m.ids_valid_per_session[s] = valid_bars;
  ++m.year_sessions[yidx];
  m.sessions = s + 1;
  if (m.rebalances > 0 && m.effective_keys[m.rebalances - 1] == 0) {
    m.effective_keys[m.rebalances - 1] = session_key; // R15-4: next observed session
  }
  return Ok();
}

// ===========================================================================
//  §3.4 rebalance
// ===========================================================================

Result<PitRebalanceView> PitUniverseBuilder::rebalance(atx::i64 rank_session_key) {
  Impl &m = *impl_;
  if (m.sessions == 0 || rank_session_key != m.session_keys[m.sessions - 1]) {
    return Err(ErrorCode::InvalidArgument, "pit universe: rank session has no data");
  }
  if (m.rebalances >= m.R) {
    return Err(ErrorCode::OutOfRange, "pit universe: max_rebalances exceeded");
  }
  if (m.rebalances > 0 && rank_session_key <= m.rank_keys[m.rebalances - 1]) {
    return Err(ErrorCode::InvalidArgument,
               "pit universe: rank session not after the previous rank session");
  }
  const atx::usize Rs = m.sessions - 1;
  const atx::usize r = m.rebalances;

  // --- 1. eligibility and key ------------------------------------------------
  atx::usize E = 0;
  for (atx::usize u = 0; u < m.source_ids; ++u) {
    const bool bar_on_rank = m.last_bar[u] == static_cast<atx::u32>(Rs);
    atx::usize valid_count = 0;
    for (atx::usize w = 0; w < m.W; ++w) {
      const atx::f64 v = m.ring[u * m.W + w];
      if (std::isfinite(v)) {
        m.median_scratch[valid_count] = v;
        ++valid_count;
      }
    }
    atx::f64 adv = kNaN;
    if (valid_count > 0) {
      adv = median_in_place(std::span<atx::f64>{m.median_scratch.data(), valid_count});
    }
    const bool eligible = bar_on_rank && m.last_close[u] > m.cfg.min_raw_price_exclusive &&
                          valid_count >= m.cfg.min_valid_observations &&
                          adv >= m.cfg.min_adv_usd; // NaN compares false
    if (!eligible) {
      continue;
    }
    const atx::f64 cap = (m.last_shares[u] > 0.0) ? m.last_shares[u] * m.last_close[u] : kNaN;
    m.ranked_scratch[E] = PitRankedRow{static_cast<atx::u32>(u),
                                       m.id[u],
                                       0u,
                                       adv,
                                       static_cast<atx::u32>(valid_count),
                                       m.last_close[u],
                                       cap,
                                       m.last_gics[u]};
    ++E;
  }

  // --- 2. order: key descending, then first-seen slot ascending ---------------
  for (atx::usize p = 0; p < E; ++p) {
    m.order[p] = static_cast<atx::u32>(p);
  }
  std::sort(m.order.begin(), m.order.begin() + static_cast<std::ptrdiff_t>(E),
            [&m](atx::u32 a, atx::u32 b) noexcept {
              const PitRankedRow &ra = m.ranked_scratch[a];
              const PitRankedRow &rb = m.ranked_scratch[b];
              return ra.adv63_usd > rb.adv63_usd ||
                     (ra.adv63_usd == rb.adv63_usd && ra.slot < rb.slot);
            });
  for (atx::usize p = 0; p < E; ++p) {
    m.ranked[p] = m.ranked_scratch[m.order[p]];
    m.ranked[p].rank = static_cast<atx::u32>(p + 1);
  }

  // --- 3. per cut ---------------------------------------------------------------
  std::fill(m.new_bits.begin(), m.new_bits.begin() + static_cast<std::ptrdiff_t>(m.source_ids),
            atx::u8{0});
  for (atx::usize ti = 0; ti < m.cfg.top_n_count; ++ti) {
    for (atx::usize bi = 0; bi < m.cfg.band_count; ++bi) {
      const atx::usize c = ti * m.cfg.band_count + bi;
      const atx::usize n = m.cfg.top_n[ti];
      const atx::usize kband = band_threshold(n, m.cfg.band_bp[bi]);
      const atx::u8 bit = static_cast<atx::u8>(1u << c);
      PitMemberRow *mem = m.members_scratch.data() + c * m.N;
      PitDropRow *drp = m.drops_scratch.data() + c * m.N;
      atx::usize mc = 0;
      atx::usize dc = 0;
      // 3a keep: incumbents within the band, best-ranked first, at most n.
      for (atx::usize p = 0; p < E && mc < n; ++p) {
        const PitRankedRow &row = m.ranked[p];
        if ((m.member_bits[row.slot] & bit) != 0 && static_cast<atx::usize>(row.rank) <= kband) {
          mem[mc] = PitMemberRow{row.slot, row.rank, PitMemberStatus::Keep};
          ++mc;
          m.new_bits[row.slot] = static_cast<atx::u8>(m.new_bits[row.slot] | bit);
        }
      }
      // 3b fill: best-ranked rows not yet in the list.
      for (atx::usize p = 0; p < E && mc < n; ++p) {
        const PitRankedRow &row = m.ranked[p];
        if ((m.new_bits[row.slot] & bit) != 0) {
          continue;
        }
        const PitMemberStatus status =
            ((m.member_bits[row.slot] & bit) != 0) ? PitMemberStatus::Keep : PitMemberStatus::Add;
        mem[mc] = PitMemberRow{row.slot, row.rank, status};
        ++mc;
        m.new_bits[row.slot] = static_cast<atx::u8>(m.new_bits[row.slot] | bit);
      }
      // 3c drops: incumbents not in the new list, slot ascending, kind by formula (DR15-10).
      atx::u32 drops_rank = 0;
      atx::u32 drops_last_bar = 0;
      for (atx::usize u = 0; u < m.source_ids; ++u) {
        if ((m.member_bits[u] & bit) == 0 || (m.new_bits[u] & bit) != 0) {
          continue;
        }
        const PitDropKind kind =
            (m.last_bar[u] < static_cast<atx::u32>(Rs)) ? PitDropKind::LastBar : PitDropKind::Rank;
        drp[dc] = PitDropRow{static_cast<atx::u32>(u), kind};
        ++dc;
        m.last_drop_kind[u * m.C + c] = static_cast<atx::u8>(kind);
        if (kind == PitDropKind::LastBar) {
          ++drops_last_bar;
        } else {
          ++drops_rank;
        }
      }
      // 3d churn, 3e integers (DR15-4).
      atx::u32 adds = 0;
      atx::u32 kept = 0;
      atx::u32 gics_missing = 0;
      atx::u64 valid_total = 0;
      for (atx::usize i = 0; i < mc; ++i) {
        if (mem[i].status == PitMemberStatus::Keep) {
          ++kept;
        } else {
          ++adds;
        }
        const PitRankedRow &row = m.ranked[mem[i].rank - 1];
        if (std::isnan(row.gics)) {
          ++gics_missing;
        }
        valid_total += row.valid_observations;
      }
      m.churn[r * m.C + c] =
          PitChurn{adds, drops_rank, drops_last_bar, kept, static_cast<atx::u32>(mc)};
      m.gics_missing[r * m.C + c] = gics_missing;
      m.valid_total[r * m.C + c] = valid_total;
      // 3f orders (DR15-2): view rank ascending; retained slot ascending.
      std::sort(mem, mem + mc, [](const PitMemberRow &a, const PitMemberRow &b) noexcept {
        return a.rank < b.rank;
      });
      for (atx::usize i = 0; i < mc; ++i) {
        m.order[i] = static_cast<atx::u32>(i);
      }
      std::sort(m.order.begin(), m.order.begin() + static_cast<std::ptrdiff_t>(mc),
                [mem](atx::u32 a, atx::u32 b) noexcept { return mem[a].slot < mem[b].slot; });
      const atx::usize base = (r * m.C + c) * m.N;
      for (atx::usize i = 0; i < mc; ++i) {
        m.retained_slots[base + i] = mem[m.order[i]].slot;
        m.retained_ranks[base + i] = mem[m.order[i]].rank;
      }
      m.retained_count[r * m.C + c] = static_cast<atx::u32>(mc);
      for (atx::usize i = 0; i < mc; ++i) {
        const atx::usize idx = static_cast<atx::usize>(mem[i].slot) * m.C + c;
        if (m.first_member_r[idx] == kPitNoBar) {
          m.first_member_r[idx] = static_cast<atx::u32>(r);
        }
        m.last_member_r[idx] = static_cast<atx::u32>(r);
        m.ever_bits[mem[i].slot] = static_cast<atx::u8>(m.ever_bits[mem[i].slot] | bit);
      }
      m.member_spans[c] = std::span<const PitMemberRow>{mem, mc};
      m.drop_spans[c] = std::span<const PitDropRow>{drp, dc};
    }
  }
  for (atx::usize u = 0; u < m.source_ids; ++u) {
    m.member_bits[u] = m.new_bits[u];
  }

  // --- 4. keys and view ---------------------------------------------------------
  m.rank_keys[r] = rank_session_key;
  m.effective_keys[r] = 0; // pending until the next observe_session (R15-4)
  m.reb_ids_seen[r] = static_cast<atx::u32>(m.source_ids);
  m.reb_ids_valid[r] = m.ids_valid_per_session[Rs];
  m.reb_eligible[r] = static_cast<atx::u32>(E);
  m.rebalances = r + 1;

  PitRebalanceView view{};
  view.index = r;
  view.rank_session_key = rank_session_key;
  view.ids_seen = m.source_ids;
  view.ids_with_valid_bar = m.ids_valid_per_session[Rs];
  view.eligible = E;
  view.ranked = std::span<const PitRankedRow>{m.ranked.data(), E};
  view.members = std::span<const std::span<const PitMemberRow>>{m.member_spans.data(), m.C};
  view.drops = std::span<const std::span<const PitDropRow>>{m.drop_spans.data(), m.C};
  view.churn = std::span<const PitChurn>{m.churn.data() + r * m.C, m.C};
  view.gics_missing_members = std::span<const atx::u32>{m.gics_missing.data() + r * m.C, m.C};
  view.valid_observations_total = std::span<const atx::u64>{m.valid_total.data() + r * m.C, m.C};
  return Ok(view);
}

// ===========================================================================
//  Accessors (§3.1)
// ===========================================================================

const PitUniverseConfig &PitUniverseBuilder::config() const noexcept { return impl_->cfg; }
atx::usize PitUniverseBuilder::sessions() const noexcept { return impl_->sessions; }
atx::usize PitUniverseBuilder::rebalances() const noexcept { return impl_->rebalances; }
atx::usize PitUniverseBuilder::source_ids() const noexcept { return impl_->source_ids; }
atx::usize PitUniverseBuilder::cuts() const noexcept { return impl_->C; }

atx::i64 PitUniverseBuilder::session_key(atx::usize ordinal) const noexcept {
  return impl_->session_keys[ordinal];
}
atx::usize PitUniverseBuilder::ids_with_valid_bar(atx::usize ordinal) const noexcept {
  return impl_->ids_valid_per_session[ordinal];
}
atx::i64 PitUniverseBuilder::security_id(atx::u32 slot) const noexcept { return impl_->id[slot]; }
atx::u32 PitUniverseBuilder::first_seen(atx::u32 slot) const noexcept {
  return impl_->first_seen[slot];
}
atx::u32 PitUniverseBuilder::first_bar(atx::u32 slot) const noexcept {
  return impl_->first_bar[slot];
}
atx::u32 PitUniverseBuilder::last_bar(atx::u32 slot) const noexcept {
  return impl_->last_bar[slot];
}

std::span<const atx::u32> PitUniverseBuilder::members(atx::usize r, atx::usize c) const noexcept {
  const Impl &m = *impl_;
  return std::span<const atx::u32>{m.retained_slots.data() + (r * m.C + c) * m.N,
                                   m.retained_count[r * m.C + c]};
}
std::span<const atx::u32> PitUniverseBuilder::member_ranks(atx::usize r,
                                                           atx::usize c) const noexcept {
  const Impl &m = *impl_;
  return std::span<const atx::u32>{m.retained_ranks.data() + (r * m.C + c) * m.N,
                                   m.retained_count[r * m.C + c]};
}
const PitChurn &PitUniverseBuilder::churn(atx::usize r, atx::usize c) const noexcept {
  return impl_->churn[r * impl_->C + c];
}
atx::i64 PitUniverseBuilder::rank_key(atx::usize r) const noexcept { return impl_->rank_keys[r]; }
atx::i64 PitUniverseBuilder::effective_key(atx::usize r) const noexcept {
  return impl_->effective_keys[r];
}

// ===========================================================================
//  §4.3 coverage_by_year
// ===========================================================================

Status PitUniverseBuilder::coverage_by_year(std::span<PitCoverageYear> out, atx::usize &n) const {
  const Impl &m = *impl_;
  n = 0;
  atx::usize count = 0;
  for (atx::usize y = 0; y < kPitMaxYears; ++y) {
    if (m.year_sessions[y] > 0) {
      ++count;
    }
  }
  if (out.size() < count) {
    return Err(ErrorCode::OutOfRange, "pit universe: coverage output span too small");
  }
  for (atx::usize y = 0; y < kPitMaxYears; ++y) {
    if (m.year_sessions[y] == 0) {
      continue;
    }
    const atx::i32 year = m.first_year + static_cast<atx::i32>(y);
    PitCoverageYear row{};
    row.year = year;
    row.sessions = m.year_sessions[y];
    row.ids_seen = m.year_ids_seen[y];
    row.cuts = m.C;
    atx::usize k = 0;
    for (atx::usize s = 0; s < m.sessions; ++s) {
      if (pit_year_of(m.session_keys[s]) == year) {
        m.query_median[k] = static_cast<atx::f64>(m.ids_valid_per_session[s]);
        ++k;
      }
    }
    row.ids_with_valid_bar_median = median_in_place(std::span<atx::f64>{m.query_median.data(), k});
    k = 0;
    for (atx::usize r = 0; r < m.rebalances; ++r) {
      if (pit_year_of(m.rank_keys[r]) == year) {
        m.query_median[k] = static_cast<atx::f64>(m.reb_eligible[r]);
        ++k;
      }
    }
    row.rebalances = k;
    row.eligible_median = median_in_place(std::span<atx::f64>{m.query_median.data(), k});
    for (atx::usize c = 0; c < m.C; ++c) {
      k = 0;
      for (atx::usize r = 0; r < m.rebalances; ++r) {
        if (pit_year_of(m.rank_keys[r]) == year) {
          m.query_median[k] = static_cast<atx::f64>(m.retained_count[r * m.C + c]);
          ++k;
        }
      }
      row.members_median[c] = median_in_place(std::span<atx::f64>{m.query_median.data(), k});
      k = 0;
      for (atx::usize r = 0; r < m.rebalances; ++r) {
        if (pit_year_of(m.rank_keys[r]) == year) {
          const atx::u32 members = m.retained_count[r * m.C + c];
          // One division (C-4); 0.0 when members == 0. Reported only, never compared.
          m.query_median[k] =
              (members == 0) ? 0.0
                             : static_cast<atx::f64>(m.valid_total[r * m.C + c]) /
                                   static_cast<atx::f64>(m.W * static_cast<atx::usize>(members));
          ++k;
        }
      }
      row.nonmissing_fraction_median[c] =
          median_in_place(std::span<atx::f64>{m.query_median.data(), k});
      k = 0;
      for (atx::usize r = 0; r < m.rebalances; ++r) {
        if (pit_year_of(m.rank_keys[r]) == year) {
          m.query_median[k] = static_cast<atx::f64>(m.gics_missing[r * m.C + c]);
          ++k;
        }
      }
      row.gics_missing_members_median[c] =
          median_in_place(std::span<atx::f64>{m.query_median.data(), k});
    }
    out[n] = row;
    ++n;
  }
  return Ok();
}

// ===========================================================================
//  §4.4 union_by_year — year of the EFFECTIVE session; pending rebalances are
//  not yet effective anywhere and are skipped.
// ===========================================================================

Status PitUniverseBuilder::union_by_year(std::span<PitUnionYear> out, atx::usize &n) const {
  const Impl &m = *impl_;
  n = 0;
  atx::usize count = 0;
  atx::i32 last_year = 0;
  for (atx::usize r = 0; r < m.rebalances; ++r) {
    if (m.effective_keys[r] == 0) {
      continue;
    }
    const atx::i32 year = pit_year_of(m.effective_keys[r]);
    if (count == 0 || year != last_year) {
      ++count;
      last_year = year;
    }
  }
  if (count > kPitMaxYears) {
    return Err(ErrorCode::Internal, "pit universe: effective years exceed kPitMaxYears");
  }
  if (out.size() < count) {
    return Err(ErrorCode::OutOfRange, "pit universe: union output span too small");
  }
  for (atx::usize k = 0; k < count; ++k) {
    out[k] = PitUnionYear{};
    out[k].cuts = m.C;
  }
  for (atx::usize c = 0; c < m.C; ++c) {
    std::fill(m.union_bitmap.begin(),
              m.union_bitmap.begin() + static_cast<std::ptrdiff_t>(m.source_ids), atx::u8{0});
    std::fill(m.year_mark.begin(), m.year_mark.begin() + static_cast<std::ptrdiff_t>(m.source_ids),
              atx::u16{0});
    atx::usize cumulative = 0;
    atx::usize distinct = 0;
    atx::usize k = 0;
    bool open = false;
    for (atx::usize r = 0; r < m.rebalances; ++r) {
      if (m.effective_keys[r] == 0) {
        continue;
      }
      const atx::i32 year = pit_year_of(m.effective_keys[r]);
      if (!open) {
        open = true;
        out[k].year = year;
      } else if (year != out[k].year) {
        out[k].distinct[c] = distinct;
        out[k].cumulative[c] = cumulative;
        ++k;
        out[k].year = year;
        distinct = 0;
      }
      const atx::u16 mark = static_cast<atx::u16>(k + 1);
      const std::span<const atx::u32> slots = members(r, c);
      for (const atx::u32 slot : slots) {
        if (m.year_mark[slot] != mark) {
          m.year_mark[slot] = mark;
          ++distinct;
        }
        if (m.union_bitmap[slot] == 0) {
          m.union_bitmap[slot] = 1u;
          ++cumulative;
        }
      }
    }
    if (open) {
      out[k].distinct[c] = distinct;
      out[k].cumulative[c] = cumulative;
    }
  }
  n = count;
  return Ok();
}

// ===========================================================================
//  §4.6 survivorship
// ===========================================================================

Result<PitSurvivorship> PitUniverseBuilder::survivorship() const {
  const Impl &m = *impl_;
  PitSurvivorship out{};
  out.cuts = m.C;
  if (m.sessions == 0) {
    return Ok(out);
  }
  const atx::u32 final_ordinal = static_cast<atx::u32>(m.sessions - 1);
  // First / last observed session ordinal per observed year.
  std::array<atx::usize, kPitMaxYears> first_s{};
  std::array<atx::usize, kPitMaxYears> last_s{};
  std::array<bool, kPitMaxYears> seen{};
  for (atx::usize s = 0; s < m.sessions; ++s) {
    const atx::usize y = m.year_index(pit_year_of(m.session_keys[s]));
    if (!seen[y]) {
      seen[y] = true;
      first_s[y] = s;
    }
    last_s[y] = s;
  }
  for (atx::usize c = 0; c < m.C; ++c) {
    PitSurvivorshipCut &cut = out.cut[c];
    const atx::u8 bit = static_cast<atx::u8>(1u << c);
    for (atx::usize u = 0; u < m.source_ids; ++u) {
      if ((m.ever_bits[u] & bit) == 0) {
        continue;
      }
      ++cut.ever_members;
      if (m.last_bar[u] < final_ordinal) { // archive-end guard: == final => censored
        ++cut.ended_before_window_end;
      }
    }
    cut.censored = cut.ever_members - cut.ended_before_window_end;
    // (b) one row per observed year whose FIRST session is the effective session
    // of a rebalance (rank date = last session of the previous year); a year with
    // no such rebalance (the warmup year) has no row.
    atx::usize k = 0;
    for (atx::usize y = 0; y < kPitMaxYears; ++y) {
      if (!seen[y]) {
        continue;
      }
      const atx::i64 first_key = m.session_keys[first_s[y]];
      for (atx::usize r = 0; r < m.rebalances; ++r) {
        if (m.effective_keys[r] != first_key) {
          continue;
        }
        cut.years[k] = m.first_year + static_cast<atx::i32>(y);
        const std::span<const atx::u32> slots = members(r, c);
        cut.year_start_members[k] = slots.size();
        atx::usize exits = 0;
        for (const atx::u32 slot : slots) {
          if (m.last_bar[slot] < static_cast<atx::u32>(last_s[y])) {
            ++exits;
          }
        }
        cut.year_exits[k] = exits;
        ++k;
        break;
      }
    }
    cut.year_count = k;
  }
  return Ok(out);
}

// ===========================================================================
//  §4.5 exits
// ===========================================================================

Status PitUniverseBuilder::exits(atx::usize cut, std::span<PitExitRecord> out,
                                 atx::usize &n) const {
  const Impl &m = *impl_;
  n = 0;
  if (cut >= m.C) {
    return Err(ErrorCode::InvalidArgument, "pit universe: cut index out of range");
  }
  const atx::u8 bit = static_cast<atx::u8>(1u << cut);
  atx::usize count = 0;
  for (atx::usize u = 0; u < m.source_ids; ++u) {
    if ((m.ever_bits[u] & bit) != 0) {
      ++count;
    }
  }
  if (out.size() < count) {
    return Err(ErrorCode::OutOfRange, "pit universe: exits output span too small");
  }
  for (atx::usize u = 0; u < m.source_ids; ++u) {
    if ((m.ever_bits[u] & bit) == 0) {
      continue;
    }
    const atx::usize idx = u * m.C + cut;
    PitExitKind kind = PitExitKind::WindowEnd;
    if ((m.member_bits[u] & bit) == 0) {
      const atx::u8 last_drop = m.last_drop_kind[idx];
      if (last_drop == kNoDrop) {
        return Err(ErrorCode::Internal, "pit universe: ever-member without a recorded drop");
      }
      kind = (last_drop == static_cast<atx::u8>(PitDropKind::Rank))
                 ? PitExitKind::RankDrop
                 : PitExitKind::LastBarWithinWindow;
    }
    out[n] = PitExitRecord{m.id[u],
                           m.first_seen[u],
                           m.first_bar[u],
                           m.last_bar[u],
                           m.first_member_r[idx],
                           m.last_member_r[idx],
                           kind};
    ++n;
  }
  std::sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n),
            [](const PitExitRecord &a, const PitExitRecord &b) noexcept {
              return a.security_id < b.security_id;
            });
  return Ok();
}

// ===========================================================================
//  §4.7 membership.bin codec (DR15-6)
// ===========================================================================

atx::u64 pit_fnv1a64(std::string_view bytes) noexcept {
  atx::u64 h = kFnvOffset;
  for (const char ch : bytes) {
    h ^= static_cast<atx::u64>(static_cast<unsigned char>(ch));
    h *= kFnvPrime;
  }
  return h;
}

Result<std::string> encode_membership_bin(const PitUniverseBuilder &b) {
  const PitUniverseConfig &cfg = b.config();
  constexpr atx::usize kU32Max = static_cast<atx::usize>((std::numeric_limits<atx::u32>::max)());
  if (cfg.adv_window > kU32Max || cfg.min_valid_observations > kU32Max) {
    return Err(ErrorCode::Internal, "pit universe: window does not fit the u32 header field");
  }
  for (atx::usize i = 0; i < cfg.top_n_count; ++i) {
    if (cfg.top_n[i] > kU32Max) {
      return Err(ErrorCode::Internal, "pit universe: top_n does not fit the u32 header field");
    }
  }
  if (b.rebalances() > kU32Max) {
    return Err(ErrorCode::Internal, "pit universe: rebalance count does not fit u32");
  }
  std::string out;
  out.append(kPitMembershipMagic.data(), kPitMembershipMagic.size());
  put_u32(out, kPitMembershipVersion);
  put_u32(out, static_cast<atx::u32>(cfg.adv_window));
  put_u32(out, static_cast<atx::u32>(cfg.min_valid_observations));
  put_f64(out, cfg.min_raw_price_exclusive);
  put_u32(out, static_cast<atx::u32>(cfg.top_n_count));
  for (atx::usize i = 0; i < cfg.top_n_count; ++i) {
    put_u32(out, static_cast<atx::u32>(cfg.top_n[i]));
  }
  put_u32(out, static_cast<atx::u32>(cfg.band_count));
  for (atx::usize i = 0; i < cfg.band_count; ++i) {
    put_u32(out, cfg.band_bp[i]);
  }
  put_u32(out, static_cast<atx::u32>(b.rebalances()));
  std::vector<std::pair<atx::i64, atx::u32>> rows; // post-run allocation (§3.1)
  for (atx::usize r = 0; r < b.rebalances(); ++r) {
    if (b.effective_key(r) == 0) {
      return Err(ErrorCode::Internal, "pit universe: rebalance " + std::to_string(r) +
                                          " has no effective session");
    }
    put_i64(out, b.rank_key(r));
    put_i64(out, b.effective_key(r));
    for (atx::usize c = 0; c < b.cuts(); ++c) {
      const std::span<const atx::u32> slots = b.members(r, c);
      const std::span<const atx::u32> ranks = b.member_ranks(r, c);
      rows.clear();
      for (atx::usize i = 0; i < slots.size(); ++i) {
        rows.emplace_back(b.security_id(slots[i]), ranks[i]);
      }
      using IdRank = std::pair<atx::i64, atx::u32>;
      std::sort(rows.begin(), rows.end(),
                [](const IdRank &x, const IdRank &y) noexcept { return x.first < y.first; });
      put_u32(out, static_cast<atx::u32>(rows.size()));
      for (const auto &row : rows) {
        put_i64(out, row.first);
      }
      for (const auto &row : rows) {
        put_u32(out, row.second);
      }
    }
  }
  put_u64(out, pit_fnv1a64(out));
  return Ok(std::move(out));
}

Result<PitMembershipImage> decode_membership_bin(std::string_view bytes) {
  constexpr atx::usize kHeaderMin = 8 + 4;
  constexpr atx::usize kTrailer = 8;
  if (bytes.size() < kHeaderMin + kTrailer) {
    return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin too short");
  }
  if (bytes.substr(0, 8) != kPitMembershipMagic) {
    return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin bad magic");
  }
  atx::u32 version = 0;
  {
    ByteReader head{bytes.substr(8)};
    if (!head.take_u32(version)) {
      return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin too short");
    }
  }
  if (version != kPitMembershipVersion) {
    return Err(ErrorCode::InvalidArgument,
               "pit universe: membership.bin version " + std::to_string(version) + " unsupported");
  }
  const atx::usize body_size = bytes.size() - kTrailer;
  atx::u64 trailer = 0;
  {
    ByteReader tail{bytes.substr(body_size)};
    if (!tail.take_u64(trailer)) {
      return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin too short");
    }
  }
  // The trailer is verified only after the body parses structurally, so a short
  // read reports InvalidArgument (design §4.7) and only a well-formed body with a
  // wrong digest reports Internal.
  ByteReader body{bytes.substr(0, body_size)};
  const auto short_buffer = [] {
    return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin truncated body");
  };
  // Magic and version were validated above; skip them.
  {
    atx::u64 skip = 0;
    atx::u32 skip32 = 0;
    if (!body.take_u64(skip) || !body.take_u32(skip32)) {
      return short_buffer();
    }
  }
  PitMembershipImage img{};
  img.fnv1a64 = trailer;
  if (!body.take_u32(img.adv_window) || !body.take_u32(img.min_valid_observations) ||
      !body.take_f64(img.min_raw_price_exclusive)) {
    return short_buffer();
  }
  atx::u32 t = 0;
  if (!body.take_u32(t)) {
    return short_buffer();
  }
  if (t == 0 || t > kPitMaxTopNValues) {
    return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin top_n_count out of range");
  }
  img.top_n.resize(t);
  for (atx::u32 i = 0; i < t; ++i) {
    if (!body.take_u32(img.top_n[i])) {
      return short_buffer();
    }
  }
  atx::u32 bcount = 0;
  if (!body.take_u32(bcount)) {
    return short_buffer();
  }
  if (bcount == 0 || bcount > kPitMaxBandValues) {
    return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin band_count out of range");
  }
  img.band_bp.resize(bcount);
  for (atx::u32 i = 0; i < bcount; ++i) {
    if (!body.take_u32(img.band_bp[i])) {
      return short_buffer();
    }
  }
  atx::u32 rcount = 0;
  if (!body.take_u32(rcount)) {
    return short_buffer();
  }
  if (rcount > kPitMaxRebalances) {
    return Err(ErrorCode::InvalidArgument,
               "pit universe: membership.bin rebalance_count out of range");
  }
  const atx::usize cuts = static_cast<atx::usize>(t) * static_cast<atx::usize>(bcount);
  img.rebalances.resize(rcount);
  for (atx::u32 r = 0; r < rcount; ++r) {
    PitMembershipRebalance &reb = img.rebalances[r];
    if (!body.take_i64(reb.rank_session_key) || !body.take_i64(reb.effective_session_key)) {
      return short_buffer();
    }
    if (reb.effective_session_key == 0) {
      return Err(ErrorCode::InvalidArgument,
                 "pit universe: membership.bin effective_session_key is 0");
    }
    reb.cuts.resize(cuts);
    for (atx::usize c = 0; c < cuts; ++c) {
      atx::u32 n = 0;
      if (!body.take_u32(n)) {
        return short_buffer();
      }
      if (static_cast<atx::usize>(n) > body.remaining() / 12) { // 8 id + 4 rank bytes each
        return short_buffer();
      }
      PitMembershipCut &cut = reb.cuts[c];
      cut.security_ids.resize(n);
      cut.ranks.resize(n);
      for (atx::u32 i = 0; i < n; ++i) {
        if (!body.take_i64(cut.security_ids[i])) {
          return short_buffer();
        }
        if (i > 0 && cut.security_ids[i] <= cut.security_ids[i - 1]) {
          return Err(ErrorCode::InvalidArgument,
                     "pit universe: membership.bin ids not strictly ascending");
        }
      }
      for (atx::u32 i = 0; i < n; ++i) {
        if (!body.take_u32(cut.ranks[i])) {
          return short_buffer();
        }
      }
    }
  }
  if (body.remaining() != 0) {
    return Err(ErrorCode::InvalidArgument, "pit universe: membership.bin trailing bytes");
  }
  if (pit_fnv1a64(bytes.substr(0, body_size)) != trailer) {
    return Err(ErrorCode::Internal, "pit universe: membership.bin trailer mismatch");
  }
  return Ok(std::move(img));
}

} // namespace atx::engine::data
