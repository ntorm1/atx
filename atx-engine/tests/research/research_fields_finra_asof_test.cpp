// si_shares / si_dtc and the as-of clock rule (migration slice 1): the strict CSV contract, the
// rule in closed form, the look-ahead probe (and a planted same-day leak it must catch), and the
// builder's input checks.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/engine/research/fields/asof_series.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/finra_asof_field.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using atx::f64;
using atx::i64;

namespace {

constexpr const char *kHeader = "security_id,available_at,value\n";

i64 day(atx::i64 y, atx::u32 m, atx::u32 d) { return fields::days_from_civil(y, m, d); }

} // namespace

TEST(ResearchFieldsFinraAsof, CsvContract) {
  const auto rows = fields::parse_asof_csv("security_id,available_at,value\r\n"
                                           "22,2021-03-09,\r\n"
                                           "11,2021-01-24,1250000.5\r\n"
                                           "11,2020-12-24,NaN\r\n"
                                           "22,2022-01-24,-0\r\n"
                                           "33,2021-04-09,2.5E-1\r\n");
  ASSERT_TRUE(rows.has_value()) << rows.error().to_string();
  ASSERT_EQ(rows->size(), 5U);
  EXPECT_EQ((*rows)[0].security_id, 11);
  EXPECT_EQ((*rows)[0].available_day, day(2020, 12, 24));
  EXPECT_TRUE(std::isnan((*rows)[0].value));
  EXPECT_EQ((*rows)[1].value, 1250000.5);
  EXPECT_TRUE(std::isnan((*rows)[2].value)); // an empty value is NaN
  EXPECT_TRUE(support::same_bits((*rows)[3].value, -0.0));
  EXPECT_EQ((*rows)[4].value, 0.25);
  EXPECT_TRUE(fields::parse_asof_csv(kHeader).has_value()); // a header alone: no rows
  for (const std::string &bad :
       {std::string("security_id,available_at,values\n1,2021-01-04,1\n"), // header
        std::string(kHeader) + "1,2021-01-04,1\n\n",                      // empty line
        std::string(kHeader) + "1,2021-01-04,1\r\r\n",                    // bare CR line
        std::string(kHeader) + "1,2021-01-04,1\n\r",                      // trailing CR
        std::string(kHeader) + "1,2021-01-04\n",                          // two fields
        std::string(kHeader) + "1,2021-01-04,1,2\n",                      // four fields
        std::string(kHeader) + "-1,2021-01-04,1\n",                       // id sign
        std::string(kHeader) + "0,2021-01-04,1\n",                        // id zero
        std::string(kHeader) + "12a,2021-01-04,1\n",                      // id text
        std::string(kHeader) + "1,2021-02-30,1\n",                        // date
        std::string(kHeader) + "1,2021-01-04,+5\n",                       // plus sign
        std::string(kHeader) + "1,2021-01-04,1e\n",                       // exponent digits
        std::string(kHeader) + "1,2021-01-04,inf\n",                      // infinity
        std::string(kHeader) + "1,2021-01-04,1e400\n",                    // overflow
        std::string(kHeader) + "1,2021-01-04, 5\n",                       // whitespace
        std::string(kHeader) + "1,2021-01-04,1\n1,2021-01-04,2\n",        // duplicate pair
        std::string("")}) {
    EXPECT_FALSE(fields::parse_asof_csv(bad).has_value()) << bad;
  }
}

TEST(ResearchFieldsFinraAsof, AsofRuleClosedForm) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const auto series =
      fields::AsofSeries::create({{0, 100, 1.0}, {0, 110, nan}, {0, 120, 3.0}, {1, 50, 7.0}}, 2, 45)
          .value();
  std::vector<f64> v(2);
  std::vector<i64> src(2);
  const auto at = [&](i64 d) { series.row(d, v, src); };
  at(100);
  EXPECT_TRUE(
      std::isnan(v[0])); // published on day 100: not visible at the day-100 session (strict)
  EXPECT_EQ(src[0], fields::AsofSeries::kNoSource);
  EXPECT_TRUE(std::isnan(v[1])); // age 50 > 45: stale
  at(101);
  EXPECT_EQ(v[0], 1.0);
  EXPECT_EQ(src[0], 100);
  at(111);
  EXPECT_TRUE(std::isnan(v[0])); // a visible NaN stays NaN: no skip-back to 1.0
  EXPECT_EQ(src[0], 110);
  at(165);
  EXPECT_EQ(v[0], 3.0); // age 45: visible
  at(166);
  EXPECT_TRUE(std::isnan(v[0])); // age 46: stale
  EXPECT_EQ(src[0], fields::AsofSeries::kNoSource);
  at(50);
  EXPECT_TRUE(std::isnan(v[1]));
  at(51);
  EXPECT_EQ(v[1], 7.0);
  at(95);
  EXPECT_EQ(v[1], 7.0);
  at(96);
  EXPECT_TRUE(std::isnan(v[1]));
  EXPECT_FALSE(
      fields::AsofSeries::create({{1, 5, 1.0}, {0, 9, 1.0}}, 2, 45).has_value()); // unsorted
  EXPECT_FALSE(
      fields::AsofSeries::create({{0, 5, 1.0}, {0, 5, 2.0}}, 2, 45).has_value()); // duplicate
  EXPECT_FALSE(fields::AsofSeries::create({{2, 5, 1.0}}, 2, 45).has_value());     // off the axis
  EXPECT_FALSE(fields::AsofSeries::create({}, 2, -1).has_value());
}

TEST(ResearchFieldsFinraAsof, LookAheadProbeHoldsAndCatchesALeak) {
  // One column, observations published on days 100, 110 and 120; sessions are days 95..140.
  // Perturbing the value published on day 110 may change sessions dated after 110 only.
  const std::vector<fields::AsofObservation> base{{0, 100, 1.0}, {0, 110, 2.0}, {0, 120, 3.0}};
  auto perturbed = base;
  perturbed[1].value = 2000.0;
  const auto honest = [](const std::vector<fields::AsofObservation> &obs, i64 d) {
    const auto series = fields::AsofSeries::create(obs, 1, 45).value();
    std::vector<f64> v(1);
    std::vector<i64> src(1);
    series.row(d, v, src);
    return v[0];
  };
  // The planted leak: latest observation available on or BEFORE the session day (<= instead of <).
  const auto leaky = [](const std::vector<fields::AsofObservation> &obs, i64 d) {
    f64 out = std::numeric_limits<f64>::quiet_NaN();
    for (const auto &o : obs) {
      if (o.available_day <= d) {
        out = o.value;
      }
    }
    return out;
  };
  const auto first_changed = [&](const auto &rule) -> std::optional<i64> {
    for (i64 d = 95; d <= 140; ++d) {
      if (!support::same_bits(rule(base, d), rule(perturbed, d))) {
        return d;
      }
    }
    return std::nullopt;
  };
  const auto changed = first_changed(honest);
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(*changed, 111); // the first session after the publication day
  const auto leaked = first_changed(leaky);
  ASSERT_TRUE(leaked.has_value());
  EXPECT_LE(*leaked, 110); // the probe flags a same-day read
}

namespace {

struct TinyFinra {
  support::fs::path root;
  fields::RoleAxes role;
};

// Role: 10 consecutive days from 2021-01-04, ids 11 and 22, all members. FINRA: si_shares rows for
// 11 (published 2021-01-05), 22 (published 2020-11-01: 64 days old at the first session, stale), 33
// (off the axis) and a sealed row of 11; the schedule's settlements before 2021-06-01 end with the
// 2021-01-05 dissemination.
TinyFinra tiny_finra(const support::fs::path &dir, const std::string &csv, bool pin_receipt) {
  support::TinyRole r;
  r.days = support::consecutive_days(day(2021, 1, 4), 10);
  r.ids = {11, 22};
  r.member.assign(20, 1);
  r.present.assign(20, 1);
  r.volume.assign(20, 1.0);
  const std::string sha = support::write_role(dir / "role", r);
  const auto finra = dir / "finra";
  support::fs::create_directories(finra / "asof");
  support::write_bytes(finra / "dissemination_schedule.csv",
                       "settlement_date,due_date,dissemination_date,source\n"
                       "2020-10-23,x,2020-11-01,\"official, FINRA\"\n"
                       "2020-12-27,x,2021-01-05,official\n"
                       "\n"
                       "2024-01-29,x,2024-02-07,official\n");
  support::write_bytes(finra / "asof" / "si_shares.csv", csv);
  const std::string pin = pin_receipt ? support::sha256_of(csv) : std::string(64, 'f');
  support::write_bytes(finra / "asof" / "manifest.json",
                       "{\"outputs\": {\"si_shares\": {\"sha256\": \"" + pin + "\"}}}\n");
  return TinyFinra{finra, fields::RoleAxes::load(dir / "role", sha).value()};
}

const std::string kTinyCsv = std::string(kHeader) + "11,2021-01-05,100\n"
                                                    "22,2020-11-01,5\n"
                                                    "33,2021-01-05,1\n"
                                                    "11,2024-02-07,999\n";

} // namespace

TEST(ResearchFieldsFinraAsof, SiSharesOnATinyRole) {
  const auto dir = support::scratch("finra_tiny");
  const auto tiny = tiny_finra(dir, kTinyCsv, true);
  const auto schedule = fields::read_dissemination_schedule(tiny.root).value();
  EXPECT_EQ(schedule.dissemination_days,
            (std::vector<i64>{day(2020, 11, 1), day(2021, 1, 5), day(2024, 2, 7)}));
  EXPECT_EQ(schedule.vintage_cutoff_day, day(2021, 1, 5));
  support::fs::create_directories(dir / "out");
  const auto built =
      fields::build_finra_field("si_shares", tiny.root, schedule, tiny.role, dir / "out").value();
  const std::string bytes = support::read_bytes(dir / "out" / "si_shares.f64");
  ASSERT_EQ(bytes.size(), 20 * sizeof(f64));
  std::vector<f64> v(20);
  std::memcpy(v.data(), bytes.data(), bytes.size());
  for (std::size_t t = 0; t < 10; ++t) {
    if (t < 2) {
      EXPECT_TRUE(std::isnan(v[2 * t])) << t; // 2021-01-04 and the publication day 2021-01-05
    } else {
      EXPECT_EQ(v[2 * t], 100.0) << t;
    }
    EXPECT_TRUE(std::isnan(v[2 * t + 1])) << t; // stale from the first session on
  }
  EXPECT_EQ(built.stats.rows_total, 4U);
  EXPECT_EQ(built.stats.rows_sealed, 1U); // the reader-side seal drops the 2024 row before use
  EXPECT_EQ(built.stats.rows_matched_axis, 2U);
  EXPECT_EQ(built.stats.rows_ignored_unknown_id, 1U);
  EXPECT_EQ(built.vintage.finite_member_cells, 8U);
  EXPECT_EQ(built.vintage.last_session_with_republished_visible_cell,
            std::optional<std::string>("2021-01-13"));
  EXPECT_FALSE(built.vintage.first_session_vintage_safe.has_value());
  EXPECT_EQ(built.vintage_safe_from, std::optional<std::string>("2021-01-06"));
  EXPECT_EQ(built.vintage.rule,
            "visible row disseminated on or before 2021-01-05 (settlement before 2021-06-01): "
            "later FINRA republication");
  ASSERT_EQ(built.sources.size(), 3U);
  EXPECT_EQ(built.sources[0].sha256, support::sha256_of(kTinyCsv));
}

TEST(ResearchFieldsFinraAsof, InputsAreChecked) {
  {
    const auto dir = support::scratch("finra_receipt");
    const auto tiny = tiny_finra(dir, kTinyCsv, false);
    const auto schedule = fields::read_dissemination_schedule(tiny.root).value();
    EXPECT_FALSE(
        fields::build_finra_field("si_shares", tiny.root, schedule, tiny.role, dir).has_value());
  }
  {
    const auto dir = support::scratch("finra_schedule");
    const auto tiny =
        tiny_finra(dir, std::string(kHeader) + "11,2021-01-06,1\n", true); // not a dissemination
    const auto schedule = fields::read_dissemination_schedule(tiny.root).value();
    EXPECT_FALSE(
        fields::build_finra_field("si_shares", tiny.root, schedule, tiny.role, dir).has_value());
    EXPECT_FALSE(
        fields::build_finra_field("si_volume", tiny.root, schedule, tiny.role, dir).has_value());
  }
}
