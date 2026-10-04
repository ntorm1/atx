// Byte identity of the six vendor-panel fields (P9 lane A3, migration slice 4) against the EXISTING
// Python builders (research_fields_price.py, research_fields_ohlc.py).
//
// fixtures/research_fields/vendor holds a synthetic vendor TickerHistory3 parquet, the role
// projected from it, and what prepare_research_fields.py wrote from them for the six fields in one
// run (make_vendor_panel_fixture.py; its pytest re-runs the Python and must reproduce every
// committed byte). Here the engine reads the same inputs: the panel's read statistics, every
// payload byte, every coverage number (bit for bit), the sources, the spec fingerprint and every
// entry key of the six fields must equal the Python manifest's, through the row kinds and through a
// registry build of the committed field registry with the six rows flipped to kind engine.
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
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
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/research_fields_cli.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/vendor_panel.hpp"
#include "atx/engine/research/fields/vendor_fields.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::u64;
using atx::usize;

namespace {

const fs::path &vendor_dir() {
  static const fs::path dir = support::fixture_dir() / "vendor";
  return dir;
}

Json expected_manifest() {
  return Json::parse(support::read_bytes(vendor_dir() / "expected" / "manifest.normalized.json"));
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
  const std::string sha = support::sha256_of(support::read_bytes(vendor_dir() / "role" /
                                                                  "manifest.json"));
  return fields::RoleAxes::load(vendor_dir() / "role", sha).value();
}

fields::VendorPanelRequest union_request() {
  fields::VendorPanelRequest r;
  for (const std::string_view name : fields::vendor_field_names()) {
    r.merge(*fields::vendor_request(name));
  }
  return r;
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

void expect_sources(const Json &want, const std::vector<fields::SourceRecord> &got,
                    const std::string &name) {
  ASSERT_EQ(want.size(), got.size()) << name;
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(want.at(i).at("sha256").get<std::string>(), got[i].sha256) << name << " " << i;
    EXPECT_EQ(want.at(i).at("bytes").get<u64>(), got[i].bytes) << name << " " << i;
  }
}

// The keys every manifest entry has; the rest of a Python entry are its producer's extras.
const std::set<std::string> &common_keys() {
  static const std::set<std::string> keys{
      "caveats", "clock",          "coverage", "definition",      "dtype",         "file",
      "formula_id", "formula_sha256", "layout", "min_history",   "name",          "sha256",
      "non_pit_aspects", "point_in_time", "shape", "source_columns", "sources", "staleness",
      "units"};
  return keys;
}

// The engine panel's scan block against a Python group's "source" statistics: every Python key
// (its rule text aside) under the engine's name for it.
void expect_scan(const Json &python, const Json &engine, const std::string &group) {
  for (auto it = python.begin(); it != python.end(); ++it) {
    if (it.key() == "rule") {
      continue; // the Python group's rule text; the engine records the clock per field
    }
    const std::string key = it.key() == "rows_scanned"                    ? "rows_in_file"
                            : it.key() == "rows_on_or_after_seal_skipped" ? "rows_sealed_dropped"
                                                                          : it.key();
    ASSERT_TRUE(engine.contains(key)) << group << ": " << key;
    EXPECT_EQ(engine.at(key), it.value()) << group << ": " << key;
  }
}

// One field built from `panel` against the Python entry: payload bytes, sha256, coverage, sources,
// fingerprint, formula id, minimum history and every extra key (guarded / outside-domain / order /
// missing-bar counts, domain, lag, session calendar).
void expect_field(const Json &manifest, std::string_view name, const fields::VendorPanel &panel,
                  const fields::RoleAxes &role, const fs::path &out) {
  const std::string n(name);
  const auto built = fields::build_vendor_field(name, panel, role, out);
  ASSERT_TRUE(built.has_value()) << n << ": " << built.error().message();
  const Json &e = entry_of(manifest, name);
  const std::string engine = support::read_bytes(out / (n + ".f64"));
  const std::string python = support::read_bytes(vendor_dir() / "expected" / (n + ".f64"));
  EXPECT_EQ(engine.size(), python.size()) << n;
  EXPECT_TRUE(engine == python) << n << ": payload bytes differ from the Python builder's";
  EXPECT_EQ(built->field.sha256, e.at("sha256").get<std::string>()) << n;
  EXPECT_EQ(built->field.bytes, manifest.at("files").at(n + ".f64").at("bytes").get<u64>()) << n;
  expect_coverage(e.at("coverage"), built->field.coverage, n);
  expect_sources(e.at("sources"), built->sources, n);
  const fields::FieldSpec &spec = *fields::vendor_field_spec(name);
  EXPECT_EQ(fields::formula_sha256(spec).value(), e.at("formula_sha256").get<std::string>()) << n;
  EXPECT_EQ(spec.formula_id, e.at("formula_id").get<std::string>()) << n;
  EXPECT_EQ(spec.min_history, e.at("min_history").get<std::string>()) << n;
  std::set<std::string> extras;
  for (auto it = e.begin(); it != e.end(); ++it) {
    if (common_keys().count(it.key()) == 0) {
      extras.insert(it.key());
      ASSERT_TRUE(built->extra.contains(it.key())) << n << ": no extra " << it.key();
      EXPECT_EQ(built->extra.at(it.key()), it.value()) << n << ": " << it.key();
    }
  }
  EXPECT_EQ(built->extra.size(), extras.size()) << n << ": " << built->extra.dump();
}

fields::FieldRegistry flipped_registry() {
  const fs::path path =
      support::fixture_dir() / ".." / ".." / ".." / "tools" / "field_registry.json";
  Json doc = Json::parse(support::read_bytes(path));
  usize flipped = 0;
  for (Json &row : doc.at("fields")) {
    const std::string name = row.at("name").get<std::string>();
    if (fields::vendor_field_spec(name) != nullptr) {
      row["kind"] = "engine";
      row["builder"] = name;
      ++flipped;
    }
  }
  EXPECT_EQ(flipped, 6U);
  return fields::parse_field_registry(doc.dump()).value();
}

} // namespace

// The union panel of the six fields (ceq_iss_5y's history, the open, the shares, the bars) reads
// the file as the Python price module's panel does: its statistics are the Python "price" source
// checks.
TEST(ResearchFieldsVendorFixture, UnionPanelIsThePythonPricePanel) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role();
  const auto panel = fields::VendorPanel::load(vendor_dir() / "th.parquet", role, union_request());
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  const Json &python = manifest.at("source_checks").at("price");
  const auto out = support::scratch("vendor_fixture_union");
  const auto built = fields::build_vendor_field("ret_overnight", *panel, role, out);
  ASSERT_TRUE(built.has_value()) << built.error().message();
  const Json &checks = built->source_checks;
  EXPECT_EQ(checks.at("clock"), python.at("clock"));
  EXPECT_EQ(checks.at("lag_sessions"), python.at("lag_sessions"));
  expect_scan(python.at("source"), checks.at("source"), "price");
  const fields::VendorScanStats &s = panel->stats();
  EXPECT_EQ(s.rows_in_file, python.at("source").at("rows_scanned").get<u64>());
  EXPECT_EQ(panel->rows(), python.at("source").at("extended_sessions").get<usize>());
  EXPECT_EQ(panel->prefix(), python.at("source").at("sessions_before_role").get<usize>());
  EXPECT_EQ(fields::iso_date(panel->days().front()),
            python.at("source").at("first_session").get<std::string>());
  // The Python price source pin also carries the file's row groups and rows.
  const Json &pin = entry_of(manifest, "ret_overnight").at("sources").at(0);
  EXPECT_EQ(s.row_groups, pin.at("row_groups").get<u64>());
  EXPECT_EQ(s.rows_in_file, pin.at("rows").get<u64>());
  EXPECT_EQ(panel->source().sha256, pin.at("sha256").get<std::string>());
  EXPECT_EQ(panel->source().bytes, pin.at("bytes").get<u64>());
}

// The bars alone (no factor, the role's own sessions) read the file as the Python ohlc module's
// bar panel does: its statistics are the Python "ohlc" source checks, its fields the same bytes.
TEST(ResearchFieldsVendorFixture, BarPanelIsThePythonOhlcPanel) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role();
  const auto request = fields::vendor_request("open_adj");
  ASSERT_TRUE(request.has_value());
  const auto panel = fields::VendorPanel::load(vendor_dir() / "th.parquet", role, *request);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  EXPECT_EQ(panel->prefix(), 0U);
  const Json &python = manifest.at("source_checks").at("ohlc");
  const auto out = support::scratch("vendor_fixture_bars");
  for (const std::string_view name : {"open_adj", "high_adj", "low_adj"}) {
    expect_field(manifest, name, *panel, role, out);
    const auto again = support::scratch("vendor_fixture_bars_again");
    const auto built = fields::build_vendor_field(name, *panel, role, again);
    ASSERT_TRUE(built.has_value()) << built.error().message();
    const Json &checks = built->source_checks;
    for (const char *key : {"clock", "lag_sessions", "seal", "window"}) {
      EXPECT_EQ(checks.at(key), python.at(key)) << name << ": " << key;
    }
    expect_scan(python.at("source"), checks.at("source"), "ohlc");
  }
}

// Each of the six fields from the run's one shared panel: bytes, coverage, sources, fingerprint and
// every extra key equal the Python entry's.
TEST(ResearchFieldsVendorFixture, FieldsAreByteIdentical) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role();
  const auto panel = fields::VendorPanel::load(vendor_dir() / "th.parquet", role, union_request());
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  const auto out = support::scratch("vendor_fixture_fields");
  for (const std::string_view name : fields::vendor_field_names()) {
    expect_field(manifest, name, *panel, role, out);
  }
}

// The verification flow for root: the committed field registry with the six rows flipped to kind
// engine (builder = the kind id), built by the registry path. Every manifest entry equals the
// Python entry key for key (the engine adds only its producer block; a source records path, bytes
// and sha256, where the Python price pin also carries row_groups and rows).
TEST(ResearchFieldsVendorFixture, RegistryBuildEntriesAreThePythonEntries) {
  const Json python = expected_manifest();
  const auto registry = flipped_registry();
  const auto dir = support::scratch("vendor_fixture_registry");
  fields::BuildSpec spec;
  spec.role_dir = vendor_dir() / "role";
  spec.role_manifest_sha256 =
      support::sha256_of(support::read_bytes(vendor_dir() / "role" / "manifest.json"));
  spec.output_dir = dir / "out";
  fs::create_directories(spec.output_dir);
  spec.price_source = vendor_dir() / "th.parquet";
  for (const std::string_view name : fields::vendor_field_names()) {
    spec.fields.emplace_back(name);
  }
  const fields::ProducerIdentity producer{std::string(64, 'e'), "feedface", "Debug"};
  const auto built =
      fields::build_registry_fields(spec, registry, std::string(64, 'a'), producer);
  ASSERT_TRUE(built.has_value()) << built.error().message();
  EXPECT_EQ(built->built, 6U);
  const Json manifest = Json::parse(built->manifest);
  ASSERT_EQ(manifest.at("fields").size(), 6U);
  for (const Json &e : manifest.at("fields")) {
    const std::string name = e.at("name").get<std::string>();
    const Json &want = entry_of(python, name);
    for (auto it = want.begin(); it != want.end(); ++it) {
      ASSERT_TRUE(e.contains(it.key())) << name << ": no key " << it.key();
      if (it.key() == "sources") {
        ASSERT_EQ(e.at("sources").size(), it->size()) << name;
        for (usize i = 0; i < it->size(); ++i) {
          EXPECT_EQ(e.at("sources").at(i).at("sha256"), it->at(i).at("sha256")) << name << i;
          EXPECT_EQ(e.at("sources").at(i).at("bytes"), it->at(i).at("bytes")) << name << i;
        }
        continue;
      }
      EXPECT_EQ(e.at(it.key()), it.value()) << name << ": " << it.key();
    }
    for (auto it = e.begin(); it != e.end(); ++it) {
      EXPECT_TRUE(want.contains(it.key()) || it.key() == "producer") << name << ": " << it.key();
    }
    EXPECT_EQ(manifest.at("files").at(name + ".f64"), python.at("files").at(name + ".f64"));
  }
  // The price fields' source checks are the Python price group's (the union panel).
  for (const char *name : {"ret_overnight", "ret_intraday", "ceq_iss_5y"}) {
    const Json &checks = manifest.at("source_checks").at(name);
    expect_scan(python.at("source_checks").at("price").at("source"), checks.at("source"), name);
  }
}
