// The atx-research-fields verb (migration slice 3): spec contract, the receipt of a build on the
// identity fixture (every block equals the Python manifest's), exclusive publish-last receipts.
#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/research_fields_cli.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using Json = nlohmann::json;

namespace {

const support::fs::path kFixture{ATX_RESEARCH_FIELDS_FIXTURE};

Json expected_manifest() {
  return Json::parse(support::read_bytes(kFixture / "expected" / "manifest.normalized.json"));
}

std::string spec_text(const support::fs::path &out, const std::vector<std::string> &names) {
  const Json manifest = expected_manifest();
  Json spec{{"schema", "atx.research-fields-spec/v1"},
            {"role", Json{{"dir", (kFixture / "role").string()},
                          {"manifest_sha256", manifest.at("role").at("manifest_sha256")}}},
            {"output_dir", out.string()},
            {"fields", Json(names)},
            {"finra", (kFixture / "finra").string()}};
  return spec.dump();
}

// A JSON value equal to another bit for bit: integers by value (signed or not, as Python reads
// them), floats by their bits (a float is never an integer), everything else structurally.
void expect_same(const Json &want, const Json &got, const std::string &where) {
  if (want.is_number_integer() && got.is_number_integer()) {
    EXPECT_EQ(want.get<std::int64_t>(), got.get<std::int64_t>()) << where;
    return;
  }
  ASSERT_EQ(want.type(), got.type()) << where << ": " << want.dump() << " vs " << got.dump();
  if (want.is_number_float()) {
    EXPECT_TRUE(support::same_bits(want.get<double>(), got.get<double>()))
        << where << ": " << want.dump() << " vs " << got.dump();
  } else if (want.is_object()) {
    ASSERT_EQ(want.size(), got.size()) << where << ": " << want.dump() << " vs " << got.dump();
    for (auto it = want.begin(); it != want.end(); ++it) {
      ASSERT_TRUE(got.contains(it.key())) << where << "." << it.key();
      expect_same(it.value(), got.at(it.key()), where + "." + it.key());
    }
  } else if (want.is_array()) {
    ASSERT_EQ(want.size(), got.size()) << where;
    for (std::size_t i = 0; i < want.size(); ++i) {
      expect_same(want.at(i), got.at(i), where + "[" + std::to_string(i) + "]");
    }
  } else {
    EXPECT_EQ(want, got) << where;
  }
}

const Json &entry_of(const Json &manifest, const std::string &name) {
  for (const auto &e : manifest.at("fields")) {
    if (e.at("name") == name) {
      return e;
    }
  }
  throw std::runtime_error("no entry " + name);
}

} // namespace

TEST(ResearchFieldsCli, SpecContract) {
  const auto out = support::scratch("cli_spec");
  const auto good = fields::parse_build_spec(spec_text(out, {"si_dtc", "vol_126"}));
  ASSERT_TRUE(good.has_value()) << good.error().message();
  EXPECT_EQ(good->fields, (std::vector<std::string>{"si_dtc", "vol_126"}));
  EXPECT_TRUE(good->finra.has_value());
  EXPECT_FALSE(fields::parse_build_spec("{}").has_value());
  EXPECT_FALSE(fields::parse_build_spec("not json").has_value());
  EXPECT_FALSE(fields::parse_build_spec(spec_text(out, {"mkt_cap"})).has_value()); // no builder
  EXPECT_FALSE(fields::parse_build_spec(spec_text(out, {"vol_126", "vol_126"})).has_value());
  EXPECT_FALSE(fields::parse_build_spec(spec_text(out, {})).has_value());
  Json no_finra = Json::parse(spec_text(out, {"si_shares"}));
  no_finra.erase("finra");
  EXPECT_FALSE(fields::parse_build_spec(no_finra.dump()).has_value()); // a FINRA field needs finra
  Json extra = Json::parse(spec_text(out, {"vol_126"}));
  extra["window"] = "train";
  EXPECT_FALSE(fields::parse_build_spec(extra.dump()).has_value()); // unknown keys are refused
}

// The receipt of a build on the fixture carries, field by field, exactly the blocks the Python
// manifest records (coverage with numpy's quantiles, sources, FINRA checks, vintage, extra).
TEST(ResearchFieldsCli, ReceiptEqualsThePythonManifest) {
  const auto out = support::scratch("cli_build");
  const std::vector<std::string> names{"si_shares", "si_dtc", "vol_126"};
  const auto spec = fields::parse_build_spec(spec_text(out, names));
  ASSERT_TRUE(spec.has_value());
  const auto text = fields::build_fields(*spec, std::string(64, 'a'));
  ASSERT_TRUE(text.has_value()) << text.error().message();
  const Json receipt = Json::parse(*text);
  const Json manifest = expected_manifest();
  EXPECT_EQ(receipt.at("schema"), "atx.research-fields-receipt/v1");
  EXPECT_EQ(receipt.at("status"), "complete");
  EXPECT_EQ(receipt.at("spec_sha256"), std::string(64, 'a'));
  EXPECT_EQ(receipt.at("role").at("manifest_sha256"), manifest.at("role").at("manifest_sha256"));
  ASSERT_EQ(receipt.at("fields").size(), names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    const Json &got = receipt.at("fields").at(i);
    const Json &want = entry_of(manifest, names[i]);
    EXPECT_EQ(got.at("name"), names[i]);
    EXPECT_EQ(got.at("file"), names[i] + ".f64");
    EXPECT_EQ(got.at("sha256"), want.at("sha256"));
    EXPECT_EQ(got.at("bytes"), manifest.at("files").at(names[i] + ".f64").at("bytes"));
    expect_same(want.at("coverage"), got.at("coverage"), names[i] + ".coverage");
    ASSERT_EQ(got.at("sources").size(), want.at("sources").size());
    for (std::size_t s = 0; s < want.at("sources").size(); ++s) {
      EXPECT_EQ(got.at("sources").at(s).at("sha256"), want.at("sources").at(s).at("sha256"));
      EXPECT_EQ(got.at("sources").at(s).at("bytes"), want.at("sources").at(s).at("bytes"));
    }
    EXPECT_EQ(support::read_bytes(out / (names[i] + ".f64")),
              support::read_bytes(kFixture / "expected" / (names[i] + ".f64")));
  }
  for (const char *finra : {"si_shares", "si_dtc"}) {
    const Json &got = receipt.at("fields").at(finra == std::string("si_shares") ? 0U : 1U);
    expect_same(manifest.at("source_checks").at(finra), got.at("source_checks"),
                std::string(finra) + ".source_checks");
    EXPECT_EQ(got.at("extra").at("vintage_safe_from"),
              entry_of(manifest, finra).at("vintage_safe_from"));
  }
  EXPECT_EQ(receipt.at("fields").at(2).at("formula_sha256"),
            entry_of(manifest, "vol_126").at("formula_sha256"));
}

TEST(ResearchFieldsCli, MainPublishesTheReceiptLastAndOnce) {
  const auto dir = support::scratch("cli_main");
  support::fs::create_directories(dir / "out");
  support::write_bytes(dir / "spec.json", spec_text(dir / "out", {"vol_126"}));
  const std::string spec = (dir / "spec.json").string();
  const std::string receipt = (dir / "receipt.json").string();
  const std::vector<std::string_view> args{"atx-research-fields", "build", "--spec", spec,
                                           "--receipt",           receipt};
  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(fields::research_fields_main(args, out, err), 0) << err.str();
  const Json written = Json::parse(support::read_bytes(dir / "receipt.json"));
  EXPECT_EQ(written.at("spec_sha256"), support::sha256_of(support::read_bytes(dir / "spec.json")));
  // A second build finds the payload in place (exclusive create): exit 1, the receipt untouched.
  const std::string before = support::read_bytes(dir / "receipt.json");
  EXPECT_EQ(fields::research_fields_main(args, out, err), 1);
  EXPECT_EQ(support::read_bytes(dir / "receipt.json"), before);
  // Usage errors exit 2.
  const std::vector<std::string_view> bad{"atx-research-fields", "build", "--spec", spec};
  EXPECT_EQ(fields::research_fields_main(bad, out, err), 2);
  const std::vector<std::string_view> verb{"atx-research-fields", "fit"};
  EXPECT_EQ(fields::research_fields_main(verb, out, err), 2);
}
