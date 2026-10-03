#pragma once

// atx::engine::research::fields -- the Russell-proxy reader (P9 lane AL-SIG, ruling P18): the
// atx-db alpha-panel indexes/ stage (schema atx.alpha-panel.indexes/v1, rule russell-proxy-v1,
// atx-db/docs/datasets/indexes.md) read into one membership snapshot per Russell reconstitution.
//
// constituents.parquet holds one row per (index_id, security_id, recon_date): every member line of
// a proxy index at each reconstitution, with the rank date's available_at (rank date 22:00 UTC).
// A reconstitution's snapshot is its r1000 and r2000 rows (index ids declared below: another
// spelling of the two is refused with the ids seen); its visibility is the maximum available_at of
// its rows. Reader-side seal: a row available on or after the research seal is dropped and
// counted. Rows of other indexes (r3000, sp500) are counted and ignored; a row with a null
// security_id, recon_date or available_at is counted as unusable.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/sources/parquet_columns.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"

namespace atx::engine::research::fields {

inline constexpr StageContract kIndexesStage{"indexes", "atx.alpha-panel.indexes/v1", "",
                                             std::nullopt, ""};
inline constexpr std::string_view kRussell1000Id = "r1000";
inline constexpr std::string_view kRussell2000Id = "r2000";
inline constexpr std::string_view kConstituentsFile = "constituents.parquet";

// One constituents.parquet row (nulls as parquet_columns.hpp maps them).
struct RussellRow {
  std::string index_id;
  i64 security_id{-1};
  i64 rank_day{kNullDay};
  i64 recon_day{kNullDay};
  i64 available_ns{kNeverNs};
};

struct RussellRecon {
  i64 recon_day{};
  i64 rank_day{};          // the latest rank_date of its rows
  i64 visible_ns{};        // the maximum available_at of its rows
  std::vector<i64> r1000;  // ascending unique security ids
  std::vector<i64> r2000;  // ascending unique security ids
};

struct RussellCounts {
  u64 rows_total{};
  u64 rows_other_index{};
  u64 rows_sealed{};
  u64 rows_unusable{};
};

struct RussellSnapshots {
  std::vector<RussellRecon> recons; // ascending by recon_day
  RussellCounts counts;
  std::vector<std::string> index_ids; // every index id seen, ascending
};

// The snapshots of decoded rows (pure). Err(InvalidArgument) when no usable row is an r1000 or
// r2000 row (the message lists the index ids seen).
[[nodiscard]] core::Result<RussellSnapshots> russell_snapshots(std::span<const RussellRow> rows);

// Decodes the stage's constituents.parquet and builds its snapshots.
[[nodiscard]] core::Result<RussellSnapshots> read_russell_snapshots(PinnedStage &stage);

// The source_checks block: counts, ids seen and each reconstitution's dates, visibility and sizes.
[[nodiscard]] nlohmann::json russell_checks(const RussellSnapshots &snapshots);

} // namespace atx::engine::research::fields
