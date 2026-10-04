// The publish-last manifest, the producer identity and the reuse decision of a registry build
// (migration slice 3; P9 contract K-P9-3, fields review FD-1): the manifest is published after the
// receipt and never replaced, every built entry carries the engine producer block, and a prior
// payload is reused only by the same executable on the same role, seal, formula and source bytes.
// The payloads stay byte-identical to the Python builder's fixture outputs throughout.
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/research/fields/file_io.hpp"
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/research_fields_cli.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

const std::vector<std::string> kNames{"si_shares", "si_dtc", "vol_126"};

fields::ProducerIdentity identity(char digit) {
  return fields::ProducerIdentity{std::string(64, digit), "feedface", "Debug"};
}

Json read_json(const fs::path &path) { return Json::parse(support::read_bytes(path)); }

// A registry build of the three ported fields into dir/<tag> (created), receipt dir/<tag>.json.
atx::core::Result<fields::RegistryBuild>
build_into(const fs::path &dir, const std::string &tag, const fields::ProducerIdentity &producer,
           const std::optional<fields::ReuseRequest> &reuse = std::nullopt) {
  const fs::path out = dir / tag;
  fs::create_directories(out);
  auto spec = support::fixture_spec(out, kNames);
  spec.reuse = reuse;
  const auto registry = support::engine_registry();
  const std::string spec_sha256(64, 'a');
  ATX_TRY(auto built, fields::build_registry_fields(spec, registry, spec_sha256, producer));
  ATX_TRY_VOID(fields::write_registry_build(built, dir / (tag + ".json"), out));
  return atx::core::Ok(std::move(built));
}

fields::ReuseRequest reuse_from(const fs::path &dir) { return fields::ReuseRequest{dir, {}}; }

void rewrite_manifest(const fs::path &dir, const std::function<void(Json &)> &mutate) {
  Json manifest = read_json(dir / "manifest.json");
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

void expect_fixture_payloads(const fs::path &out) {
  for (const auto &name : kNames) {
    EXPECT_EQ(support::read_bytes(out / (name + ".f64")),
              support::read_bytes(support::fixture_dir() / "expected" / (name + ".f64")))
        << name << ": payload bytes differ from the Python builder's";
  }
}

} // namespace

TEST(ResearchFieldsManifest, PublishLast) {
  const auto dir = support::scratch("manifest_publish");
  const fs::path path = dir / "manifest.json";
  ASSERT_TRUE(fields::publish_exclusive(path, "{\"a\": 1}\n").has_value());
  EXPECT_EQ(support::read_bytes(path), "{\"a\": 1}\n");
  EXPECT_FALSE(fs::exists(dir / ".manifest.json.pending"));
  // A published manifest is never replaced; the refused publish leaves its pending file, never the
  // published name.
  EXPECT_FALSE(fields::publish_exclusive(path, "{}\n").has_value());
  EXPECT_EQ(support::read_bytes(path), "{\"a\": 1}\n");
  EXPECT_TRUE(fs::exists(dir / ".manifest.json.pending"));
  // The streamed digest and exclusive copy that reuse rests on.
  const auto digest = fields::digest_file(path);
  ASSERT_TRUE(digest.has_value()) << digest.error().message();
  EXPECT_EQ(digest->bytes, 9U);
  EXPECT_EQ(digest->sha256, support::sha256_of("{\"a\": 1}\n"));
  const auto copied = fields::copy_exclusive(path, dir / "copy.json");
  ASSERT_TRUE(copied.has_value()) << copied.error().message();
  EXPECT_EQ(copied->sha256, digest->sha256);
  EXPECT_FALSE(fields::copy_exclusive(path, dir / "copy.json").has_value());
}

TEST(ResearchFieldsManifest, ProducerBlock) {
  const fields::ProducerIdentity id{std::string(64, 'c'), "abc123-dirty", "Release"};
  const Json block = fields::producer_block(id, std::string(64, 'd'));
  EXPECT_EQ(block, (Json{{"kind", "engine"},
                         {"exe_sha256", std::string(64, 'c')},
                         {"git_sha", "abc123-dirty"},
                         {"build_type", "Release"},
                         {"receipt_sha256", std::string(64, 'd')}}));
  const auto back = fields::engine_producer_of(Json{{"producer", block}});
  ASSERT_TRUE(back.has_value()) << back.error().message();
  ASSERT_TRUE(back->has_value());
  EXPECT_TRUE(**back == id);
  // The legacy Python shape (no kind) and an entry without a producer are not engine producers.
  const Json python{{"producer", Json{{"module", "research_fields_price.py"},
                                      {"code_sha256", std::string(64, '0')}}}};
  EXPECT_FALSE(fields::engine_producer_of(python).value().has_value());
  EXPECT_FALSE(fields::engine_producer_of(Json{{"name", "vol_126"}}).value().has_value());
  // Another kind, or an engine block without its identity keys, is refused.
  Json other{{"producer", block}};
  other["producer"]["kind"] = "python";
  EXPECT_FALSE(fields::engine_producer_of(other).has_value());
  Json partial{{"producer", block}};
  partial["producer"].erase("git_sha");
  EXPECT_FALSE(fields::engine_producer_of(partial).has_value());
  EXPECT_TRUE(fields::is_known(id));
  EXPECT_FALSE(fields::is_known(fields::ProducerIdentity{"unknown", "feedface", "Debug"}));
  EXPECT_FALSE(fields::build_git_sha().empty());
  EXPECT_FALSE(fields::build_type_name().empty());
}

// atx-research-fields build --registry: payloads equal the fixture, the receipt is written first
// and the manifest published last, every entry carries this executable's producer block.
TEST(ResearchFieldsManifest, RegistryBuildPublishesTheManifestLast) {
  const auto dir = support::scratch("manifest_main");
  fs::create_directories(dir / "out");
  support::write_bytes(dir / "registry.json",
                       support::registry_document({support::ported_row("si_shares"),
                                                   support::ported_row("si_dtc"),
                                                   support::ported_row("vol_126")})
                           .dump(2));
  const Json spec{{"schema", "atx.research-fields-spec/v2"},
                  {"role", Json{{"dir", (support::fixture_dir() / "role").string()},
                                {"manifest_sha256", support::fixture_role_sha256()}}},
                  {"output_dir", (dir / "out").string()},
                  {"fields", Json::array({"vol_126", "si_dtc", "si_shares"})},
                  {"finra", (support::fixture_dir() / "finra").string()}};
  support::write_bytes(dir / "spec.json", spec.dump());
  const std::string spec_path = (dir / "spec.json").string();
  const std::string receipt_path = (dir / "receipt.json").string();
  const std::string registry_path = (dir / "registry.json").string();
  const std::vector<std::string_view> args{"atx-research-fields", "build",     "--spec",
                                           spec_path,             "--receipt", receipt_path,
                                           "--registry",          registry_path};
  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(fields::research_fields_main(args, out, err), 0) << err.str();
  expect_fixture_payloads(dir / "out");
  EXPECT_FALSE(fs::exists(dir / "out" / ".manifest.json.pending"));
  const std::string receipt_bytes = support::read_bytes(dir / "receipt.json");
  const Json receipt = Json::parse(receipt_bytes);
  const Json manifest = read_json(dir / "out" / "manifest.json");
  const Json expected = read_json(support::fixture_dir() / "expected" / "manifest.normalized.json");
  const auto self = fields::current_producer();
  ASSERT_TRUE(self.has_value()) << self.error().message();
  const Json producer = fields::producer_block(*self, support::sha256_of(receipt_bytes));
  EXPECT_EQ(manifest.at("schema"), "atx.research-role-fields/v1");
  EXPECT_EQ(manifest.at("status"), "complete");
  EXPECT_EQ(manifest.at("receipt_sha256"), support::sha256_of(receipt_bytes));
  EXPECT_EQ(manifest.at("spec_sha256"), support::sha256_of(spec.dump()));
  EXPECT_EQ(manifest.at("registry").at("sha256"),
            support::sha256_of(support::read_bytes(dir / "registry.json")));
  EXPECT_EQ(manifest.at("seal").at("exclusive_end"), "2024-01-01");
  EXPECT_EQ(manifest.at("role").at("manifest_sha256"), support::fixture_role_sha256());
  EXPECT_EQ(manifest.at("engine").at("exe_sha256"), self->exe_sha256);
  EXPECT_FALSE(manifest.contains("reuse"));
  EXPECT_EQ(receipt.at("registry").at("sha256"), manifest.at("registry").at("sha256"));
  EXPECT_EQ(receipt.at("reused"), Json::array());
  // Registration order, whatever the spec order; the producer block of K-P9-3 on every entry.
  ASSERT_EQ(manifest.at("fields").size(), kNames.size());
  const auto plans = fields::plan_fields(support::fixture_spec(dir / "out", kNames), nullptr);
  ASSERT_TRUE(plans.has_value()) << plans.error().message();
  for (std::size_t i = 0; i < kNames.size(); ++i) {
    const Json &e = manifest.at("fields").at(i);
    const std::string &name = kNames[i];
    EXPECT_EQ(e.at("name"), name);
    EXPECT_EQ(e.at("producer"), producer) << name;
    EXPECT_EQ(e.at("producer").size(), 5U) << name;
    const Json &python = [&]() -> const Json & {
      for (const Json &p : expected.at("fields")) {
        if (p.at("name") == name) {
          return p;
        }
      }
      throw std::runtime_error("no expected entry " + name);
    }();
    EXPECT_EQ(e.at("sha256"), python.at("sha256")) << name;
    EXPECT_EQ(manifest.at("files").at(name + ".f64").at("sha256"), python.at("sha256")) << name;
    EXPECT_EQ(e.at("coverage").at("member_cells"), python.at("coverage").at("member_cells"));
    EXPECT_EQ(e.at("formula_sha256"),
              fields::formula_sha256(support::ported_spec(name)).value());
    // The kind's planned inputs are the sources its build records, in order.
    const auto &inputs = plans->at(i).inputs;
    ASSERT_EQ(e.at("sources").size(), inputs.size()) << name;
    for (std::size_t s = 0; s < inputs.size(); ++s) {
      EXPECT_EQ(e.at("sources").at(s).at("path"), fields::record_path(inputs[s])) << name;
    }
  }
  EXPECT_EQ(manifest.at("source_checks").at("si_shares").at("rows_sealed_dropped"), 1);
  // Never replaced: a second build into the same directory is refused, the manifest untouched.
  const std::string before = support::read_bytes(dir / "out" / "manifest.json");
  ASSERT_TRUE(fs::remove(dir / "receipt.json"));
  EXPECT_EQ(fields::research_fields_main(args, out, err), 1);
  EXPECT_EQ(support::read_bytes(dir / "out" / "manifest.json"), before);
  // The spec schema follows the flag: a v2 spec without --registry is a spec error.
  const std::vector<std::string_view> plain{"atx-research-fields", "build", "--spec", spec_path,
                                            "--receipt", receipt_path};
  EXPECT_EQ(fields::research_fields_main(plain, out, err), 2);
}

TEST(ResearchFieldsManifest, ReuseHitOnTheSameExecutable) {
  const auto dir = support::scratch("manifest_reuse_hit");
  const auto id = identity('1');
  const auto first = build_into(dir, "a", id);
  ASSERT_TRUE(first.has_value()) << first.error().message();
  EXPECT_EQ(first->built, 3U);
  const auto second = build_into(dir, "b", id, reuse_from(dir / "a"));
  ASSERT_TRUE(second.has_value()) << second.error().message();
  EXPECT_EQ(second->reused, 3U);
  EXPECT_EQ(second->built, 0U);
  expect_fixture_payloads(dir / "b");
  const Json a = read_json(dir / "a" / "manifest.json");
  const Json b = read_json(dir / "b" / "manifest.json");
  const Json &reuse = b.at("reuse");
  EXPECT_EQ(reuse.at("reused"), Json(kNames));
  EXPECT_TRUE(reuse.at("recomputed").empty()) << reuse.dump();
  EXPECT_EQ(reuse.at("seal"), "match");
  EXPECT_EQ(reuse.at("manifest_sha256"),
            support::sha256_of(support::read_bytes(dir / "a" / "manifest.json")));
  for (std::size_t i = 0; i < kNames.size(); ++i) {
    Json got = b.at("fields").at(i);
    const Json &prior = a.at("fields").at(i);
    // The producer still names the build that wrote the payload (a's receipt).
    EXPECT_EQ(got.at("producer"), prior.at("producer")) << kNames[i];
    EXPECT_EQ(got.at("reused_from").at("payload_sha256"), prior.at("sha256"));
    EXPECT_EQ(got.at("reused_from").at("mode"), "copy");
    got.erase("reused_from");
    EXPECT_EQ(got, prior) << kNames[i];
  }
  EXPECT_EQ(b.at("files"), a.at("files"));
  EXPECT_EQ(b.at("source_checks"), a.at("source_checks"));
  const Json receipt = read_json(dir / "b.json");
  EXPECT_TRUE(receipt.at("fields").empty());
  EXPECT_EQ(receipt.at("reused"), Json(kNames));
}

TEST(ResearchFieldsManifest, ReuseMissOnAnotherOrUnknownExecutable) {
  const auto dir = support::scratch("manifest_reuse_exe");
  ASSERT_TRUE(build_into(dir, "a", identity('1')).has_value());
  const auto other = build_into(dir, "b", identity('2'), reuse_from(dir / "a"));
  ASSERT_TRUE(other.has_value()) << other.error().message();
  EXPECT_EQ(other->reused, 0U);
  EXPECT_EQ(other->built, 3U);
  expect_fixture_payloads(dir / "b"); // rebuilt, still the same bytes
  const Json b = read_json(dir / "b" / "manifest.json");
  for (const auto &name : kNames) {
    const std::string why = b.at("reuse").at("recomputed").at(name).get<std::string>();
    EXPECT_NE(why.find("producer identity differs"), std::string::npos) << why;
  }
  EXPECT_EQ(b.at("fields").at(0).at("producer").at("exe_sha256"), std::string(64, '2'));
  const fields::ProducerIdentity unknown{"unknown", "feedface", "Debug"};
  ASSERT_TRUE(build_into(dir, "c", unknown, reuse_from(dir / "a")).has_value());
  const Json c = read_json(dir / "c" / "manifest.json");
  const std::string why = c.at("reuse").at("recomputed").at("vol_126").get<std::string>();
  EXPECT_NE(why.find("identity is unknown"), std::string::npos) << why;
}

TEST(ResearchFieldsManifest, ReuseMissReasons) {
  const auto dir = support::scratch("manifest_reuse_reasons");
  const auto id = identity('1');
  ASSERT_TRUE(build_into(dir, "a", id).has_value());
  rewrite_manifest(dir / "a", [](Json &m) {
    entry_of(m, "si_shares").at("producer").erase("kind"); // the legacy Python shape
    entry_of(m, "si_dtc")["formula_sha256"] = std::string(64, '1');
    entry_of(m, "vol_126").at("sources").at(0)["sha256"] = std::string(64, '0');
  });
  const auto b = build_into(dir, "b", id, reuse_from(dir / "a"));
  ASSERT_TRUE(b.has_value()) << b.error().message();
  EXPECT_EQ(b->reused, 0U);
  const Json recomputed = read_json(dir / "b" / "manifest.json").at("reuse").at("recomputed");
  const auto reason = [&recomputed](const char *name) {
    return recomputed.at(name).get<std::string>();
  };
  EXPECT_NE(reason("si_shares").find("not produced by the engine"), std::string::npos);
  EXPECT_NE(reason("si_dtc").find("formula fingerprint differs"), std::string::npos);
  EXPECT_NE(reason("vol_126").find("source bytes differ"), std::string::npos);
  expect_fixture_payloads(dir / "b");
}

TEST(ResearchFieldsManifest, ReuseRefusesAnotherSealRoleOrPin) {
  const auto dir = support::scratch("manifest_reuse_seal");
  const auto id = identity('1');
  ASSERT_TRUE(build_into(dir, "a", id).has_value());
  const std::string original = support::read_bytes(dir / "a" / "manifest.json");
  // A prior built under another seal is refused (ruling P13: present and different).
  rewrite_manifest(dir / "a", [](Json &m) { m["seal"]["exclusive_end"] = "2025-01-01"; });
  const auto sealed = build_into(dir, "b", id, reuse_from(dir / "a"));
  ASSERT_FALSE(sealed.has_value());
  EXPECT_NE(sealed.error().message().find("seal"), std::string::npos)
      << sealed.error().message();
  // An absent seal block is recorded, not refused (wave 1).
  support::write_bytes(dir / "a" / "manifest.json", original);
  rewrite_manifest(dir / "a", [](Json &m) { m.erase("seal"); });
  const auto absent = build_into(dir, "c", id, reuse_from(dir / "a"));
  ASSERT_TRUE(absent.has_value()) << absent.error().message();
  EXPECT_EQ(absent->reused, 3U);
  EXPECT_EQ(read_json(dir / "c" / "manifest.json").at("reuse").at("seal"), "absent");
  // A prior bound to another role, or one whose SHA-256 is not the pin, is refused.
  support::write_bytes(dir / "a" / "manifest.json", original);
  rewrite_manifest(dir / "a", [](Json &m) { m["role"]["ids_sha256"] = std::string(64, '0'); });
  EXPECT_FALSE(build_into(dir, "d", id, reuse_from(dir / "a")).has_value());
  support::write_bytes(dir / "a" / "manifest.json", original);
  fields::ReuseRequest pinned = reuse_from(dir / "a");
  pinned.manifest_sha256 = std::string(64, '0');
  EXPECT_FALSE(build_into(dir, "e", id, pinned).has_value());
  pinned.manifest_sha256 = support::sha256_of(original);
  EXPECT_TRUE(build_into(dir, "f", id, pinned).has_value());
}

TEST(ResearchFieldsManifest, ReuseRefusesACorruptPriorPayload) {
  const auto dir = support::scratch("manifest_reuse_corrupt");
  const auto id = identity('1');
  ASSERT_TRUE(build_into(dir, "a", id).has_value());
  std::string payload = support::read_bytes(dir / "a" / "vol_126.f64");
  ASSERT_FALSE(payload.empty());
  payload[payload.size() / 2] = static_cast<char>(payload[payload.size() / 2] ^ 0x01);
  support::write_bytes(dir / "a" / "vol_126.f64", payload);
  const auto b = build_into(dir, "b", id, reuse_from(dir / "a"));
  ASSERT_FALSE(b.has_value());
  EXPECT_NE(b.error().message().find("corrupt prior directory"), std::string::npos)
      << b.error().message();
  EXPECT_FALSE(fs::exists(dir / "b" / "manifest.json"));
}
