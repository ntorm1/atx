// The holdings fields ported from research_fields_holdings.py (P9 lane A4, migration slice 5):
// ftd_shares_ratio21 and regsho_threshold_days63.
//
// ResearchFieldsHoldingsFixture.*: fixtures/research_fields_holdings holds lane T2's re-dated
// synthetic stages and role, this run's shares_out input and what the EXISTING Python builder
// wrote from them (make_holdings_fixture.py; its pytest re-runs the Python and must reproduce
// every committed byte). The engine reads the same inputs: every payload byte, every coverage
// number (bit for bit), the NaN reasons, every extra key, the stage files read and the Python
// source_checks.holdings block of the stage must equal the Python manifest's, directly and through
// a registry build of the committed field registry with the two rows flipped to kind engine.
//
// ResearchFieldsHoldingsProbe.*: one planted-leak probe per builder (migration plan section 3.4).
// A fail planted on settlement date D (FTD) or an on-list row planted on list date D (Reg SHO) may
// change no row whose clock cannot see it; the honest kernel's first changed row is exactly the
// first that can. The same kernel run one session late (the next session's clock: a planted leak)
// changes an earlier row, so the probe has teeth.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_stats.hpp"
#include "atx/engine/research/fields/holdings_fields.hpp"
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/research_fields_cli.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/run_payload.hpp"
#include "atx/engine/research/fields/session_clock.hpp"
#include "atx/engine/research/fields/sources/nyse_calendar.hpp"
#include "atx/engine/research/fields/sources/short_side.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::f64;
using atx::i64;
using atx::u64;
using atx::u8;
using atx::usize;

namespace {

constexpr i64 kDayNs = 86'400'000'000'000LL;
constexpr i64 kMarkNs = 22LL * 3'600'000'000'000LL;
constexpr i64 kHourNs = 3'600'000'000'000LL;

const fs::path &holdings_dir() {
  static const fs::path dir = support::fixture_dir() / ".." / "research_fields_holdings";
  return dir;
}

Json expected_manifest() {
  return Json::parse(
      support::read_bytes(holdings_dir() / "expected" / "manifest.normalized.json"));
}

const Json &entry_of(const Json &manifest, std::string_view name) {
  for (const Json &e : manifest.at("fields")) {
    if (e.at("name").get<std::string>() == name) {
      return e;
    }
  }
  throw std::runtime_error("no manifest entry " + std::string(name));
}

fields::RoleAxes fixture_role() {
  const std::string sha =
      support::sha256_of(support::read_bytes(holdings_dir() / "role" / "manifest.json"));
  return fields::RoleAxes::load(holdings_dir() / "role", sha).value();
}

fields::StageRequest stage_request(const std::string &key) {
  const fs::path dir = holdings_dir() / key;
  return fields::StageRequest{dir, support::sha256_of(support::read_bytes(dir / "manifest.json"))};
}

fields::FileRequest shares_out_request() {
  const fs::path path = holdings_dir() / "inputs" / "shares_out.f64";
  return fields::FileRequest{path, support::sha256_of(support::read_bytes(path))};
}

// A JSON number or null against an optional double, bit for bit.
void expect_number(const Json &j, const std::optional<double> &got, const std::string &what) {
  if (j.is_null()) {
    EXPECT_FALSE(got.has_value()) << what;
    return;
  }
  ASSERT_TRUE(got.has_value()) << what;
  EXPECT_TRUE(support::same_bits(j.get<double>(), *got))
      << what << ": " << j.dump() << " vs " << *got;
}

void expect_coverage(const Json &want, const fields::Coverage &c, const std::string &name) {
  EXPECT_EQ(want.at("member_cells").get<u64>(), c.member_cells) << name;
  EXPECT_EQ(want.at("finite_member_cells").get<u64>(), c.finite_member_cells) << name;
  expect_number(want.at("finite_member_frac"),
                fields::rounded_fraction(c.finite_member_cells, c.member_cells),
                name + " finite_member_frac");
  EXPECT_EQ(want.at("finite_cells_all").get<u64>(), c.finite_cells_all) << name;
  const Json &score = want.at("score_window");
  EXPECT_EQ(score.at("member_cells").get<u64>(), c.score_member_cells) << name;
  EXPECT_EQ(score.at("finite_member_cells").get<u64>(), c.score_finite_member_cells) << name;
  const Json &years = want.at("per_year");
  ASSERT_EQ(years.size(), c.per_year.size()) << name;
  for (const auto &y : c.per_year) {
    const Json &w = years.at(std::to_string(y.year));
    EXPECT_EQ(w.at("member_cells").get<u64>(), y.member_cells) << name << " " << y.year;
    EXPECT_EQ(w.at("finite_member_cells").get<u64>(), y.finite_member_cells)
        << name << " " << y.year;
  }
  expect_number(want.at("member_finite_min"), c.member_finite_min, name + " min");
  expect_number(want.at("member_finite_max"), c.member_finite_max, name + " max");
  expect_number(want.at("member_finite_mean"), c.member_finite_mean, name + " mean");
  const Json &q = want.at("member_finite_quantiles");
  ASSERT_EQ(q.is_null(), !c.member_finite_quantiles.has_value()) << name;
  if (c.member_finite_quantiles) {
    const auto &g = *c.member_finite_quantiles;
    expect_number(q.at("p0.1"), g.p0_1, name + " p0.1");
    expect_number(q.at("p1"), g.p1, name + " p1");
    expect_number(q.at("p50"), g.p50, name + " p50");
    expect_number(q.at("p99"), g.p99, name + " p99");
    expect_number(q.at("p99.9"), g.p99_9, name + " p99.9");
  }
}

// The keys every holdings manifest entry has; the rest of a Python entry are its extras.
const std::set<std::string> &common_keys() {
  static const std::set<std::string> keys{
      "caveats",    "clock",  "coverage", "definition",      "dtype",         "file",
      "formula_id", "layout", "name",     "non_pit_aspects", "point_in_time", "sha256",
      "shape",      "source_columns",     "sources",         "staleness",     "units"};
  return keys;
}

// The Python entry's sources split into the stage manifests (and this run's shares_out) the
// engine's entry names, and the stage files the engine names in source_checks.stage_files.
struct SplitSources {
  Json pinned = Json::array();
  Json files = Json::array();
};

SplitSources split_sources(const Json &python_sources) {
  SplitSources out;
  for (const Json &s : python_sources) {
    const std::string path = s.at("path").get<std::string>();
    const bool pinned = path.ends_with("/manifest.json") || path.ends_with("/shares_out.f64");
    Json record{{"bytes", s.at("bytes")}, {"sha256", s.at("sha256")}};
    (pinned ? out.pinned : out.files).push_back(std::move(record));
  }
  return out;
}

Json digests_of(const Json &records) {
  Json out = Json::array();
  for (const Json &r : records) {
    out.push_back(Json{{"bytes", r.at("bytes")}, {"sha256", r.at("sha256")}});
  }
  return out;
}

Json digests_of(const std::vector<fields::SourceRecord> &records) {
  Json out = Json::array();
  for (const fields::SourceRecord &r : records) {
    out.push_back(Json{{"bytes", r.bytes}, {"sha256", r.sha256}});
  }
  return out;
}

// One built field against the Python entry and the Python source_checks.holdings block.
void expect_field(const Json &manifest, std::string_view name, const fields::HoldingsField &built,
                  const fs::path &out, const std::string &check_key) {
  const std::string n(name);
  const Json &e = entry_of(manifest, name);
  const std::string engine = support::read_bytes(out / (n + ".f64"));
  const std::string python = support::read_bytes(holdings_dir() / "expected" / (n + ".f64"));
  EXPECT_EQ(engine.size(), python.size()) << n;
  EXPECT_TRUE(engine == python) << n << ": payload bytes differ from the Python builder's";
  EXPECT_EQ(built.field.sha256, e.at("sha256").get<std::string>()) << n;
  EXPECT_EQ(built.field.bytes, manifest.at("files").at(n + ".f64").at("bytes").get<u64>()) << n;
  expect_coverage(e.at("coverage"), built.field.coverage, n);
  const SplitSources split = split_sources(e.at("sources"));
  EXPECT_EQ(digests_of(built.sources), split.pinned) << n;
  EXPECT_EQ(digests_of(built.source_checks.at("stage_files")), split.files) << n;
  std::set<std::string> extras;
  for (auto it = e.begin(); it != e.end(); ++it) {
    if (common_keys().count(it.key()) == 0) {
      extras.insert(it.key());
      ASSERT_TRUE(built.extra.contains(it.key())) << n << ": no extra " << it.key();
      EXPECT_EQ(built.extra.at(it.key()), it.value()) << n << ": " << it.key();
    }
  }
  EXPECT_EQ(built.extra.size(), extras.size()) << n << ": " << built.extra.dump();
  const Json &holdings = manifest.at("source_checks").at("holdings");
  EXPECT_EQ(built.source_checks.at(check_key), holdings.at(check_key)) << n;
  for (auto it = built.source_checks.at("stage").begin();
       it != built.source_checks.at("stage").end(); ++it) {
    EXPECT_EQ(it.value(), holdings.at("stages").at(it.key())) << n << ": stage " << it.key();
  }
}

fields::FieldRegistry flipped_registry() {
  const fs::path path =
      support::fixture_dir() / ".." / ".." / ".." / "tools" / "field_registry.json";
  Json doc = Json::parse(support::read_bytes(path));
  usize flipped = 0;
  for (Json &row : doc.at("fields")) {
    const std::string name = row.at("name").get<std::string>();
    if (name == "ftd_shares_ratio21" || name == "regsho_threshold_days63") {
      row["kind"] = "engine";
      row["builder"] = name;
      ++flipped;
    }
  }
  EXPECT_EQ(flipped, 2U);
  return fields::parse_field_registry(doc.dump()).value();
}

fields::BuildSpec fixture_spec(const fs::path &out) {
  fields::BuildSpec spec;
  spec.role_dir = holdings_dir() / "role";
  spec.role_manifest_sha256 =
      support::sha256_of(support::read_bytes(holdings_dir() / "role" / "manifest.json"));
  spec.output_dir = out;
  spec.fields = {"ftd_shares_ratio21", "regsho_threshold_days63"};
  spec.ftd = stage_request("ftd");
  spec.regsho_threshold = stage_request("regsho_threshold");
  spec.security_master = stage_request("security_master");
  spec.shares_out = shares_out_request();
  return spec;
}

// ---- probe support ------------------------------------------------------------------------------

i64 day_of(atx::i64 year, atx::u32 month, atx::u32 day) {
  return fields::days_from_civil(year, month, day);
}

bool weekday(i64 d) { return fields::weekday_of(d) < 5; }

std::vector<i64> weekdays(i64 first, i64 last) {
  std::vector<i64> out;
  for (i64 d = first; d <= last; ++d) {
    if (weekday(d)) {
      out.push_back(d);
    }
  }
  return out;
}

// A two-line role (ids 11, 22) on `days`, every cell a member.
struct ProbeRole {
  fs::path dir;
  fields::RoleAxes role;
};

ProbeRole probe_role(const std::string &name, const std::vector<i64> &days) {
  support::TinyRole tiny;
  tiny.days = days;
  tiny.ids = {11, 22};
  tiny.member.assign(days.size() * 2, 1);
  tiny.present.assign(days.size() * 2, 1);
  tiny.volume.assign(days.size() * 2, 1e4);
  const fs::path dir = support::scratch(name) / "role";
  const std::string sha = support::write_role(dir, tiny);
  return ProbeRole{dir, fields::RoleAxes::load(dir, sha).value()};
}

fields::RunPayload flat_shares_out(const fs::path &dir, const fields::RoleAxes &role) {
  const std::vector<f64> values(role.dates() * role.instruments(), 1e6);
  const std::string blob = support::bytes_of(values);
  const fs::path path = dir / "shares_out.f64";
  support::write_bytes(path, blob);
  return fields::RunPayload::load(fields::FileRequest{path, support::sha256_of(blob)}, role)
      .value();
}

// The index of the first row whose bits differ (nullopt: equal payloads).
std::optional<usize> first_change(const std::vector<f64> &a, const std::vector<f64> &b,
                                  usize columns) {
  for (usize i = 0; i < a.size(); ++i) {
    if (std::bit_cast<u64>(a[i]) != std::bit_cast<u64>(b[i])) {
      return i / columns;
    }
  }
  return std::nullopt;
}

// The session marks one session late: row t sees session t's own mark (a planted leak).
std::vector<i64> late_marks(const fields::RoleAxes &role) {
  std::vector<i64> out;
  for (const i64 d : role.days()) {
    out.push_back(d * kDayNs + kMarkNs);
  }
  return out;
}

} // namespace

// ---- fixture identity ------------------------------------------------------------------------

TEST(ResearchFieldsHoldingsFixture, FormulaFingerprintsAreTheRegistrys) {
  const fs::path path =
      support::fixture_dir() / ".." / ".." / ".." / "tools" / "field_registry.json";
  const Json doc = Json::parse(support::read_bytes(path));
  usize seen = 0;
  for (const Json &row : doc.at("fields")) {
    const std::string name = row.at("name").get<std::string>();
    const fields::FieldSpec *spec = nullptr;
    if (name == "ftd_shares_ratio21") {
      spec = &fields::ftd_shares_ratio21_spec();
    } else if (name == "regsho_threshold_days63") {
      spec = &fields::regsho_threshold_days63_spec();
    }
    if (spec == nullptr) {
      continue;
    }
    ++seen;
    EXPECT_EQ(fields::formula_sha256(*spec).value(), row.at("formula_sha256").get<std::string>())
        << name;
    EXPECT_EQ(spec->group, row.at("options").at("group").get<std::string>()) << name;
    const Json &text = row.at("spec_text");
    EXPECT_EQ(spec->definition.units, text.at("units").get<std::string>()) << name;
    EXPECT_EQ(spec->definition.source_columns,
              text.at("source_columns").get<std::vector<std::string>>())
        << name;
  }
  EXPECT_EQ(seen, 2U);
}

TEST(ResearchFieldsHoldingsFixture, FtdIsThePythonField) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role();
  auto stage = fields::PinnedStage::open(stage_request("ftd"), fields::kFtdStage);
  ASSERT_TRUE(stage.has_value()) << stage.error().message();
  const auto shares = fields::RunPayload::load(shares_out_request(), role);
  ASSERT_TRUE(shares.has_value()) << shares.error().message();
  EXPECT_EQ(shares->source().sha256,
            manifest.at("files").at("shares_out.f64").at("sha256").get<std::string>());
  const auto out = support::scratch("holdings_fixture_ftd");
  const auto built = fields::build_ftd_shares_ratio21(*stage, *shares, role, out);
  ASSERT_TRUE(built.has_value()) << built.error().message();
  expect_field(manifest, "ftd_shares_ratio21", *built, out, "ftd");
}

TEST(ResearchFieldsHoldingsFixture, RegshoIsThePythonField) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role();
  auto lists = fields::PinnedStage::open(stage_request("regsho_threshold"), fields::kRegshoStage);
  ASSERT_TRUE(lists.has_value()) << lists.error().message();
  auto names =
      fields::PinnedStage::open(stage_request("security_master"), fields::kSecurityMasterStage);
  ASSERT_TRUE(names.has_value()) << names.error().message();
  const auto out = support::scratch("holdings_fixture_regsho");
  const auto built = fields::build_regsho_threshold_days63(*lists, *names, role, out);
  ASSERT_TRUE(built.has_value()) << built.error().message();
  expect_field(manifest, "regsho_threshold_days63", *built, out, "regsho_threshold");
}

// The verification flow for root: the committed field registry with the two rows flipped to kind
// engine, built by the registry path. Every Python entry key but sources equals the engine
// entry's (the engine adds formula_sha256 and its producer); the engine's sources are the pinned
// stage manifests and shares_out of the Python sources.
TEST(ResearchFieldsHoldingsFixture, RegistryBuildEntriesAreThePythonEntries) {
  const Json python = expected_manifest();
  const auto registry = flipped_registry();
  const auto dir = support::scratch("holdings_fixture_registry");
  fs::create_directories(dir / "out");
  const fields::ProducerIdentity producer{std::string(64, 'e'), "feedface", "Debug"};
  const auto built = fields::build_registry_fields(fixture_spec(dir / "out"), registry,
                                                   std::string(64, 'a'), producer);
  ASSERT_TRUE(built.has_value()) << built.error().message();
  EXPECT_EQ(built->built, 2U);
  const Json manifest = Json::parse(built->manifest);
  ASSERT_EQ(manifest.at("fields").size(), 2U);
  for (const Json &e : manifest.at("fields")) {
    const std::string name = e.at("name").get<std::string>();
    const Json &want = entry_of(python, name);
    for (auto it = want.begin(); it != want.end(); ++it) {
      ASSERT_TRUE(e.contains(it.key())) << name << ": no key " << it.key();
      if (it.key() == "sources") {
        EXPECT_EQ(digests_of(e.at("sources")), split_sources(*it).pinned) << name;
        continue;
      }
      EXPECT_EQ(e.at(it.key()), it.value()) << name << ": " << it.key();
    }
    for (auto it = e.begin(); it != e.end(); ++it) {
      EXPECT_TRUE(want.contains(it.key()) || it.key() == "producer" ||
                  it.key() == "formula_sha256")
          << name << ": " << it.key();
    }
    EXPECT_EQ(manifest.at("files").at(name + ".f64"), python.at("files").at(name + ".f64"));
  }
}

// A spec without a kind's inputs is refused at parse; a wrong stage pin before any output.
TEST(ResearchFieldsHoldingsFixture, MissingOrWrongInputsAreRefused) {
  const auto dir = support::scratch("holdings_fixture_refused");
  fs::create_directories(dir / "out");
  fields::BuildSpec spec = fixture_spec(dir / "out");
  spec.shares_out.reset();
  const auto no_shares = fields::plan_fields(spec, nullptr);
  ASSERT_FALSE(no_shares.has_value());
  EXPECT_NE(no_shares.error().message().find("shares_out"), std::string::npos);
  spec = fixture_spec(dir / "out");
  spec.security_master.reset();
  EXPECT_FALSE(fields::plan_fields(spec, nullptr).has_value());
  spec = fixture_spec(dir / "out");
  spec.ftd->manifest_sha256 = std::string(64, '0');
  const fields::ProducerIdentity producer{std::string(64, 'e'), "feedface", "Debug"};
  const auto wrong = fields::build_fields(spec, std::string(64, 'a'), producer);
  ASSERT_FALSE(wrong.has_value());
  EXPECT_NE(wrong.error().message().find("manifest SHA-256 does not match"), std::string::npos)
      << wrong.error().message();
  EXPECT_FALSE(fs::exists(dir / "out" / "ftd_shares_ratio21.f64"));
  spec = fixture_spec(dir / "out");
  spec.shares_out->sha256 = std::string(64, '0');
  const auto wrong_shares = fields::build_fields(spec, std::string(64, 'a'), producer);
  ASSERT_FALSE(wrong_shares.has_value());
  EXPECT_NE(wrong_shares.error().message().find("shares_out"), std::string::npos)
      << wrong_shares.error().message();
}

// ---- planted-leak probes ----------------------------------------------------------------------

TEST(ResearchFieldsHoldingsProbe, FtdLookAheadProbeHasTeeth) {
  const std::vector<i64> days = weekdays(day_of(2023, 3, 1), day_of(2023, 4, 28));
  const ProbeRole pr = probe_role("holdings_probe_ftd", days);
  const fields::RoleAxes &role = pr.role;
  const fields::RunPayload shares = flat_shares_out(pr.dir.parent_path(), role);
  // every weekday settles; each date is published three days later at 00:00 UTC
  const auto rows_of = [](i64 plant_day, f64 plant) {
    fields::FtdRows rows;
    for (const i64 d : weekdays(day_of(2023, 1, 2), day_of(2023, 4, 28))) {
      for (const i64 sid : {11, 22}) {
        rows.settlement_day.push_back(d);
        rows.security_id.push_back(sid);
        const f64 regular = 100.0 + static_cast<f64>(sid);
        rows.quantity.push_back(d == plant_day && sid == 11 ? plant : regular);
        rows.available_ns.push_back((d + 3) * kDayNs);
        rows.collision.push_back(0);
      }
    }
    return rows;
  };
  const auto payload = [&](const fields::FtdRows &rows, std::span<const i64> marks) {
    fields::FtdIndexer indexer(role);
    indexer.add(rows, "year=2023/ftd.parquet");
    const fields::FtdPanel panel = indexer.finish().value();
    std::vector<f64> out;
    std::vector<f64> row(role.instruments());
    fields::FtdReasons reasons;
    for (usize t = 0; t < role.dates(); ++t) {
      fields::ftd_row(panel, role, shares, t, marks[t], role.member_row(t), reasons, row);
      out.insert(out.end(), row.begin(), row.end());
    }
    return out;
  };
  const i64 plant_day = day_of(2023, 3, 15);
  const i64 plant_visible = (plant_day + 3) * kDayNs; // its available_at
  const std::vector<i64> honest = fields::previous_session_marks(role.days());
  const auto base = payload(rows_of(plant_day, 116.0), honest);
  const auto planted = payload(rows_of(plant_day, 1e9), honest);
  usize visible_row = 0; // the first row whose clock sees the plant: available_at < mark(t-1)
  while (visible_row < role.dates() && !(plant_visible < honest[visible_row])) {
    ++visible_row;
  }
  ASSERT_LT(visible_row, role.dates());
  const auto honest_change = first_change(base, planted, role.instruments());
  ASSERT_TRUE(honest_change.has_value()) << "the plant never reaches the field";
  EXPECT_EQ(*honest_change, visible_row) << "no row before the plant's clock may change";
  const std::vector<i64> late = late_marks(role);
  const auto leaky = first_change(payload(rows_of(plant_day, 116.0), late),
                                  payload(rows_of(plant_day, 1e9), late), role.instruments());
  ASSERT_TRUE(leaky.has_value());
  EXPECT_LT(*leaky, visible_row) << "the probe must catch a clock one session late";
}

TEST(ResearchFieldsHoldingsProbe, RegshoLookAheadProbeHasTeeth) {
  const std::vector<i64> days = weekdays(day_of(2023, 3, 1), day_of(2023, 6, 30));
  const ProbeRole pr = probe_role("holdings_probe_regsho", days);
  const fields::RoleAxes &role = pr.role;
  // nasdaq lists every weekday from December, each published the next day 06:00 UTC
  fields::RegshoListRows lists;
  for (const i64 d : weekdays(day_of(2022, 12, 1), day_of(2023, 6, 30))) {
    lists.market.push_back(0);
    lists.list_day.push_back(d);
    lists.status.emplace_back("list");
    lists.available_ns.push_back((d + 1) * kDayNs + 6 * kHourNs);
  }
  // line 11 is NNM (nasdaq), its FINRA name rows every 15 days, visible at dissemination 22:00
  fields::FinraNameRows names;
  for (i64 d = day_of(2022, 12, 1); d <= day_of(2023, 6, 30); d += 15) {
    names.security_id.push_back(11);
    names.available_ns.push_back(d * kDayNs + kMarkNs);
    names.dissemination_day.push_back(d);
    names.market_class.emplace_back("NNM");
  }
  const fields::ListingRows listing = fields::index_listing_rows(names, role);
  const auto threshold_of = [](std::optional<i64> plant_day) {
    fields::ThresholdRows rows;
    for (const i64 d : weekdays(day_of(2023, 4, 3), day_of(2023, 4, 14))) {
      rows.list_day.push_back(d);
      rows.market.push_back(0);
      rows.security_id.push_back(11);
      rows.on_list.push_back(1);
      rows.available_ns.push_back((d + 1) * kDayNs + 6 * kHourNs);
    }
    if (plant_day) {
      rows.list_day.push_back(*plant_day);
      rows.market.push_back(0);
      rows.security_id.push_back(11);
      rows.on_list.push_back(1);
      rows.available_ns.push_back((*plant_day + 1) * kDayNs + 6 * kHourNs);
    }
    return rows;
  };
  // the kernel; `shift` 1 computes row t as the honest row of session t + 1 (a planted leak)
  const auto payload = [&](const fields::ThresholdRows &rows, usize shift) {
    fields::RegshoIndexer indexer(lists, role);
    indexer.add(rows, "year=2023/threshold.parquet");
    const fields::RegshoPanel panel = indexer.finish();
    auto latest =
        fields::LatestRows::create(listing.available_ns, listing.column, role.instruments())
            .value();
    const std::vector<i64> marks = fields::previous_session_marks(role.days());
    std::vector<f64> out;
    std::vector<f64> row(role.instruments());
    fields::RegshoReasons reasons;
    std::vector<u64> by_market(fields::kRegshoMarkets.size(), 0);
    for (usize t = 0; t < role.dates(); ++t) {
      const usize s = std::min(t + shift, role.dates() - 1);
      const auto visible = latest.before(marks[s]);
      fields::regsho_row(panel, listing, visible, role, s, marks[s], role.member_row(s), reasons,
                         by_market, row);
      out.insert(out.end(), row.begin(), row.end());
    }
    return out;
  };
  const i64 plant_day = day_of(2023, 5, 10);
  const auto base = payload(threshold_of(std::nullopt), 0);
  const auto planted = payload(threshold_of(plant_day), 0);
  // the plant's list file is visible from the mark after plant_day + 1 06:00, and the window ends
  // at session t - 2: the first row that can count it is the plant's session + 2.
  const auto at = std::lower_bound(days.begin(), days.end(), plant_day);
  const usize plant_row = static_cast<usize>(at - days.begin());
  const auto honest_change = first_change(base, planted, role.instruments());
  ASSERT_TRUE(honest_change.has_value()) << "the plant never reaches the field";
  EXPECT_EQ(*honest_change, plant_row + 2) << "no row before the plant's clock may change";
  EXPECT_EQ(base[(plant_row + 2) * 2], 10.0); // the ten April on-list days, still in the window
  EXPECT_EQ(planted[(plant_row + 2) * 2], 11.0);
  const auto leaky = first_change(payload(threshold_of(std::nullopt), 1),
                                  payload(threshold_of(plant_day), 1), role.instruments());
  ASSERT_TRUE(leaky.has_value());
  EXPECT_LT(*leaky, plant_row + 2) << "the probe must catch a clock one session late";
}
