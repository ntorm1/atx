// Builder kinds and the field registry (P9 contracts K-P9-2, kind half, and the engine's K-P9-1
// reader): kind lookup, refusal of an unknown kind and of every request a kind does not build, the
// registry row contract and its parse round trip, and the plans' order and inputs.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/research_fields_cli.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using Json = nlohmann::json;

namespace {

// Planning reads neither the role nor the output directory.
fields::BuildSpec plan_spec(std::vector<std::string> names) {
  return support::fixture_spec(support::fs::temp_directory_path(), std::move(names));
}

} // namespace

TEST(ResearchFieldsRegistry, KindLookup) {
  const auto kinds = fields::builder_kinds();
  ASSERT_EQ(kinds.size(), 9U);
  const auto ids = fields::builder_kind_ids();
  // P9 A3 appended the six vendor-panel kinds (vendor_fields.hpp) after lane A2's three.
  const std::vector<std::string_view> want{"si_shares",     "si_dtc",       "vol_126",
                                           "ret_overnight", "ret_intraday", "ceq_iss_5y",
                                           "open_adj",      "high_adj",     "low_adj"};
  EXPECT_TRUE(std::equal(ids.begin(), ids.end(), want.begin(), want.end()));
  const auto names = fields::engine_field_names(); // the registry-less names are the kind ids
  EXPECT_TRUE(std::equal(names.begin(), names.end(), want.begin(), want.end()));
  auto spec_of = [](std::string_view id) {
    auto spec = plan_spec({std::string(id)});
    spec.price_source = support::fixture_dir() / "vendor" / "th.parquet"; // vendor kinds' input
    return spec;
  };
  for (const fields::BuilderKind &kind : kinds) {
    const fields::BuilderKind *found = fields::find_builder_kind(kind.id);
    ASSERT_NE(found, nullptr) << kind.id;
    EXPECT_EQ(found, &kind);
    EXPECT_TRUE(kind.parse != nullptr && kind.build != nullptr) << kind.id;
    // A ported kind builds exactly its own field: its plan names the field and pins its spec.
    const auto plan = kind.parse(kind.id, Json::object(), spec_of(kind.id));
    ASSERT_TRUE(plan.has_value()) << plan.error().message();
    EXPECT_EQ(plan->name, kind.id);
    ASSERT_NE(plan->spec, nullptr);
    EXPECT_EQ(plan->spec->name, kind.id);
  }
  EXPECT_EQ(fields::find_builder_kind("mkt_cap"), nullptr);
  EXPECT_EQ(fields::find_builder_kind("VOL_126"), nullptr);
  EXPECT_EQ(fields::find_builder_kind(""), nullptr);
}

TEST(ResearchFieldsRegistry, UnknownKindRefused) {
  // Registry-less: a field name that names no kind.
  EXPECT_FALSE(fields::plan_fields(plan_spec({"mkt_cap"}), nullptr).has_value());
  // An engine row whose builder is not a registered kind.
  Json unknown = support::ported_row("vol_126");
  unknown["builder"] = "vol_252";
  const auto bad_kind = support::registry_of({unknown});
  const auto refused = fields::plan_fields(plan_spec({"vol_126"}), &bad_kind);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().message().find("unknown builder kind vol_252"), std::string::npos)
      << refused.error().message();
  // A python row, and a field the registry does not hold.
  const auto python = support::registry_of({support::ported_row("vol_126", "python")});
  EXPECT_FALSE(fields::plan_fields(plan_spec({"vol_126"}), &python).has_value());
  const auto registry = support::engine_registry();
  EXPECT_FALSE(fields::plan_fields(plan_spec({"mkt_cap"}), &registry).has_value());
  // A kind asked to build another field (its spec text names its own).
  Json renamed = support::ported_row("vol_126");
  renamed["name"] = "vol_127";
  const auto other = support::registry_of({renamed});
  EXPECT_FALSE(fields::plan_fields(plan_spec({"vol_127"}), &other).has_value());
  // A FINRA kind without its input root, and a repeated field.
  auto no_finra = plan_spec({"si_dtc"});
  no_finra.finra.reset();
  EXPECT_FALSE(fields::plan_fields(no_finra, &registry).has_value());
  EXPECT_FALSE(fields::plan_fields(plan_spec({"si_dtc", "si_dtc"}), &registry).has_value());
}

TEST(ResearchFieldsRegistry, RowRequestsAKindRefuses) {
  const auto refuses = [](const std::function<void(Json &)> &mutate) {
    Json row = support::ported_row("si_dtc");
    mutate(row);
    const auto registry = support::registry_of({row});
    return !fields::plan_fields(plan_spec({"si_dtc"}), &registry).has_value();
  };
  EXPECT_TRUE(refuses([](Json &r) { r["dtype"] = "group"; }));                       // f64 kind
  EXPECT_TRUE(refuses([](Json &r) { r["formula_sha256"] = std::string(64, '1'); })); // drift
  EXPECT_TRUE(refuses([](Json &r) { r["options"]["window"] = 126; }));               // foreign
  EXPECT_TRUE(refuses([](Json &r) { r["options"]["group"] = "price_volume"; }));     // group
  EXPECT_FALSE(refuses([](Json &r) { r["options"] = Json::object(); }));              // none
  EXPECT_FALSE(refuses([](Json &r) { r["formula_sha256"] = nullptr; }));              // undeclared
  EXPECT_FALSE(refuses([](Json &) {}));
}

TEST(ResearchFieldsRegistry, ParseRoundTrip) {
  Json grp = support::python_row("grp_sector");
  grp["dtype"] = "group";
  grp["formula_sha256"] = nullptr;
  grp["first_session"] = "2020-01-02";
  grp["requires"] = Json::array({"me_company"});
  grp["spec_text"] = Json{{"units", "categorical code: SIC sector"}, {"domain", nullptr}};
  const Json doc = support::registry_document({support::ported_row("si_shares"),
                                               support::python_row("me_company"), grp,
                                               support::ported_row("vol_126")});
  const auto parsed = fields::parse_field_registry(doc.dump());
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
  ASSERT_EQ(parsed->rows.size(), 4U);
  EXPECT_EQ(parsed->sha256, support::sha256_of(doc.dump()));
  const fields::RegistryRow &shares = parsed->rows[0];
  EXPECT_TRUE(shares.kind == fields::FieldKind::Engine);
  EXPECT_EQ(shares.builder, "si_shares");
  EXPECT_EQ(shares.options, (Json{{"group", "finra"}}));
  const std::string shares_formula =
      fields::formula_sha256(support::ported_spec("si_shares")).value();
  EXPECT_EQ(shares.formula_sha256, shares_formula);
  const fields::RegistryRow &sector = parsed->rows[2];
  EXPECT_TRUE(sector.kind == fields::FieldKind::Python);
  EXPECT_TRUE(sector.dtype == fields::FieldDtype::Group);
  EXPECT_FALSE(sector.formula_sha256.has_value());
  EXPECT_EQ(sector.first_session, std::optional<std::string>("2020-01-02"));
  EXPECT_EQ(sector.required_fields, (std::vector<std::string>{"me_company"}));
  EXPECT_EQ(parsed->index_of("vol_126"), std::optional<std::size_t>(3));
  EXPECT_EQ(parsed->find("mkt_cap"), nullptr);
  // The document of the parsed rows is the input's rows (informational keys are not carried), and
  // it parses back to itself.
  const Json again = fields::field_registry_document(*parsed);
  EXPECT_EQ(again.at("fields"), doc.at("fields"));
  EXPECT_EQ(again.at("schema"), "atx.field-registry/v1");
  EXPECT_FALSE(again.contains("generated_from"));
  const auto reparsed = fields::parse_field_registry(again.dump());
  ASSERT_TRUE(reparsed.has_value()) << reparsed.error().message();
  EXPECT_EQ(fields::field_registry_document(*reparsed), again);
  // A kind's plan carries the row's options as given.
  const auto plans = fields::plan_fields(plan_spec({"si_shares"}), &*parsed);
  ASSERT_TRUE(plans.has_value()) << plans.error().message();
  EXPECT_EQ(plans->at(0).options, shares.options);
  EXPECT_EQ(plans->at(0).kind, fields::find_builder_kind("si_shares"));
}

TEST(ResearchFieldsRegistry, RowContract) {
  const Json good = support::registry_document({support::ported_row("si_shares")});
  const auto refused = [](const Json &doc) {
    return !fields::parse_field_registry(doc.dump()).has_value();
  };
  const auto with = [&good](const char *key, Json value) {
    Json doc = good;
    doc["fields"][0][key] = std::move(value);
    return doc;
  };
  EXPECT_FALSE(refused(good));
  EXPECT_FALSE(fields::parse_field_registry("not json").has_value());
  Json schema = good;
  schema["schema"] = "atx.field-registry/v0";
  EXPECT_TRUE(refused(schema));
  Json empty = good;
  empty["fields"] = Json::array();
  EXPECT_TRUE(refused(empty));
  for (const char *key : {"name", "kind", "builder", "dtype", "point_in_time", "spec_text",
                          "formula_sha256", "requires", "options", "sources", "first_session",
                          "owner"}) {
    Json missing = good;
    missing["fields"][0].erase(std::string(key));
    EXPECT_TRUE(refused(missing)) << key;
  }
  EXPECT_TRUE(refused(with("name", "")));
  EXPECT_TRUE(refused(with("kind", "rust")));
  EXPECT_TRUE(refused(with("builder", 7)));
  EXPECT_TRUE(refused(with("dtype", "i64")));
  EXPECT_TRUE(refused(with("point_in_time", "yes")));
  EXPECT_TRUE(refused(with("formula_sha256", "abc")));
  EXPECT_TRUE(refused(with("formula_sha256", std::string(64, 'A')))); // lower-case hex only
  EXPECT_TRUE(refused(with("requires", "me_company")));
  EXPECT_TRUE(refused(with("options", Json::array())));
  EXPECT_TRUE(refused(with("sources", Json::array({1}))));
  EXPECT_TRUE(refused(with("first_session", "2020-13-01")));
  EXPECT_FALSE(refused(with("informational", 1))); // unknown row keys are ignored
  EXPECT_TRUE(refused(support::registry_document(
      {support::ported_row("si_shares"), support::ported_row("si_shares")})));
}

TEST(ResearchFieldsRegistry, PlansKeepSourceOrderAndRegistrationOrder) {
  // Registry-less: spec order; each plan's inputs in its entry's source order.
  const auto plain = fields::plan_fields(plan_spec({"vol_126", "si_dtc"}), nullptr);
  ASSERT_TRUE(plain.has_value()) << plain.error().message();
  ASSERT_EQ(plain->size(), 2U);
  EXPECT_EQ(plain->at(0).name, "vol_126");
  const auto &role = plain->at(0).inputs;
  ASSERT_EQ(role.size(), 2U);
  EXPECT_EQ(role[0].string(), (support::fixture_dir() / "role" / "volume.f64").string());
  EXPECT_EQ(role[1].string(), (support::fixture_dir() / "role" / "present.u8").string());
  const auto &finra = plain->at(1).inputs;
  ASSERT_EQ(finra.size(), 3U);
  const auto root = support::fixture_dir() / "finra";
  EXPECT_EQ(finra[0].string(), (root / "asof" / "si_dtc.csv").string());
  EXPECT_EQ(finra[1].string(), (root / "asof" / "manifest.json").string());
  EXPECT_EQ(finra[2].string(), (root / "dissemination_schedule.csv").string());
  // Through a registry: registration order, whatever the spec order.
  const auto registry = support::engine_registry();
  const auto registered =
      fields::plan_fields(plan_spec({"vol_126", "si_dtc", "si_shares"}), &registry);
  ASSERT_TRUE(registered.has_value()) << registered.error().message();
  ASSERT_EQ(registered->size(), 3U);
  EXPECT_EQ(registered->at(0).name, "si_shares");
  EXPECT_EQ(registered->at(1).name, "si_dtc");
  EXPECT_EQ(registered->at(2).name, "vol_126");
}

TEST(ResearchFieldsRegistry, SpecSchemaFollowsTheRegistryFlag) {
  const auto registry = support::engine_registry();
  Json spec{{"schema", "atx.research-fields-spec/v2"},
            {"role", Json{{"dir", (support::fixture_dir() / "role").string()},
                          {"manifest_sha256", support::fixture_role_sha256()}}},
            {"output_dir", support::fs::temp_directory_path().string()},
            {"fields", Json::array({"vol_126", "si_shares"})},
            {"finra", (support::fixture_dir() / "finra").string()},
            {"reuse", Json{{"dir", "prior"}, {"manifest_sha256", std::string(64, 'A')}}}};
  const auto v2 = fields::parse_build_spec(spec.dump(), &registry);
  ASSERT_TRUE(v2.has_value()) << v2.error().message();
  ASSERT_TRUE(v2->reuse.has_value());
  EXPECT_EQ(v2->reuse->dir.string(), "prior");
  EXPECT_EQ(v2->reuse->manifest_sha256, std::string(64, 'a')); // the pin compares lower-case
  EXPECT_FALSE(fields::parse_build_spec(spec.dump()).has_value()); // v2 needs --registry
  Json v1 = spec;
  v1["schema"] = "atx.research-fields-spec/v1";
  EXPECT_FALSE(fields::parse_build_spec(v1.dump()).has_value()); // reuse is a v2 key
  v1.erase("reuse");
  EXPECT_TRUE(fields::parse_build_spec(v1.dump()).has_value());
  EXPECT_FALSE(fields::parse_build_spec(v1.dump(), &registry).has_value()); // v1 has no registry
  Json bad_pin = spec;
  bad_pin["reuse"]["manifest_sha256"] = "abc";
  EXPECT_FALSE(fields::parse_build_spec(bad_pin.dump(), &registry).has_value());
  Json python = spec;
  python["fields"] = Json::array({"vol_126"});
  const auto python_rows = support::registry_of({support::ported_row("vol_126", "python")});
  EXPECT_FALSE(fields::parse_build_spec(python.dump(), &python_rows).has_value());
}
