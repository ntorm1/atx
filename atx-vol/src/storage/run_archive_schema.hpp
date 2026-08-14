#pragma once

// RunArchive (ATXRUN01) column registry — the single source of truth for every
// result section a backtest run can emit. Modeled on the ATXVSA2 surface
// archive's schema discipline (surface_archive.hpp): the writer, the reader and
// the Python bindings all consume THIS registry, and `ra_schema_hash()` folds
// it into one constexpr value the file header pins, so any column add / rename /
// reorder / dtype change is a new schema hash (never a silent drift).
//
// Column provenance (do not invent columns — each section mirrors the TSV
// writer that owns the output today):
//   backtest        <- append_backtest_series_tsv (src/tearsheet.cpp) over
//                      BacktestResult: date, ts_ns, then 25 F64 series. The
//                      per-signal columns (BacktestResult::signals) are appended
//                      DYNAMICALLY at write time and are not in this registry.
//   projected_cold / projected_nodiv reuse the backtest column set.
//   reconciliation  <- serialize_listed_reconciliation
//                      (src/listed_dispersion_reconciliation.cpp).
//   trade_schedule / projected_schedule
//                   <- serialize_listed_dispersion_schedule kHeader
//                      (src/listed_dispersion_schedule.cpp).
//   contract_marks  <- serialize_listed_contract_marks
//                      (src/listed_dispersion_reconciliation.cpp).
//   mark_divergence <- write_mark_divergence_replay header
//                      (tools/spy_dispersion_backtest.cpp).
//   diagnostics     <- write_diagnostics header (tools).
//   meta            <- ScalarKV: resolved spec echo, window, roll-level
//                      scalars, input hashes, counts — key/value pairs.
//
// Integer dtypes are the container's, not C++'s: there is deliberately no U64
// dtype, so u64 source fields (fingerprints) are stored as I64 bit patterns.

#include <cstdint>
#include <span>
#include <string_view>

namespace atx::vol {

// ── Format identity ──────────────────────────────────────────────────────────

inline constexpr char kRaMagic[8] = {'A', 'T', 'X', 'R', 'U', 'N', '0', '1'};
inline constexpr std::uint16_t kRaMajor = 1;
inline constexpr std::uint16_t kRaMinor = 0;

// Salt for ra_schema_hash(): "ATXRUN01" as a big-endian u64. Changing the salt
// (like changing the registry) invalidates every existing archive on purpose.
inline constexpr std::uint64_t kRaSchemaSalt = 0x41545852554E3031ull;

// ── Registry vocabulary ──────────────────────────────────────────────────────

enum class RaDType : std::uint8_t {
  F64 = 0,     // double
  I64 = 1,     // int64 (also carries u64 sources as bit patterns)
  U32 = 2,     // uint32
  U8Enum = 3,  // uint8 code + label table (bools, Side, roles, statuses)
  DictStr = 4, // uint32 code + string table
};

enum class RaSectionKind : std::uint8_t {
  ScalarKV = 0,   // one row per key: (key, value)
  TimeSeries = 1, // one row per recorded clock step
  SubTable = 2,   // many rows per step/roll (legs, marks, phases)
};

struct RaColumn {
  std::string_view name;
  RaDType dtype;
  std::string_view unit; // informational; "" where the convention is not pinned
};

struct RaSection {
  std::string_view name;
  RaSectionKind kind;
  std::span<const RaColumn> columns;
};

// ── Column sets ──────────────────────────────────────────────────────────────

// meta: run-level scalars as key/value pairs (values serialized as strings;
// doubles %.17g, hashes hex) — the ScalarKV shape shared by every run.
inline constexpr RaColumn kMetaCols[] = {
    {"key", RaDType::DictStr, ""},
    {"value", RaDType::DictStr, ""},
};

// backtest: EXACTLY the append_backtest_series_tsv order (tearsheet.cpp:190-216)
// — date + ts_ns + 25 F64. This order is load-bearing: `nav` is column 16.
inline constexpr RaColumn kBacktestCols[] = {
    {"date", RaDType::DictStr, ""},
    {"ts_ns", RaDType::I64, "ns"},
    {"pnl_total", RaDType::F64, "usd"},
    {"pnl_delta", RaDType::F64, "usd"},
    {"pnl_gamma", RaDType::F64, "usd"},
    {"pnl_vega", RaDType::F64, "usd"},
    {"pnl_vanna", RaDType::F64, "usd"},
    {"pnl_volga", RaDType::F64, "usd"},
    {"pnl_theta", RaDType::F64, "usd"},
    {"pnl_rho", RaDType::F64, "usd"},
    {"pnl_charm", RaDType::F64, "usd"},
    {"pnl_unexplained", RaDType::F64, "usd"},
    {"pnl_settlement", RaDType::F64, "usd"},
    {"pnl_shares", RaDType::F64, "usd"},
    {"financing", RaDType::F64, "usd"},
    {"cost", RaDType::F64, "usd"},
    {"nav", RaDType::F64, "usd"},
    {"cash", RaDType::F64, "usd"},
    {"gross_delta", RaDType::F64, ""},
    {"gross_gamma", RaDType::F64, ""},
    {"gross_vega", RaDType::F64, ""},
    {"gross_theta", RaDType::F64, ""},
    {"turnover_notional", RaDType::F64, "usd"},
    {"turnover_vega", RaDType::F64, ""},
    {"n_open_lots", RaDType::F64, "count"},
    {"n_unpriced_lots", RaDType::F64, "count"},
    {"n_unpriced_greeks", RaDType::F64, "count"},
};

// reconciliation: serialize_listed_reconciliation column order.
inline constexpr RaColumn kReconciliationCols[] = {
    {"date", RaDType::DictStr, ""},
    {"valuation_ts_ns", RaDType::I64, "ns"},
    {"held_cohort", RaDType::U32, ""},
    {"model_option_pnl", RaDType::F64, "usd"},
    {"quote_mid_pnl", RaDType::F64, "usd"},
    {"model_minus_quote_pnl", RaDType::F64, "usd"},
    {"model_nav", RaDType::F64, "usd"},
    {"quote_mid_nav", RaDType::F64, "usd"},
    {"quote_mid_coverage", RaDType::F64, ""},
    {"n_held_lots", RaDType::U32, "count"},
    {"n_quote_mid_lots", RaDType::U32, "count"},
};

// trade_schedule / projected_schedule: serialize_listed_dispersion_schedule
// kHeader column order (roll fields repeated per leg row).
inline constexpr RaColumn kScheduleCols[] = {
    {"roll_date", RaDType::DictStr, ""},
    {"valuation_ts_ns", RaDType::I64, "ns"},
    {"cohort", RaDType::U32, ""},
    {"expiry_ts_ns", RaDType::I64, "ns"},
    {"gross_index_vega_target", RaDType::F64, "usd_per_volpt"},
    {"net_vega", RaDType::F64, "usd_per_volpt"},
    {"gross_vega", RaDType::F64, "usd_per_volpt"},
    {"n_names", RaDType::U32, "count"},
    {"is_index", RaDType::U8Enum, ""},
    {"symbol", RaDType::DictStr, ""},
    {"uid", RaDType::U32, ""},
    {"instrument_id", RaDType::U32, ""},
    {"raw_symbol", RaDType::DictStr, ""},
    {"strike", RaDType::F64, "usd"},
    {"side", RaDType::U8Enum, ""},
    {"quantity", RaDType::F64, "contracts"},
    {"multiplier", RaDType::F64, ""},
    {"raw_bid", RaDType::F64, "usd"},
    {"raw_ask", RaDType::F64, "usd"},
    {"raw_mid", RaDType::F64, "usd"},
    {"model_mark", RaDType::F64, "usd"},
    {"delta_per_share", RaDType::F64, ""},
    {"vega_per_unit_vol", RaDType::F64, "usd_per_unitvol"},
    {"vega_per_contract_per_vol_point", RaDType::F64, "usd_per_volpt"},
    {"normalized_weight", RaDType::F64, ""},
    {"target_straddle_vega", RaDType::F64, "usd_per_volpt"},
    {"achieved_leg_vega", RaDType::F64, "usd_per_volpt"},
    {"source_fingerprint", RaDType::I64, ""},
    {"surface_fingerprint", RaDType::I64, ""},
};

// contract_marks: serialize_listed_contract_marks column order.
inline constexpr RaColumn kContractMarksCols[] = {
    {"date", RaDType::DictStr, ""},
    {"valuation_ts_ns", RaDType::I64, "ns"},
    {"role", RaDType::U8Enum, ""},
    {"cohort", RaDType::U32, ""},
    {"symbol", RaDType::DictStr, ""},
    {"uid", RaDType::U32, ""},
    {"instrument_id", RaDType::U32, ""},
    {"raw_symbol", RaDType::DictStr, ""},
    {"expiry_ts_ns", RaDType::I64, "ns"},
    {"strike", RaDType::F64, "usd"},
    {"side", RaDType::U8Enum, ""},
    {"quantity", RaDType::F64, "contracts"},
    {"multiplier", RaDType::F64, ""},
    {"status", RaDType::U8Enum, ""},
    {"raw_bid", RaDType::F64, "usd"},
    {"raw_ask", RaDType::F64, "usd"},
    {"raw_mid", RaDType::F64, "usd"},
    {"model_mark", RaDType::F64, "usd"},
    {"model_in_spread", RaDType::U8Enum, ""},
};

// mark_divergence: write_mark_divergence_replay header column order.
inline constexpr RaColumn kMarkDivergenceCols[] = {
    {"date", RaDType::DictStr, ""},
    {"symbol", RaDType::DictStr, ""},
    {"raw_symbol", RaDType::DictStr, ""},
    {"strike", RaDType::F64, "usd"},
    {"expiry_ts_ns", RaDType::I64, "ns"},
    {"side", RaDType::U8Enum, ""},
    {"schedule_mark", RaDType::F64, "usd"},
    {"live_mark", RaDType::F64, "usd"},
    {"diff", RaDType::F64, "usd"},
    {"abs_diff_bps_of_mark", RaDType::F64, "bps"},
};

// diagnostics: write_diagnostics header column order.
inline constexpr RaColumn kDiagnosticsCols[] = {
    {"subcommand", RaDType::DictStr, ""},
    {"phase", RaDType::DictStr, ""},
    {"wall_ms", RaDType::F64, "ms"},
    {"count", RaDType::I64, "count"},
};

// ── Registry ─────────────────────────────────────────────────────────────────

// projected_cold / projected_nodiv / projected_schedule alias the arrays of the
// section they mirror, so the pairs can never drift.
inline constexpr RaSection kRaSections[] = {
    {"meta", RaSectionKind::ScalarKV, kMetaCols},
    {"backtest", RaSectionKind::TimeSeries, kBacktestCols},
    {"projected_cold", RaSectionKind::TimeSeries, kBacktestCols},
    {"projected_nodiv", RaSectionKind::TimeSeries, kBacktestCols},
    {"reconciliation", RaSectionKind::TimeSeries, kReconciliationCols},
    {"trade_schedule", RaSectionKind::SubTable, kScheduleCols},
    {"projected_schedule", RaSectionKind::SubTable, kScheduleCols},
    {"contract_marks", RaSectionKind::SubTable, kContractMarksCols},
    {"mark_divergence", RaSectionKind::SubTable, kMarkDivergenceCols},
    {"diagnostics", RaSectionKind::SubTable, kDiagnosticsCols},
};

[[nodiscard]] constexpr std::span<const RaSection> ra_sections() noexcept {
  return kRaSections;
}

// ── Schema hash ──────────────────────────────────────────────────────────────

namespace ra_detail {

inline constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

[[nodiscard]] constexpr std::uint64_t fnv1a_byte(std::uint64_t h, std::uint8_t b) noexcept {
  return (h ^ b) * kFnvPrime;
}

[[nodiscard]] constexpr std::uint64_t fnv1a_bytes(std::uint64_t h, std::string_view s) noexcept {
  for (char c : s) h = fnv1a_byte(h, static_cast<std::uint8_t>(c));
  return h;
}

[[nodiscard]] constexpr std::uint64_t fnv1a_u64(std::uint64_t h, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) h = fnv1a_byte(h, static_cast<std::uint8_t>(v >> (8 * i)));
  return h;
}

} // namespace ra_detail

// FNV-1a-64 fold over the whole registry — every section (name, kind) and every
// column (name, dtype, unit) — salted with kRaSchemaSalt. ASCII field/record/
// group separators (0x1F/0x1E/0x1D) delimit fields so no concatenation of two
// different registries can collide by juxtaposition. Pure constexpr: usable in
// static_assert, and the writer stamps it into RunArchiveHeader::schema_hash.
[[nodiscard]] constexpr std::uint64_t ra_schema_hash() noexcept {
  std::uint64_t h = ra_detail::kFnvOffset;
  h = ra_detail::fnv1a_u64(h, kRaSchemaSalt);
  for (const RaSection &section : ra_sections()) {
    h = ra_detail::fnv1a_bytes(h, section.name);
    h = ra_detail::fnv1a_byte(h, 0x1F);
    h = ra_detail::fnv1a_byte(h, static_cast<std::uint8_t>(section.kind));
    for (const RaColumn &column : section.columns) {
      h = ra_detail::fnv1a_bytes(h, column.name);
      h = ra_detail::fnv1a_byte(h, 0x1F);
      h = ra_detail::fnv1a_byte(h, static_cast<std::uint8_t>(column.dtype));
      h = ra_detail::fnv1a_bytes(h, column.unit);
      h = ra_detail::fnv1a_byte(h, 0x1E);
    }
    h = ra_detail::fnv1a_byte(h, 0x1D);
  }
  return h;
}

} // namespace atx::vol
