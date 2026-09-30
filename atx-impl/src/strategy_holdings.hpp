#pragma once

// PRIVATE to atx-impl (v7 W4, finding L3-F1). The compact binary holdings layout of
// `nav --emit-holdings` (the default --holdings-format f64) and its reader, used by the
// daily decide path. The v1 holdings.csv (--holdings-format csv) stays readable by
// strategy_live.cpp's CSV reader.
//
// Directory (manifest.json atx.nav-holdings/v2 LAST, as v1):
//   holdings.f64        little-endian IEEE-754 binary64, row-major, row_width values per
//                       row, the observed book's names of one session per contiguous run
//                       of rows, sessions ascending, names ascending by role index;
//   holdings_index.json atx.nav-holdings-f64/v1: the column names, the flag layout, the
//                       role's instrument ids (a row's `name` is an index into them), the
//                       session table [session_index, session_ns, nav_post_bits,
//                       decision, first_row, rows] (nav_post as its exact u64 bits) and
//                       holdings.f64's SHA-256, byte and row counts;
//   holdings_days.csv   unchanged from v1.
// Every NavHolding field is recoverable bit for bit: held_weight = held_dollars /
// nav_post (the replay's own expression), side = sign(held_dollars), planned_trade_weight
// and planned_trade_dollars as the v1 CSV declares them.
// v8 E-16: a hold-band book (b > 0 declared) writes hold_row_width values per row, the
// row_width above then rank_set and desired_prev (NavHolding's hold-band state, NaN =
// unset); the index names the columns, so the reader takes either layout. Without a declared
// band every byte is the layout above.

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "strategy_nav_replay.hpp"

namespace atx::impl::strategy::holdings {
inline constexpr const char* manifest_schema_v2 = "atx.nav-holdings/v2";
inline constexpr const char* index_schema = "atx.nav-holdings-f64/v1";
inline constexpr const char* data_file = "holdings.f64";
inline constexpr const char* index_file = "holdings_index.json";
inline constexpr const char* days_file = "holdings_days.csv";
inline constexpr const char* manifest_file = "manifest.json";

inline constexpr atx::usize row_width = 11;
// Column positions in a row (every value an f64; the first three exact integers).
inline constexpr atx::usize col_session = 0; // ordinal into the index's session table
inline constexpr atx::usize col_name = 1;    // index into the index's instrument ids
inline constexpr atx::usize col_flags = 2;   // bit-packed, below
inline constexpr atx::usize col_held = 3, col_filled = 4, col_fill_cost = 5, col_unfilled = 6;
inline constexpr atx::usize col_desired = 7, col_rule = 8, col_target = 9, col_order = 10;
inline constexpr std::array<const char*, row_width> column_names{
    "session", "name", "flags", "held_dollars", "filled_dollars", "fill_cost_dollars",
    "unfilled_dollars", "desired_weight", "rule_weight", "target_weight", "order_dollars"};
// flags = member | stale << 1 | order_placed << 2 | locate_blocked << 3 |
//         order_working << 4 | tier << 8 | tier_missing << 16 | fill << 24
inline constexpr atx::u32 flag_member = 1U, flag_stale = 2U, flag_order_placed = 4U;
inline constexpr atx::u32 flag_locate_blocked = 8U, flag_order_working = 16U;
inline constexpr atx::u32 tier_shift = 8U, tier_missing_shift = 16U, fill_shift = 24U;
inline constexpr atx::u32 max_tier = 3U, max_fill = 4U;
// v8 E-16: the hold-band state columns a hold-band book appends to every row.
inline constexpr atx::usize hold_row_width = row_width + 2;
inline constexpr atx::usize col_rank_set = row_width, col_desired_prev = row_width + 1;
inline constexpr std::array<const char*, 2> hold_column_names{"rank_set", "desired_prev"};

using Row = std::array<atx::f64, row_width>;
using HoldRow = std::array<atx::f64, hold_row_width>;
// `session` is the row's ordinal in the session table; h.index its role index.
[[nodiscard]] Row pack(atx::usize session, const NavHolding& h) noexcept;
// pack(), then h.rank_set and h.desired_prev (a hold-band book's row).
[[nodiscard]] HoldRow pack_hold(atx::usize session, const NavHolding& h) noexcept;

// One entry of the session table.
struct SessionEntry {
  atx::usize session_index{}; // role row t
  atx::i64 session_ns{};
  atx::f64 nav_post{};        // the book's post-trade NAV of t (exact bits in the index)
  bool decision{};
  atx::u64 first_row{}, rows{};
};

// Buffered writer of holdings.f64 (a fixed 1 MiB buffer; no other allocation) with the
// file's SHA-256 accumulated as it is written, so the file is never re-read. Every row has
// the `width` given to open (row_width, or hold_row_width for a hold-band book); a row of
// another width is an Internal error.
class BinaryAppender {
public:
  [[nodiscard]] atx::core::Status open(const std::filesystem::path& path,
                                       atx::usize width = row_width);
  [[nodiscard]] atx::core::Status append(std::span<const atx::f64> row);
  struct Closed {
    std::string sha256;
    atx::u64 bytes{}, rows{};
  };
  // Flushes, closes and returns the digest; the appender cannot be reused.
  [[nodiscard]] atx::core::Result<Closed> close();

private:
  [[nodiscard]] atx::core::Status flush();
  std::vector<atx::f64> buffer_;
  std::ofstream file_;
  atx::core::Sha256 sha_;
  atx::u64 rows_{};
  atx::usize width_{row_width};
};

// Writes holdings_index.json and returns its SHA-256. `ids` is the role's instrument
// order; `sessions` must tile [0, data.rows) in order. hold_state: the rows are
// hold_row_width wide (the index names the two state columns and declares them); false
// writes the row_width index byte for byte as before v8.
[[nodiscard]] atx::core::Result<std::string> write_index(
    const std::filesystem::path& path, std::span<const atx::u64> ids,
    std::span<const SessionEntry> sessions, const BinaryAppender::Closed& data,
    bool hold_state = false);

// One session of an f64 holdings layout. `path` is the emitted holdings directory
// (manifest.json atx.nav-holdings/v2 with status complete, binding the index SHA) or its
// holdings_index.json. holdings.f64 must match the index's size and SHA-256 and the
// session table must tile it; the session must be in the table (a session the replay did
// not report is refused, never read as a flat book). Rows come back in file order with
// every NavHolding field (index = the name's position in `ids`); rank_set and desired_prev
// only from a hold-band layout (hold_state), NaN otherwise.
struct SessionRead {
  SessionEntry session;
  std::vector<atx::u64> ids;      // the index's instrument ids
  std::vector<NavHolding> names;
  bool hold_state{};              // v8 E-16: the rows carry the hold-band state columns
};
[[nodiscard]] atx::core::Result<SessionRead> read_session(const std::string& path,
                                                          atx::i64 session_ns);
// True for a directory or a *.json path (the f64 layout); false for a CSV.
[[nodiscard]] bool is_f64_layout(const std::string& path);
} // namespace atx::impl::strategy::holdings
