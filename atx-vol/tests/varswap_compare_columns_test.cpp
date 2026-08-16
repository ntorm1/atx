// Task F-8 fix round 8 (I-A): the P&L-explain column gate that observes the
// ARTIFACT rather than the source text.
//
// WHAT THIS REPLACES. From round 3 to round 7 the only thing standing between
// examples/varswap_compare_example.cpp and a silently truncated attribution tail
// was python/tests/test_render_strangle_vs_varswap.py, which READS THAT FILE AS
// TEXT and asserts things about the shape of the loop it finds. Each round
// sharpened the predicate; each round the predicate was still an approximation
// of the property. Two holes were MEASURED in the round-7 version:
//
//   * `if (column.name != "swap_explain_skew") { attach_one(...); }` -- a skip
//     with no `continue`/`break`/`return` token to find.
//   * `swap_explain_columns().subspan(0, 4)` -- the loop is intact, iterates the
//     roster, attaches unconditionally, and emits half the columns.
//
// Both left the Python module green. Neither can survive this file, because this
// file does not care how the emission is spelled: it links the example's own
// translation unit, calls the shipped `attach_swap_columns`, writes a real TSV
// through the shipped `write_backtest_tsv`, and diffs the header line it finds
// there against `swap_explain_columns()`. Anything that changes which names
// reach the file, or their order, changes the header.
//
// WHAT IT DOES NOT COVER, stated so the next reader does not over-trust it: the
// name/member PAIRING inside a roster row is not independently checked here --
// the example attaches `column.name` with `column.member` from one row, so a
// swapped pair would move name and data together. That property is pinned
// structurally at the roster instead (`roster_rows_match_their_indices()` plus
// the `-Wswitch` walk in src/backtest/backtest.cpp). What IS checked here is that the
// value emitted under each header came from the member the roster names for it,
// which catches a re-ordered or off-by-one emission.
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"
#include "backtest/backtest_series_columns.hpp"
#include "atx/vol/tools/tearsheet.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::vol::varswap_compare {
// Defined in examples/varswap_compare_example.cpp, which this target compiles
// with ATX_VARSWAP_COMPARE_NO_MAIN so the example's `main` is suppressed. A
// declaration that does not match the definition is a link error, so this cannot
// silently drift onto some other function.
[[nodiscard]] Status attach_swap_columns(BacktestResult &r);
} // namespace atx::vol::varswap_compare

namespace atx::vol {
namespace {

constexpr std::size_t kRows = 3;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400000000000LL;

// The sentinel written into the explain member the roster lists at index `i`.
// Distinct per column and per row, so a re-ordered emission, an off-by-one, and
// a transposed row all read back as the wrong number rather than as a match.
[[nodiscard]] double sentinel(std::size_t column_index, std::size_t row) noexcept {
  return 1000.0 + 10.0 * static_cast<double>(column_index) + static_cast<double>(row);
}

// A minimally-valid result: every mandatory series column sized to the row
// count. Driven off `backtest_series_columns()` so a column added to the frozen
// registry is filled here without an edit. The registry stores POINTER-TO-CONST
// members (it exists to read columns out); `r` is a non-const object, so casting
// the constness off to fill it is defined.
[[nodiscard]] BacktestResult make_result() {
  BacktestResult r;
  for (std::size_t i = 0; i < kRows; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(i) + 1);
    r.date.emplace_back(buf);
    r.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(i) * kDayNs);
  }
  for (const BacktestSeriesColumn &col : backtest_series_columns()) {
    const_cast<std::vector<double> &>(r.*col.member).assign(kRows, 0.0);
  }
  r.swap_pv.assign(kRows, 7.0);
  r.swap_pnl.assign(kRows, 8.0);
  std::size_t ix = 0;
  for (const BacktestExplainColumn &col : swap_explain_columns()) {
    std::vector<double> &column = r.*col.member;
    column.resize(kRows);
    for (std::size_t row = 0; row < kRows; ++row) {
      column[row] = sentinel(ix, row);
    }
    ++ix;
  }
  return r;
}

[[nodiscard]] std::vector<std::string> split_tabs(const std::string &line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string field;
  while (std::getline(ss, field, '\t')) {
    out.push_back(field);
  }
  return out;
}

// Emit through the shipped path and hand back the TSV's lines.
[[nodiscard]] std::vector<std::string> emit_and_read() {
  BacktestResult r = make_result();
  const Status attached = varswap_compare::attach_swap_columns(r);
  EXPECT_TRUE(attached.has_value())
      << (attached.has_value() ? std::string{} : attached.error().to_string());
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "atx-varswap-compare-columns.tsv";
  const Status written = write_backtest_tsv(r, path.string());
  EXPECT_TRUE(written.has_value())
      << (written.has_value() ? std::string{} : written.error().to_string());
  std::vector<std::string> lines;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

// The number of header fields before the dynamic signal tail: `date`, `ts_ns`,
// then the frozen F64 registry. Computed, not spelled, so growing the registry
// does not false-fail this test.
[[nodiscard]] std::size_t fixed_prefix_width() noexcept {
  return 2u + backtest_series_columns().size();
}

// The mark-domain counters `append_backtest_series_tsv` (src/backtest/
// tearsheet.cpp) appends LAST by documented contract: report-only columns,
// deliberately outside the frozen RunArchive-pinned registry, always the
// rightmost fields — after the frozen block AND after every attached signal
// column. Pinned here by name and order so a writer change that drops or
// reorders them fails this gate, and so the signal-tail checks below can
// exclude exactly these fields and nothing else.
constexpr std::string_view kTrailingMarkDomainCols[] = {
    "n_extrapolated_marks",
    "n_carried_marks",
};

// THE HEADER IS THE ROSTER. Not "contains every roster name" -- equality, so an
// EXTRA column is a failure too. A stray hand-added `swap_explain_*` name is how
// the sixth copy of this roster would get back in.
TEST(VarswapCompareColumns, TheEmittedHeaderIsTheRosterInRosterOrder) {
  const std::vector<std::string> lines = emit_and_read();
  ASSERT_FALSE(lines.empty()) << "the example's writer produced no header line";
  const std::vector<std::string> header = split_tabs(lines.front());
  const std::size_t n_trailing = std::size(kTrailingMarkDomainCols);
  ASSERT_GE(header.size(), fixed_prefix_width() + n_trailing)
      << "header is shorter than the frozen series registry plus the trailing "
         "mark-domain counters: "
      << lines.front();

  // The writer's contract puts the mark-domain counters rightmost, in order —
  // pinning them here is what licenses slicing them off the signal tail below.
  for (std::size_t i = 0; i < n_trailing; ++i) {
    EXPECT_EQ(header[header.size() - n_trailing + i], kTrailingMarkDomainCols[i])
        << "the trailing mark-domain counter block moved or lost a column";
  }

  const std::vector<std::string> tail(
      header.begin() + static_cast<std::ptrdiff_t>(fixed_prefix_width()),
      header.end() - static_cast<std::ptrdiff_t>(n_trailing));
  std::vector<std::string> want{"swap_pv", "swap_pnl"};
  for (const BacktestExplainColumn &col : swap_explain_columns()) {
    want.emplace_back(col.name);
  }
  EXPECT_EQ(tail, want) << "the TSV's signal tail is not `swap_pv, swap_pnl` followed by "
                           "`swap_explain_columns()` in order. A column the roster declares "
                           "but the example does not emit is an attribution the report "
                           "silently drops; an extra one is a fifth copy of the roster "
                           "coming back.";
}

// THE DATA UNDER EACH HEADER CAME FROM THAT COLUMN'S MEMBER. The header diff
// alone cannot see an emission that writes the right names over shifted data --
// e.g. attaching `roster[i].name` with `roster[i + 1].member`.
TEST(VarswapCompareColumns, EachExplainHeaderSitsOverItsOwnMembersValues) {
  const std::vector<std::string> lines = emit_and_read();
  ASSERT_GE(lines.size(), kRows + 1u) << "expected a header plus " << kRows << " data rows";
  const std::vector<std::string> header = split_tabs(lines.front());

  const std::size_t explain_start = fixed_prefix_width() + 2u; // past swap_pv/swap_pnl
  std::size_t ix = 0;
  for (const BacktestExplainColumn &col : swap_explain_columns()) {
    const std::size_t field = explain_start + ix;
    ASSERT_LT(field, header.size()) << "no field for roster column " << col.name;
    for (std::size_t row = 0; row < kRows; ++row) {
      const std::vector<std::string> cells = split_tabs(lines[row + 1u]);
      ASSERT_EQ(cells.size(), header.size()) << "row " << row << " is ragged against the header";
      EXPECT_DOUBLE_EQ(std::stod(cells[field]), sentinel(ix, row))
          << "column `" << col.name << "` (roster index " << ix << ") carries row " << row
          << " of some OTHER member: the emission is misrouted or re-ordered";
    }
    ++ix;
  }
}

// The two lead columns are the quantity being explained, not components of it,
// and the renderer finds the explain tail by the `swap_explain_` prefix -- so
// nothing before that tail may carry the prefix, and nothing in it may lack it.
TEST(VarswapCompareColumns, TheExplainPrefixPartitionsTheSignalTail) {
  const std::vector<std::string> lines = emit_and_read();
  ASSERT_FALSE(lines.empty());
  const std::vector<std::string> header = split_tabs(lines.front());
  const std::size_t width = fixed_prefix_width();
  ASSERT_GT(header.size(), width + 1u + std::size(kTrailingMarkDomainCols));

  EXPECT_EQ(header[width], "swap_pv");
  EXPECT_EQ(header[width + 1u], "swap_pnl");
  // The writer appends the mark-domain counters after every signal column, so
  // the prefix-carrying tail ends where that pinned trailing block begins. A
  // stray un-prefixed column between the explain tail and the counters lands
  // inside [width+2, tail_end) and still fails the partition.
  const std::size_t tail_end = header.size() - std::size(kTrailingMarkDomainCols);
  for (std::size_t i = 0; i < header.size(); ++i) {
    const bool has_prefix = header[i].rfind("swap_explain_", 0) == 0;
    const bool in_tail = i >= width + 2u && i < tail_end;
    EXPECT_EQ(has_prefix, in_tail)
        << "field " << i << " (`" << header[i]
        << "`) breaks the partition the renderer relies on: exactly the fields between "
           "`swap_pv`/`swap_pnl` and the trailing mark-domain counters carry the "
           "`swap_explain_` prefix";
  }
}

} // namespace
} // namespace atx::vol
