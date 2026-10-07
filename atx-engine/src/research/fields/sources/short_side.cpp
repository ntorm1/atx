// The short-side stage readers: ftd/, regsho_threshold/ and security_master/ (P9 lane A4).
// Declared in short_side.hpp.
#include "atx/engine/research/fields/sources/short_side.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <utility>

#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/sources/parquet_columns.hpp"

namespace atx::engine::research::fields {
namespace {

using core::io::ParquetTable;

// A BOOL column as 0 / 1; null -> 0 (pc.fill_null(on_list, False)).
[[nodiscard]] core::Result<std::vector<u8>> flag_column(const ParquetTable &table,
                                                        std::string_view name) {
  const core::io::ColumnInfo *info = table.schema().find(name);
  if (info == nullptr || info->dtype != core::io::DType::Bool) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "stage column " + std::string(name) + " is absent or not BOOL");
  }
  ATX_TRY(const auto column, table.to_column<bool>(name));
  ATX_TRY(const auto nulls, table.null_mask(name));
  const auto values = column.view();
  if (!nulls.empty() && nulls.size() != values.size()) {
    return core::Err(core::ErrorCode::Internal,
                     "stage column " + std::string(name) + ": null mask length differs");
  }
  std::vector<u8> out(values.size(), 0);
  for (usize i = 0; i < values.size(); ++i) {
    const bool null = !nulls.empty() && nulls[i] != 0;
    out[i] = static_cast<u8>((!null && values[i]) ? 1 : 0);
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] std::vector<i64> market_codes(const std::vector<std::string> &text) {
  std::vector<i64> out;
  out.reserve(text.size());
  for (const std::string &m : text) {
    out.push_back(code_of(m, kRegshoMarkets));
  }
  return out;
}

[[nodiscard]] core::Status same_length(usize rows, std::initializer_list<usize> sizes,
                                       std::string_view what) {
  for (const usize s : sizes) {
    if (s != rows) {
      return core::Err(core::ErrorCode::Internal,
                       std::string(what) + ": decoded columns differ in length");
    }
  }
  return core::Ok();
}

} // namespace

i64 code_of(std::string_view text, std::span<const std::string_view> values) noexcept {
  const auto it = std::find(values.begin(), values.end(), text);
  return it == values.end() ? -1 : static_cast<i64>(it - values.begin());
}

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

core::Result<FtdRows> read_ftd_rows(PinnedStage &stage, std::string_view rel) {
  static constexpr std::array<std::string_view, 5> kColumns{
      "settlement_date", "security_id", "quantity", "available_at", "map_basis"};
  ATX_TRY(const auto table, stage.table(rel, kColumns));
  FtdRows out;
  ATX_TRY(out.settlement_day, day_column(table, "settlement_date"));
  ATX_TRY(out.security_id, id_column(table, "security_id"));
  ATX_TRY(out.quantity, real_column(table, "quantity"));
  ATX_TRY(out.available_ns, instant_column(table, "available_at"));
  ATX_TRY(const auto basis, text_column(table, "map_basis"));
  ATX_TRY(const auto nulls, table.null_mask("map_basis"));
  out.collision.assign(basis.size(), 0);
  for (usize i = 0; i < basis.size(); ++i) { // a null basis is not a collision (index_in -> -1)
    const bool null = i < nulls.size() && nulls[i] != 0;
    out.collision[i] = static_cast<u8>((!null && basis[i] == "collision") ? 1 : 0);
  }
  ATX_TRY_VOID(same_length(out.settlement_day.size(),
                           {out.security_id.size(), out.quantity.size(), out.available_ns.size(),
                            out.collision.size()},
                           "ftd"));
  return core::Ok(std::move(out));
}

core::Result<RegshoListRows> read_regsho_lists(PinnedStage &stage) {
  static constexpr std::array<std::string_view, 4> kColumns{"market", "list_date", "status",
                                                            "available_at"};
  ATX_TRY(const auto table, stage.table("lists.parquet", kColumns));
  RegshoListRows out;
  ATX_TRY(const auto market, text_column(table, "market"));
  out.market = market_codes(market);
  ATX_TRY(out.list_day, day_column(table, "list_date"));
  ATX_TRY(out.status, text_column(table, "status"));
  ATX_TRY(out.available_ns, instant_column(table, "available_at"));
  ATX_TRY_VOID(same_length(out.market.size(),
                           {out.list_day.size(), out.status.size(), out.available_ns.size()},
                           "regsho lists"));
  return core::Ok(std::move(out));
}

core::Result<ThresholdRows> read_threshold_rows(PinnedStage &stage, std::string_view rel) {
  static constexpr std::array<std::string_view, 5> kColumns{"list_date", "market", "security_id",
                                                            "on_list", "available_at"};
  ATX_TRY(const auto table, stage.table(rel, kColumns));
  ThresholdRows out;
  ATX_TRY(out.list_day, day_column(table, "list_date"));
  ATX_TRY(const auto market, text_column(table, "market"));
  out.market = market_codes(market);
  ATX_TRY(out.security_id, id_column(table, "security_id"));
  ATX_TRY(out.on_list, flag_column(table, "on_list"));
  ATX_TRY(out.available_ns, instant_column(table, "available_at"));
  ATX_TRY_VOID(same_length(out.list_day.size(),
                           {out.market.size(), out.security_id.size(), out.on_list.size(),
                            out.available_ns.size()},
                           "regsho threshold"));
  return core::Ok(std::move(out));
}

core::Result<FinraNameRows> read_finra_names(PinnedStage &stage) {
  static constexpr std::array<std::string_view, 4> kColumns{"security_id", "available_at",
                                                            "dissemination_date", "market_class"};
  ATX_TRY(const auto table, stage.table("finra_names.parquet", kColumns));
  FinraNameRows out;
  ATX_TRY(out.security_id, id_column(table, "security_id"));
  ATX_TRY(out.available_ns, instant_column(table, "available_at"));
  ATX_TRY(out.dissemination_day, day_column(table, "dissemination_date"));
  ATX_TRY(out.market_class, text_column(table, "market_class"));
  ATX_TRY_VOID(same_length(out.security_id.size(),
                           {out.available_ns.size(), out.dissemination_day.size(),
                            out.market_class.size()},
                           "finra_names"));
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
