#pragma once

// atx::engine::research::fields -- the short-side stage readers (P9 lane A4, migration slice 5; in
// the style of lane AL-SIG's 13F reader, ruling P18): the atx-db alpha-panel ftd/ (D3a),
// regsho_threshold/ (D3b) and security_master/ stages (research_fields_holdings.py STAGES), each
// read through a PinnedStage (stage.hpp: the manifest pinned by its SHA-256 and its declared
// rules, every file hashed against it before it is decoded), decoded into plain rows with the
// Python column helpers' null mapping (parquet_columns.hpp: a null id -1, a null day kNullDay, a
// null instant kNeverNs, a null text "", a null float NaN, a null flag false).
//
// Reader-side seal: a year partition year=YYYY whose year begins on or after the research seal is
// never opened (Stage.years, research_window.partition_is_sealed); a builder drops every row
// available on or after the seal before it takes a date's or a session's visibility clock.

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"

namespace atx::engine::research::fields {

inline constexpr StageContract kFtdStage{"ftd", "atx.alpha-panel.ftd/v1",
                                         "ftd-publication-halfmonth-v1", 60, ""};
inline constexpr StageContract kRegshoStage{"regsho_threshold",
                                            "atx.alpha-panel.regsho-threshold/v1",
                                            "regsho-publication-v1", 10, ""};
inline constexpr StageContract kSecurityMasterStage{
    "security_master", "atx.alpha-panel.security-master/v1", "", std::nullopt,
    "security-master-v1"};

// The Reg SHO list markets (REGSHO_MARKETS), in index order.
inline constexpr std::array<std::string_view, 4> kRegshoMarkets{"nasdaq", "nyse", "cboe_bzx",
                                                                "finra_otc"};

// The index of `text` in `values`; -1 when it is not one of them (the Python _codes).
[[nodiscard]] i64 code_of(std::string_view text, std::span<const std::string_view> values) noexcept;

// The manifest-listed files "year=YYYY/<name>" for YYYY in [lo, hi] whose year begins before the
// research seal, ascending (Stage.years).
[[nodiscard]] std::vector<std::string> stage_year_files(const PinnedStage &stage,
                                                        std::string_view name, i64 lo, i64 hi);

// One ftd.parquet: settlement_date, security_id, quantity, available_at, map_basis.
struct FtdRows {
  std::vector<i64> settlement_day;
  std::vector<i64> security_id;
  std::vector<f64> quantity;
  std::vector<i64> available_ns;
  std::vector<u8> collision; // map_basis == "collision"
};
[[nodiscard]] core::Result<FtdRows> read_ftd_rows(PinnedStage &stage, std::string_view rel);

// regsho_threshold/lists.parquet: one row per (market, list_date) file status.
struct RegshoListRows {
  std::vector<i64> market; // kRegshoMarkets index, -1 for another market
  std::vector<i64> list_day;
  std::vector<std::string> status;
  std::vector<i64> available_ns;
};
[[nodiscard]] core::Result<RegshoListRows> read_regsho_lists(PinnedStage &stage);

// One threshold.parquet: list_date, market, security_id, on_list, available_at.
struct ThresholdRows {
  std::vector<i64> list_day;
  std::vector<i64> market; // kRegshoMarkets index, -1 for another market
  std::vector<i64> security_id;
  std::vector<u8> on_list;
  std::vector<i64> available_ns;
};
[[nodiscard]] core::Result<ThresholdRows> read_threshold_rows(PinnedStage &stage,
                                                              std::string_view rel);

// security_master/finra_names.parquet: the FINRA short-interest name rows (PIT at their
// dissemination): security_id, available_at, dissemination_date, market_class.
struct FinraNameRows {
  std::vector<i64> security_id;
  std::vector<i64> available_ns;
  std::vector<i64> dissemination_day;
  std::vector<std::string> market_class;
};
[[nodiscard]] core::Result<FinraNameRows> read_finra_names(PinnedStage &stage);

} // namespace atx::engine::research::fields
