// The six vendor-panel builders (P9 lane A3): one planted-leak probe with teeth per builder, the A8
// share lag of ceq_iss_5y, and the formula fingerprint of every ported spec against the committed
// field registry.
//
// A probe plants one change at axis row P + t0 (P: the panel's prefix, the axis row of role row 0)
// and finds the first role row whose output moves. The honest builder (build_vendor_field, which
// owns the clock) must first move at the row its clock allows: t0 + 1 for the price-close-lag1-v1
// fields (row t reads session t-1), t0 for the ohlc-same-session-v1 bars (row t reads session t).
// The teeth: the same row kernel driven by a clock one session later must move earlier, and the
// probe must say so.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/nyse_calendar.hpp"
#include "atx/engine/research/fields/sources/vendor_panel.hpp"
#include "atx/engine/research/fields/vendor_fields.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::f32;
using atx::f64;
using atx::i64;
using atx::u8;
using atx::usize;

namespace {

// The axis: ceq_iss_5y's history (1,261 sessions) and the A8 share lookback (490 days) before the
// role, all consecutive calendar days, so that row 0 of the role already reads two share
// observations; then 100 role sessions from 2021-01-04.
constexpr usize kPrefix = fields::kPriceHistorySessions +
                          static_cast<usize>(fields::kSharesLagDays + fields::kSharesMaxAgeDays);
constexpr usize kDates = 100;
constexpr usize kLines = 2;
constexpr usize kT0 = 5;
constexpr usize kPlantRow = kPrefix + kT0;

using Rows = std::vector<std::vector<f64>>; // role rows x lines

enum class Plant : u8 { None, Open, Close, Shares, Bar };

i64 role_start() { return fields::days_from_civil(2021, 1, 4); }

// Every line observed on every axis row (factor 1, close 10, open 10.1, high 11, low 9, volume 1,
// shares 1000 + row), with one planted change at axis row kPlantRow.
fields::VendorPanel probe_panel(Plant plant) {
  fields::VendorPanelRequest request;
  request.pre_sessions = kPrefix;
  request.price = true;
  request.shares = true;
  request.price_open = true;
  request.bars = true;
  const fields::ExtendedAxis axis{
      support::consecutive_days(role_start() - static_cast<i64>(kPrefix), kPrefix + kDates),
      kPrefix};
  auto assembler = fields::VendorPanel::Assembler::create(axis, kLines, request).value();
  for (usize r = 0; r < kPrefix + kDates; ++r) {
    for (usize j = 0; j < kLines; ++j) {
      fields::VendorObservation o;
      o.row = r;
      o.line = j;
      o.factor = 1.0;
      o.close = 10.0F;
      o.volume = 1.0;
      o.shares = 1000 + static_cast<i64>(r);
      o.open = 10.1;
      o.high = 11.0;
      o.low = 9.0;
      if (r == kPlantRow) {
        switch (plant) {
        case Plant::None:
          break;
        case Plant::Open:
          o.open = 10.1 * 1.01;
          break;
        case Plant::Close:
          o.close = std::numeric_limits<f32>::quiet_NaN();
          break;
        case Plant::Shares:
          o.shares *= 2;
          break;
        case Plant::Bar:
          o.open = 10.5;
          o.high = 11.5;
          o.low = 8.5;
          break;
        }
      }
      assembler.add(o);
    }
  }
  return std::move(assembler).finish({}, {});
}

// The role the panel serves: its tail sessions, ids {1, 2}, every cell a member, present, with
// close and raw close 10 (the ohlc kinds read them).
fields::RoleAxes probe_role(const fs::path &dir) {
  support::TinyRole r;
  r.days = support::consecutive_days(role_start(), kDates);
  r.ids = {1, 2};
  r.member.assign(kDates * kLines, 1);
  r.present.assign(kDates * kLines, 1);
  r.volume.assign(kDates * kLines, 1.0);
  r.close.assign(kDates * kLines, 10.0);
  r.raw_close.assign(kDates * kLines, 10.0);
  const std::string sha = support::write_role(dir, r);
  return fields::RoleAxes::load(dir, sha).value();
}

// The honest builder's payload, as role rows.
Rows built_rows(std::string_view name, const fields::VendorPanel &panel,
                const fields::RoleAxes &role, const fs::path &out) {
  fs::create_directories(out);
  const auto built = fields::build_vendor_field(name, panel, role, out);
  EXPECT_TRUE(built.has_value()) << name << ": " << built.error().message();
  const std::string bytes = support::read_bytes(out / (std::string(name) + ".f64"));
  Rows rows(kDates, std::vector<f64>(kLines));
  EXPECT_EQ(bytes.size(), kDates * kLines * sizeof(f64)) << name;
  for (usize t = 0; t < kDates && bytes.size() == kDates * kLines * sizeof(f64); ++t) {
    std::memcpy(rows[t].data(), bytes.data() + t * kLines * sizeof(f64), kLines * sizeof(f64));
  }
  return rows;
}

// The axis row a clock reads for role row t (nullopt: none).
using Clock = std::function<std::optional<usize>(usize)>;

// The field's row kernel driven by `clock`, as role rows.
Rows kernel_rows(std::string_view name, const fields::VendorPanel &panel,
                 const fields::RoleAxes &role, const Clock &clock) {
  Rows rows(kDates, std::vector<f64>(kLines, std::numeric_limits<f64>::quiet_NaN()));
  const fields::ShareIndex shares(panel);
  const std::vector<f64> close(kLines, 10.0);
  const std::vector<u8> present(kLines, 1);
  fields::BarCounts counts;
  for (usize t = 0; t < kDates; ++t) {
    std::optional<usize> a = clock(t);
    if (a && *a >= panel.rows()) {
      a.reset(); // past the axis: nothing to read
    }
    const auto member = role.member_row(t);
    if (name == "ret_overnight" || name == "ret_intraday") {
      const auto which =
          name == "ret_overnight" ? fields::OpenReturn::Overnight : fields::OpenReturn::Intraday;
      if (a && *a >= 1) {
        static_cast<void>(fields::open_return_row(which, panel, *a, member, rows[t]));
      }
    } else if (name == "ceq_iss_5y") {
      if (a) {
        static_cast<void>(fields::ceq_issuance_row(panel, shares, *a, member, rows[t]));
      }
    } else {
      const auto which = name == "open_adj"   ? fields::Bar::Open
                         : name == "high_adj" ? fields::Bar::High
                                              : fields::Bar::Low;
      fields::ohlc_bar_row(which, panel, a, close, close, present, member, counts, rows[t]);
    }
  }
  return rows;
}

bool same_row(const std::vector<f64> &a, const std::vector<f64> &b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)) == 0;
}

std::optional<usize> first_changed_row(const Rows &before, const Rows &after) {
  for (usize t = 0; t < before.size() && t < after.size(); ++t) {
    if (!same_row(before[t], after[t])) {
      return t;
    }
  }
  return std::nullopt;
}

bool same_rows(const Rows &a, const Rows &b) {
  return a.size() == b.size() && !first_changed_row(a, b).has_value();
}

// The honest clocks (the builders' own) and the leaky ones, one session later.
std::optional<usize> lag1(usize t) { return kPrefix + t - fields::kPriceLagSessions; }
std::optional<usize> lag1_leaky(usize t) { return kPrefix + t; }
std::optional<usize> same_session(usize t) { return kPrefix + t - fields::kOhlcLagSessions; }
std::optional<usize> same_session_leaky(usize t) { return kPrefix + t + 1; }

struct Probe {
  std::optional<usize> honest; // the builder's first changed role row
  std::optional<usize> leaky;  // the kernel's under the leaky clock
};

// Plants `plant`, then runs the builder and the leaky kernel on the base and planted panels. The
// honest kernel must reproduce the builder's payload on both panels (the probe drives the
// builder's own arithmetic).
Probe probe(std::string_view name, Plant plant, const Clock &honest, const Clock &leaky) {
  const auto dir = support::scratch("vendor_probe_" + std::string(name));
  const auto role = probe_role(dir / "role");
  const fields::VendorPanel base = probe_panel(Plant::None);
  const fields::VendorPanel planted = probe_panel(plant);
  const Rows built_base = built_rows(name, base, role, dir / "base");
  const Rows built_planted = built_rows(name, planted, role, dir / "planted");
  EXPECT_TRUE(same_rows(built_base, kernel_rows(name, base, role, honest))) << name;
  EXPECT_TRUE(same_rows(built_planted, kernel_rows(name, planted, role, honest))) << name;
  return Probe{first_changed_row(built_base, built_planted),
               first_changed_row(kernel_rows(name, base, role, leaky),
                                 kernel_rows(name, planted, role, leaky))};
}

const Json &registry_row(const Json &registry, std::string_view name) {
  for (const Json &row : registry.at("fields")) {
    if (row.at("name").get<std::string>() == name) {
      return row;
    }
  }
  throw std::runtime_error("no field registry row " + std::string(name));
}

} // namespace

TEST(ResearchFieldsVendorFields, OpenReturnsLookAheadProbe) {
  for (const std::string_view name : {"ret_overnight", "ret_intraday"}) {
    const Probe p = probe(name, Plant::Open, lag1, lag1_leaky);
    ASSERT_TRUE(p.honest.has_value()) << name << ": the plant never reached the output";
    EXPECT_EQ(*p.honest, kT0 + 1) << name << ": row t reads session t-1 only";
    ASSERT_TRUE(p.leaky.has_value()) << name;
    EXPECT_LE(*p.leaky, kT0) << name << ": the probe misses a rule that reads session t";
  }
}

TEST(ResearchFieldsVendorFields, CeqIssuanceLookAheadProbe) {
  const Probe p = probe("ceq_iss_5y", Plant::Close, lag1, lag1_leaky);
  ASSERT_TRUE(p.honest.has_value()) << "the plant never reached the output";
  EXPECT_EQ(*p.honest, kT0 + 1); // row t reads session t-1 only
  ASSERT_TRUE(p.leaky.has_value());
  EXPECT_LE(*p.leaky, kT0);
}

// A8: a share count dated session s is first used 90 days later (consecutive calendar days here:
// role row t reads the share observation of axis row P + t - 1 - 90).
TEST(ResearchFieldsVendorFields, CeqIssuanceSharesLagNinetyDays) {
  const Probe p = probe("ceq_iss_5y", Plant::Shares, lag1, lag1_leaky);
  ASSERT_TRUE(p.honest.has_value());
  EXPECT_EQ(*p.honest, kT0 + static_cast<usize>(fields::kSharesLagDays) + 1);
  ASSERT_TRUE(p.leaky.has_value());
  EXPECT_EQ(*p.leaky, kT0 + static_cast<usize>(fields::kSharesLagDays));
}

TEST(ResearchFieldsVendorFields, OhlcBarsLookAheadProbe) {
  for (const std::string_view name : {"open_adj", "high_adj", "low_adj"}) {
    const Probe p = probe(name, Plant::Bar, same_session, same_session_leaky);
    ASSERT_TRUE(p.honest.has_value()) << name << ": the plant never reached the output";
    EXPECT_EQ(*p.honest, kT0) << name << ": row t reads session t, nothing later";
    ASSERT_TRUE(p.leaky.has_value()) << name;
    EXPECT_LT(*p.leaky, kT0) << name << ": the probe misses a rule that reads session t+1";
  }
}

// Every ported spec text fingerprints to the committed registry row of its field (the Python
// builder's formula_id of research_fields_price.py / research_fields_ohlc.py), and its group is
// the row's producer group.
TEST(ResearchFieldsVendorFields, FormulaFingerprintsAreTheRegistrys) {
  const fs::path path =
      support::fixture_dir() / ".." / ".." / ".." / "tools" / "field_registry.json";
  const Json registry = Json::parse(support::read_bytes(path));
  ASSERT_EQ(fields::vendor_field_names().size(), 6U);
  for (const std::string_view name : fields::vendor_field_names()) {
    const fields::FieldSpec *spec = fields::vendor_field_spec(name);
    ASSERT_NE(spec, nullptr) << name;
    EXPECT_EQ(spec->name, name);
    const Json &row = registry_row(registry, name);
    const auto formula = fields::formula_sha256(*spec);
    ASSERT_TRUE(formula.has_value()) << name << ": " << formula.error().message();
    EXPECT_EQ(*formula, row.at("formula_sha256").get<std::string>()) << name;
    EXPECT_EQ(spec->group, row.at("options").at("group").get<std::string>()) << name;
    EXPECT_TRUE(fields::vendor_request(name).has_value()) << name;
  }
  EXPECT_EQ(fields::vendor_field_spec("coskew_60m"), nullptr); // stays a Python field
  EXPECT_EQ(fields::vendor_field_spec("xrd0_ttm"), nullptr);
  EXPECT_FALSE(fields::vendor_request("vol_126").has_value());
}
