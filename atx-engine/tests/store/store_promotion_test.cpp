// store_promotion_test.cpp — env_config kv + cross-env promotion via ATTACH (Task 8).
#include <filesystem>
#include <string>
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/alpha_catalog.hpp"
#include "atx/engine/store/env_config.hpp"
#include "atx/engine/store/promotion.hpp"

namespace atxtest_store_promotion_test {
using atx::engine::store::StoreDb;
namespace cat = atx::engine::store::alpha_catalog;
namespace ec = atx::engine::store::env_config;
namespace pr = atx::engine::store::promotion;

[[nodiscard]] std::string tmpdir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_store_promo" /
      (std::string(info->test_suite_name()) + "_" + info->name());
  std::error_code e; std::filesystem::remove_all(dir, e); std::filesystem::create_directories(dir, e);
  return dir.string();
}

TEST(EnvConfig, SetGetRoundTrip) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  ASSERT_TRUE(ec::set(s->db(), "cost_model", "flat_5bps").has_value());
  auto v = ec::get(s->db(), "cost_model"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "flat_5bps");
  EXPECT_FALSE(ec::get(s->db(), "missing").has_value());
}

TEST(Promotion, PromotesAlphaIdentityIntoDestEnv) {
  const std::string dir = tmpdir();
  const std::string dev = dir + "/atx_dev.sqlite";
  const std::string uat = dir + "/atx_uat.sqlite";
  // seed dev with an alpha + lineage
  { auto d = StoreDb::open(dev); ASSERT_TRUE(d.has_value());
    ASSERT_TRUE(cat::upsert(d->db(), 0xABCull, 1, "rank(close)", 10, "run1").has_value());
    ASSERT_TRUE(cat::add_lineage(d->db(), 0xABCull, 0x111ull, 7, 42).has_value()); }
  // create dest env so its schema exists
  { auto u = StoreDb::open(uat); ASSERT_TRUE(u.has_value()); }
  // promote dev -> uat
  auto d = StoreDb::open(dev); ASSERT_TRUE(d.has_value());
  pr::PromotionRequest req{0xABCull, "dev", "uat", "run1", "nathan", /*ts*/20, uat};
  ASSERT_TRUE(pr::promote(d->db(), req).has_value());
  // verify the alpha now exists in uat
  auto u = StoreDb::open(uat); ASSERT_TRUE(u.has_value());
  auto ex = cat::exists(u->db(), 0xABCull); ASSERT_TRUE(ex.has_value()); EXPECT_TRUE(*ex);
}

}  // namespace atxtest_store_promotion_test
