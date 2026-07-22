// RunArchive (ATXRUN01) — the binary result container for backtest outputs.
//
// Task 1 (schema): the column registry in run_archive_schema.hpp is the single
// source of truth for every result section. The `backtest` column order is
// load-bearing — it matches `append_backtest_series_tsv` (tearsheet.cpp) and
// `BacktestResult` — and every other section's columns are enumerated from the
// existing TSV writer that owns that output (listed_dispersion_reconciliation,
// listed_dispersion_schedule, the example's mark-divergence replay,
// write_diagnostics). `ra_schema_hash()` folds the whole registry into one
// constexpr FNV-1a-64 value so a header can pin the schema at open time.

#include "atx/vol/run_archive_schema.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace atx::vol {
namespace {

const RaSection* find_section(std::string_view name) {
  for (const RaSection& section : ra_sections()) {
    if (section.name == name) return &section;
  }
  return nullptr;
}

TEST(RunArchiveSchema, BacktestSectionPinned) {
  // backtest section present, 27 cols (date + ts_ns + 25 doubles), nav at 16.
  const RaSection* bt = find_section("backtest");
  ASSERT_NE(bt, nullptr);
  EXPECT_EQ(bt->kind, RaSectionKind::TimeSeries);
  ASSERT_EQ(bt->columns.size(), 27u);
  EXPECT_EQ(bt->columns[0].name, "date");
  EXPECT_EQ(bt->columns[0].dtype, RaDType::DictStr);
  EXPECT_EQ(bt->columns[1].name, "ts_ns");
  EXPECT_EQ(bt->columns[1].dtype, RaDType::I64);
  EXPECT_EQ(bt->columns[16].name, "nav");
  EXPECT_EQ(bt->columns[16].dtype, RaDType::F64);
  EXPECT_EQ(bt->columns[26].name, "n_unpriced_greeks");
  // Everything after date/ts_ns is a plain double column.
  for (std::size_t i = 2; i < bt->columns.size(); ++i) {
    EXPECT_EQ(bt->columns[i].dtype, RaDType::F64) << "column " << bt->columns[i].name;
  }
}

TEST(RunArchiveSchema, ProjectedSectionsReuseBacktestColumns) {
  // projected_cold / projected_nodiv share the backtest column array (same
  // pointer, not a copy) so the three sections can never drift.
  const RaSection* bt = find_section("backtest");
  const RaSection* cold = find_section("projected_cold");
  const RaSection* nodiv = find_section("projected_nodiv");
  ASSERT_NE(bt, nullptr);
  ASSERT_NE(cold, nullptr);
  ASSERT_NE(nodiv, nullptr);
  EXPECT_EQ(cold->kind, RaSectionKind::TimeSeries);
  EXPECT_EQ(nodiv->kind, RaSectionKind::TimeSeries);
  EXPECT_EQ(cold->columns.data(), bt->columns.data());
  EXPECT_EQ(nodiv->columns.data(), bt->columns.data());
  EXPECT_EQ(cold->columns.size(), bt->columns.size());
  EXPECT_EQ(nodiv->columns.size(), bt->columns.size());
}

TEST(RunArchiveSchema, WriterOwnedSectionsPinned) {
  // Column counts come from the TSV writers that own each output today:
  //   reconciliation  <- serialize_listed_reconciliation (11 cols)
  //   trade_schedule  <- serialize_listed_dispersion_schedule kHeader (29 cols)
  //   contract_marks  <- serialize_listed_contract_marks (19 cols)
  //   mark_divergence <- write_mark_divergence_replay header (10 cols)
  //   diagnostics     <- write_diagnostics header (4 cols)
  const RaSection* meta = find_section("meta");
  ASSERT_NE(meta, nullptr);
  EXPECT_EQ(meta->kind, RaSectionKind::ScalarKV);

  const RaSection* rec = find_section("reconciliation");
  ASSERT_NE(rec, nullptr);
  EXPECT_EQ(rec->kind, RaSectionKind::TimeSeries);
  ASSERT_EQ(rec->columns.size(), 11u);
  EXPECT_EQ(rec->columns[0].name, "date");
  EXPECT_EQ(rec->columns[10].name, "n_quote_mid_lots");

  const RaSection* sched = find_section("trade_schedule");
  const RaSection* proj_sched = find_section("projected_schedule");
  ASSERT_NE(sched, nullptr);
  ASSERT_NE(proj_sched, nullptr);
  EXPECT_EQ(sched->kind, RaSectionKind::SubTable);
  EXPECT_EQ(proj_sched->kind, RaSectionKind::SubTable);
  ASSERT_EQ(sched->columns.size(), 29u);
  EXPECT_EQ(sched->columns[0].name, "roll_date");
  EXPECT_EQ(sched->columns[28].name, "surface_fingerprint");
  EXPECT_EQ(proj_sched->columns.data(), sched->columns.data());
  EXPECT_EQ(proj_sched->columns.size(), sched->columns.size());

  const RaSection* marks = find_section("contract_marks");
  ASSERT_NE(marks, nullptr);
  EXPECT_EQ(marks->kind, RaSectionKind::SubTable);
  ASSERT_EQ(marks->columns.size(), 19u);
  EXPECT_EQ(marks->columns[0].name, "date");
  EXPECT_EQ(marks->columns[18].name, "model_in_spread");

  const RaSection* div = find_section("mark_divergence");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(div->kind, RaSectionKind::SubTable);
  ASSERT_EQ(div->columns.size(), 10u);
  EXPECT_EQ(div->columns[0].name, "date");
  EXPECT_EQ(div->columns[9].name, "abs_diff_bps_of_mark");

  const RaSection* diag = find_section("diagnostics");
  ASSERT_NE(diag, nullptr);
  EXPECT_EQ(diag->kind, RaSectionKind::SubTable);
  ASSERT_EQ(diag->columns.size(), 4u);
  EXPECT_EQ(diag->columns[0].name, "subcommand");
  EXPECT_EQ(diag->columns[1].name, "phase");
  EXPECT_EQ(diag->columns[2].name, "wall_ms");
  EXPECT_EQ(diag->columns[3].name, "count");

  EXPECT_EQ(ra_sections().size(), 10u);
}

TEST(RunArchiveSchema, SchemaHashStableAndNonzero) {
  // Usable in constant expressions (a header stamps it at compile time).
  static_assert(ra_schema_hash() != 0, "RunArchive schema hash must be nonzero");
  EXPECT_EQ(ra_schema_hash(), ra_schema_hash());
}

TEST(RunArchiveSchema, FormatConstants) {
  EXPECT_EQ(std::string_view(kRaMagic, sizeof kRaMagic), "ATXRUN01");
  EXPECT_EQ(kRaMajor, 1u);
  EXPECT_EQ(kRaMinor, 0u);
}

}  // namespace
}  // namespace atx::vol
