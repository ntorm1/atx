// The shared vendor panel (P9 lane A3; contract K-P9-2, source half): the NYSE rule calendar and
// the extended axis it reads, factor-break-v1 in closed form, one hash and one scan per build
// whatever the number of vendor kinds (HashOnce), and the seal pushed down so far that a sealed row
// group is never read and a sealed row's values are never decoded (SealPushDown).
//
// The vendor inputs are the synthetic fixture of make_vendor_panel_fixture.py
// (fixtures/research_fields/vendor): its pytest re-runs the Python builder on them and must
// reproduce every committed byte.
#include <gtest/gtest.h>

#include <arrow/io/file.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/file_io.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/factor_break.hpp"
#include "atx/engine/research/fields/sources/nyse_calendar.hpp"
#include "atx/engine/research/fields/sources/vendor_panel.hpp"
#include "atx/engine/research/fields/vendor_fields.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::f32;
using atx::f64;
using atx::i64;
using atx::u64;
using atx::usize;

namespace {

const fs::path &vendor_dir() {
  static const fs::path dir = support::fixture_dir() / "vendor";
  return dir;
}

i64 day(i64 y, unsigned m, unsigned d) { return fields::days_from_civil(y, m, d); }

Json expected_manifest() {
  return Json::parse(support::read_bytes(vendor_dir() / "expected" / "manifest.normalized.json"));
}

// The fixture's vendor file and role, copied into `dir` (the role keeps its manifest bytes).
struct Copied {
  fs::path parquet;
  fs::path role;
  std::string role_sha256;
};

Copied copy_fixture(const fs::path &dir) {
  Copied out{dir / "th.parquet", dir / "role", {}};
  fs::copy_file(vendor_dir() / "th.parquet", out.parquet);
  fs::create_directories(out.role);
  for (const auto &entry : fs::directory_iterator(vendor_dir() / "role")) {
    fs::copy_file(entry.path(), out.role / entry.path().filename());
  }
  out.role_sha256 = support::sha256_of(support::read_bytes(out.role / "manifest.json"));
  return out;
}

fields::VendorPanelRequest union_request() {
  fields::VendorPanelRequest r;
  for (const std::string_view name : fields::vendor_field_names()) {
    r.merge(*fields::vendor_request(name));
  }
  return r;
}

void expect_payloads_equal_python(const fs::path &out) {
  for (const std::string_view name : fields::vendor_field_names()) {
    const std::string file = std::string(name) + ".f64";
    EXPECT_TRUE(support::read_bytes(out / file) ==
                support::read_bytes(vendor_dir() / "expected" / file))
        << name << ": payload bytes differ from the Python builder's";
  }
}

// A row group's tradingDate statistics [min, max] (date32 = INT32).
std::pair<i64, i64> date_bounds(const parquet::RowGroupMetaData &rg, int leaf) {
  const auto stats = rg.ColumnChunk(leaf)->statistics();
  const auto typed = std::static_pointer_cast<parquet::Int32Statistics>(stats);
  return {static_cast<i64>(typed->min()), static_cast<i64>(typed->max())};
}

struct ChunkRange {
  i64 offset{};
  i64 length{};
};

// The byte ranges of the column chunks to overwrite: every chunk of a wholly sealed row group, and
// the value chunks (all but tradingDate and securityID) of a group straddling the seal.
struct SealedLayout {
  std::vector<ChunkRange> ranges;
  int sealed_group{-1};
  int straddle_group{-1};
  u64 sealed_rows{};
  std::vector<int> key_leaves;
  std::vector<int> value_leaves;
};

SealedLayout sealed_layout(const fs::path &parquet) {
  SealedLayout out;
  const auto reader = parquet::ParquetFileReader::OpenFile(parquet.string());
  const auto meta = reader->metadata();
  const int date_leaf = meta->schema()->ColumnIndex("tradingDate");
  const int id_leaf = meta->schema()->ColumnIndex("securityID");
  out.key_leaves = {date_leaf, id_leaf};
  for (int c = 0; c < meta->num_columns(); ++c) {
    if (c != date_leaf && c != id_leaf) {
      out.value_leaves.push_back(c);
    }
  }
  const i64 seal = fields::seal_day();
  for (int g = 0; g < meta->num_row_groups(); ++g) {
    const auto rg = meta->RowGroup(g);
    const auto [lo, hi] = date_bounds(*rg, date_leaf);
    const bool sealed = lo >= seal;
    const bool straddles = lo < seal && hi >= seal;
    if (sealed) {
      out.sealed_group = g;
      out.sealed_rows += static_cast<u64>(rg->num_rows());
    }
    if (straddles) {
      out.straddle_group = g;
    }
    for (int c = 0; c < meta->num_columns(); ++c) {
      const bool key = c == date_leaf || c == id_leaf;
      if (!(sealed || (straddles && !key))) {
        continue;
      }
      const auto chunk = rg->ColumnChunk(c);
      const i64 start = chunk->has_dictionary_page() ? chunk->dictionary_page_offset()
                                                     : chunk->data_page_offset();
      out.ranges.push_back(ChunkRange{start, chunk->total_compressed_size()});
    }
  }
  return out;
}

void overwrite(const fs::path &path, const std::vector<ChunkRange> &ranges) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  for (const ChunkRange &r : ranges) {
    const std::string garbage(static_cast<usize>(r.length), '\xFF');
    f.seekp(static_cast<std::streamoff>(r.offset));
    f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
  }
}

// True when reading these columns of this row group fails (an error or an exception).
bool read_fails(const fs::path &path, int group, const std::vector<int> &leaves) {
  try {
    auto file = arrow::io::ReadableFile::Open(path.string());
    if (!file.ok()) {
      return true;
    }
    auto reader = parquet::arrow::OpenFile(*file, arrow::default_memory_pool());
    if (!reader.ok()) {
      return true;
    }
    auto table = (*reader)->ReadRowGroup(group, leaves);
    if (!table.ok()) {
      return true;
    }
    return !(*table)->ValidateFull().ok();
  } catch (const std::exception &) {
    return true;
  }
}

} // namespace

TEST(ResearchFieldsNyseCalendar, RuleSessions) {
  EXPECT_EQ(fields::weekday_of(0), 3); // 1970-01-01 was a Thursday
  EXPECT_EQ(fields::weekday_of(day(2021, 1, 4)), 0);
  // Session counts of the rule calendar (research_fields_sec.nyse_sessions, computed in Python).
  const std::vector<std::pair<i64, usize>> per_year{{2017, 251}, {2018, 251}, {2019, 252},
                                                    {2020, 253}, {2021, 252}, {2022, 251},
                                                    {2023, 250}};
  for (const auto &[year, count] : per_year) {
    EXPECT_EQ(fields::nyse_sessions(day(year, 1, 1), day(year, 12, 31)).size(), count) << year;
  }
  const auto is_session = [](i64 d) { return fields::nyse_sessions(d, d).size() == 1U; };
  // The 2021 and 2022 closures (Python nyse_holidays): weekdays that are not sessions.
  for (const i64 d : {day(2021, 1, 1), day(2021, 1, 18), day(2021, 2, 15), day(2021, 4, 2),
                      day(2021, 5, 31), day(2021, 7, 5), day(2021, 9, 6), day(2021, 11, 25),
                      day(2021, 12, 24), day(2022, 1, 17), day(2022, 2, 21), day(2022, 4, 15),
                      day(2022, 5, 30), day(2022, 6, 20), day(2022, 7, 4), day(2022, 9, 5),
                      day(2022, 11, 24), day(2022, 12, 26), day(2018, 12, 5)}) {
    EXPECT_LT(fields::weekday_of(d), 5) << fields::iso_date(d);
    EXPECT_FALSE(is_session(d)) << fields::iso_date(d);
  }
  EXPECT_TRUE(is_session(day(2021, 12, 31))); // a Saturday New Year (2022) is not observed
  EXPECT_TRUE(is_session(day(2021, 6, 18)));  // Juneteenth only from 2022
  EXPECT_FALSE(is_session(day(2021, 1, 9)));  // a Saturday
  EXPECT_TRUE(fields::nyse_sessions(day(2021, 1, 5), day(2021, 1, 4)).empty());
}

TEST(ResearchFieldsNyseCalendar, ExtendedAxisIsThePythonAxis) {
  const std::vector<i64> role{day(2021, 1, 4), day(2021, 1, 5)};
  // research_fields_price.extended_days on the same role (values computed in Python).
  const auto two = fields::extended_axis(role, 2, 0);
  ASSERT_TRUE(two.has_value()) << two.error().message();
  EXPECT_EQ(two->prefix, 2U);
  EXPECT_EQ(two->days, (std::vector<i64>{day(2020, 12, 30), day(2020, 12, 31), role[0], role[1]}));
  const auto ceq = fields::extended_axis(role, 1261, 490);
  ASSERT_TRUE(ceq.has_value()) << ceq.error().message();
  EXPECT_EQ(ceq->prefix, 1599U);
  EXPECT_EQ(ceq->days.size(), 1601U);
  EXPECT_EQ(ceq->days.front(), day(2014, 8, 27));
  EXPECT_TRUE(std::is_sorted(ceq->days.begin(), ceq->days.end()));
  const auto none = fields::extended_axis(role, 0, 0);
  ASSERT_TRUE(none.has_value()) << none.error().message();
  EXPECT_EQ(none->prefix, 0U);
  EXPECT_EQ(none->days, role);
  EXPECT_FALSE(fields::extended_axis(std::vector<i64>{}, 2, 0).has_value());
  EXPECT_FALSE(fields::extended_axis(role, 2, -1).has_value());
}

TEST(ResearchFieldsNyseCalendar, SessionCalendarPinIsThePythonPin) {
  const auto pin = fields::session_calendar_pin();
  ASSERT_TRUE(pin.has_value()) << pin.error().message();
  EXPECT_EQ(pin->rule, "nyse-rule-v1");
  EXPECT_EQ(pin->first, "1970-01-01");
  EXPECT_EQ(pin->last, fields::iso_date(fields::seal_day() - 1));
  EXPECT_EQ(pin->sessions, fields::nyse_sessions(0, fields::seal_day() - 1).size());
  // research_fields_price.session_calendar, as the Python builder recorded it in the fixture.
  const Json manifest = expected_manifest();
  for (const Json &e : manifest.at("fields")) {
    if (!e.contains("session_calendar")) {
      continue;
    }
    const Json &want = e.at("session_calendar");
    EXPECT_EQ(want.at("rule").get<std::string>(), pin->rule);
    EXPECT_EQ(want.at("first").get<std::string>(), pin->first);
    EXPECT_EQ(want.at("last").get<std::string>(), pin->last);
    EXPECT_EQ(want.at("sessions").get<u64>(), pin->sessions);
    EXPECT_EQ(want.at("sha256").get<std::string>(), pin->sha256);
  }
}

// factor-break-v1 on a seven-row axis (days 100..104, 120, 121) and 56 lines, every count and
// class derived by hand:
//   lines 0-49  factor 1 -> 0.5 -> 0.25 at rows 2 and 3, raw close flat: jump cells at rows 2 and 3
//               (|s| = ln 2), both mass sessions; each step repaired, k = 0.5;
//   line 50     factor halves at row 2 while the raw close doubles: s = -ln 2, r = ln 2, no jump
//               cell, kept_split_follow;
//   line 51     factor x1.1 at row 2: a jump cell, kept_distribution (0 < s < ln 1.25);
//   line 52     observed at rows 0 and 5 only (20 days apart), factor 0.8 at row 5: no cell, one
//               kept_gap step listed once, under row 2 (it crosses row 3 too);
//   line 53     a flat factor: |s| at the noise level, never listed;
//   line 54     factor x2 at row 2: a jump cell, repaired (k = 2);
//   line 55     factor halves at row 6: one jump cell, below the mass threshold.
// jump = [0, 0, 52, 50, 0, 0, 1]; row 2 lists 54 steps (51 repaired, 1 kept_gap, 1
// kept_split_follow, 1 kept_distribution), row 3 lists 50 (repaired).
TEST(ResearchFieldsFactorBreak, ClosedForm) {
  const std::vector<i64> days{100, 101, 102, 103, 104, 120, 121};
  constexpr usize kLines = 56;
  const usize rows = days.size();
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  std::vector<f64> factor(rows * kLines, 1.0);
  std::vector<f32> raw(rows * kLines, 10.0F);
  const auto at = [](usize r, usize j) { return r * kLines + j; };
  for (usize r = 0; r < rows; ++r) {
    for (usize j = 0; j < 50; ++j) {
      factor[at(r, j)] = r < 2 ? 1.0 : (r < 3 ? 0.5 : 0.25);
    }
    factor[at(r, 50)] = r < 2 ? 1.0 : 0.5;
    raw[at(r, 50)] = r < 2 ? 10.0F : 20.0F;
    factor[at(r, 51)] = r < 2 ? 1.0 : 1.1;
    factor[at(r, 52)] = (r == 0) ? 1.0 : (r == 5 ? 0.8 : nan);
    factor[at(r, 54)] = r < 2 ? 1.0 : 2.0;
    factor[at(r, 55)] = r < 6 ? 1.0 : 0.5;
  }
  const fields::FactorBreaks fb = fields::factor_breaks_v1(factor, raw, days, kLines);
  EXPECT_EQ(fb.jump, (std::vector<i64>{0, 0, 52, 50, 0, 0, 1}));
  ASSERT_EQ(fb.mass.size(), 2U);
  EXPECT_EQ(fb.mass[0].row, 2U);
  EXPECT_EQ(fb.mass[0].jump_cells, 52);
  EXPECT_EQ(fb.mass[0].crossing_steps, 54);
  EXPECT_EQ(fb.mass[0].by_action, (std::array<i64, 4>{51, 1, 1, 1}));
  EXPECT_EQ(fb.mass[1].row, 3U);
  EXPECT_EQ(fb.mass[1].crossing_steps, 50);
  EXPECT_EQ(fb.mass[1].by_action, (std::array<i64, 4>{50, 0, 0, 0}));
  EXPECT_EQ(fb.max_non_mass, 1);
  EXPECT_EQ(fb.max_non_mass_row, std::optional<usize>(6));
  ASSERT_EQ(fb.steps.size(), 104U);
  const auto step_of = [&fb](usize line, usize row) -> const fields::FactorBreakStep * {
    for (const auto &s : fb.steps) {
      if (s.line == line && s.row == row) {
        return &s;
      }
    }
    return nullptr;
  };
  using A = fields::FactorBreakAction;
  for (usize j = 0; j < 50; ++j) {
    for (const usize r : {usize{2}, usize{3}}) {
      const auto *s = step_of(j, r);
      ASSERT_NE(s, nullptr) << j << " " << r;
      EXPECT_TRUE(s->action == A::Repaired);
      EXPECT_EQ(s->k, 0.5);
      EXPECT_EQ(s->day, days[r]);
    }
  }
  ASSERT_NE(step_of(50, 2), nullptr);
  EXPECT_TRUE(step_of(50, 2)->action == A::KeptSplitFollow);
  ASSERT_NE(step_of(51, 2), nullptr);
  EXPECT_TRUE(step_of(51, 2)->action == A::KeptDistribution);
  ASSERT_NE(step_of(52, 5), nullptr); // listed under mass session 2, its end row is 5
  EXPECT_TRUE(step_of(52, 5)->action == A::KeptGap);
  EXPECT_EQ(step_of(53, 2), nullptr);
  ASSERT_NE(step_of(54, 2), nullptr);
  EXPECT_TRUE(step_of(54, 2)->action == A::Repaired);
  EXPECT_EQ(step_of(54, 2)->k, 2.0);
  EXPECT_EQ(step_of(55, 6), nullptr); // not on a mass session
  // Span queries: line 52's kept_gap step ends at row 5.
  const fields::KeptGaps gaps(fb, kLines);
  EXPECT_EQ(gaps.count(), 1U);
  EXPECT_TRUE(gaps.crosses(52, 4, 5));
  EXPECT_TRUE(gaps.crosses(52, 0, 6));
  EXPECT_TRUE(gaps.crosses(52, 5, 0)); // order-free
  EXPECT_FALSE(gaps.crosses(52, 0, 4));
  EXPECT_FALSE(gaps.crosses(52, 5, 6));
  EXPECT_FALSE(gaps.crosses(51, 0, 6));
  EXPECT_FALSE(gaps.crosses(kLines, 0, 6)); // no such line

  // The panel's chained factor: every repaired step divided out from its row on.
  fields::VendorPanelRequest request;
  request.price = true;
  auto assembler = fields::VendorPanel::Assembler::create(fields::ExtendedAxis{days, 0}, kLines,
                                                          request);
  ASSERT_TRUE(assembler.has_value()) << assembler.error().message();
  for (usize r = 0; r < rows; ++r) {
    for (usize j = 0; j < kLines; ++j) {
      fields::VendorObservation o;
      o.row = r;
      o.line = j;
      o.factor = factor[at(r, j)];
      o.close = raw[at(r, j)];
      o.volume = 1.0;
      if (std::isfinite(o.factor)) {
        assembler->add(o);
      }
    }
  }
  const fields::VendorPanel panel = std::move(*assembler).finish({}, {});
  EXPECT_EQ(panel.repaired_steps(), 101U);
  EXPECT_EQ(panel.kept_gap_steps(), 1U);
  for (usize r = 0; r < rows; ++r) {
    for (usize j = 0; j < 50; ++j) {
      EXPECT_EQ(panel.factor(r, j), 1.0) << r << " " << j;
    }
    EXPECT_EQ(panel.factor(r, 54), 1.0) << r;
    EXPECT_EQ(panel.factor(r, 50), factor[at(r, 50)]) << r; // kept: not divided out
    EXPECT_EQ(panel.factor(r, 51), factor[at(r, 51)]) << r;
  }
  EXPECT_TRUE(panel.gap_crosses(52, 2, 5));
  EXPECT_FALSE(panel.gap_crosses(52, 5, 6));
}

TEST(ResearchFieldsVendorPanel, DuplicateKeysAreQuarantinedEverywhere) {
  fields::VendorPanelRequest request;
  request.price = true;
  request.price_open = true;
  request.shares = true;
  request.bars = true;
  auto assembler =
      fields::VendorPanel::Assembler::create(fields::ExtendedAxis{{10, 11}, 0}, 2, request);
  ASSERT_TRUE(assembler.has_value()) << assembler.error().message();
  fields::VendorObservation o;
  o.factor = 1.0;
  o.close = 10.0F;
  o.volume = 5.0;
  o.shares = 1000;
  o.open = 9.5;
  o.high = 10.5;
  o.low = 9.0;
  for (const usize line : {usize{0}, usize{1}, usize{1}}) { // line 1's key twice
    o.line = line;
    assembler->add(o);
  }
  o.line = 0;
  o.row = 1;
  o.shares = 200'000'000; // above the A9 ceiling: withheld from day 11 on (C-81)
  assembler->add(o);
  const fields::VendorPanel p = std::move(*assembler).finish({}, {});
  EXPECT_EQ(p.stats().duplicate_keys_quarantined, 1U);
  EXPECT_EQ(p.factor(0, 0), 1.0);
  EXPECT_EQ(p.shares(0, 0), 1000.0F);
  EXPECT_EQ(p.price_open(0, 0), 9.5F);
  for (const f64 v : {p.factor(0, 1), static_cast<f64>(p.close(0, 1)),
                      static_cast<f64>(p.shares(0, 1)), static_cast<f64>(p.price_open(0, 1)),
                      static_cast<f64>(p.bar_open(0, 1)), static_cast<f64>(p.bar_high(0, 1)),
                      static_cast<f64>(p.bar_low(0, 1))}) {
    EXPECT_TRUE(std::isnan(v));
  }
  EXPECT_TRUE(std::isnan(p.shares(1, 0)));
  EXPECT_EQ(p.first_above(0), 11);
  EXPECT_EQ(p.first_above(1), fields::kNeverDay);
  EXPECT_EQ(p.stats().shares_rows_above_a9_ceiling, 1U);
  EXPECT_EQ(p.stats().shares_lines_withheld_c81, 1U);
  // shares / price_open need the price matrices.
  fields::VendorPanelRequest no_price;
  no_price.shares = true;
  EXPECT_FALSE(
      fields::VendorPanel::Assembler::create(fields::ExtendedAxis{{10}, 0}, 1, no_price)
          .has_value());
}

TEST(ResearchFieldsVendorPanel, ToF32RoundsAsNumpy) {
  EXPECT_EQ(fields::to_f32(0.1), 0.1F);
  EXPECT_EQ(fields::to_f32(-2.5), -2.5F);
  EXPECT_TRUE(std::isnan(fields::to_f32(std::numeric_limits<f64>::quiet_NaN())));
  constexpr f32 kMax = std::numeric_limits<f32>::max();
  EXPECT_EQ(fields::to_f32(static_cast<f64>(kMax) * (1.0 + 1e-9)), kMax); // below the midpoint
  EXPECT_EQ(fields::to_f32(0x1.ffffffp+127), std::numeric_limits<f32>::infinity());
  EXPECT_EQ(fields::to_f32(-1e300), -std::numeric_limits<f32>::infinity());
}

TEST(ResearchFieldsVendorPanel, RefusesAFileThatIsNotTheRoles) {
  const auto dir = support::scratch("vendor_not_roles");
  const Copied c = copy_fixture(dir);
  const auto role = fields::RoleAxes::load(c.role, c.role_sha256);
  ASSERT_TRUE(role.has_value()) << role.error().message();
  ASSERT_TRUE(fields::VendorPanel::load(c.parquet, *role, union_request()).has_value());
  // One appended byte: another file than the one the role was projected from.
  {
    std::ofstream f(c.parquet, std::ios::binary | std::ios::app);
    f.put('\0');
  }
  const auto refused = fields::VendorPanel::load(c.parquet, *role, union_request());
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().message().find("source_sha256"), std::string::npos)
      << refused.error().message();
  EXPECT_FALSE(fields::VendorPanel::load(c.parquet, *role, {}).has_value()); // nothing requested
}

// One build of the six vendor kinds hashes and scans the file once: the first vendor kind loads
// the panel with every vendor plan's request; once it is built the file is moved away and the other
// five still build from the shared panel, byte-identical to the Python builder.
TEST(ResearchFieldsVendorPanel, HashOnce) {
  const auto dir = support::scratch("vendor_hash_once");
  const Copied c = copy_fixture(dir);
  fields::BuildSpec spec;
  spec.role_dir = c.role;
  spec.role_manifest_sha256 = c.role_sha256;
  spec.output_dir = dir / "out";
  fs::create_directories(spec.output_dir);
  spec.price_source = c.parquet;
  for (const std::string_view name : fields::vendor_field_names()) {
    spec.fields.emplace_back(name);
  }
  const auto plans = fields::plan_fields(spec, nullptr);
  ASSERT_TRUE(plans.has_value()) << plans.error().message();
  ASSERT_EQ(plans->size(), 6U);
  const auto role = fields::RoleAxes::load(spec.role_dir, spec.role_manifest_sha256);
  ASSERT_TRUE(role.has_value()) << role.error().message();
  fields::BuildContext ctx{spec, *role, {}, *plans};
  const fields::FieldPlan &first = plans->front();
  ASSERT_TRUE(first.kind->build(first, ctx).has_value());
  ASSERT_TRUE(ctx.sources.vendor_panel.has_value());
  const fields::VendorPanel &panel = *ctx.sources.vendor_panel;
  // The union of the six requests (ceq_iss_5y's history, the open, the shares and the bars).
  EXPECT_TRUE(panel.request().price && panel.request().shares && panel.request().price_open &&
              panel.request().bars);
  const Json checks = expected_manifest().at("source_checks").at("price").at("source");
  EXPECT_EQ(panel.prefix(), checks.at("sessions_before_role").get<usize>());
  fs::rename(c.parquet, dir / "th.moved"); // nothing after the first kind may read the file
  for (usize i = 1; i < plans->size(); ++i) {
    const fields::FieldPlan &plan = (*plans)[i];
    const auto built = plan.kind->build(plan, ctx);
    ASSERT_TRUE(built.has_value()) << plan.name << ": " << built.error().message();
  }
  EXPECT_EQ(&*ctx.sources.vendor_panel, &panel); // never reloaded
  expect_payloads_equal_python(spec.output_dir);
  // The teeth: without the shared panel a vendor kind must read the (now absent) file.
  fs::create_directories(dir / "again");
  spec.output_dir = dir / "again";
  fields::BuildContext fresh{spec, *role, {}, *plans};
  EXPECT_FALSE((*plans)[1].kind->build((*plans)[1], fresh).has_value());
}

// The seal pushed down: the wholly sealed row group's column chunks and the value chunks of the
// group straddling the seal are overwritten with 0xFF (the footer is kept), so any decode of a
// sealed row's value fails. The panel still loads, its sealed rows counted exactly as the Python
// reader counts the rows it decodes and drops, and the six fields stay byte-identical.
TEST(ResearchFieldsVendorPanel, SealPushDown) {
  const auto dir = support::scratch("vendor_seal");
  const Copied c = copy_fixture(dir);
  const SealedLayout layout = sealed_layout(c.parquet);
  ASSERT_GE(layout.sealed_group, 0);
  ASSERT_GE(layout.straddle_group, 0);
  ASSERT_FALSE(layout.ranges.empty());
  // Before: every row group decodes.
  EXPECT_FALSE(read_fails(c.parquet, layout.sealed_group, layout.value_leaves));
  overwrite(c.parquet, layout.ranges);
  // The teeth: a reader that decodes the sealed rows (the Python scan) now fails.
  EXPECT_TRUE(read_fails(c.parquet, layout.sealed_group, layout.key_leaves));
  EXPECT_TRUE(read_fails(c.parquet, layout.sealed_group, layout.value_leaves));
  EXPECT_TRUE(read_fails(c.parquet, layout.straddle_group, layout.value_leaves));
  EXPECT_FALSE(read_fails(c.parquet, layout.straddle_group, layout.key_leaves));
  // The role pins the corrupted file (it is "the file the role was projected from").
  const Json python = expected_manifest();
  const std::string old_sha =
      python.at("fields").at(0).at("sources").at(0).at("sha256").get<std::string>();
  const auto digest = fields::digest_file(c.parquet);
  ASSERT_TRUE(digest.has_value()) << digest.error().message();
  std::string manifest = support::read_bytes(c.role / "manifest.json");
  const auto at = manifest.find(old_sha);
  ASSERT_NE(at, std::string::npos);
  manifest.replace(at, old_sha.size(), digest->sha256);
  support::write_bytes(c.role / "manifest.json", manifest);
  const auto role = fields::RoleAxes::load(c.role, support::sha256_of(manifest));
  ASSERT_TRUE(role.has_value()) << role.error().message();

  const auto panel = fields::VendorPanel::load(c.parquet, *role, union_request());
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  const fields::VendorScanStats &s = panel->stats();
  EXPECT_EQ(s.row_groups_pruned_sealed, 1U);
  EXPECT_EQ(s.rows_in_row_groups_pruned_sealed, layout.sealed_rows);
  const Json checks = expected_manifest().at("source_checks").at("price").at("source");
  EXPECT_EQ(s.rows_sealed_dropped, checks.at("rows_on_or_after_seal_skipped").get<u64>());
  EXPECT_GT(s.rows_sealed_dropped, s.rows_in_row_groups_pruned_sealed); // straddle: by key
  EXPECT_EQ(s.row_groups_keys_decoded + s.row_groups_pruned_sealed +
                s.row_groups_pruned_outside_window,
            s.row_groups);
  EXPECT_LT(s.row_groups_values_decoded, s.row_groups_keys_decoded); // the straddling group
  EXPECT_EQ(s.rows_selected, checks.at("rows_selected").get<u64>());
  const fs::path out = dir / "out";
  fs::create_directories(out);
  for (const std::string_view name : fields::vendor_field_names()) {
    const auto built = fields::build_vendor_field(name, *panel, *role, out);
    ASSERT_TRUE(built.has_value()) << name << ": " << built.error().message();
  }
  expect_payloads_equal_python(out);
}
