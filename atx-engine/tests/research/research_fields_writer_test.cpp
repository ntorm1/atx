// The field writer and its numpy-exact statistics (migration slice 1). Expected values were
// computed with numpy 1.26.4 / Python 3.12 (the interpreter the Python builder runs on).
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/engine/research/fields/field_stats.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using atx::f64;
using atx::u64;

TEST(ResearchFieldsWriter, PairwiseSumIsNumpys) {
  EXPECT_TRUE(support::same_bits(fields::numpy_pairwise_sum({}), 0.0));
  const std::vector<f64> negative_zero{-0.0};
  EXPECT_TRUE(
      support::same_bits(fields::numpy_pairwise_sum(negative_zero), 0.0)); // sum starts at +0.0
  const std::vector<f64> seven{1e16, 1, 1, 1, 1, 1, 1};
  EXPECT_EQ(fields::numpy_pairwise_sum(seven), 1e16); // n < 8: in order, every +1 rounds away
  const std::vector<f64> nine{1e16, 1, 1, 1, 1, 1, 1, 1, 1};
  EXPECT_EQ(fields::numpy_pairwise_sum(nine),
            1.0000000000000008e16); // eight accumulators, then the tail
  const std::vector<f64> blocked{1.0, 1e16, -1e16, 1.0, 0, 0, 0, 0, 0};
  EXPECT_EQ(fields::numpy_pairwise_sum(blocked), 0.0); // a sequential sum would give 1.0
  std::vector<f64> large(1000);
  for (std::size_t i = 0; i < large.size(); ++i) {
    large[i] = 1.0 / static_cast<f64>(i + 1);
  }
  f64 sequential = 0.0;
  for (const f64 x : large) {
    sequential += x;
  }
  EXPECT_NEAR(fields::numpy_pairwise_sum(large), sequential,
              1e-12); // the recursive split path stays a sum
}

TEST(ResearchFieldsWriter, LinearQuantileIsNumpys) {
  const std::vector<f64> four{1.0, 2.0, 3.0, 4.0};
  EXPECT_EQ(fields::numpy_linear_quantile(four, 0.5), 2.5);
  EXPECT_EQ(fields::numpy_linear_quantile(four, 0.001), 1.003);
  EXPECT_EQ(fields::numpy_linear_quantile(four, 0.999), 3.997);
  EXPECT_EQ(fields::numpy_linear_quantile(std::vector<f64>{7.0}, 0.5), 7.0);
  EXPECT_EQ(fields::numpy_linear_quantile(std::vector<f64>{1.0, 2.0}, 0.75), 1.75);
  std::vector<f64> zeros(10, -0.0);
  zeros.push_back(5.0);
  // numpy's _lerp turns the signed zero positive: -0.0 + (0.0 * gamma) = +0.0.
  EXPECT_TRUE(support::same_bits(fields::numpy_linear_quantile(zeros, 0.001), 0.0));
  EXPECT_TRUE(support::same_bits(fields::numpy_linear_quantile(zeros, 0.01), 0.0));
}

TEST(ResearchFieldsWriter, RoundedFractionIsPythonRound) {
  EXPECT_EQ(fields::rounded_fraction(1, 3), 0.333333);
  EXPECT_EQ(fields::rounded_fraction(2, 3), 0.666667);
  EXPECT_EQ(fields::rounded_fraction(385, 2255), 0.170732);
  EXPECT_EQ(fields::rounded_fraction(169, 1302), 0.1298);
  EXPECT_EQ(fields::rounded_fraction(1, 8), 0.125);
  EXPECT_FALSE(fields::rounded_fraction(0, 0).has_value());
}

namespace {

// Three sessions (2021-12-31, 2022-01-03, 2022-01-04) x two ids; member {1 1 / 1 0 / 0 1}; scored
// from row 1.
fields::RoleAxes tiny_role(const support::fs::path &dir) {
  support::TinyRole r;
  r.days = {fields::days_from_civil(2021, 12, 31), fields::days_from_civil(2022, 1, 3),
            fields::days_from_civil(2022, 1, 4)};
  r.ids = {7, 9};
  r.member = {1, 1, 1, 0, 0, 1};
  r.present = {1, 1, 1, 1, 1, 1};
  r.volume = {1, 1, 1, 1, 1, 1};
  r.score_begin = 1;
  const std::string sha = support::write_role(dir, r);
  return fields::RoleAxes::load(dir, sha).value();
}

} // namespace

TEST(ResearchFieldsWriter, RoleAxesRefuseAWrongPinAndASealedSession) {
  const auto dir = support::scratch("role_pins");
  support::TinyRole r;
  r.days = {fields::seal_day() - 1, fields::seal_day()};
  r.ids = {7};
  r.member = {1, 1};
  r.present = {1, 1};
  r.volume = {1, 1};
  const std::string sha = support::write_role(dir / "role", r);
  EXPECT_FALSE(fields::RoleAxes::load(dir / "role", std::string(64, '0')).has_value());
  const auto sealed = fields::RoleAxes::load(dir / "role", sha);
  ASSERT_FALSE(sealed.has_value());
  EXPECT_NE(sealed.error().message().find("research seal"), std::string::npos);
}

TEST(ResearchFieldsWriter, CanonicalNanAndCoverage) {
  const auto dir = support::scratch("writer_coverage");
  const auto role = tiny_role(dir / "role");
  ASSERT_EQ(role.column_of(9), std::optional<std::size_t>{1});
  EXPECT_FALSE(role.column_of(8).has_value());
  auto writer = fields::FieldWriter::create(dir, "f", role).value();
  const f64 inf = std::numeric_limits<f64>::infinity();
  const f64 odd_nan = std::bit_cast<f64>(0xfff8000000000123ULL);
  ASSERT_TRUE(writer.write(std::vector<f64>{inf, 2.0}).has_value());
  ASSERT_TRUE(writer.write(std::vector<f64>{odd_nan, 3.0}).has_value());
  ASSERT_TRUE(writer.write(std::vector<f64>{4.0, -inf}).has_value());
  const auto written = writer.close().value();
  const std::string bytes = support::read_bytes(dir / "f.f64");
  ASSERT_EQ(bytes.size(), 6 * sizeof(f64));
  std::vector<u64> bits(6);
  std::memcpy(bits.data(), bytes.data(), bytes.size());
  const u64 nan_bits = 0x7ff8000000000000ULL;
  EXPECT_EQ(bits, (std::vector<u64>{nan_bits, std::bit_cast<u64>(2.0), nan_bits,
                                    std::bit_cast<u64>(3.0), std::bit_cast<u64>(4.0), nan_bits}));
  EXPECT_EQ(written.sha256, support::sha256_of(bytes));
  EXPECT_EQ(written.bytes, 48U);
  const auto &c = written.coverage;
  EXPECT_EQ(c.member_cells, 4U);
  EXPECT_EQ(c.finite_member_cells, 1U); // only (row 0, id 9) = 2.0 is finite and a member
  EXPECT_EQ(c.finite_cells_all, 3U);
  EXPECT_EQ(c.score_member_cells, 2U);
  EXPECT_EQ(c.score_finite_member_cells, 0U);
  ASSERT_EQ(c.per_year.size(), 2U);
  EXPECT_EQ(c.per_year[0].year, 2021);
  EXPECT_EQ(c.per_year[0].member_cells, 2U);
  EXPECT_EQ(c.per_year[0].finite_member_cells, 1U);
  EXPECT_EQ(c.per_year[1].year, 2022);
  EXPECT_EQ(c.per_year[1].member_cells, 2U);
  EXPECT_EQ(c.per_year[1].finite_member_cells, 0U);
  EXPECT_EQ(c.member_finite_min, 2.0);
  EXPECT_EQ(c.member_finite_max, 2.0);
  EXPECT_EQ(c.member_finite_mean, 2.0);
  ASSERT_TRUE(c.member_finite_quantiles.has_value());
  EXPECT_EQ(c.member_finite_quantiles->p0_1, 2.0);
  EXPECT_EQ(c.member_finite_quantiles->p99_9, 2.0);
}

TEST(ResearchFieldsWriter, ExclusiveCreateAndShapeChecks) {
  const auto dir = support::scratch("writer_checks");
  const auto role = tiny_role(dir / "role");
  auto writer = fields::FieldWriter::create(dir, "g", role).value();
  EXPECT_FALSE(fields::FieldWriter::create(dir, "g", role)
                   .has_value()); // an existing file is never truncated
  EXPECT_FALSE(writer.write(std::vector<f64>{1.0}).has_value()); // wrong row size
  ASSERT_TRUE(writer.write(std::vector<f64>{1.0, 2.0}).has_value());
  EXPECT_FALSE(writer.close().has_value());                           // one of three rows written
  EXPECT_FALSE(writer.write(std::vector<f64>{1.0, 2.0}).has_value()); // closed
}
