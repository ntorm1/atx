#pragma once

// MarketState kind (`.atxms`) private on-disk records and the kind's schema table
// (design wave3-persist D10; spec docs/storage/PERSIST_FORMAT.md "MarketState kind").
// Every record is trivially copyable, little-endian, naturally aligned and padding-free:
// each one's field list tiles its sizeof (static_assert below), and the field lists
// are folded into the schema fingerprint, so moving or resizing a field changes the
// fingerprint and an old file is refused (SchemaMismatch), never misread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/persist/persist_format.hpp"
#include "atx/vol/api/persist/state_archive.hpp"
#include "atx/vol/api/pricing/rates_curve.hpp"
#include "atx/vol/api/risk/market_state.hpp"

namespace atx::vol::state_format {

// The TimeSpec the Meta record serializes field by field: a new TimeSpec / VolTimeParams
// member must be added here (and to the fingerprint) or this fires.
static_assert(sizeof(VolTimeParams) == 5 * sizeof(double));
static_assert(sizeof(TimeSpec) == 48);
static_assert(kPersistPod<ExpiryCarry> && kPersistPod<DividendEvent>);

// What the schema fingerprint cannot see (review 1): it folds field names, offsets and
// sizes, not field TYPES nor the numeric values of enums stored as bytes. Those are
// pinned here instead, so a retyped field of a library POD this kind stores in place, or
// a renumbered enum, fails to compile rather than misreading old files.
static_assert(static_cast<std::uint8_t>(Side::Call) == 0 &&
              static_cast<std::uint8_t>(Side::Put) == 1);                 // `side` column
static_assert(static_cast<std::uint8_t>(CarryFrom::State) == 0 &&
              static_cast<std::uint8_t>(CarryFrom::Surface) == 1);        // UnderlierRow::carry
static_assert(static_cast<std::uint8_t>(TimeConvention::Calendar365) == 0 &&
              static_cast<std::uint8_t>(TimeConvention::VolTime) == 1);   // Meta::time_convention
static_assert(std::is_same_v<std::underlying_type_t<Side>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<CarryFrom>, std::uint8_t> &&
              std::is_same_v<std::underlying_type_t<TimeConvention>, std::uint8_t>);
static_assert(std::is_same_v<decltype(ExpiryCarry::expiry_ns), std::int64_t> &&
              std::is_same_v<decltype(ExpiryCarry::pv_divs), double> &&
              std::is_same_v<decltype(ExpiryCarry::rate), double> &&
              std::is_same_v<decltype(ExpiryCarry::q), double> &&
              std::is_same_v<decltype(ExpiryCarry::T_override), double>);
static_assert(std::is_same_v<decltype(DividendEvent::ex_date_ns), std::int64_t> &&
              std::is_same_v<decltype(DividendEvent::amount), double>);
static_assert(std::is_same_v<decltype(VolTimeParams::alpha), double> &&
              std::is_same_v<decltype(VolTimeParams::trading_hours_per_year), double> &&
              std::is_same_v<decltype(VolTimeParams::nontrading_hours_per_year), double> &&
              std::is_same_v<decltype(VolTimeParams::session_open_hour_et), double> &&
              std::is_same_v<decltype(VolTimeParams::session_span_hours), double>);

struct Meta {
  std::int64_t ts_ns{};
  std::uint8_t time_convention{};   // TimeConvention
  std::uint8_t has_vendor_price{};  // the VendorPrice column is present
  std::uint8_t has_vendor_delta{};
  std::uint8_t has_extras{};        // the seven extras columns are present
  std::uint32_t conventions_version{};
  double vt_alpha{};
  double vt_trading_hours_per_year{};
  double vt_nontrading_hours_per_year{};
  double vt_session_open_hour_et{};
  double vt_session_span_hours{};
  std::uint64_t n_underliers{};
  std::uint64_t n_marks{};
  std::uint64_t n_term_carry{};
  std::uint64_t n_cash_divs{};
  std::uint64_t n_yield_pillars{};
  std::uint64_t n_surfaces{};
  std::uint64_t rep_n_uids{};
  std::uint64_t rep_n_uids_dropped{};
  std::uint64_t rep_n_marks{};
  std::uint64_t rep_n_uids_with_divs{};
  std::uint64_t rep_n_expiry_rows{};
  double rep_max_ddiv_spread{};
  std::uint64_t rep_n_duplicate_marks{};
  std::uint64_t rep_n_rows_bad_sigma{};
  std::uint8_t has_report{};
  std::array<std::uint8_t, 7> reserved0{};
  std::array<char, 16> date{};      // NUL padded, <= 15 bytes
  std::array<char, 8> bucket_et{};  // NUL padded, <= 7 bytes
  std::array<std::uint8_t, 56> reserved1{};
};

// UnderlierRow::mark_flags
inline constexpr std::uint8_t kRowHasMarks = 1u;    // a ContractMarks entry exists (maybe empty)
inline constexpr std::uint8_t kRowVendorPrice = 2u; // its vendor_price span is populated
inline constexpr std::uint8_t kRowVendorDelta = 4u;
inline constexpr std::uint8_t kRowKnownFlags = 7u;

struct UnderlierRow {
  std::uint32_t uid{};
  std::uint8_t carry{};      // CarryFrom
  std::uint8_t mark_flags{}; // kRow*
  std::uint16_t reserved0{};
  double spot{};
  double flat_rate{};
  double flat_q{};
  std::uint64_t marks_begin{}; // rows of the mark columns, contiguous in uid order
  std::uint64_t marks_count{};
  std::uint32_t carry_begin{}; // TermCarry rows
  std::uint32_t carry_count{};
  std::uint32_t divs_begin{};  // CashDivs rows
  std::uint32_t divs_count{};
  std::uint32_t curve_begin{}; // YieldPillars rows; count 0 = no yield curve
  std::uint32_t curve_count{};
  std::uint32_t ticker_offset{}; // into Strings
  std::uint32_t ticker_length{};
  std::uint32_t surface_slot{kPersistNone}; // SurfaceIndex row, or none
  std::uint32_t reserved1{};
  std::uint64_t reserved2{};
};

struct YieldPillar {
  double t_years{};
  double zero_rate{};
};

// One uid of the writer's uid table (row i names uid i + 1).
struct UidTableRow {
  std::uint32_t offset{}; // into Strings
  std::uint32_t length{};
};

struct SurfaceIndexRow {
  std::uint32_t uid{};
  std::uint32_t reserved{};
  std::uint32_t symbol_offset{}; // into Strings: the record's ATXVSA2 symbol
  std::uint32_t symbol_length{};
};

#define ATX_PERSIST_FIELD(T, f) PersistFieldSpec{#f, offsetof(T, f), sizeof(T::f)}

inline constexpr PersistFieldSpec kMetaFields[] = {
    ATX_PERSIST_FIELD(Meta, ts_ns),
    ATX_PERSIST_FIELD(Meta, time_convention),
    ATX_PERSIST_FIELD(Meta, has_vendor_price),
    ATX_PERSIST_FIELD(Meta, has_vendor_delta),
    ATX_PERSIST_FIELD(Meta, has_extras),
    ATX_PERSIST_FIELD(Meta, conventions_version),
    ATX_PERSIST_FIELD(Meta, vt_alpha),
    ATX_PERSIST_FIELD(Meta, vt_trading_hours_per_year),
    ATX_PERSIST_FIELD(Meta, vt_nontrading_hours_per_year),
    ATX_PERSIST_FIELD(Meta, vt_session_open_hour_et),
    ATX_PERSIST_FIELD(Meta, vt_session_span_hours),
    ATX_PERSIST_FIELD(Meta, n_underliers),
    ATX_PERSIST_FIELD(Meta, n_marks),
    ATX_PERSIST_FIELD(Meta, n_term_carry),
    ATX_PERSIST_FIELD(Meta, n_cash_divs),
    ATX_PERSIST_FIELD(Meta, n_yield_pillars),
    ATX_PERSIST_FIELD(Meta, n_surfaces),
    ATX_PERSIST_FIELD(Meta, rep_n_uids),
    ATX_PERSIST_FIELD(Meta, rep_n_uids_dropped),
    ATX_PERSIST_FIELD(Meta, rep_n_marks),
    ATX_PERSIST_FIELD(Meta, rep_n_uids_with_divs),
    ATX_PERSIST_FIELD(Meta, rep_n_expiry_rows),
    ATX_PERSIST_FIELD(Meta, rep_max_ddiv_spread),
    ATX_PERSIST_FIELD(Meta, rep_n_duplicate_marks),
    ATX_PERSIST_FIELD(Meta, rep_n_rows_bad_sigma),
    ATX_PERSIST_FIELD(Meta, has_report),
    ATX_PERSIST_FIELD(Meta, reserved0),
    ATX_PERSIST_FIELD(Meta, date),
    ATX_PERSIST_FIELD(Meta, bucket_et),
    ATX_PERSIST_FIELD(Meta, reserved1),
};

inline constexpr PersistFieldSpec kUnderlierFields[] = {
    ATX_PERSIST_FIELD(UnderlierRow, uid),
    ATX_PERSIST_FIELD(UnderlierRow, carry),
    ATX_PERSIST_FIELD(UnderlierRow, mark_flags),
    ATX_PERSIST_FIELD(UnderlierRow, reserved0),
    ATX_PERSIST_FIELD(UnderlierRow, spot),
    ATX_PERSIST_FIELD(UnderlierRow, flat_rate),
    ATX_PERSIST_FIELD(UnderlierRow, flat_q),
    ATX_PERSIST_FIELD(UnderlierRow, marks_begin),
    ATX_PERSIST_FIELD(UnderlierRow, marks_count),
    ATX_PERSIST_FIELD(UnderlierRow, carry_begin),
    ATX_PERSIST_FIELD(UnderlierRow, carry_count),
    ATX_PERSIST_FIELD(UnderlierRow, divs_begin),
    ATX_PERSIST_FIELD(UnderlierRow, divs_count),
    ATX_PERSIST_FIELD(UnderlierRow, curve_begin),
    ATX_PERSIST_FIELD(UnderlierRow, curve_count),
    ATX_PERSIST_FIELD(UnderlierRow, ticker_offset),
    ATX_PERSIST_FIELD(UnderlierRow, ticker_length),
    ATX_PERSIST_FIELD(UnderlierRow, surface_slot),
    ATX_PERSIST_FIELD(UnderlierRow, reserved1),
    ATX_PERSIST_FIELD(UnderlierRow, reserved2),
};

// The library's own carry PODs are stored directly (zero-copy at open): their field
// lists pin them, so a member added to ExpiryCarry / DividendEvent changes the
// fingerprint instead of silently shifting the mapped rows.
inline constexpr PersistFieldSpec kExpiryCarryFields[] = {
    ATX_PERSIST_FIELD(ExpiryCarry, expiry_ns),
    ATX_PERSIST_FIELD(ExpiryCarry, pv_divs),
    ATX_PERSIST_FIELD(ExpiryCarry, rate),
    ATX_PERSIST_FIELD(ExpiryCarry, q),
    // Lane E (design D22): the per-expiry clock override, stored as-is (NaN = none). The
    // 32 -> 40 byte row moves the MarketState fingerprint: a 32-byte-row archive is
    // refused with SchemaMismatch and rebuilt, never misread.
    ATX_PERSIST_FIELD(ExpiryCarry, T_override),
};
inline constexpr PersistFieldSpec kDividendFields[] = {
    ATX_PERSIST_FIELD(DividendEvent, ex_date_ns),
    ATX_PERSIST_FIELD(DividendEvent, amount),
};
inline constexpr PersistFieldSpec kPillarFields[] = {
    ATX_PERSIST_FIELD(YieldPillar, t_years),
    ATX_PERSIST_FIELD(YieldPillar, zero_rate),
};
inline constexpr PersistFieldSpec kUidTableFields[] = {
    ATX_PERSIST_FIELD(UidTableRow, offset),
    ATX_PERSIST_FIELD(UidTableRow, length),
};
inline constexpr PersistFieldSpec kSurfaceIndexFields[] = {
    ATX_PERSIST_FIELD(SurfaceIndexRow, uid),
    ATX_PERSIST_FIELD(SurfaceIndexRow, reserved),
    ATX_PERSIST_FIELD(SurfaceIndexRow, symbol_offset),
    ATX_PERSIST_FIELD(SurfaceIndexRow, symbol_length),
};

#undef ATX_PERSIST_FIELD

static_assert(kPersistPod<Meta> && sizeof(Meta) == 256);
static_assert(kPersistPod<UnderlierRow> && sizeof(UnderlierRow) == 96);
static_assert(kPersistPod<YieldPillar> && sizeof(YieldPillar) == 16);
static_assert(kPersistPod<SurfaceIndexRow> && sizeof(SurfaceIndexRow) == 16);
static_assert(kPersistPod<UidTableRow> && sizeof(UidTableRow) == 8);
static_assert(persist_fields_tile(kUidTableFields, sizeof(UidTableRow)));
static_assert(sizeof(ExpiryCarry) == 40 && sizeof(DividendEvent) == 16);
static_assert(persist_fields_tile(kMetaFields, sizeof(Meta)));
static_assert(persist_fields_tile(kUnderlierFields, sizeof(UnderlierRow)));
static_assert(persist_fields_tile(kExpiryCarryFields, sizeof(ExpiryCarry)));
static_assert(persist_fields_tile(kDividendFields, sizeof(DividendEvent)));
static_assert(persist_fields_tile(kPillarFields, sizeof(YieldPillar)));
static_assert(persist_fields_tile(kSurfaceIndexFields, sizeof(SurfaceIndexRow)));
static_assert(offsetof(Meta, date) == 176 && offsetof(Meta, bucket_et) == 192);
static_assert(offsetof(UnderlierRow, marks_begin) == 32);
static_assert(offsetof(UnderlierRow, surface_slot) == 80);

[[nodiscard]] constexpr std::uint32_t sec(MarketStateSection s) noexcept {
  return static_cast<std::uint32_t>(s);
}

// The kind's schema: what the writer emits, what the reader accepts, and (with the
// salt) the fingerprint. Order is the file's section order.
inline constexpr PersistSectionSpec kSchema[] = {
    {sec(MarketStateSection::Meta), 1, PersistDType::Pod, sizeof(Meta), "meta", kMetaFields, true},
    {sec(MarketStateSection::Underliers), 1, PersistDType::Pod, sizeof(UnderlierRow), "underliers",
     kUnderlierFields, true},
    {sec(MarketStateSection::Strings), 1, PersistDType::Bytes, 1, "strings", {}, true},
    {sec(MarketStateSection::TermCarry), 1, PersistDType::Pod, sizeof(ExpiryCarry), "term-carry",
     kExpiryCarryFields, true},
    {sec(MarketStateSection::CashDivs), 1, PersistDType::Pod, sizeof(DividendEvent), "cash-divs",
     kDividendFields, true},
    {sec(MarketStateSection::YieldPillars), 1, PersistDType::Pod, sizeof(YieldPillar),
     "yield-pillars", kPillarFields, true},
    {sec(MarketStateSection::UidTable), 1, PersistDType::Pod, sizeof(UidTableRow), "uid-table",
     kUidTableFields, false},
    {sec(MarketStateSection::Strike), 1, PersistDType::F64, 8, "strike", {}, true},
    {sec(MarketStateSection::Expiry), 1, PersistDType::I64, 8, "expiry-ns", {}, true},
    {sec(MarketStateSection::Side), 1, PersistDType::U8, 1, "side", {}, true},
    {sec(MarketStateSection::Sigma), 1, PersistDType::F64, 8, "sigma", {}, true},
    {sec(MarketStateSection::VendorPrice), 1, PersistDType::F64, 8, "vendor-price", {}, false},
    {sec(MarketStateSection::VendorDelta), 1, PersistDType::F64, 8, "vendor-delta", {}, false},
    {sec(MarketStateSection::Bid), 1, PersistDType::F64, 8, "bid", {}, false},
    {sec(MarketStateSection::Ask), 1, PersistDType::F64, 8, "ask", {}, false},
    {sec(MarketStateSection::VendorGamma), 1, PersistDType::F64, 8, "vendor-gamma", {}, false},
    {sec(MarketStateSection::VendorVega), 1, PersistDType::F64, 8, "vendor-vega", {}, false},
    {sec(MarketStateSection::VendorTheta), 1, PersistDType::F64, 8, "vendor-theta", {}, false},
    {sec(MarketStateSection::VendorT), 1, PersistDType::F64, 8, "vendor-t", {}, false},
    {sec(MarketStateSection::RowUprc), 1, PersistDType::F64, 8, "row-uprc", {}, false},
    {sec(MarketStateSection::SurfaceIndex), 1, PersistDType::Pod, sizeof(SurfaceIndexRow),
     "surface-index", kSurfaceIndexFields, false},
    {sec(MarketStateSection::SurfaceArchive), 1, PersistDType::Bytes, 1, "surface-archive", {},
     false},
};

inline constexpr std::uint64_t kFingerprint =
    persist_fingerprint(persist_salt(kMarketStateSalt), kSchema);

// The seven extras columns, in section order (MarkExtrasColumns member order).
inline constexpr MarketStateSection kExtrasSections[] = {
    MarketStateSection::Bid,         MarketStateSection::Ask,
    MarketStateSection::VendorGamma, MarketStateSection::VendorVega,
    MarketStateSection::VendorTheta, MarketStateSection::VendorT,
    MarketStateSection::RowUprc,
};

} // namespace atx::vol::state_format
