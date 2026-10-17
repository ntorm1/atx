#pragma once

// atx::engine::research::fields -- the short-side stage reader (P9 lane A4, migration slice 5; in
// the style of lane AL-SIG's 13F reader, ruling P18): the atx-db alpha-panel ftd/ (D3a),
// regsho_threshold/ (D3b) and security_master/ stages (research_fields_holdings.py STAGES).
//
// One reader for every short-side file: read_short_side<Rows> streams a file of a PinnedStage
// (stage.hpp: the manifest pinned by its SHA-256 and its declared rules, the file hashed against it
// before it is decoded) one parquet row group at a time into a Rows batch, decoding exactly the
// columns of that Rows' layout (short_side.cpp) with the Python column helpers' null mapping
// (parquet_columns.hpp: a null id -1, a null day kNullDay, a null instant kNeverNs, a null float
// NaN, a null flag 0, a null or unlisted text code -1). Text columns are decoded to codes in a
// fixed value list (no text is kept per row). Memory: one row group of the layout's columns;
// read_short_side_all gathers a small file (lists, FINRA names) whole.
//
// Reader-side seal: a year partition year=YYYY whose year begins on or after the research seal is
// never opened (Stage.years, research_window.partition_is_sealed); a builder drops every row
// available on or after the seal before it takes a date's or a session's visibility clock.
// Cold path; not thread-safe (a PinnedStage records the files it reads).

#include <array>
#include <functional>
#include <optional>
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

// The code lists of the short-side text columns (index = code; -1 = null or unlisted).
// ftd map_basis: only "collision" matters (a collision row counts as no fail).
inline constexpr std::array<std::string_view, 1> kFtdBases{"collision"};
inline constexpr i64 kFtdCollision = 0;
// The Reg SHO list markets (REGSHO_MARKETS), in index order: lists.market, threshold.market.
inline constexpr std::array<std::string_view, 4> kRegshoMarkets{"nasdaq", "nyse", "cboe_bzx",
                                                                "finra_otc"};
// lists.status values of a usable list (REGSHO_LIST_STATUSES): any code >= 0 is usable.
inline constexpr std::array<std::string_view, 2> kRegshoListStatuses{"list", "empty_list"};
// FINRA marketClassCode values with a threshold list source (REGSHO_MARKET_OF_CLASS keys, in
// order); kFinraClassMarket maps each to its kRegshoMarkets index: NNM / SC -> nasdaq,
// NYSE / AMEX / ARCA -> nyse, BZX -> cboe_bzx, OTC -> finra_otc.
inline constexpr std::array<std::string_view, 7> kFinraMarketClasses{"NNM",  "SC",  "NYSE", "AMEX",
                                                                     "ARCA", "BZX", "OTC"};
inline constexpr std::array<i64, 7> kFinraClassMarket{0, 0, 1, 1, 1, 2, 3};

// The manifest-listed files "year=YYYY/<name>" for YYYY in [lo, hi] whose year begins before the
// research seal, ascending (Stage.years).
[[nodiscard]] std::vector<std::string> stage_year_files(const PinnedStage &stage,
                                                        std::string_view name, i64 lo, i64 hi);

// ---- the Rows batches (one per short-side file layout) ----------------------------------------

// ftd/year=YYYY/ftd.parquet: settlement_date, security_id, quantity, available_at, map_basis.
struct FtdRows {
  std::vector<i64> settlement_day;
  std::vector<i64> security_id;
  std::vector<f64> quantity;
  std::vector<i64> available_ns;
  std::vector<i64> basis; // map_basis as a kFtdBases code (kFtdCollision for a collision)
  [[nodiscard]] usize size() const noexcept { return settlement_day.size(); }
};

// regsho_threshold/lists.parquet: one row per (market, list_date) file status.
struct RegshoListRows {
  std::vector<i64> market; // kRegshoMarkets code
  std::vector<i64> list_day;
  std::vector<i64> status; // kRegshoListStatuses code (>= 0: a usable list)
  std::vector<i64> available_ns;
  [[nodiscard]] usize size() const noexcept { return market.size(); }
};

// regsho_threshold/year=YYYY/threshold.parquet: list_date, market, security_id, on_list,
// available_at.
struct ThresholdRows {
  std::vector<i64> list_day;
  std::vector<i64> market; // kRegshoMarkets code
  std::vector<i64> security_id;
  std::vector<u8> on_list;
  std::vector<i64> available_ns;
  [[nodiscard]] usize size() const noexcept { return list_day.size(); }
};

// security_master/finra_names.parquet: the FINRA short-interest name rows (PIT at their
// dissemination): security_id, available_at, dissemination_date, market_class.
struct FinraNameRows {
  std::vector<i64> security_id;
  std::vector<i64> available_ns;
  std::vector<i64> dissemination_day;
  std::vector<i64> market_class; // kFinraMarketClasses code
  [[nodiscard]] usize size() const noexcept { return security_id.size(); }
};

// ---- the reader --------------------------------------------------------------------------------

template <class Rows> using RowsVisitor = std::function<core::Status(const Rows &)>;

// Streams the verified file `rel` of `stage` one row group at a time: `visit` sees each group's
// rows (every member of one length) in file order; its Err stops the read and is returned.
// Err(InvalidArgument) for a file the manifest does not list or whose bytes are not the pinned
// ones, a missing layout column or one of another type; Err(IoError) when it cannot be read or
// changed while it was read. Rows is FtdRows, RegshoListRows, ThresholdRows or FinraNameRows.
template <class Rows>
[[nodiscard]] core::Status read_short_side(PinnedStage &stage, std::string_view rel,
                                           const RowsVisitor<Rows> &visit);

// Every row of `rel` in one Rows (the groups appended in file order): for the small files.
template <class Rows>
[[nodiscard]] core::Result<Rows> read_short_side_all(PinnedStage &stage, std::string_view rel);

} // namespace atx::engine::research::fields
