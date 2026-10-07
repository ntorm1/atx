// The short-side stage reader: ftd/, regsho_threshold/ and security_master/ (P9 lane A4).
// Declared in short_side.hpp.
#include "atx/engine/research/fields/sources/short_side.hpp"

#include <array>
#include <span>
#include <string>
#include <utility>

#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/sources/parquet_columns.hpp"

namespace atx::engine::research::fields {
namespace {

using core::io::ParquetTable;

// How one layout column decodes (parquet_columns.hpp) and which Rows member it fills: `ints` for
// Id / Day / Instant / Code, `reals` for Real, `flags` for Flag (the others null).
enum class Decode : u8 { Id, Day, Instant, Real, Flag, Code };

template <class Rows> struct Column {
  std::string_view name;
  Decode decode{};
  std::vector<i64> Rows::*ints = nullptr;
  std::vector<f64> Rows::*reals = nullptr;
  std::vector<u8> Rows::*flags = nullptr;
  std::span<const std::string_view> codes{}; // Code: the value list
};

// The column constructors of the layouts below (one per member type).
template <class Rows>
constexpr Column<Rows> int_col(std::string_view name, Decode decode, std::vector<i64> Rows::*m) {
  Column<Rows> c;
  c.name = name;
  c.decode = decode;
  c.ints = m;
  return c;
}
template <class Rows>
constexpr Column<Rows> code_col(std::string_view name, std::vector<i64> Rows::*m,
                                std::span<const std::string_view> codes) {
  Column<Rows> c = int_col<Rows>(name, Decode::Code, m);
  c.codes = codes;
  return c;
}
template <class Rows>
constexpr Column<Rows> real_col(std::string_view name, std::vector<f64> Rows::*m) {
  Column<Rows> c;
  c.name = name;
  c.decode = Decode::Real;
  c.reals = m;
  return c;
}
template <class Rows>
constexpr Column<Rows> flag_col(std::string_view name, std::vector<u8> Rows::*m) {
  Column<Rows> c;
  c.name = name;
  c.decode = Decode::Flag;
  c.flags = m;
  return c;
}

// The column layout of each Rows type: the stage columns it reads, in read order.
template <class Rows> struct Layout;

template <> struct Layout<FtdRows> {
  using R = FtdRows;
  static constexpr std::array<Column<R>, 5> columns{
      int_col<R>("settlement_date", Decode::Day, &R::settlement_day),
      int_col<R>("security_id", Decode::Id, &R::security_id),
      real_col<R>("quantity", &R::quantity),
      int_col<R>("available_at", Decode::Instant, &R::available_ns),
      code_col<R>("map_basis", &R::basis, kFtdBases)};
};

template <> struct Layout<RegshoListRows> {
  using R = RegshoListRows;
  static constexpr std::array<Column<R>, 4> columns{
      code_col<R>("market", &R::market, kRegshoMarkets),
      int_col<R>("list_date", Decode::Day, &R::list_day),
      code_col<R>("status", &R::status, kRegshoListStatuses),
      int_col<R>("available_at", Decode::Instant, &R::available_ns)};
};

template <> struct Layout<ThresholdRows> {
  using R = ThresholdRows;
  static constexpr std::array<Column<R>, 5> columns{
      int_col<R>("list_date", Decode::Day, &R::list_day),
      code_col<R>("market", &R::market, kRegshoMarkets),
      int_col<R>("security_id", Decode::Id, &R::security_id),
      flag_col<R>("on_list", &R::on_list),
      int_col<R>("available_at", Decode::Instant, &R::available_ns)};
};

template <> struct Layout<FinraNameRows> {
  using R = FinraNameRows;
  static constexpr std::array<Column<R>, 4> columns{
      int_col<R>("security_id", Decode::Id, &R::security_id),
      int_col<R>("available_at", Decode::Instant, &R::available_ns),
      int_col<R>("dissemination_date", Decode::Day, &R::dissemination_day),
      code_col<R>("market_class", &R::market_class, kFinraMarketClasses)};
};

// Every column fills exactly the member its decoding produces (checked at compile time).
template <class Rows> consteval bool well_formed() {
  for (const Column<Rows> &c : Layout<Rows>::columns) {
    const bool ints = c.ints != nullptr;
    const bool reals = c.reals != nullptr;
    const bool flags = c.flags != nullptr;
    switch (c.decode) {
    case Decode::Id:
    case Decode::Day:
    case Decode::Instant:
      if (!ints || reals || flags || !c.codes.empty()) {
        return false;
      }
      break;
    case Decode::Code:
      if (!ints || reals || flags || c.codes.empty()) {
        return false;
      }
      break;
    case Decode::Real:
      if (ints || !reals || flags) {
        return false;
      }
      break;
    case Decode::Flag:
      if (ints || reals || !flags) {
        return false;
      }
      break;
    }
  }
  return true;
}
static_assert(well_formed<FtdRows>(), "FtdRows layout");
static_assert(well_formed<RegshoListRows>(), "RegshoListRows layout");
static_assert(well_formed<ThresholdRows>(), "ThresholdRows layout");
static_assert(well_formed<FinraNameRows>(), "FinraNameRows layout");

template <class Rows> constexpr auto column_names() {
  std::array<std::string_view, Layout<Rows>::columns.size()> out{};
  for (usize i = 0; i < out.size(); ++i) {
    out[i] = Layout<Rows>::columns[i].name;
  }
  return out;
}

// One layout column of a decoded group into its Rows member (the member replaced).
template <class Rows>
[[nodiscard]] core::Status decode_column(const ParquetTable &table, const Column<Rows> &c,
                                         Rows &rows) {
  switch (c.decode) {
  case Decode::Id: {
    ATX_TRY(rows.*(c.ints), id_column(table, c.name));
    return core::Ok();
  }
  case Decode::Day: {
    ATX_TRY(rows.*(c.ints), day_column(table, c.name));
    return core::Ok();
  }
  case Decode::Instant: {
    ATX_TRY(rows.*(c.ints), instant_column(table, c.name));
    return core::Ok();
  }
  case Decode::Code: {
    ATX_TRY(rows.*(c.ints), code_column(table, c.name, c.codes));
    return core::Ok();
  }
  case Decode::Real: {
    ATX_TRY(rows.*(c.reals), real_column(table, c.name));
    return core::Ok();
  }
  case Decode::Flag: {
    ATX_TRY(rows.*(c.flags), flag_column(table, c.name));
    return core::Ok();
  }
  }
  return core::Err(core::ErrorCode::Internal, "short-side reader: unknown column decoding");
}

template <class Rows> [[nodiscard]] usize member_size(const Rows &rows, const Column<Rows> &c) {
  if (c.reals != nullptr) {
    return (rows.*(c.reals)).size();
  }
  if (c.flags != nullptr) {
    return (rows.*(c.flags)).size();
  }
  return (rows.*(c.ints)).size();
}

// The decoded group, every member checked to one length.
template <class Rows>
[[nodiscard]] core::Status decode_group(const ParquetTable &table, std::string_view rel,
                                        Rows &rows) {
  for (const Column<Rows> &c : Layout<Rows>::columns) {
    ATX_TRY_VOID(decode_column(table, c, rows));
  }
  for (const Column<Rows> &c : Layout<Rows>::columns) {
    if (member_size(rows, c) != rows.size()) {
      return core::Err(core::ErrorCode::Internal,
                       std::string(rel) + ": decoded columns differ in length");
    }
  }
  return core::Ok();
}

template <class T> void append(std::vector<T> &to, const std::vector<T> &from) {
  to.insert(to.end(), from.begin(), from.end());
}

} // namespace

std::vector<std::string> stage_year_files(const PinnedStage &stage, std::string_view name, i64 lo,
                                          i64 hi) {
  std::vector<std::string> out;
  for (i64 year = lo; year <= hi; ++year) {
    const std::string rel = "year=" + std::to_string(year) + "/" + std::string(name);
    if (stage.lists(rel) && !is_sealed_day(days_from_civil(year, 1, 1))) {
      out.push_back(rel);
    }
  }
  return out;
}

template <class Rows>
core::Status read_short_side(PinnedStage &stage, std::string_view rel,
                             const RowsVisitor<Rows> &visit) {
  static constexpr auto kNames = column_names<Rows>();
  Rows rows; // one group's batch: every member replaced per group
  return stage.for_each_group(rel, kNames, [&](const ParquetTable &group) -> core::Status {
    ATX_TRY_VOID(decode_group(group, rel, rows));
    return visit(rows);
  });
}

template <class Rows>
core::Result<Rows> read_short_side_all(PinnedStage &stage, std::string_view rel) {
  Rows all;
  const RowsVisitor<Rows> gather = [&all](const Rows &rows) -> core::Status {
    for (const Column<Rows> &c : Layout<Rows>::columns) {
      if (c.reals != nullptr) {
        append(all.*(c.reals), rows.*(c.reals));
      } else if (c.flags != nullptr) {
        append(all.*(c.flags), rows.*(c.flags));
      } else {
        append(all.*(c.ints), rows.*(c.ints));
      }
    }
    return core::Ok();
  };
  ATX_TRY_VOID(read_short_side<Rows>(stage, rel, gather));
  return core::Ok(std::move(all));
}

// The Rows types of short_side.hpp.
template core::Status read_short_side<FtdRows>(PinnedStage &, std::string_view,
                                               const RowsVisitor<FtdRows> &);
template core::Status read_short_side<RegshoListRows>(PinnedStage &, std::string_view,
                                                      const RowsVisitor<RegshoListRows> &);
template core::Status read_short_side<ThresholdRows>(PinnedStage &, std::string_view,
                                                     const RowsVisitor<ThresholdRows> &);
template core::Status read_short_side<FinraNameRows>(PinnedStage &, std::string_view,
                                                     const RowsVisitor<FinraNameRows> &);
template core::Result<FtdRows> read_short_side_all<FtdRows>(PinnedStage &, std::string_view);
template core::Result<RegshoListRows> read_short_side_all<RegshoListRows>(PinnedStage &,
                                                                          std::string_view);
template core::Result<ThresholdRows> read_short_side_all<ThresholdRows>(PinnedStage &,
                                                                        std::string_view);
template core::Result<FinraNameRows> read_short_side_all<FinraNameRows>(PinnedStage &,
                                                                        std::string_view);

} // namespace atx::engine::research::fields
