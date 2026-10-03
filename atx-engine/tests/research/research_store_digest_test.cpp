// atx.record-digest/v1 and atx.catalog-digest/v1 (DigestStream, the descriptor encode()).

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/store/digest.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/rows_core.hpp"
#include "research/research_store_test_support.hpp"

namespace store = atx::engine::research::store;
using store::DigestStream;

namespace {

std::string sha256_of(std::string_view bytes) {
  auto hex = atx::core::sha256_hex(bytes);
  EXPECT_TRUE(hex);
  return hex ? *hex : std::string{};
}

atx::u64 parse_hex64(const std::string &hex) {
  return static_cast<atx::u64>(std::strtoull(hex.c_str(), nullptr, 16));
}

std::vector<std::byte> parse_hex_bytes(const std::string &hex) {
  std::vector<std::byte> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<std::byte>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

// The generic oracle path: feed one golden_rows.json row to a DigestStream by column type.
std::string stream_digest(const nlohmann::json &row) {
  DigestStream stream;
  stream.begin_row(row.at("table").get<std::string>(), row.at("version").get<atx::i32>());
  for (const nlohmann::json &column : row.at("columns")) {
    const std::string name = column.at("name").get<std::string>();
    const std::string type = column.at("type").get<std::string>();
    if (column.contains("value") && column.at("value").is_null()) {
      stream.add_null(name);
    } else if (type == "int") {
      stream.add_int(name, column.at("value").get<atx::i64>());
    } else if (type == "bool") {
      stream.add_bool(name, column.at("value").get<bool>());
    } else if (type == "real") {
      stream.add_real(name, std::bit_cast<atx::f64>(parse_hex64(column.at("bits"))));
    } else if (type == "u64") {
      stream.add_u64(name, parse_hex64(column.at("hex")));
    } else if (type == "blob") {
      stream.add_blob(name, parse_hex_bytes(column.at("hex").get<std::string>()));
    } else {
      stream.add_text(name, column.at("value").get<std::string>());
    }
  }
  return stream.finish();
}

} // namespace

TEST(ResearchStoreDigest, EncodingLiteral) {
  DigestStream stream;
  stream.begin_row("toy", 2);
  stream.add_int("i", -5);
  stream.add_text("t", "ab");
  stream.add_null("n");
  stream.add_bool("b", true);
  stream.add_real("r", 1.0);
  stream.add_u64("u", 255);
  const std::vector<std::byte> blob{std::byte{0x01}, std::byte{0x0A}};
  stream.add_blob("x", blob);
  const std::string expected_bytes{"atx.record-digest/v1\n"
                                   "row toy@2\n"
                                   "i=i-5\n"
                                   "t=t2:ab\n"
                                   "n=~\n"
                                   "b=b1\n"
                                   "r=r3ff0000000000000\n"
                                   "u=u00000000000000ff\n"
                                   "x=x2:\x01\x0a\n"};
  EXPECT_EQ(stream.finish(), sha256_of(expected_bytes));

  DigestStream catalog{DigestStream::Kind::Catalog};
  catalog.add_entry("artifact", std::string(64, 'f'));
  EXPECT_EQ(catalog.finish(),
            sha256_of("atx.catalog-digest/v1\nartifact " + std::string(64, 'f') + "\n"));
}

TEST(ResearchStoreDigest, GoldenVectors) {
  const nlohmann::json rows =
      nlohmann::json::parse(store::test::read_bytes(store::test::fixture("golden_rows.json")));
  const nlohmann::json golden =
      nlohmann::json::parse(store::test::read_bytes(store::test::fixture("golden_digests.json")));
  ASSERT_TRUE(rows.is_array());
  ASSERT_EQ(rows.size(), golden.size());
  for (const nlohmann::json &row : rows) {
    const std::string name = row.at("name").get<std::string>();
    EXPECT_EQ(stream_digest(row), golden.at(name).get<std::string>()) << name;
  }
  // The same vectors through the descriptors: the toy row and a catalog_core artifact row.
  store::test::ToyRow toy = store::test::sample_toy("key-1");
  toy.s.clear();
  for (int n = 0; n < 32; ++n) {
    toy.s += "ab";
  }
  EXPECT_EQ(store::row_digest(store::test::kToyTable, toy),
            golden.at("toy-every-type").get<std::string>());
  store::ArtifactRow artifact;
  artifact.path_key = "scripts/specs/p9/a.json";
  artifact.path = "scripts/specs/p9/A.json";
  artifact.sha256 = std::string(64, 'a');
  artifact.bytes = 120;
  artifact.artifact_class = "spec";
  artifact.sha_source = "verified";
  artifact.eol = "lf";
  EXPECT_EQ(store::digest(artifact), golden.at("artifact").get<std::string>());
}

TEST(ResearchStoreDigest, VolatileColumnsExcluded) {
  store::test::ToyRow a = store::test::sample_toy("k");
  store::test::ToyRow b = a;
  b.v = 99; // kVolatile
  EXPECT_EQ(store::row_digest(store::test::kToyTable, a),
            store::row_digest(store::test::kToyTable, b));
  b.v = std::nullopt;
  EXPECT_EQ(store::row_digest(store::test::kToyTable, a),
            store::row_digest(store::test::kToyTable, b));
  b.i = a.i + 1; // not volatile
  EXPECT_NE(store::row_digest(store::test::kToyTable, a),
            store::row_digest(store::test::kToyTable, b));
  // A volatile table's rows still have a record digest of their non-volatile columns.
  store::CatalogRunRow run;
  run.catalog_run_id = "r";
  run.roots = "[]";
  run.started_utc = "t";
  EXPECT_EQ(store::digest(run).size(), 64U);
}

TEST(ResearchStoreDigest, RealsByBitPattern) {
  const auto real_digest = [](atx::f64 value) {
    DigestStream stream;
    stream.begin_row("reals", 1);
    stream.add_real("r", value);
    return stream.finish();
  };
  EXPECT_NE(real_digest(0.0), real_digest(-0.0));
  const atx::f64 quiet = std::bit_cast<atx::f64>(0x7FF8000000000000ULL);
  const atx::f64 payload = std::bit_cast<atx::f64>(0x7FF8000000000001ULL);
  ASSERT_TRUE(std::isnan(quiet) && std::isnan(payload));
  EXPECT_NE(real_digest(quiet), real_digest(payload));
  EXPECT_EQ(real_digest(-0.0),
            sha256_of("atx.record-digest/v1\nrow reals@1\nr=r8000000000000000\n"));
  EXPECT_EQ(real_digest(payload),
            sha256_of("atx.record-digest/v1\nrow reals@1\nr=r7ff8000000000001\n"));
}
