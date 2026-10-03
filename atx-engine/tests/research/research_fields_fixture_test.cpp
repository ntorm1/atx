// Byte identity of the engine field builders against the EXISTING Python builder (migration
// slice 1).
//
// atx-engine/tests/fixtures/research_fields/ holds synthetic inputs (a role, FINRA as-of files) and
// the outputs prepare_research_fields.py wrote from them (make_research_fields_fixture.py; its
// pytest re-runs the Python and must reproduce every committed byte). Here the engine reads the
// same inputs: every payload byte, every coverage number (counts, fractions, min / max / mean, the
// five quantiles, bit for bit), the FINRA source checks and vintage block, and vol_126's formula
// fingerprint must equal the Python manifest's.
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_stats.hpp"
#include "atx/engine/research/fields/finra_asof_field.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/volume_mean_field.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using Json = nlohmann::json;

namespace {

const support::fs::path kFixture{ATX_RESEARCH_FIELDS_FIXTURE};

Json expected_manifest() {
  return Json::parse(support::read_bytes(kFixture / "expected" / "manifest.normalized.json"));
}

const Json &entry_of(const Json &manifest, const std::string &name) {
  for (const auto &e : manifest.at("fields")) {
    if (e.at("name") == name) {
      return e;
    }
  }
  throw std::runtime_error("no manifest entry " + name);
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
  EXPECT_EQ(want.at("member_cells").get<atx::u64>(), c.member_cells) << name;
  EXPECT_EQ(want.at("finite_member_cells").get<atx::u64>(), c.finite_member_cells) << name;
  expect_number(want.at("finite_member_frac"),
                fields::rounded_fraction(c.finite_member_cells, c.member_cells),
                name + " finite_member_frac");
  EXPECT_EQ(want.at("finite_cells_all").get<atx::u64>(), c.finite_cells_all) << name;
  const Json &score = want.at("score_window");
  EXPECT_EQ(score.at("member_cells").get<atx::u64>(), c.score_member_cells) << name;
  EXPECT_EQ(score.at("finite_member_cells").get<atx::u64>(), c.score_finite_member_cells) << name;
  expect_number(score.at("finite_member_frac"),
                fields::rounded_fraction(c.score_finite_member_cells, c.score_member_cells),
                name + " score frac");
  const Json &years = want.at("per_year");
  ASSERT_EQ(years.size(), c.per_year.size()) << name;
  for (const auto &y : c.per_year) {
    const Json &w = years.at(std::to_string(y.year));
    EXPECT_EQ(w.at("member_cells").get<atx::u64>(), y.member_cells) << name << " " << y.year;
    EXPECT_EQ(w.at("finite_member_cells").get<atx::u64>(), y.finite_member_cells)
        << name << " " << y.year;
    expect_number(w.at("finite_member_frac"),
                  fields::rounded_fraction(y.finite_member_cells, y.member_cells),
                  name + " year frac");
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

void expect_payload(const Json &manifest, const fields::WrittenField &w,
                    const support::fs::path &out) {
  const Json &e = entry_of(manifest, w.name);
  const std::string engine = support::read_bytes(out / (w.name + ".f64"));
  const std::string python = support::read_bytes(kFixture / "expected" / (w.name + ".f64"));
  EXPECT_EQ(engine.size(), python.size()) << w.name;
  EXPECT_TRUE(engine == python) << w.name << ": payload bytes differ from the Python builder's";
  EXPECT_EQ(w.sha256, e.at("sha256").get<std::string>()) << w.name;
  EXPECT_EQ(w.bytes, manifest.at("files").at(w.name + ".f64").at("bytes").get<atx::u64>())
      << w.name;
  expect_coverage(e.at("coverage"), w.coverage, w.name);
}

void expect_sources(const Json &entry, const std::vector<fields::SourceRecord> &got) {
  const Json &want = entry.at("sources");
  ASSERT_EQ(want.size(), got.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(want.at(i).at("sha256").get<std::string>(), got[i].sha256) << i;
    EXPECT_EQ(want.at(i).at("bytes").get<atx::u64>(), got[i].bytes) << i;
  }
}

fields::RoleAxes fixture_role(const Json &manifest) {
  return fields::RoleAxes::load(kFixture / "role",
                                manifest.at("role").at("manifest_sha256").get<std::string>())
      .value();
}

} // namespace

TEST(ResearchFieldsFixture, RoleAxesMatchThePythonRole) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role(manifest);
  const Json &r = manifest.at("role");
  EXPECT_EQ(r.at("dates").get<std::size_t>(), role.dates());
  EXPECT_EQ(r.at("instruments").get<std::size_t>(), role.instruments());
  EXPECT_EQ(r.at("first_session").get<std::string>(), fields::iso_date(role.days().front()));
  EXPECT_EQ(r.at("last_session").get<std::string>(), fields::iso_date(role.days().back()));
  EXPECT_EQ(r.at("score_begin").get<atx::i64>(), role.score_begin());
  EXPECT_EQ(r.at("score_end").get<atx::i64>(), role.score_end());
}

TEST(ResearchFieldsFixture, Vol126IsByteIdentical) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role(manifest);
  const auto out = support::scratch("fixture_vol_126");
  const auto built = fields::build_vol_126(role, out).value();
  expect_payload(manifest, built.field, out);
  const Json &e = entry_of(manifest, "vol_126");
  expect_sources(e, built.sources);
  EXPECT_EQ(fields::formula_sha256(fields::vol_126_spec()).value(),
            e.at("formula_sha256").get<std::string>());
  EXPECT_EQ(fields::vol_126_spec().formula_id, e.at("formula_id").get<std::string>());
  EXPECT_EQ(fields::vol_126_spec().min_history, e.at("min_history").get<std::string>());
  EXPECT_EQ(e.at("lag_sessions").get<int>(), 1);
}

TEST(ResearchFieldsFixture, FinraFieldsAreByteIdentical) {
  const Json manifest = expected_manifest();
  const auto role = fixture_role(manifest);
  const auto schedule = fields::read_dissemination_schedule(kFixture / "finra").value();
  for (const char *field : {"si_shares", "si_dtc"}) {
    const std::string name(field);
    const auto out = support::scratch("fixture_" + name);
    const auto built =
        fields::build_finra_field(name, kFixture / "finra", schedule, role, out).value();
    expect_payload(manifest, built.field, out);
    const Json &e = entry_of(manifest, name);
    expect_sources(e, built.sources);
    const Json &checks = manifest.at("source_checks").at(name);
    EXPECT_EQ(checks.at("rows_total").get<atx::u64>(), built.stats.rows_total) << name;
    EXPECT_EQ(checks.at("rows_sealed_dropped").get<atx::u64>(), built.stats.rows_sealed) << name;
    EXPECT_EQ(checks.at("rows_matched_axis").get<atx::u64>(), built.stats.rows_matched_axis)
        << name;
    EXPECT_EQ(checks.at("rows_ignored_unknown_id").get<atx::u64>(),
              built.stats.rows_ignored_unknown_id)
        << name;
    EXPECT_EQ(checks.at("max_stale_days").get<atx::i64>(), built.stats.max_stale_days) << name;
    const Json &v = e.at("coverage").at("vintage_risk");
    EXPECT_EQ(v.at("rule").get<std::string>(), built.vintage.rule) << name;
    EXPECT_EQ(v.at("finite_member_cells").get<atx::u64>(), built.vintage.finite_member_cells)
        << name;
    const auto text = [](const Json &j) {
      return j.is_null() ? std::optional<std::string>{} : j.get<std::string>();
    };
    EXPECT_EQ(text(v.at("last_session_with_republished_visible_cell")),
              built.vintage.last_session_with_republished_visible_cell)
        << name;
    EXPECT_EQ(text(v.at("first_session_vintage_safe")), built.vintage.first_session_vintage_safe)
        << name;
    EXPECT_EQ(text(e.at("vintage_safe_from")), built.vintage_safe_from) << name;
  }
  // The seal probe: one synthetic si_shares row is dated after the seal; both builders drop and
  // count it.
  EXPECT_EQ(manifest.at("source_checks").at("si_shares").at("rows_sealed_dropped").get<int>(), 1);
}
