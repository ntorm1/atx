#pragma once

// atx::engine::research::store -- compile-time table descriptors (ruling SQL-5,
// sql-design section 3.6). PRIVATE header: included only by the instantiating TUs
// (core_ops.cpp, cache_ops.cpp, SQL2's records_ops.cpp) and the research-store gtests.
//
// A row is a plain aggregate; nullability is the member type (std::optional<X> = NULL
// allowed, anything else NOT NULL), so an illegal NULL is unrepresentable. A descriptor is
//
//   inline constexpr auto kRunTable = table<RunRow>(
//       "run", TableOpts{.version = 1, .since = 1},
//       col<Sql::RelPath>("run_dir", &RunRow::run_dir, kKey),
//       col<Sql::Sha256>("argv_sha256", &RunRow::argv_sha256, kIndexed),
//       col<Sql::Real>("wall_seconds", &RunRow::wall_seconds, kVolatile));
//
// and detail/table_ops.hpp turns it into DDL, bind / read code, the record digest and the
// schema object. Everything here is checked at compile time: col() static_asserts the member
// type against the Sql type; table() rejects (as a constant-evaluation error naming the rule)
// 0 or more than 32 columns, no key column, a duplicate or non-identifier name, a nullable
// key, a later column (since > table since) that is not optional; col() also rejects an
// `allowed` list on a non-Text column or with a quote / non-ASCII value, and kKey | kVolatile.

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::research::store {

// The column kinds. Storage: Int / Bool / U64 INTEGER (U64 as its signed bit pattern,
// Bool 0 / 1), Real REAL, Text / Sha256 / RelPath / Json TEXT, Blob BLOB.
enum class Sql : u8 { Int, Bool, Real, U64, Text, Sha256, RelPath, Json, Blob };

// Column flags (a u8 bit set).
inline constexpr u8 kKey = 1U << 0U;      // primary-key member; the key is these, in order
inline constexpr u8 kIndexed = 1U << 1U;  // one single-column index ix_<table>_<column>
inline constexpr u8 kVolatile = 1U << 2U; // stored, never digested
inline constexpr u8 kAllFlags = kKey | kIndexed | kVolatile;

inline constexpr usize kMaxColumns = 32;

struct ColOpts {
  u8 flags{};
  i32 since{1};
  std::span<const std::string_view> allowed{};
};

struct TableOpts {
  i32 version{1};              // the table's own version (the digest's "row <t>@<version>")
  i32 since{1};                // the schema version that creates the table
  bool append_only{false};     // BEFORE UPDATE / DELETE triggers raise
  bool volatile_table{false};  // left out of the catalog digest
  std::string_view check{};    // optional table CHECK expression (", CHECK(<expr>)")
};

namespace detail {

template <class M> struct Unwrap {
  using type = M;
  static constexpr bool optional = false;
};
template <class X> struct Unwrap<std::optional<X>> {
  using type = X;
  static constexpr bool optional = true;
};

template <Sql T> struct SqlCpp;
template <> struct SqlCpp<Sql::Int> { using type = i64; };
template <> struct SqlCpp<Sql::Bool> { using type = bool; };
template <> struct SqlCpp<Sql::Real> { using type = f64; };
template <> struct SqlCpp<Sql::U64> { using type = u64; };
template <> struct SqlCpp<Sql::Text> { using type = std::string; };
template <> struct SqlCpp<Sql::Sha256> { using type = std::string; };
template <> struct SqlCpp<Sql::RelPath> { using type = std::string; };
template <> struct SqlCpp<Sql::Json> { using type = std::string; };
template <> struct SqlCpp<Sql::Blob> { using type = std::vector<std::byte>; };

// Reaching one of these during constant evaluation is the compile error; the function name
// is the rule that failed. Deliberately not constexpr.
inline void descriptor_error_name_not_lowercase_identifier() noexcept {}
inline void descriptor_error_bad_flags() noexcept {}
inline void descriptor_error_since_below_one() noexcept {}
inline void descriptor_error_allowed_value_has_quote_or_non_ascii() noexcept {}
inline void descriptor_error_allowed_on_non_text_column() noexcept {}
inline void descriptor_error_duplicate_column_name() noexcept {}
inline void descriptor_error_no_key_column() noexcept {}
inline void descriptor_error_key_column_is_optional() noexcept {}
inline void descriptor_error_later_column_not_optional() noexcept {}
inline void descriptor_error_version_or_since_below_one() noexcept {}

// [a-z_][a-z0-9_]* and at most 64 bytes: names are spliced into SQL unquoted.
[[nodiscard]] consteval bool is_identifier(std::string_view name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  for (usize i = 0; i < name.size(); ++i) {
    const char c = name[i];
    const bool lower = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    if (!(lower || c == '_' || (digit && i > 0))) {
      return false;
    }
  }
  return true;
}

} // namespace detail

// A member type a column of kind T may hold: exactly SqlCpp<T>, or std::optional of it.
template <class M, Sql T>
concept StorableAs =
    std::same_as<typename detail::Unwrap<M>::type, typename detail::SqlCpp<T>::type>;

// A row: a default-constructible aggregate (plain struct of the member types above).
template <class Row>
concept RowType = std::is_aggregate_v<Row> && std::default_initializable<Row>;

// Constrained here as well as in col(), so a Column built by hand cannot bypass the check.
template <class Row, class M, Sql T>
  requires StorableAs<M, T>
struct Column {
  using row_type = Row;
  using member_type = M;
  using value_type = typename detail::Unwrap<M>::type;
  static constexpr Sql sql = T;
  static constexpr bool nullable = detail::Unwrap<M>::optional;

  std::string_view name;
  M Row::*member;
  ColOpts opts;

  [[nodiscard]] constexpr bool has(u8 flag) const noexcept { return (opts.flags & flag) != 0; }
};

// The column factory. `name` is the SQL column name; `member` the row member it maps;
// `flags` a kKey / kIndexed / kVolatile set; `allowed` a span over a namespace-scope
// constexpr array of the permitted text values (CHECK(<c> IN (...))); `since` the schema
// version that adds the column (a column later than its table must be optional). Refused:
// `allowed` on a kind other than Text, an `allowed` value with a quote or a non-ASCII byte
// (the printed schema must stay ASCII), and kKey together with kVolatile (a key is digested).
// The return type is deduced so a type mismatch reports the static_assert message below.
template <Sql T, RowType Row, class M>
[[nodiscard]] consteval auto col(std::string_view name, M Row::*member, u8 flags = 0,
                                 std::span<const std::string_view> allowed = {}, i32 since = 1) {
  static_assert(StorableAs<M, T>,
                "store::col: the member type does not match the Sql type (Int i64, Bool bool, "
                "Real f64, U64 u64, Text/Sha256/RelPath/Json std::string, Blob "
                "std::vector<std::byte>, each optionally std::optional)");
  if (!detail::is_identifier(name)) {
    detail::descriptor_error_name_not_lowercase_identifier();
  }
  if ((flags & ~kAllFlags) != 0 || (flags & (kKey | kVolatile)) == (kKey | kVolatile)) {
    detail::descriptor_error_bad_flags();
  }
  if (since < 1) {
    detail::descriptor_error_since_below_one();
  }
  if (!allowed.empty() && T != Sql::Text) {
    detail::descriptor_error_allowed_on_non_text_column();
  }
  for (const std::string_view value : allowed) {
    for (const char c : value) {
      if (c == '\'' || static_cast<unsigned char>(c) > 0x7FU) {
        detail::descriptor_error_allowed_value_has_quote_or_non_ascii();
      }
    }
  }
  return Column<Row, M, T>{name, member, ColOpts{flags, since, allowed}};
}

template <class Row, class... Cols> struct Table {
  using row_type = Row;
  static constexpr usize column_count = sizeof...(Cols);

  std::string_view name;
  TableOpts opts;
  std::tuple<Cols...> cols;
};

// The table factory: `name`, its options and its columns in declared order (the order of
// the CREATE TABLE, of bind parameters, of result columns and of the digest).
template <RowType Row, class... Cols>
[[nodiscard]] consteval Table<Row, Cols...> table(std::string_view name, TableOpts opts,
                                                  Cols... cols) {
  static_assert(sizeof...(Cols) >= 1 && sizeof...(Cols) <= kMaxColumns,
                "store::table: a table has 1 to 32 columns");
  static_assert((std::same_as<typename Cols::row_type, Row> && ...),
                "store::table: every column must map a member of this table's row type");
  constexpr usize n = sizeof...(Cols);
  const std::array<std::string_view, n> names{cols.name...};
  const std::array<u8, n> flags{cols.opts.flags...};
  const std::array<i32, n> since{cols.opts.since...};
  constexpr std::array<bool, n> nullable{Cols::nullable...};
  if (!detail::is_identifier(name)) {
    detail::descriptor_error_name_not_lowercase_identifier();
  }
  if (opts.version < 1 || opts.since < 1) {
    detail::descriptor_error_version_or_since_below_one();
  }
  bool any_key = false;
  for (usize i = 0; i < n; ++i) {
    for (usize j = i + 1; j < n; ++j) {
      if (names[i] == names[j]) {
        detail::descriptor_error_duplicate_column_name();
      }
    }
    const bool key = (flags[i] & kKey) != 0;
    any_key = any_key || key;
    if (key && nullable[i]) {
      detail::descriptor_error_key_column_is_optional();
    }
    if (since[i] > opts.since && !nullable[i]) {
      detail::descriptor_error_later_column_not_optional();
    }
  }
  if (!any_key) {
    detail::descriptor_error_no_key_column();
  }
  return Table<Row, Cols...>{name, opts, std::tuple<Cols...>{cols...}};
}

// Any instantiation of Table.
template <class T> inline constexpr bool kIsTable = false;
template <class Row, class... Cols> inline constexpr bool kIsTable<Table<Row, Cols...>> = true;
template <class T>
concept TableDescriptor = kIsTable<std::remove_cvref_t<T>>;

} // namespace atx::engine::research::store
