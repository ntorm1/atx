// P9 lane A4: the reuse decision of a registry build against the vendor panel's history (A3 review
// S-2) and one digest per input file per decision (A3-3), on lane A3's vendor fixture
// (fixtures/research_fields/vendor: the Python builder's six fields from one run, so its price
// fields were computed on the union panel, ceq_iss_5y's 1,261 sessions before the role).
//
// A price field's bits depend on the panel's history, which the co-requested fields set. A build
// now (a) rebuilds a field on the union of EVERY planned field's request, reused ones included, and
// (b) reuses a price field only from a panel with this build's history (reuse.hpp rule 6). Each
// test fails with its half removed: without (a) the rebuilt field's panel has 2 sessions before
// the role; without (b) the 2-session payload is carried into a 1,261-session build.
#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/research_fields_cli.hpp"
#include "atx/engine/research/fields/vendor_fields.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::u64;

namespace {

const fs::path &vendor_dir() {
  static const fs::path dir = support::fixture_dir() / "vendor";
  return dir;
}

// The committed field registry with the six vendor rows of kind engine (lane A3's flip).
const fields::FieldRegistry &vendor_registry() {
  static const fields::FieldRegistry registry = [] {
    const fs::path path =
        support::fixture_dir() / ".." / ".." / ".." / "tools" / "field_registry.json";
    Json doc = Json::parse(support::read_bytes(path));
    for (Json &row : doc.at("fields")) {
      const std::string name = row.at("name").get<std::string>();
      if (fields::vendor_field_spec(name) != nullptr) {
        row["kind"] = "engine";
        row["builder"] = name;
      }
    }
    return fields::parse_field_registry(doc.dump()).value();
  }();
  return registry;
}

const fields::ProducerIdentity &producer() {
  static const fields::ProducerIdentity id{std::string(64, 'e'), "feedface", "Debug"};
  return id;
}

// A registry build of `names` on the vendor fixture into dir/<tag> (receipt dir/<tag>.json).
atx::core::Result<fields::RegistryBuild>
build_into(const fs::path &dir, const std::string &tag, std::vector<std::string> names,
           const std::optional<fs::path> &reuse = std::nullopt) {
  const fs::path out = dir / tag;
  fs::create_directories(out);
  fields::BuildSpec spec;
  spec.role_dir = vendor_dir() / "role";
  spec.role_manifest_sha256 =
      support::sha256_of(support::read_bytes(vendor_dir() / "role" / "manifest.json"));
  spec.output_dir = out;
  spec.fields = std::move(names);
  spec.price_source = vendor_dir() / "th.parquet";
  if (reuse) {
    spec.reuse = fields::ReuseRequest{*reuse, {}};
  }
  ATX_TRY(auto built, fields::build_registry_fields(spec, vendor_registry(),
                                                    std::string(64, 'a'), producer()));
  ATX_TRY_VOID(fields::write_registry_build(built, dir / (tag + ".json"), out));
  return atx::core::Ok(std::move(built));
}

Json manifest_of(const fs::path &dir) {
  return Json::parse(support::read_bytes(dir / "manifest.json"));
}

u64 history_of(const Json &manifest, const std::string &name) {
  return manifest.at("source_checks").at(name).at("source").at("sessions_before_role").get<u64>();
}

void rewrite_manifest(const fs::path &dir, const std::function<void(Json &)> &mutate) {
  Json manifest = manifest_of(dir);
  mutate(manifest);
  support::write_bytes(dir / "manifest.json", manifest.dump(2) + "\n");
}

Json &entry_of(Json &manifest, const std::string &name) {
  for (Json &e : manifest.at("fields")) {
    if (e.at("name") == name) {
      return e;
    }
  }
  throw std::runtime_error("no entry " + name);
}

std::string python_payload(const std::string &name) {
  return support::read_bytes(vendor_dir() / "expected" / (name + ".f64"));
}

} // namespace

// (a) ceq_iss_5y reused, ret_overnight rebuilt: the rebuilt field's panel is the whole build's
// (ceq_iss_5y's history), so its bytes are the fresh build's and the Python builder's.
TEST(ResearchFieldsA4Reuse, RebuiltPriceFieldUsesTheWholeBuildsPanel) {
  const auto dir = support::scratch("a4_reuse_union");
  const auto fresh = build_into(dir, "a", {"ret_overnight", "ceq_iss_5y"});
  ASSERT_TRUE(fresh.has_value()) << fresh.error().message();
  // ceq_iss_5y's request: 1,261 sessions plus the rule sessions of its 490-day share lookback
  // (1,600 on this role: test_vendor_panel_fixture.py pins the Python panel's count).
  const u64 union_history = history_of(manifest_of(dir / "a"), "ceq_iss_5y");
  EXPECT_EQ(union_history, 1600U);
  EXPECT_EQ(history_of(manifest_of(dir / "a"), "ret_overnight"), union_history);
  rewrite_manifest(dir / "a", [](Json &m) { // ret_overnight misses on rule 4, ceq_iss_5y is kept
    entry_of(m, "ret_overnight")["formula_sha256"] = std::string(64, '1');
  });
  const auto second = build_into(dir, "b", {"ret_overnight", "ceq_iss_5y"}, dir / "a");
  ASSERT_TRUE(second.has_value()) << second.error().message();
  EXPECT_EQ(second->reused, 1U);
  EXPECT_EQ(second->built, 1U);
  const Json b = manifest_of(dir / "b");
  EXPECT_EQ(b.at("reuse").at("reused"), Json::array({"ceq_iss_5y"}));
  EXPECT_EQ(history_of(b, "ret_overnight"), union_history)
      << "the rebuilt field's panel must be the union of every planned field's request";
  EXPECT_EQ(support::read_bytes(dir / "b" / "ret_overnight.f64"), python_payload("ret_overnight"));
  EXPECT_EQ(support::read_bytes(dir / "b" / "ceq_iss_5y.f64"), python_payload("ceq_iss_5y"));
}

// (b) A price field is reused only from a panel with this build's history, in both directions;
// the same history reuses.
TEST(ResearchFieldsA4Reuse, PriceFieldReusedOnlyWithTheBuildsPanelHistory) {
  const auto dir = support::scratch("a4_reuse_history");
  ASSERT_TRUE(build_into(dir, "short", {"ret_overnight"}).has_value());
  EXPECT_EQ(history_of(manifest_of(dir / "short"), "ret_overnight"),
            fields::kOpenReturnSessions);
  const auto longer = build_into(dir, "long", {"ret_overnight", "ceq_iss_5y"}, dir / "short");
  ASSERT_TRUE(longer.has_value()) << longer.error().message();
  EXPECT_EQ(longer->reused, 0U);
  const Json l = manifest_of(dir / "long");
  const std::string why = l.at("reuse").at("recomputed").at("ret_overnight").get<std::string>();
  EXPECT_NE(why.find("vendor panel history differs"), std::string::npos) << why;
  EXPECT_EQ(support::read_bytes(dir / "long" / "ret_overnight.f64"),
            python_payload("ret_overnight"));
  const auto shorter = build_into(dir, "short2", {"ret_overnight"}, dir / "long");
  ASSERT_TRUE(shorter.has_value()) << shorter.error().message();
  EXPECT_EQ(shorter->reused, 0U);
  const Json s2 = manifest_of(dir / "short2");
  const std::string back = s2.at("reuse").at("recomputed").at("ret_overnight").get<std::string>();
  EXPECT_NE(back.find("vendor panel history differs"), std::string::npos) << back;
  const auto same = build_into(dir, "short3", {"ret_overnight"}, dir / "short");
  ASSERT_TRUE(same.has_value()) << same.error().message();
  EXPECT_EQ(same->reused, 1U) << manifest_of(dir / "short3").at("reuse").dump();
}

// A3-3: one reuse decision over the six vendor fields digests each distinct input file once (the
// vendor file, close, raw close, presence), not once per field that reads it.
TEST(ResearchFieldsA4Reuse, EachInputFileIsDigestedOncePerDecision) {
  const auto dir = support::scratch("a4_reuse_digests");
  std::vector<std::string> six;
  for (const std::string_view name : fields::vendor_field_names()) {
    six.emplace_back(name);
  }
  ASSERT_TRUE(build_into(dir, "a", six).has_value());
  const auto again = build_into(dir, "b", six, dir / "a");
  ASSERT_TRUE(again.has_value()) << again.error().message();
  EXPECT_EQ(again->reused, 6U);
  const Json reuse = manifest_of(dir / "b").at("reuse");
  EXPECT_EQ(reuse.at("inputs_digested").get<u64>(), 4U) << reuse.dump();
  for (const std::string &name : six) {
    EXPECT_EQ(support::read_bytes(dir / "b" / (name + ".f64")), python_payload(name)) << name;
  }
}
