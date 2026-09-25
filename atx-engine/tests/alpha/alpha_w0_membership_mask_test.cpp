#include <array>
#include <bit>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/fusion.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx_test_w0_membership_vm {
namespace alpha = atx::engine::alpha;
constexpr atx::usize kDates = 8;

alpha::Panel panel(atx::usize names) {
  std::vector<double> close(kDates * names);
  std::vector<double> sector(kDates * names, 1.0);
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize i = 0; i < names; ++i) {
      close[d * names + i] = 20.0 + static_cast<double>(d * (i + 1) + i * i);
    }
    if (names == 4) close[d * names + 3] = 1e6 + static_cast<double>(d);
  }
  return alpha::Panel::create(kDates, names, {"close", "sector"},
                              {std::move(close), std::move(sector)}, {}).value();
}

std::vector<atx::u8> mask() {
  std::vector<atx::u8> out(kDates * 4, 1U);
  for (atx::usize d = 0; d < kDates; ++d) out[d * 4 + 3] = 0;
  return out;
}

TEST(AlphaCrossSectionMembership, EveryCsOpcodeMatchesAnActuallyReducedUniverse) {
  // The reference physically lacks the excluded name; post-masking output cannot
  // reproduce it for ranks, normalization, group statistics or winsorization.
  const std::array<std::string_view, 16> dsl{
      "rank(close)", "zscore(close)", "scale(close)", "normalize(close)",
      "winsorize(close, 1)", "indneutralize(close, sector)",
      "group_neutralize(close, sector)", "group_rank(close, sector)",
      "group_zscore(close, sector)", "group_count(close, sector)",
      "group_mean(close, sector)", "group_scale(close, sector)",
      "cs_residualize(close, sector)", "quantile(close, 3)",
      "vec_sum(close)", "vec_avg(close)"};
  const alpha::Library lib;
  const auto program = alpha::compile_batch(dsl, lib);
  ASSERT_TRUE(program.has_value()) << program.error().message();
  const auto small = panel(3);
  const auto large = panel(4);
  alpha::Engine reference(small);
  const auto expected = reference.evaluate(*program);
  ASSERT_TRUE(expected.has_value());
  alpha::Engine engine(large);
  ASSERT_TRUE(engine.set_cross_section_mask(mask()).has_value());
  atx::engine::parallel::DetPool pool{2};
  engine.set_cs_pool(&pool);
  const auto fused = alpha::fuse(*program);
  ASSERT_TRUE(fused.has_value());
  const auto actual = engine.evaluate(*fused);
  ASSERT_TRUE(actual.has_value());
  for (atx::usize a = 0; a < dsl.size(); ++a) {
    for (atx::usize d = 0; d < kDates; ++d) {
      for (atx::usize i = 0; i < 3; ++i) {
        EXPECT_EQ(std::bit_cast<atx::u64>(actual->alphas[a].values[d * 4 + i]),
                  std::bit_cast<atx::u64>(expected->alphas[a].values[d * 3 + i]))
            << dsl[a] << " date " << d << " instrument " << i;
      }
      EXPECT_TRUE(std::isnan(actual->alphas[a].values[d * 4 + 3])) << dsl[a];
    }
  }
}

TEST(AlphaCrossSectionMembership, OwnsAndValidatesMaskWithoutReusingUnmaskedCache) {
  const auto input = panel(4);
  const alpha::Library lib;
  const std::array<std::string_view, 2> dsl{"rank(close)", "ts_mean(close, 3)"};
  const auto program = alpha::compile_batch(dsl, lib);
  ASSERT_TRUE(program.has_value());
  alpha::Engine engine(input);
  alpha::SubtreeCache cache{1 << 20};
  const auto unmasked = engine.evaluate(*program, &cache);
  ASSERT_TRUE(unmasked.has_value());
  ASSERT_GT(cache.stats().entries, 0U);
  const auto before = cache.stats();
  auto eligibility = mask();
  ASSERT_TRUE(engine.set_cross_section_mask(eligibility).has_value());
  eligibility.assign(eligibility.size(), 0); // Engine owns an independent mask.
  EXPECT_FALSE(engine.set_cross_section_mask({1U}).has_value());
  EXPECT_FALSE(engine.set_cross_section_mask(std::vector<atx::u8>(input.cells(), 2U)));
  const auto masked = engine.evaluate(*program, &cache);
  ASSERT_TRUE(masked.has_value());
  EXPECT_EQ(cache.stats().hits, before.hits);
  EXPECT_EQ(cache.stats().entries, before.entries);
  EXPECT_DOUBLE_EQ(masked->alphas[0].values[2], 1.0);
  EXPECT_NE(masked->alphas[0].values[2], unmasked->alphas[0].values[2]);
  for (atx::usize d = 2; d < kDates; ++d) {
    // A never-eligible name still retains all its raw temporal observations.
    EXPECT_DOUBLE_EQ(masked->alphas[1].values[d * 4 + 3],
                     unmasked->alphas[1].values[d * 4 + 3]);
  }
  ASSERT_TRUE(engine.set_cross_section_mask({}).has_value());
  const auto restored = engine.evaluate(*program, &cache);
  ASSERT_TRUE(restored.has_value());
  EXPECT_GT(cache.stats().hits, before.hits);
  EXPECT_EQ(restored->alphas[0].values, unmasked->alphas[0].values);
}
} // namespace atx_test_w0_membership_vm
