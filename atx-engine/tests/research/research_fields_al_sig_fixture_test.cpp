// Lane AL-SIG end to end on the synthetic stage fixture (tests/fixtures/research_fields_al_sig,
// written with its independent numpy oracle by make_al_sig_fixture.py): the committed field
// registry's engine-only rows gia_13f and russell_recon pin the kinds' spec text and fingerprint;
// a registry build of both from the pinned thirteenf / indexes stages reproduces the oracle's
// payloads (NaN pattern exact, values within 1e-12) and the reader counts exactly; each entry's
// source is its stage manifest, so reuse follows the stage pin; the kinds refuse a spec without
// their stage, and the v2 spec keys "thirteenf" / "indexes" parse and refuse bad pins.
#include <gtest/gtest.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/gia_13f_field.hpp"
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/research_fields_cli.hpp"
#include "atx/engine/research/fields/russell_recon_field.hpp"
#include "research/research_fields_al_sig_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::f64;
using atx::u64;
using atx::usize;

namespace {

const std::vector<std::string> kNames{"gia_13f", "russell_recon"};

fields::FieldRegistry committed_registry() {
  return fields::load_field_registry(support::committed_registry_path()).value();
}

fields::ProducerIdentity producer() {
  return fields::ProducerIdentity{std::string(64, 'e'), "0123abc", "Debug"};
}

std::string role_pin() {
  return support::sha256_of(support::read_bytes(support::al_sig_dir() / "role" / "manifest.json"));
}

// A build of both fields on the fixture role and stages into `out`.
fields::BuildSpec al_sig_spec(const fs::path &out) {
  fields::BuildSpec spec;
  spec.role_dir = support::al_sig_dir() / "role";
  spec.role_manifest_sha256 = role_pin();
  spec.output_dir = out;
  spec.fields = kNames;
  spec.thirteenf = support::al_sig_stage("thirteenf");
  spec.indexes = support::al_sig_stage("indexes");
  return spec;
}

std::vector<f64> payload(const fs::path &path) {
  const std::string bytes = support::read_bytes(path);
  std::vector<f64> out(bytes.size() / sizeof(f64));
  if (!out.empty()) {
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(f64));
  }
  return out;
}

// The spec text a registry row carries: the definition keys of the kind's formula fingerprint.
Json spec_text_of(const fields::FieldSpec &spec) {
  const fields::FieldDefinition &d = spec.definition;
  return Json{{"units", d.units},
              {"clock", d.clock},
              {"staleness", d.staleness},
              {"source_columns", d.source_columns},
              {"definition", d.definition ? Json(*d.definition) : Json(nullptr)},
              {"point_in_time", d.point_in_time},
              {"non_pit_aspects", d.non_pit_aspects},
              {"domain", d.domain ? Json::array({(*d.domain)[0], (*d.domain)[1]}) : Json(nullptr)}};
}

const Json &entry_named(const Json &document, const std::string &name) {
  for (const Json &e : document.at("fields")) {
    if (e.at("name") == name) {
      return e;
    }
  }
  return document.at("no entry " + name); // throws: the test fails on the missing entry
}

// A registry build of both fields into dir/<tag> (created), receipt dir/<tag>.json, published.
atx::core::Result<fields::RegistryBuild>
build_into(const fs::path &dir, const std::string &tag, fields::BuildSpec spec) {
  const fs::path out = dir / tag;
  fs::create_directories(out);
  spec.output_dir = out;
  const auto registry = committed_registry();
  ATX_TRY(auto built,
          fields::build_registry_fields(spec, registry, std::string(64, 'a'), producer()));
  ATX_TRY_VOID(fields::write_registry_build(built, dir / (tag + ".json"), out));
  return atx::core::Ok(std::move(built));
}

} // namespace

TEST(ResearchFieldsAlSigFixture, CommittedRowsPinTheSpecs) {
  const auto registry = fields::load_field_registry(support::committed_registry_path());
  ASSERT_TRUE(registry.has_value()) << registry.error().message();
  for (const fields::FieldSpec *spec : {&fields::gia_13f_spec(), &fields::russell_recon_spec()}) {
    const fields::RegistryRow *row = registry->find(spec->name);
    ASSERT_NE(row, nullptr) << spec->name;
    EXPECT_TRUE(row->kind == fields::FieldKind::Engine) << spec->name; // engine-only (DEC-5)
    EXPECT_EQ(row->builder, spec->name);
    EXPECT_TRUE(row->dtype == fields::FieldDtype::F64) << spec->name;
    EXPECT_TRUE(row->point_in_time) << spec->name;
    const std::string formula = fields::formula_sha256(*spec).value();
    EXPECT_EQ(row->formula_sha256, std::optional<std::string>(formula))
        << spec->name << ": the committed fingerprint is not the kind's";
    EXPECT_EQ(row->spec_text, spec_text_of(*spec)) << spec->name << ": spec text drift";
    EXPECT_EQ(row->options, (Json{{"group", spec->group}})) << spec->name;
    EXPECT_TRUE(row->required_fields.empty()) << spec->name;
    ASSERT_NE(fields::find_builder_kind(spec->name), nullptr) << spec->name;
  }
  EXPECT_EQ(registry->find("gia_13f")->sources, (std::vector<std::string>{"thirteenf"}));
  EXPECT_EQ(registry->find("russell_recon")->sources, (std::vector<std::string>{"indexes"}));
}

TEST(ResearchFieldsAlSigFixture, BuildMatchesTheOracle) {
  const Json counts = support::al_sig_counts();
  // The fixture's pins are the generator's (a regenerated fixture moves them together).
  EXPECT_EQ(role_pin(), counts.at("pins").at("role").get<std::string>());
  EXPECT_EQ(support::al_sig_stage("thirteenf").manifest_sha256,
            counts.at("pins").at("thirteenf").get<std::string>());
  EXPECT_EQ(support::al_sig_stage("indexes").manifest_sha256,
            counts.at("pins").at("indexes").get<std::string>());
  const auto dir = support::scratch("al_sig_build");
  const auto built = build_into(dir, "out", al_sig_spec(dir));
  ASSERT_TRUE(built.has_value()) << built.error().message();
  EXPECT_EQ(built->built, 2U);
  EXPECT_EQ(built->reused, 0U);
  for (const std::string &name : kNames) {
    const auto got = payload(dir / "out" / (name + ".f64"));
    const auto want = payload(support::al_sig_dir() / "expected" / (name + ".f64"));
    ASSERT_EQ(want.size(), 260U * 9U) << name;
    ASSERT_EQ(got.size(), want.size()) << name;
    usize finite = 0;
    usize differ = 0;
    for (usize i = 0; i < got.size(); ++i) {
      const bool nan = std::isnan(want[i]);
      if (nan != std::isnan(got[i]) || (!nan && !(std::abs(got[i] - want[i]) <= 1e-12))) {
        if (differ++ < 5U) {
          ADD_FAILURE() << name << " cell " << i << ": got " << got[i] << ", oracle " << want[i];
        }
      }
      finite += nan ? 0U : 1U;
    }
    EXPECT_EQ(differ, 0U) << name;
    EXPECT_EQ(finite, counts.at("finite").at(name).get<usize>()) << name;
  }
  const Json receipt = Json::parse(built->receipt);
  // Each entry's one source is its stage manifest (which pins every stage file read).
  using NamePair = std::pair<std::string, std::string>;
  for (const auto &[name, stage] :
       {NamePair{"gia_13f", "thirteenf"}, NamePair{"russell_recon", "indexes"}}) {
    const Json &sources = entry_named(receipt, name).at("sources");
    ASSERT_EQ(sources.size(), 1U) << name;
    EXPECT_EQ(sources[0].at("sha256"), counts.at("pins").at(stage)) << name;
  }
  const Json &gia = entry_named(receipt, "gia_13f").at("source_checks");
  const Json &want_gia = counts.at("gia_13f");
  const Json &index = gia.at("thirteenf");
  EXPECT_EQ(index.at("quarters"), want_gia.at("quarters"));
  for (const char *key : {"effective_filings", "filings_after_deadline_ignored",
                          "holdings_rows_kept"}) {
    EXPECT_EQ(index.at(key), want_gia.at(key)) << key;
  }
  const Json &per_quarter = gia.at("per_quarter");
  ASSERT_EQ(per_quarter.size(), want_gia.at("per_quarter").size());
  for (usize q = 0; q < per_quarter.size(); ++q) {
    const Json &want = want_gia.at("per_quarter")[q];
    for (auto it = want.begin(); it != want.end(); ++it) {
      EXPECT_EQ(per_quarter[q].at(it.key()), it.value()) << "quarter " << q << " " << it.key();
    }
  }
  const Json &russell = entry_named(receipt, "russell_recon").at("source_checks").at("russell");
  const Json &want_russell = counts.at("russell_recon");
  EXPECT_EQ(russell.at("rows_total"), want_russell.at("rows_total"));
  EXPECT_EQ(russell.at("rows_other_index"), want_russell.at("rows_other_index"));
  EXPECT_EQ(russell.at("rows_sealed_dropped"), want_russell.at("rows_sealed"));
  EXPECT_EQ(russell.at("rows_unusable"), want_russell.at("rows_unusable"));
  EXPECT_EQ(russell.at("reconstitutions").size(),
            want_russell.at("reconstitutions").get<usize>());
  // The published manifest: registration order, every built entry an engine producer's.
  const Json manifest = Json::parse(support::read_bytes(dir / "out" / "manifest.json"));
  ASSERT_EQ(manifest.at("fields").size(), 2U);
  EXPECT_EQ(manifest.at("fields")[0].at("name"), "gia_13f");
  EXPECT_EQ(manifest.at("fields")[1].at("name"), "russell_recon");
  for (const Json &e : manifest.at("fields")) {
    EXPECT_EQ(e.at("producer").at("kind"), "engine") << e.at("name");
  }
}

TEST(ResearchFieldsAlSigFixture, ReuseFollowsTheStagePin) {
  const auto dir = support::scratch("al_sig_reuse");
  const auto first = build_into(dir, "a", al_sig_spec(dir));
  ASSERT_TRUE(first.has_value()) << first.error().message();
  // Same stages: both payloads reused.
  auto same = al_sig_spec(dir);
  same.reuse = fields::ReuseRequest{dir / "a", {}};
  const auto again = build_into(dir, "b", same);
  ASSERT_TRUE(again.has_value()) << again.error().message();
  EXPECT_EQ(again->reused, 2U);
  EXPECT_EQ(again->built, 0U);
  // A re-pinned indexes stage (same constituents, another manifest): russell_recon is recomputed
  // (its source moved), gia_13f is still reused.
  const fs::path copy = dir / "indexes_copy";
  fs::create_directories(copy);
  fs::copy_file(support::al_sig_dir() / "indexes" / "constituents.parquet",
                copy / "constituents.parquet");
  const std::string manifest =
      support::read_bytes(support::al_sig_dir() / "indexes" / "manifest.json") + "\n";
  support::write_bytes(copy / "manifest.json", manifest);
  auto moved = al_sig_spec(dir);
  moved.indexes = fields::StageRequest{copy, support::sha256_of(manifest)};
  moved.reuse = fields::ReuseRequest{dir / "a", {}};
  const auto third = build_into(dir, "c", moved);
  ASSERT_TRUE(third.has_value()) << third.error().message();
  EXPECT_EQ(third->reused, 1U);
  EXPECT_EQ(third->built, 1U);
  EXPECT_EQ(Json::parse(third->receipt).at("reused"), Json::array({"gia_13f"}));
  EXPECT_EQ(support::read_bytes(dir / "c" / "russell_recon.f64"),
            support::read_bytes(dir / "a" / "russell_recon.f64"));
}

TEST(ResearchFieldsAlSigFixture, ReuseRefusesAWrongStagePin) {
  // Review R1: with a valid reuse directory and unchanged stages, a spec whose stage pin is not
  // the stage manifest's digest is refused before anything is reused or written. Without the
  // check, reuse rule 5 (the stage manifest re-hashed against the prior entry) would pass.
  const auto dir = support::scratch("al_sig_reuse_pin");
  const auto first = build_into(dir, "a", al_sig_spec(dir));
  ASSERT_TRUE(first.has_value()) << first.error().message();
  const std::string thirteenf_pin = support::al_sig_stage("thirteenf").manifest_sha256;
  const std::string indexes_pin = support::al_sig_stage("indexes").manifest_sha256;
  const auto with_reuse = [&dir](fields::BuildSpec spec) {
    spec.reuse = fields::ReuseRequest{dir / "a", {}};
    return spec;
  };
  const auto expect_refused = [&dir](const std::string &tag, const fields::BuildSpec &spec) {
    const auto refused = build_into(dir, tag, spec);
    ASSERT_FALSE(refused.has_value()) << tag;
    EXPECT_NE(refused.error().message().find("manifest SHA-256 does not match the pin"),
              std::string::npos)
        << tag << ": " << refused.error().message();
    EXPECT_TRUE(fs::is_empty(dir / tag)) << tag; // nothing reused, copied or built
    EXPECT_FALSE(fs::exists(dir / (tag + ".json"))) << tag;
  };
  auto wrong_thirteenf = with_reuse(al_sig_spec(dir));
  wrong_thirteenf.thirteenf->manifest_sha256 = indexes_pin; // a real digest, another stage's
  expect_refused("b", wrong_thirteenf);
  auto wrong_indexes = with_reuse(al_sig_spec(dir));
  wrong_indexes.indexes->manifest_sha256 = thirteenf_pin;
  expect_refused("c", wrong_indexes);
  // A requested stage no planned field reads is pinned too.
  auto unread = with_reuse(al_sig_spec(dir));
  unread.fields = {"russell_recon"};
  unread.thirteenf->manifest_sha256 = indexes_pin;
  expect_refused("d", unread);
  // The control: the same reuse with the right pins reuses both payloads.
  const auto control = build_into(dir, "e", with_reuse(al_sig_spec(dir)));
  ASSERT_TRUE(control.has_value()) << control.error().message();
  EXPECT_EQ(control->reused, 2U);
}

TEST(ResearchFieldsAlSigFixture, KindsRefuseWithoutTheirStage) {
  const auto registry = committed_registry();
  auto no_thirteenf = al_sig_spec(fs::temp_directory_path());
  no_thirteenf.thirteenf.reset();
  const auto gia = fields::plan_fields(no_thirteenf, &registry);
  ASSERT_FALSE(gia.has_value());
  EXPECT_NE(gia.error().message().find("gia_13f needs thirteenf"), std::string::npos)
      << gia.error().message();
  auto no_indexes = al_sig_spec(fs::temp_directory_path());
  no_indexes.indexes.reset();
  const auto russell = fields::plan_fields(no_indexes, &registry);
  ASSERT_FALSE(russell.has_value());
  EXPECT_NE(russell.error().message().find("russell_recon needs indexes"), std::string::npos)
      << russell.error().message();
  // Planned with both stages: registration order and the stage manifest as the one input.
  const auto plans = fields::plan_fields(al_sig_spec(fs::temp_directory_path()), &registry);
  ASSERT_TRUE(plans.has_value()) << plans.error().message();
  ASSERT_EQ(plans->size(), 2U);
  ASSERT_EQ(plans->at(0).inputs.size(), 1U);
  EXPECT_EQ(plans->at(0).inputs[0].string(),
            (support::al_sig_dir() / "thirteenf" / "manifest.json").string());
  ASSERT_EQ(plans->at(1).inputs.size(), 1U);
  EXPECT_EQ(plans->at(1).inputs[0].string(),
            (support::al_sig_dir() / "indexes" / "manifest.json").string());
  // A stage whose manifest is not the pinned one stops the build before any receipt.
  const auto dir = support::scratch("al_sig_pin");
  auto mispinned = al_sig_spec(dir);
  mispinned.thirteenf->manifest_sha256 = std::string(64, '0');
  const auto refused =
      fields::build_registry_fields(mispinned, registry, std::string(64, 'a'), producer());
  ASSERT_FALSE(refused.has_value());
  EXPECT_FALSE(fs::exists(dir / "gia_13f.f64")); // the stage is pinned before the field's writer
}

TEST(ResearchFieldsAlSigFixture, SpecKeysParseAndRefuseBadPins) {
  const auto registry = committed_registry();
  const auto stage = [](const std::string &name, std::string pin) {
    return Json{{"dir", (support::al_sig_dir() / name).string()},
                {"manifest_sha256", std::move(pin)}};
  };
  std::string upper = support::al_sig_stage("thirteenf").manifest_sha256;
  for (char &c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  const Json spec{{"schema", "atx.research-fields-spec/v2"},
                  {"role", Json{{"dir", (support::al_sig_dir() / "role").string()},
                                {"manifest_sha256", role_pin()}}},
                  {"output_dir", fs::temp_directory_path().string()},
                  {"fields", Json::array({"russell_recon", "gia_13f"})},
                  {"thirteenf", stage("thirteenf", upper)},
                  {"indexes", stage("indexes", support::al_sig_stage("indexes").manifest_sha256)}};
  const auto parsed = fields::parse_build_spec(spec.dump(), &registry);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
  ASSERT_TRUE(parsed->thirteenf.has_value());
  ASSERT_TRUE(parsed->indexes.has_value());
  EXPECT_EQ(parsed->thirteenf->dir.string(), (support::al_sig_dir() / "thirteenf").string());
  EXPECT_EQ(parsed->thirteenf->manifest_sha256, support::al_sig_stage("thirteenf").manifest_sha256);
  EXPECT_EQ(parsed->indexes->manifest_sha256, support::al_sig_stage("indexes").manifest_sha256);
  const auto refused = [&registry, &spec](const char *key, const Json &value) {
    Json bad = spec;
    if (value.is_null()) {
      bad.erase(std::string(key));
    } else {
      bad[key] = value;
    }
    return !fields::parse_build_spec(bad.dump(), &registry).has_value();
  };
  EXPECT_TRUE(refused("thirteenf", stage("thirteenf", "abc")));            // not 64 hex digits
  EXPECT_TRUE(refused("indexes", stage("indexes", std::string(64, 'g')))); // not hex
  EXPECT_TRUE(refused("indexes", Json{{"dir", "x"}}));                     // the pin is required
  EXPECT_TRUE(refused("indexes", Json{{"dir", "x"},
                                      {"manifest_sha256", std::string(64, 'a')},
                                      {"files", 1}})); // unknown key
  EXPECT_TRUE(refused("thirteenf", Json("dir")));      // not an object
  EXPECT_TRUE(refused("thirteenf", Json(nullptr)));    // gia_13f without its stage
  EXPECT_TRUE(refused("indexes", Json(nullptr)));      // russell_recon without its stage
}
