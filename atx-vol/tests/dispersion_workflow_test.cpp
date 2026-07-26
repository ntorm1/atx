#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/dispersion_workflow.hpp" // module under test
#include "atx/vol/types.hpp"               // ErrorCode, Result, Status

using namespace atx::vol;

namespace {

// L12: a three-row point-in-time universe in which SPY is itself a
// CONSTITUENT, not the index. The pre-L12 hardcode conflated the two, so this
// shape is exactly what distinguishes "the index leg" from "a name called SPY".
std::vector<UniverseRow> make_rows() {
  return {
      UniverseRow{"2026-01-02", "AAPL", 0.40, "test", "2026-01-01"},
      UniverseRow{"2026-01-02", "MSFT", 0.35, "test", "2026-01-01"},
      UniverseRow{"2026-01-02", "SPY", 0.25, "test", "2026-01-01"},
  };
}

std::filesystem::path scratch_dir(std::string_view name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_dispersion_workflow_test" / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

// Minimal spec carrying exactly read_run_spec's four REQUIRED keys, plus
// whatever extra rows a test appends. Everything else must default.
std::filesystem::path write_spec(const std::filesystem::path &dir, std::string_view extra_rows) {
  const std::filesystem::path path = dir / "run_spec.tsv";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "key\tvalue\n"
      << "date_lo\t2026-01-02\n"
      << "date_hi\t2026-01-09\n"
      << "opra_root\topra\n"
      << "universe_schedule\tuniverse_schedule.tsv\n"
      << extra_rows;
  return path;
}

std::vector<std::string> read_lines(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    lines.emplace_back(text, start, (end == std::string::npos ? text.size() : end) - start);
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

} // namespace

// ── RunSpec.index_symbol default ────────────────────────────────────────────

TEST(DispersionWorkflow, RunSpecIndexSymbolDefaultsToSpy) {
  // The default is the entire backward-compatibility contract of L12: every
  // existing spec, caller and golden stays bit-identical because of this line.
  EXPECT_EQ(RunSpec{}.index_symbol, "SPY");
}

// ── all_symbols ─────────────────────────────────────────────────────────────

TEST(DispersionWorkflow, AllSymbolsDefaultIsUnchanged) {
  const std::vector<UniverseRow> rows = make_rows();
  const std::vector<std::string> symbols = all_symbols(std::span<const UniverseRow>{rows});
  // Pre-L12 behaviour, verbatim: seed "SPY", union the rows, dedup, sort. SPY
  // appears once even though it is also a constituent row.
  ASSERT_EQ(symbols.size(), 3u);
  EXPECT_EQ(symbols[0], "AAPL");
  EXPECT_EQ(symbols[1], "MSFT");
  EXPECT_EQ(symbols[2], "SPY");
}

TEST(DispersionWorkflow, AllSymbolsHonoursIndexSymbol) {
  const std::vector<UniverseRow> rows = make_rows();
  const std::vector<std::string> symbols = all_symbols(std::span<const UniverseRow>{rows}, "QQQ");
  // QQQ is the index leg, so it must be fetched; SPY is a constituent here, so
  // it must ALSO be fetched. The hardcode could produce only the latter.
  ASSERT_EQ(symbols.size(), 4u);
  EXPECT_EQ(symbols[0], "AAPL");
  EXPECT_EQ(symbols[1], "MSFT");
  EXPECT_EQ(symbols[2], "QQQ");
  EXPECT_EQ(symbols[3], "SPY");
  EXPECT_EQ(std::count(symbols.begin(), symbols.end(), std::string("QQQ")), 1);
}

// ── universe_at ─────────────────────────────────────────────────────────────

TEST(DispersionWorkflow, UniverseAtDefaultIsUnchanged) {
  const std::vector<UniverseRow> rows = make_rows();
  const auto out = universe_at(std::span<const UniverseRow>{rows}, "2026-01-05");
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  EXPECT_EQ(out->index.symbol, "SPY");
  EXPECT_EQ(out->index.weight, 0.0);
  ASSERT_EQ(out->names.size(), 2u);
  EXPECT_EQ(out->names[0].symbol, "AAPL");
  EXPECT_EQ(out->names[0].weight, 0.40);
  EXPECT_EQ(out->names[1].symbol, "MSFT");
  EXPECT_EQ(out->names[1].weight, 0.35);
}

TEST(DispersionWorkflow, UniverseAtHonoursIndexSymbol) {
  const std::vector<UniverseRow> rows = make_rows();
  const auto out = universe_at(std::span<const UniverseRow>{rows}, "2026-01-05", "QQQ");
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  // Two independent defects the "SPY" hardcode causes with a non-SPY index:
  // (1) the index leg is MISLABELLED as SPY, and
  // (2) SPY is silently DROPPED from the basket even though it is an authored
  //     constituent with a real weight.
  EXPECT_EQ(out->index.symbol, "QQQ");
  ASSERT_EQ(out->names.size(), 3u);
  EXPECT_EQ(out->names[0].symbol, "AAPL");
  EXPECT_EQ(out->names[1].symbol, "MSFT");
  EXPECT_EQ(out->names[2].symbol, "SPY");
  EXPECT_EQ(out->names[2].weight, 0.25); // the authored raw_weight survives
  for (const DispersionMember &name : out->names) {
    EXPECT_NE(name.symbol, "QQQ") << "the index leg must not appear in the basket";
  }
}

// ── read_run_spec ───────────────────────────────────────────────────────────

TEST(DispersionWorkflow, ReadRunSpecDefaultsIndexSymbolWhenAbsent) {
  // Backward-compatibility lock for every run_spec.tsv already on disk: the key
  // did not exist before L12, so absence must resolve to SPY.
  const std::filesystem::path dir = scratch_dir("absent");
  const auto spec = read_run_spec(write_spec(dir, ""));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  EXPECT_EQ(spec->index_symbol, "SPY");
}

TEST(DispersionWorkflow, ReadRunSpecParsesIndexSymbol) {
  const std::filesystem::path dir = scratch_dir("parses");
  const auto spec = read_run_spec(write_spec(dir, "index_symbol\tQQQ\n"));
  ASSERT_TRUE(spec.has_value()) << spec.error().to_string();
  EXPECT_EQ(spec->index_symbol, "QQQ");
}

TEST(DispersionWorkflow, ReadRunSpecRejectsEmptyIndexSymbol) {
  // Present-but-empty is an authoring error, not "use the default": an empty
  // index leg would silently poison universe_at's member and all_symbols' seed.
  const std::filesystem::path dir = scratch_dir("empty");
  const auto spec = read_run_spec(write_spec(dir, "index_symbol\t\n"));
  ASSERT_FALSE(spec.has_value());
  EXPECT_EQ(spec.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ── write_resolved_spec ─────────────────────────────────────────────────────

TEST(DispersionWorkflow, WriteResolvedSpecEmitsIndexSymbolLast) {
  const std::filesystem::path dir = scratch_dir("resolved");
  RunSpec spec;
  spec.date_lo = "2026-01-02";
  spec.date_hi = "2026-01-09";
  spec.opra_root = dir / "opra";
  spec.universe_path = dir / "universe_schedule.tsv";
  const std::filesystem::path path = dir / "run_spec.tsv";
  ASSERT_TRUE(write_resolved_spec(path, spec).has_value());

  const std::vector<std::string> lines = read_lines(path);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front(), "key\tvalue");
  // Appended LAST is a binding decision: an old-vs-new resolved spec then
  // differs by exactly one trailing line, which is trivially auditable.
  EXPECT_EQ(lines.back(), "index_symbol\tSPY");
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(lines[lines.size() - 2], "core_mode\t0");

  // write -> read closure: the emitted row round-trips through the parser.
  const auto reread = read_run_spec(path);
  ASSERT_TRUE(reread.has_value()) << reread.error().to_string();
  EXPECT_EQ(reread->index_symbol, "SPY");
}

TEST(DispersionWorkflow, WriteResolvedSpecRoundTripsNonDefaultIndexSymbol) {
  const std::filesystem::path dir = scratch_dir("resolved_qqq");
  RunSpec spec;
  spec.date_lo = "2026-01-02";
  spec.date_hi = "2026-01-09";
  spec.opra_root = dir / "opra";
  spec.universe_path = dir / "universe_schedule.tsv";
  spec.index_symbol = "QQQ";
  const std::filesystem::path path = dir / "run_spec.tsv";
  ASSERT_TRUE(write_resolved_spec(path, spec).has_value());

  const std::vector<std::string> lines = read_lines(path);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.back(), "index_symbol\tQQQ");
  const auto reread = read_run_spec(path);
  ASSERT_TRUE(reread.has_value()) << reread.error().to_string();
  EXPECT_EQ(reread->index_symbol, "QQQ");
}
