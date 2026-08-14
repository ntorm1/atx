// run_diagnostics — PhaseTimer + the `diagnostics` RunArchive section encoder
// (run_diagnostics.hpp / run_diagnostics.cpp). Mirrors the acceptance style of
// the Task 5 encoder round-trips in run_archive_test.cpp: encode a section,
// serialize with write_run_archive, reopen with RunArchive::open, and assert the
// real column values decode back — here the `phase` dict column carries both
// timed phase names plus the trailing `total` row, and `count` carries the
// per-phase units and the caller's total_count.

#include "atx/vol/research/run_diagnostics.hpp"

#include "atx/vol/research/run_archive.hpp"
#include "storage/run_archive_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// ── Wave E T1: the diagnostics ROW SET of the two instrumented subcommands ───
// Two Wave E perf passes target work that the current phase split cannot see:
// the definitions parse is buried inside `setup_read`, and the per-date snapshot
// load / OPRA quote join / reconcile are fused into one `reconciliation` number.
// Splitting them is a DATA change to the `diagnostics` section (more rows, one
// name retired), not a schema change — kDiagnosticsCols is untouched, so
// ra_schema_hash() cannot move (pinned separately in run_archive_test.cpp).
//
// The subcommands publish their phase order via kBuildSchedulePhases /
// kRunBacktestPhases, so this asserts the row set the CLI will actually emit
// without linking the example binary. Every phase is charged once so the encoded
// section carries a row per declared phase.
std::vector<std::string> encoded_phase_names(std::span<const std::string_view> order,
                                             std::string_view subcommand) {
  PhaseTimer timer(order);
  const auto start = PhaseTimer::now();
  for (const std::string_view phase : order) {
    timer.add(phase, start, 1u);
  }
  const RaSectionData sec = encode_diagnostics_section(timer, subcommand, 49u);
  auto bytes = write_run_archive(std::span(&sec, 1), 1, 1);
  EXPECT_TRUE(bytes.has_value());
  if (!bytes) return {};
  auto ar = RunArchive::open(std::move(*bytes));
  EXPECT_TRUE(ar.has_value());
  if (!ar) return {};
  auto v = ar->section("diagnostics");
  EXPECT_TRUE(v.has_value());
  if (!v) return {};
  const RaDictColumn phase = v->dict_col("phase");
  std::vector<std::string> names;
  names.reserve(phase.size());
  for (std::size_t i = 0; i < phase.size(); ++i) {
    names.emplace_back(phase.at(i));
  }
  return names;
}

bool has_phase(const std::vector<std::string>& names, std::string_view want) {
  return std::find(names.begin(), names.end(), want) != names.end();
}

TEST(RunDiagnostics, RunBacktestPublishesDecomposedPhaseRows) {
  const std::vector<std::string> names =
      encoded_phase_names(kRunBacktestPhases, "run_backtest");
  ASSERT_FALSE(names.empty());

  // P3's target: the definitions parse must be its own row, not folded into
  // setup_read.
  EXPECT_TRUE(has_phase(names, "definitions_parse")) << "definitions_parse row missing";
  // P2's target: the fused per-date pass must be three disjoint rows.
  EXPECT_TRUE(has_phase(names, "snapshot_load")) << "snapshot_load row missing";
  EXPECT_TRUE(has_phase(names, "quote_join")) << "quote_join row missing";
  EXPECT_TRUE(has_phase(names, "reconcile")) << "reconcile row missing";
  // The aggregate it replaces must be GONE — a leftover `reconciliation` row
  // would mean the split relabelled rather than partitioned, and would double
  // count against the total.
  EXPECT_FALSE(has_phase(names, "reconciliation"))
      << "aggregate `reconciliation` phase still present after the split";
  // Untouched neighbours.
  EXPECT_TRUE(has_phase(names, "setup_read"));
  EXPECT_TRUE(has_phase(names, "engine_run"));
  EXPECT_TRUE(has_phase(names, "write_outputs"));

  // Regression lock on the exact emitted order (the encoder emits the declared
  // order, then `total`). This one is a positive control, not a falsifiable
  // discovery: it restates the constant. It exists so a later pass cannot
  // reorder or silently drop a row.
  const std::vector<std::string> expect{"setup_read",   "definitions_parse", "engine_run",
                                        "snapshot_load", "quote_join",       "reconcile",
                                        "write_outputs", "total"};
  EXPECT_EQ(names, expect);
}

TEST(RunDiagnostics, BuildSchedulePublishesDefinitionsParseRow) {
  const std::vector<std::string> names =
      encoded_phase_names(kBuildSchedulePhases, "build_schedule");
  ASSERT_FALSE(names.empty());
  EXPECT_TRUE(has_phase(names, "definitions_parse")) << "definitions_parse row missing";
  EXPECT_TRUE(has_phase(names, "setup_read"));
  // Charged by the library builder through the passed-in timer.
  EXPECT_TRUE(has_phase(names, "selection"));
  EXPECT_TRUE(has_phase(names, "quote_join"));
  EXPECT_TRUE(has_phase(names, "write_outputs"));

  const std::vector<std::string> expect{"setup_read", "definitions_parse", "selection",
                                        "quote_join", "write_outputs",    "total"};
  EXPECT_EQ(names, expect);
}

// Anti-vacuity: the helper above must be able to report a MISSING phase, or the
// two tests could pass on any row set. Feeding it a list that deliberately omits
// definitions_parse must make has_phase say false.
TEST(RunDiagnostics, PhaseRowSetAssertionIsFalsifiable) {
  static constexpr std::array<std::string_view, 2> kDecoy{"setup_read", "reconciliation"};
  const std::vector<std::string> names = encoded_phase_names(kDecoy, "run_backtest");
  ASSERT_EQ(names.size(), 3u); // two phases + `total`
  EXPECT_FALSE(has_phase(names, "definitions_parse"));
  EXPECT_FALSE(has_phase(names, "snapshot_load"));
  EXPECT_TRUE(has_phase(names, "reconciliation"));
}

} // namespace
} // namespace atx::vol
