#include "atx/engine/data/price_exposure_provider.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/panel_store.hpp"

namespace {
using namespace atx;
namespace co = atx::core;
namespace da = atx::engine::data;
using atx::engine::alpha::Panel;

struct Evidence final : da::PriceExposureEvidenceSource {
  usize dates, names;
  std::vector<da::ExposureObservation> cap;
  std::vector<da::ExposureInterval> identity;
  std::vector<da::ExposureIndustryObservation> industry;
  std::vector<i64> presence_clock, member_clock, bar_clock;
  std::vector<u8> member, member_qualified, presence_qualified, bar_qualified;
  explicit Evidence(usize t, usize n) : dates(t), names(n), cap(t * n), identity(t * n),
      industry(t * n), presence_clock(t * n), member_clock(t * n, 1), bar_clock(t * n),
      member(t * n, 1), member_qualified(t * n, 1), presence_qualified(t * n, 1),
      bar_qualified(t * n, 1) {
    for (usize d = 0; d < t; ++d) for (usize i = 0; i < n; ++i) {
      const auto cell = d * n + i;
      const i64 mark = static_cast<i64>(d + 1) * 1000;
      cap[cell] = {1e9 * static_cast<f64>(i + 1) *
          (1 + .2 * std::sin(.05 * static_cast<f64>(d) + .6 * static_cast<f64>(i))),
          mark - 2, mark - 1, true};
      identity[cell] = {1, 1, std::numeric_limits<i64>::max(),
          std::numeric_limits<i64>::max(), true};
      industry[cell] = {static_cast<u8>(i % 3 + 1), identity[cell]};
      presence_clock[cell] = mark + 15 + static_cast<i64>(i % 3);
      bar_clock[cell] = mark + 2 + static_cast<i64>(i % 7);
    }
  }
  u64 retained_bytes() const noexcept override {
    return static_cast<u64>(dates * names) *
        (sizeof(da::ExposureObservation) + sizeof(da::ExposureInterval) +
         sizeof(da::ExposureIndustryObservation) + 3 * sizeof(i64) + 4 * sizeof(u8));
  }
  template<class T> std::span<const T> row(const std::vector<T>& values, usize d) const {
    return std::span<const T>(values).subspan(d * names, names);
  }
  co::Result<da::PriceExposureEvidenceRow> read_date(usize d) override {
    if (d >= dates) return co::Err(co::ErrorCode::OutOfRange, "fixture date");
    da::PriceExposureEvidenceRow out;
    out.evidence.market_cap_usd = row(cap, d); out.evidence.identity = row(identity, d);
    out.evidence.industry = row(industry, d); out.evidence.member = row(member, d);
    out.evidence.membership_available_ns = row(member_clock, d);
    out.evidence.membership_qualified = row(member_qualified, d);
    out.evidence.presence_available_ns = row(presence_clock, d);
    out.evidence.presence_qualified = row(presence_qualified, d);
    out.bar_available_ns = row(bar_clock, d); out.bar_qualified = row(bar_qualified, d);
    return co::Ok(out);
  }
};
struct Fixture {
  usize dates, names;
  da::PriceExposureConfig cfg;
  Evidence evidence;
  std::vector<f64> close, raw, volume;
  std::vector<u8> present;
  explicit Fixture(usize t = 300, usize n = 12) : dates(t), names(n), evidence(t, n),
      close(t * n), raw(t * n), volume(t * n), present(t * n, 1) {
    cfg.close_basis = da::ExposureCloseBasis::AdjustedCloseRatioV1;
    cfg.dollar_basis = da::ExposureDollarBasis::RawUsdCloseTimesRawSharesV1;
    cfg.price_source_sha256 = std::string(64, 'a');
    cfg.output.instrument_namespace = "synthetic.security";
    cfg.output.parents = {{"prices", cfg.price_source_sha256}, {"membership", std::string(64, 'b')},
        {"cap", std::string(64, 'c')}, {"identity", std::string(64, 'd')},
        {"industry", std::string(64, 'e')}};
    for (usize i = 0; i < n; ++i) cfg.output.instrument_ids.push_back(static_cast<i64>(i + 1));
    for (usize d = 0; d < t; ++d) {
      const auto mark = static_cast<i64>(d + 1) * 1000;
      cfg.mark_times_ns.push_back(mark); cfg.output.session_keys.push_back(mark);
      cfg.output.decision_times_ns.push_back(mark + 100);
      for (usize i = 0; i < n; ++i) {
        const auto cell = d * n + i;
        const f64 r = .001 * std::sin(.075 * static_cast<f64>(d)) +
            .0005 * static_cast<f64>(i + 1) *
            std::sin(.033 * static_cast<f64>(d) + .5 * static_cast<f64>(i));
        close[cell] = d ? close[cell - n] * (1 + r) : 100 + static_cast<f64>(i);
        raw[cell] = static_cast<f64>(50 + i + d % 7);
        volume[cell] = static_cast<f64>(1000 + 10 * i + d % 5);
      }
    }
  }
  co::Result<Panel> panel(usize t = 0) const {
    if (!t) t = dates;
    const auto count = t * names;
    return Panel::create(t, names, {"close", "raw_close", "volume"},
        {std::vector<f64>(close.begin(), close.begin() + static_cast<isize>(count)),
         std::vector<f64>(raw.begin(), raw.begin() + static_cast<isize>(count)),
         std::vector<f64>(volume.begin(), volume.begin() + static_cast<isize>(count))},
        std::vector<u8>(present.begin(), present.begin() + static_cast<isize>(count)));
  }
  f64 step(usize d, usize i) const { return close[d * names + i] / close[(d - 1) * names + i] - 1; }
  f64 market(usize d) const {
    f64 numerator = 0, denominator = 0;
    for (usize i = 0; i < names; ++i) {
      const auto cap = evidence.cap[(d - 1) * names + i].value;
      numerator += cap * step(d, i); denominator += cap;
    }
    return numerator / denominator;
  }
};

co::Result<da::ExposureCrossSection> extract(const da::ExposurePanel& p, usize d) {
  da::ExposureExtractConfig c; c.allow_synthetic = true;
  return da::extract_exposure_date(p, d, p.axis_sha256(), p.content_sha256(),
      p.decision_times_ns()[d], c);
}
co::Result<da::ExposurePanel> scalar_reference(const Fixture& f, usize d) {
  auto cfg = f.cfg.output;
  cfg.session_keys = {f.cfg.output.session_keys[d]};
  cfg.decision_times_ns = {f.cfg.output.decision_times_ns[d]};
  cfg.descriptor_recipes.fill("independent-scalar-oracle");
  std::vector<da::ExposureObservation> raw(6 * f.names);
  for (usize i = 0; i < f.names; ++i) {
    f64 dollars = 0, amihud = 0, mean_stock = 0, mean_market = 0;
    for (usize s = d - 62; s <= d; ++s) {
      const f64 dv = f.raw[s * f.names + i] * f.volume[s * f.names + i];
      dollars += dv; amihud += std::abs(f.step(s, i)) / dv;
    }
    for (usize s = d - 251; s <= d; ++s) {
      mean_stock += f.step(s, i); mean_market += f.market(s);
    }
    mean_stock /= 252; mean_market /= 252;
    f64 cross = 0, variance = 0;
    for (usize s = d - 251; s <= d; ++s) {
      const f64 m = f.market(s) - mean_market;
      cross += (f.step(s, i) - mean_stock) * m; variance += m * m;
    }
    const f64 beta = cross / variance;
    f64 residual = 0;
    for (usize s = d - 251; s <= d; ++s) {
      const f64 r = f.step(s, i) - mean_stock - beta * (f.market(s) - mean_market);
      residual += r * r;
    }
    const std::array<f64, 6> values{std::log(dollars / 63), amihud / 63, beta,
        std::sqrt(residual / 252), std::log(f.close[(d - 21) * f.names + i]) -
        std::log(f.close[(d - 252) * f.names + i]),
        std::log(f.close[(d - 21) * f.names + i]) - std::log(f.close[d * f.names + i])};
    for (usize k = 0; k < 6; ++k) raw[k * f.names + i] =
        {values[k], f.cfg.mark_times_ns[d], f.cfg.mark_times_ns[d] + 20, true};
  }
  ATX_TRY(auto builder, da::ExposurePanelBuilder::create(cfg));
  da::ExposureRowInput row;
  row.raw_descriptors = raw; row.market_cap_usd = f.evidence.row(f.evidence.cap, d);
  row.identity = f.evidence.row(f.evidence.identity, d);
  row.industry = f.evidence.row(f.evidence.industry, d);
  row.member = f.evidence.row(f.evidence.member, d);
  row.membership_available_ns = f.evidence.row(f.evidence.member_clock, d);
  row.membership_qualified = f.evidence.row(f.evidence.member_qualified, d);
  row.source_present = std::span<const u8>(f.present).subspan(d * f.names, f.names);
  row.presence_available_ns = f.evidence.row(f.evidence.presence_clock, d);
  row.presence_qualified = f.evidence.row(f.evidence.presence_qualified, d);
  ATX_TRY_VOID(builder.append_date(0, row)); return builder.finish();
}

TEST(PriceExposureProvider, SixComputedDescriptorsMatchIndependentScalarRecipes) {
  Fixture f; auto p = f.panel(); ASSERT_TRUE(p);
  const auto actual = da::build_price_exposures(*p, f.evidence, f.cfg);
  ASSERT_TRUE(actual) << actual.error().message();
  const auto reference = scalar_reference(f, f.dates - 1); ASSERT_TRUE(reference);
  const auto a = extract(actual->panel, f.dates - 1), b = extract(*reference, 0);
  ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_EQ(a->values.size(), b->values.size());
  for (usize j = 0; j < a->values.size(); ++j) EXPECT_NEAR(a->values[j], b->values[j], 2e-10);
  EXPECT_EQ(actual->dates[251].finite_raw_descriptors[2], 0U);
  EXPECT_EQ(actual->dates[252].finite_raw_descriptors[2], f.names);
  for (usize d = 1; d < f.dates; ++d) {
    EXPECT_NEAR(actual->dates[d].market_return, f.market(d), 1e-16);
    EXPECT_EQ(actual->dates[d].market_available_at_ns, f.cfg.mark_times_ns[d] + 17);
    EXPECT_DOUBLE_EQ(actual->dates[d].admitted_cap_weight_fraction, 1);
  }
}

TEST(PriceExposureProvider, PriorCapAndStrictEqualityNeverUseTodaysWeightsOrUnknownDenominators) {
  Fixture f(280); auto p = f.panel(); ASSERT_TRUE(p);
  const auto base = da::build_price_exposures(*p, f.evidence, f.cfg); ASSERT_TRUE(base);
  f.evidence.cap[100 * f.names].value *= 1000;
  const auto changed = da::build_price_exposures(*p, f.evidence, f.cfg); ASSERT_TRUE(changed);
  EXPECT_DOUBLE_EQ(changed->dates[100].market_return, base->dates[100].market_return);
  EXPECT_NE(changed->dates[101].market_return, base->dates[101].market_return);
  f.evidence.cap[100 * f.names].available_at_ns = f.cfg.output.decision_times_ns[100];
  f.evidence.member_qualified[120 * f.names] = 0;
  const auto unavailable = da::build_price_exposures(*p, f.evidence, f.cfg); ASSERT_TRUE(unavailable);
  EXPECT_EQ(unavailable->dates[101].unknown_cap, 1U);
  EXPECT_TRUE(std::isnan(unavailable->dates[101].admitted_cap_weight_fraction));
  EXPECT_TRUE(std::isnan(unavailable->dates[101].market_return));
  EXPECT_EQ(unavailable->dates[121].unknown_membership, 1U);
  EXPECT_TRUE(std::isnan(unavailable->dates[121].admitted_cap_weight_fraction));
}

TEST(PriceExposureProvider, FutureMutationAndTruncationPreserveEarlierRows) {
  Fixture f(310); auto p = f.panel(); ASSERT_TRUE(p);
  const auto full = da::build_price_exposures(*p, f.evidence, f.cfg); ASSERT_TRUE(full);
  constexpr usize cutoff = 280;
  auto short_cfg = f.cfg;
  short_cfg.mark_times_ns.resize(cutoff); short_cfg.output.session_keys.resize(cutoff);
  short_cfg.output.decision_times_ns.resize(cutoff);
  auto prefix = f.panel(cutoff); ASSERT_TRUE(prefix);
  const auto truncated = da::build_price_exposures(*prefix, f.evidence, short_cfg);
  ASSERT_TRUE(truncated);
  for (usize d = cutoff; d < f.dates; ++d) for (usize i = 0; i < f.names; ++i) {
    f.close[d * f.names + i] *= 4 + static_cast<f64>(i);
    f.evidence.cap[d * f.names + i].value *= 1e4;
    f.volume[d * f.names + i] *= 13;
  }
  auto future = f.panel(); ASSERT_TRUE(future);
  const auto mutated = da::build_price_exposures(*future, f.evidence, f.cfg); ASSERT_TRUE(mutated);
  for (usize d = 252; d < cutoff; ++d) {
    const auto a = extract(full->panel, d), b = extract(truncated->panel, d), c = extract(mutated->panel, d);
    ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(c);
    EXPECT_EQ(a->values, b->values); EXPECT_EQ(a->values, c->values);
    EXPECT_DOUBLE_EQ(full->dates[d].market_return, mutated->dates[d].market_return);
  }
}

TEST(PriceExposureProvider, AbsentFiniteMarksLateBarsAndZeroVolumeRemainDistinct) {
  Fixture f(280); f.present[100 * f.names] = 0; f.close[100 * f.names] = 777;
  f.evidence.bar_clock[150 * f.names + 1] = f.cfg.output.decision_times_ns[150];
  f.volume[50 * f.names + 2] = 0;
  auto p = f.panel(); ASSERT_TRUE(p);
  const auto result = da::build_price_exposures(*p, f.evidence, f.cfg); ASSERT_TRUE(result);
  EXPECT_EQ(result->dates[100].missing_market_returns, 1U);
  EXPECT_EQ(result->dates[101].missing_market_returns, 1U);
  EXPECT_LT(result->dates[100].admitted_cap_weight_fraction, 1);
  EXPECT_TRUE(std::isnan(result->dates[100].market_return));
  EXPECT_EQ(result->dates[150].missing_market_returns, 1U);
  EXPECT_EQ(result->dates[151].missing_market_returns, 1U); // late bar never backfills
  EXPECT_EQ(result->dates[63].finite_raw_descriptors[0], f.names); // real zero contributes to ADV
  EXPECT_EQ(result->dates[63].finite_raw_descriptors[1], f.names - 1); // division by zero unavailable
  EXPECT_EQ(result->dates[279].finite_raw_descriptors[2], 0U); // no gap compression
}

struct TempDir {
  std::filesystem::path path;
  bool owned{};
  TempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (usize i = 0; i < 32; ++i) {
      auto p = std::filesystem::temp_directory_path() /
          ("atx-price-exposure-" + std::to_string(stamp) + "-" + std::to_string(i));
      std::error_code ec;
      if (std::filesystem::create_directory(p, ec)) { path = std::move(p); owned = true; break; }
    }
  }
  ~TempDir() { if (owned) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
};
co::Result<std::string> write_store(const Fixture& f, const std::filesystem::path& path,
                                   bool wrong_volume_basis = false) {
  da::PanelStoreConfig c;
  c.session_keys = f.cfg.output.session_keys; c.instrument_ids = f.cfg.output.instrument_ids;
  for (usize i = 0; i < f.names; ++i) c.original_indices.push_back(i);
  c.instrument_namespace = f.cfg.output.instrument_namespace;
  c.recipe = "synthetic-price-provider-fixture"; c.membership_sha256 = std::string(64, 'b');
  c.fields = {{"close", da::LevelBasis::AdjustedLevel, da::PanelStorePrecision::Float32V2},
      {"raw_close", da::LevelBasis::Raw, da::PanelStorePrecision::Float32V2},
      {"volume", wrong_volume_basis ? da::LevelBasis::Ratio : da::LevelBasis::Raw,
          da::PanelStorePrecision::Float32V2}};
  ATX_TRY(auto writer, da::PanelStoreWriter::create(path.string(), c));
  for (usize d = 0; d < f.dates; ++d) {
    const auto cl = std::span<const f64>(f.close).subspan(d * f.names, f.names);
    const auto ra = std::span<const f64>(f.raw).subspan(d * f.names, f.names);
    const auto vo = std::span<const f64>(f.volume).subspan(d * f.names, f.names);
    const std::array fields{cl, ra, vo};
    const auto present = std::span<const u8>(f.present).subspan(d * f.names, f.names);
    const auto member = f.evidence.row(f.evidence.member, d);
    ATX_TRY_VOID(writer.append_date(d, fields, cl, present, member, f.cfg.mark_times_ns[d] - 5));
  }
  return writer.finish();
}
TEST(PriceExposureProvider, D6UsesExactCloseAndChecksPinnedAxesBasesAndJointBudget) {
  TempDir temp; ASSERT_TRUE(temp.owned);
  Fixture f(270, 6);
  const auto hash = write_store(f, temp.path / "good"); ASSERT_TRUE(hash);
  const auto store = da::PanelStore::open((temp.path / "good").string(), *hash); ASSERT_TRUE(store);
  auto cfg = f.cfg; cfg.price_source_sha256 = *hash; cfg.output.parents[0].sha256 = *hash;
  auto p = f.panel(); ASSERT_TRUE(p);
  const auto direct = da::build_price_exposures(*p, f.evidence, cfg);
  const auto mapped = da::build_price_exposures(*store, f.evidence, cfg);
  ASSERT_TRUE(direct); ASSERT_TRUE(mapped) << mapped.error().message();
  const auto a = extract(direct->panel, 269), b = extract(mapped->panel, 269);
  ASSERT_TRUE(a); ASSERT_TRUE(b); EXPECT_EQ(a->values, b->values);
  EXPECT_GT(mapped->mapped_chunk_bound_bytes, 0U);
  auto wrong = cfg; wrong.output.instrument_ids[0] = 999;
  EXPECT_FALSE(da::build_price_exposures(*store, f.evidence, wrong));
  wrong = cfg; wrong.price_source_sha256 = std::string(64, 'f');
  wrong.output.parents[0].sha256 = wrong.price_source_sha256;
  EXPECT_FALSE(da::build_price_exposures(*store, f.evidence, wrong));
  wrong = cfg; wrong.max_working_bytes = 1024;
  EXPECT_FALSE(da::build_price_exposures(*store, f.evidence, wrong));
  wrong = cfg; wrong.output.parents.push_back({std::string(1024 * 1024, 'x'), std::string(64, 'f')});
  wrong.max_working_bytes = 1024;
  const auto oversized = da::build_price_exposures(*store, f.evidence, wrong);
  ASSERT_FALSE(oversized); EXPECT_EQ(oversized.error().code(), co::ErrorCode::InvalidArgument);
  const auto bad_hash = write_store(f, temp.path / "bad", true); ASSERT_TRUE(bad_hash);
  const auto bad = da::PanelStore::open((temp.path / "bad").string(), *bad_hash); ASSERT_TRUE(bad);
  wrong = cfg; wrong.price_source_sha256 = *bad_hash; wrong.output.parents[0].sha256 = *bad_hash;
  EXPECT_FALSE(da::build_price_exposures(*bad, f.evidence, wrong));
  f.evidence.member[269 * f.names] = 0;
  EXPECT_FALSE(da::build_price_exposures(*store, f.evidence, cfg));
}
} // namespace
