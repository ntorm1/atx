// vol_126 and its trailing-window rule (migration slice 1): closed-form windows, numpy's summation
// order, the look-ahead probe (and a planted leaky order it must catch), and the builder on a tiny
// role.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/trailing_mean.hpp"
#include "atx/engine/research/fields/volume_mean_field.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using atx::f64;
using atx::u8;

namespace {

using Matrix = std::vector<std::vector<f64>>; // [session][column]

// Runs the rule over `values` (every cell present). `leaky` pushes session t BEFORE reading row t:
// the planted look-ahead the probe must catch.
Matrix run_mean(const Matrix &values, std::size_t window, std::size_t min_count, bool leaky) {
  const std::size_t n = values.front().size();
  auto mean = fields::TrailingMean::create(n, window, min_count).value();
  const std::vector<u8> present(n, 1);
  Matrix out;
  for (const auto &session : values) {
    std::vector<f64> row(n);
    if (leaky) {
      mean.push(session, present);
      mean.value(row);
    } else {
      mean.value(row);
      mean.push(session, present);
    }
    out.push_back(row);
  }
  return out;
}

bool same_row(const std::vector<f64> &a, const std::vector<f64> &b) {
  for (std::size_t j = 0; j < a.size(); ++j) {
    if (!support::same_bits(a[j], b[j])) {
      return false;
    }
  }
  return true;
}

// The look-ahead probe: perturb one input of session `s`; the first output row that changes. A
// point-in-time rule of clock lag 1 must leave rows 0..s unchanged.
std::optional<std::size_t> first_changed_row(const std::function<Matrix(const Matrix &)> &rule,
                                             Matrix input, std::size_t s) {
  const Matrix before = rule(input);
  input[s][0] = input[s][0] * 1000.0 + 7.0;
  const Matrix after = rule(input);
  for (std::size_t t = 0; t < before.size(); ++t) {
    if (!same_row(before[t], after[t])) {
      return t;
    }
  }
  return std::nullopt;
}

} // namespace

TEST(ResearchFieldsVolumeMean, ClosedFormWindowOfThree) {
  auto mean = fields::TrailingMean::create(2, 3, 2).value();
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  // column 0: 1, 2, 3, 4 (all accepted); column 1: 10, NaN, -1, 20 absent (only the 10 is accepted)
  const Matrix values{{1.0, 10.0}, {2.0, nan}, {3.0, -1.0}, {4.0, 20.0}, {5.0, 0.0}};
  const std::vector<std::vector<u8>> present{{1, 1}, {1, 1}, {1, 1}, {1, 0}, {1, 1}};
  Matrix rows;
  for (std::size_t t = 0; t < values.size(); ++t) {
    std::vector<f64> row(2);
    mean.value(row);
    rows.push_back(row);
    mean.push(values[t], present[t]);
  }
  for (std::size_t t = 0; t < 3; ++t) {
    EXPECT_TRUE(std::isnan(rows[t][0]) && std::isnan(rows[t][1]))
        << "row " << t << " precedes a full window";
  }
  EXPECT_EQ(rows[3][0], 2.0);          // (1 + 2 + 3) / 3
  EXPECT_TRUE(std::isnan(rows[3][1])); // one accepted session < min_count 2
  EXPECT_EQ(rows[4][0], 3.0);          // (2 + 3 + 4) / 3
  EXPECT_TRUE(std::isnan(rows[4][1])); // NaN, negative and absent sessions are not accepted
}

TEST(ResearchFieldsVolumeMean, NegativeZeroIsAcceptedAndCounted) {
  auto mean = fields::TrailingMean::create(2, 2, 2).value();
  mean.push(std::vector<f64>{-0.0, 1.0}, std::vector<u8>{1, 1});
  mean.push(std::vector<f64>{-0.0, 3.0}, std::vector<u8>{1, 1});
  std::vector<f64> row(2);
  mean.value(row);
  EXPECT_TRUE(support::same_bits(row[0], 0.0)); // 0.0 + -0.0 + -0.0 = +0.0, two accepted sessions
  EXPECT_EQ(row[1], 2.0);
}

// Every volume here is accepted (finite, >= 0: vol_126's rule, as volume_mean_rows' `ok`), so the
// sums see every slot. Expected values: research_fields_price.volume_mean_rows' ring rule run in
// numpy 1.26.4 on these sessions (ring.sum(axis=0) / maximum(k, 1)).
TEST(ResearchFieldsVolumeMean, SumOrderIsNumpys) {
  // Two columns: numpy adds the ring's slots in slot order. After sessions 0..3 of a 3-slot window
  // the slots hold sessions 3, 1, 2: (1 + 1) + 1e16 = 1e16 + 2, where session order would give
  // (1 + 1e16) + 1 = 1e16 (each + 1 ties to the even 1e16).
  const Matrix sessions{{5.0, 1.0}, {1.0, 1.0}, {1e16, 1.0}, {1.0, 1.0}};
  auto mean = fields::TrailingMean::create(2, 3, 1).value();
  std::vector<f64> row(2);
  for (const auto &session : sessions) {
    mean.push(session, std::vector<u8>{1, 1});
  }
  mean.value(row);
  EXPECT_EQ(row[0], 3333333333333334.0); // numpy; session order gives 3333333333333333.5
  EXPECT_EQ(row[1], 1.0);
  // One column: the slots are contiguous and numpy sums them pairwise (eight accumulators):
  // ((1e16 + 0) + (1 + 1)) + ... = 1e16 + 2, where slot order gives 1e16 (numpy: ring.sum(axis=0)
  // / 9 = 1111111111111111.4 with one column, 1111111111111111.1 with two).
  const std::vector<f64> slots{1e16, 0, 1.0, 1.0, 0, 0, 0, 0, 0};
  auto single = fields::TrailingMean::create(1, 9, 1).value();
  auto pair = fields::TrailingMean::create(2, 9, 1).value();
  for (const f64 v : slots) {
    single.push(std::vector<f64>{v}, std::vector<u8>{1});
    pair.push(std::vector<f64>{v, 1.0}, std::vector<u8>{1, 1});
  }
  std::vector<f64> one(1);
  single.value(one);
  EXPECT_EQ(one[0], 1111111111111111.4);
  pair.value(row);
  EXPECT_EQ(row[0], 1111111111111111.1);
  EXPECT_EQ(row[1], 1.0);
}

// A negative volume is never accepted (vol_126's rule): its slot holds 0.0 and is not counted, as
// research_fields_price.volume_mean_rows' `ok = present & isfinite(v) & (v >= 0)`.
TEST(ResearchFieldsVolumeMean, NegativeVolumeIsNotAccepted) {
  auto mean = fields::TrailingMean::create(1, 3, 1).value();
  for (const f64 v : {4.0, -1e16, 2.0}) {
    mean.push(std::vector<f64>{v}, std::vector<u8>{1});
  }
  std::vector<f64> one(1);
  mean.value(one);
  EXPECT_EQ(one[0], 3.0); // (4 + 0 + 2) / 2 accepted sessions
}

TEST(ResearchFieldsVolumeMean, LookAheadProbeHoldsAndCatchesALeak) {
  Matrix input;
  for (std::size_t t = 0; t < 12; ++t) {
    input.push_back({static_cast<f64>(t + 1), static_cast<f64>(2 * t + 3)});
  }
  const std::size_t s = 6;
  const auto honest = [](const Matrix &m) { return run_mean(m, 3, 1, false); };
  const auto leaky = [](const Matrix &m) { return run_mean(m, 3, 1, true); };
  const auto changed = first_changed_row(honest, input, s);
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(*changed, s + 1); // row s reads sessions s-3..s-1 only
  const auto leaked = first_changed_row(leaky, input, s);
  ASSERT_TRUE(leaked.has_value());
  EXPECT_LE(*leaked, s); // the probe flags a rule that reads its own session
}

TEST(ResearchFieldsVolumeMean, Vol126OnATinyRole) {
  const auto dir = support::scratch("vol_126");
  const std::size_t d = 130;
  support::TinyRole r;
  r.days = support::consecutive_days(fields::days_from_civil(2021, 1, 4), d);
  r.ids = {11, 22};
  for (std::size_t t = 0; t < d; ++t) {
    r.member.insert(r.member.end(), {1, 1});
    r.present.insert(r.present.end(),
                     {1, static_cast<u8>(t % 2)}); // id 22: present every other session
    r.volume.insert(r.volume.end(), {static_cast<f64>(t + 1), 100.0});
  }
  const std::string sha = support::write_role(dir / "role", r);
  const auto role = fields::RoleAxes::load(dir / "role", sha).value();
  std::filesystem::create_directories(dir / "out");
  const auto built = fields::build_vol_126(role, dir / "out").value();
  const std::string bytes = support::read_bytes(dir / "out" / "vol_126.f64");
  ASSERT_EQ(bytes.size(), d * 2 * sizeof(f64));
  std::vector<f64> v(d * 2);
  std::memcpy(v.data(), bytes.data(), bytes.size());
  for (std::size_t t = 0; t < 126; ++t) {
    EXPECT_TRUE(std::isnan(v[2 * t]) && std::isnan(v[2 * t + 1])) << t;
  }
  EXPECT_EQ(v[2 * 126], 63.5);      // mean of 1..126 (sessions 0..125)
  EXPECT_EQ(v[2 * 126 + 1], 100.0); // 63 accepted sessions of 126: exactly the minimum
  EXPECT_EQ(v[2 * 129], 66.5);      // mean of 4..129 (sessions 3..128)
  EXPECT_EQ(built.field.sha256, support::sha256_of(bytes));
  ASSERT_EQ(built.sources.size(), 2U);
  EXPECT_EQ(built.sources[0].sha256, role.receipt("volume.f64").value().sha256);
  EXPECT_EQ(built.sources[1].sha256, role.receipt("present.u8").value().sha256);
}
