// run_diagnostics — PhaseTimer + the `diagnostics` RunArchive section encoder
// (run_diagnostics.hpp / run_diagnostics.cpp). Mirrors the acceptance style of
// the Task 5 encoder round-trips in run_archive_test.cpp: encode a section,
// serialize with write_run_archive, reopen with RunArchive::open, and assert the
// real column values decode back — here the `phase` dict column carries both
// timed phase names plus the trailing `total` row, and `count` carries the
// per-phase units and the caller's total_count.

#include "atx/vol/run_diagnostics.hpp"

#include "atx/vol/run_archive.hpp"
#include "atx/vol/run_archive_schema.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <span>

namespace atx::vol {
namespace {

TEST(RunDiagnostics, DiagnosticsSectionRoundTrips) {
  // Two pre-declared phases, each charged a wall time (from a shared start) and a
  // unit count. steady_clock is real, so wall_ms is nondeterministic — the
  // deterministic asserts are the dict names and the I64 counts.
  PhaseTimer timer{"load", "solve"};
  const auto start = PhaseTimer::now();
  timer.add("load", start, 3u);
  timer.add("solve", start, 5u);

  const RaSectionData sec = encode_diagnostics_section(timer, "run_backtest", 42u);
  EXPECT_EQ(sec.name, "diagnostics");
  EXPECT_EQ(sec.kind, RaSectionKind::SubTable);
  EXPECT_EQ(sec.n_rows, 3u); // two phases + the `total` row
  ASSERT_EQ(sec.columns.size(), 4u);

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("diagnostics");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_EQ(v->kind(), RaSectionKind::SubTable);
  EXPECT_EQ(v->n_rows(), 3u);

  // phase dict: both timed names, then the trailing `total` row.
  const RaDictColumn phase = v->dict_col("phase");
  ASSERT_EQ(phase.size(), 3u);
  EXPECT_EQ(phase.at(0), "load");
  EXPECT_EQ(phase.at(1), "solve");
  EXPECT_EQ(phase.at(2), "total");

  // subcommand dict: the same token on every row.
  const RaDictColumn subcommand = v->dict_col("subcommand");
  ASSERT_EQ(subcommand.size(), 3u);
  EXPECT_EQ(subcommand.at(0), "run_backtest");
  EXPECT_EQ(subcommand.at(2), "run_backtest");

  // count I64: the per-phase units, then the caller's total_count on the total row.
  const auto count = v->i64_col("count");
  ASSERT_EQ(count.size(), 3u);
  EXPECT_EQ(count[0], 3);
  EXPECT_EQ(count[1], 5);
  EXPECT_EQ(count[2], 42);

  // wall_ms F64: real, finite, non-negative; the total row is the phase sum.
  const auto wall_ms = v->f64_col("wall_ms");
  ASSERT_EQ(wall_ms.size(), 3u);
  EXPECT_TRUE(std::isfinite(wall_ms[0]));
  EXPECT_GE(wall_ms[0], 0.0);
  EXPECT_GE(wall_ms[1], 0.0);
  EXPECT_GE(wall_ms[2], 0.0);
  EXPECT_NEAR(wall_ms[2], wall_ms[0] + wall_ms[1], 1e-6);
}

TEST(RunDiagnostics, EmptyTimerYieldsTotalRowOnly) {
  const PhaseTimer timer{};
  const RaSectionData sec = encode_diagnostics_section(timer, "build_schedule", 7u);
  EXPECT_EQ(sec.n_rows, 1u); // just the `total` row

  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().to_string();
  auto ar = RunArchive::open(std::move(*bytes));
  ASSERT_TRUE(ar.has_value()) << ar.error().to_string();
  auto v = ar->section("diagnostics");
  ASSERT_TRUE(v.has_value()) << v.error().to_string();

  const RaDictColumn phase = v->dict_col("phase");
  ASSERT_EQ(phase.size(), 1u);
  EXPECT_EQ(phase.at(0), "total");
  EXPECT_EQ(v->dict_col("subcommand").at(0), "build_schedule");
  const auto count = v->i64_col("count");
  ASSERT_EQ(count.size(), 1u);
  EXPECT_EQ(count[0], 7);
  EXPECT_EQ(v->f64_col("wall_ms")[0], 0.0); // no phases summed
}

} // namespace
} // namespace atx::vol
