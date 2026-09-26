#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/data/exposure_panel.hpp"

namespace {
using namespace atx;
namespace data = atx::engine::data;
constexpr usize kFields = data::kExposureDescriptorCount;

struct Row {
  std::vector<data::ExposureObservation> raw, caps;
  std::vector<data::ExposureIndustryObservation> industry;
  std::vector<data::ExposureInterval> identity;
  std::vector<u8> present, member, qualified;
  std::vector<i64> known;
  explicit Row(usize n = 8) : raw(6 * n), caps(n), industry(n), identity(n),
      present(n, 1), member(n, 1), qualified(n, 1), known(n, 20) {
    for (usize i = 0; i < n; ++i) {
      caps[i] = {1000 * static_cast<f64>(i + 1), 10, 20, true};
      identity[i] = {1, 2, std::numeric_limits<i64>::max(),
                     std::numeric_limits<i64>::max(), true};
      industry[i] = {static_cast<u8>(i < n / 2 ? 1 : 2), identity[i]};
      for (usize f = 0; f < 6; ++f)
        raw[f * n + i] = {static_cast<f64>((f + 1) * (i + 1)), 10, 20, true};
    }
  }
  data::ExposureRowInput view() const {
    return {raw, caps, industry, identity, present, member, known, known, qualified, qualified};
  }
};
data::ExposurePanelConfig config(usize t = 2, usize n = 8) {
  data::ExposurePanelConfig cfg;
  for (usize d = 0; d < t; ++d) {
    cfg.session_keys.push_back(static_cast<i64>(d + 1));
    cfg.decision_times_ns.push_back(static_cast<i64>((d + 1) * 1000));
  }
  for (usize i = 0; i < n; ++i) cfg.instrument_ids.push_back(static_cast<i64>(100 + i));
  cfg.instrument_namespace = "synthetic-security-id";
  for (usize f = 0; f < 6; ++f) cfg.descriptor_recipes[f] = "synthetic-planted-descriptor-" + std::to_string(f);
  for (auto role : {"identity", "membership", "prices", "cap", "industry"})
    cfg.parents.push_back({role, std::string(64, 'a')});
  return cfg;
}
core::Result<data::ExposurePanel> build(const data::ExposurePanelConfig& cfg, const std::vector<Row>& rows) {
  ATX_TRY(auto builder, data::ExposurePanelBuilder::create(cfg));
  for (usize d = 0; d < rows.size(); ++d) ATX_TRY_VOID(builder.append_date(d, rows[d].view()));
  return builder.finish();
}
core::Result<data::ExposureCrossSection> extract(const data::ExposurePanel& panel, usize d,
                                               data::ExposureExtractConfig cfg = {}) {
  cfg.allow_synthetic = true;
  return data::extract_exposure_date(panel, d, panel.axis_sha256(), panel.content_sha256(),
                                    panel.decision_times_ns()[d], cfg);
}
std::vector<u64> bits(std::span<const f64> values) {
  std::vector<u64> out; out.reserve(values.size());
  for (auto v : values) out.push_back(std::bit_cast<u64>(v));
  return out;
}

TEST(ExposurePanel, PlantedColumnsMatchIndependentCapCenterAndKeepOriginalAxes) {
  const Row row;
  const auto panel = build(config(), {row, row}); ASSERT_TRUE(panel) << panel.error().message();
  const auto section = extract(*panel, 1); ASSERT_TRUE(section) << section.error().message();
  EXPECT_EQ(section->known_member_names, 8U); EXPECT_EQ(section->unknown_membership_names, 0U);
  EXPECT_EQ(section->completeness, data::ExposureCompleteness::Complete);
  EXPECT_EQ(section->security_ids, (std::vector<i64>{100, 101, 102, 103, 104, 105, 106, 107}));
  EXPECT_EQ(section->original_slots, (std::vector<usize>{0, 1, 2, 3, 4, 5, 6, 7}));
  EXPECT_EQ(section->ff49, (std::vector<u8>{1, 1, 1, 1, 2, 2, 2, 2}));
  EXPECT_EQ(panel->parents().size(), 5U);
  for (usize f = 0; f < kFields; ++f) {
    std::vector<f64> raw;
    f64 cap_sum = 0, weighted = 0;
    for (usize i = 0; i < 8; ++i) {
      raw.push_back(f == 0 ? std::log(row.caps[i].value) : row.raw[(f - 1) * 8 + i].value);
      cap_sum += row.caps[i].value; weighted += row.caps[i].value * raw.back();
    }
    const auto mean = std::accumulate(raw.begin(), raw.end(), 0.) / 8;
    f64 ss = 0; for (auto value : raw) ss += (value - mean) * (value - mean);
    const auto sd = std::sqrt(ss / 8), cap_mean = weighted / cap_sum;
    for (usize i = 0; i < 8; ++i)
      EXPECT_NEAR(section->values[i * kFields + f], (raw[i] - cap_mean) / sd, 1e-12);
  }
  // Immutable shared ownership survives builder and another panel handle.
  auto retained = *panel; EXPECT_EQ(retained.content_sha256(), panel->content_sha256());
  EXPECT_EQ(retained.instrument_ids()[7], 107);
}

TEST(ExposurePanel, FutureKnownClosureAndFutureObservationsCannotChangeEarlierNumericRows) {
  Row first, later;
  first.identity[0].valid_to_ns = 500; first.identity[0].end_available_at_ns = 1500;
  first.industry[0].validity = first.identity[0]; later = first;
  auto altered = first; altered.identity[0].valid_to_ns = 700;
  altered.industry[0].validity = altered.identity[0];
  const auto a = build(config(), {first, later});
  const auto b = build(config(), {altered, altered}); ASSERT_TRUE(a); ASSERT_TRUE(b);
  const auto early_a = extract(*a, 0), early_b = extract(*b, 0); ASSERT_TRUE(early_a); ASSERT_TRUE(early_b);
  EXPECT_EQ(bits(early_a->values), bits(early_b->values));
  const auto expired = extract(*a, 1); ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error().code(), core::ErrorCode::Unavailable);
  data::ExposureExtractConfig drop; drop.missing = data::MissingExposurePolicy::DropUnavailableNames;
  drop.min_member_fraction = .8;
  const auto later_good = extract(*a, 1, drop); ASSERT_TRUE(later_good);
  EXPECT_EQ(later_good->original_slots.front(), 1U); EXPECT_EQ(later_good->known_member_names, 8U);
  // Equality at the end-knowledge clock is still unavailable to this decision.
  auto equal = first; equal.identity[0].end_available_at_ns = 1000;
  equal.industry[0].validity = equal.identity[0];
  const auto at_clock = build(config(1), {equal}); ASSERT_TRUE(at_clock); EXPECT_TRUE(extract(*at_clock, 0));
  // An unavailable numeric observation is canonical missing regardless of value.
  first.raw[1].available_at_ns = 3000; altered = first; altered.raw[1].value = 1e100;
  const auto c = build(config(1), {first}), d = build(config(1), {altered}); ASSERT_TRUE(c); ASSERT_TRUE(d);
  const auto pc = extract(*c, 0, drop), pd = extract(*d, 0, drop); ASSERT_TRUE(pc); ASSERT_TRUE(pd);
  EXPECT_EQ(bits(pc->values), bits(pd->values)); EXPECT_EQ(c->content_sha256(), d->content_sha256());
}

TEST(ExposurePanel, SourceAbsenceUnqualifiedCapAndIndustryKeepDenominatorAndReasons) {
  Row row; row.present[0] = 0; row.caps[1].qualified = false;
  row.industry[2].validity.qualified = false;
  const auto panel = build(config(1), {row}); ASSERT_TRUE(panel);
  EXPECT_FALSE(extract(*panel, 0));
  data::ExposureExtractConfig cfg; cfg.missing = data::MissingExposurePolicy::DropUnavailableNames;
  cfg.min_member_fraction = .5;
  const auto section = extract(*panel, 0, cfg); ASSERT_TRUE(section);
  EXPECT_EQ(section->known_member_names, 8U); EXPECT_EQ(section->original_slots.size(), 5U);
  EXPECT_EQ(section->original_slots, (std::vector<usize>{3, 4, 5, 6, 7}));
  EXPECT_EQ(section->completeness, data::ExposureCompleteness::Partial);
  EXPECT_GT(section->omission_counts[3], 0U); // source absence
  EXPECT_GT(section->omission_counts[4], 0U); // cap
  EXPECT_GT(section->omission_counts[6], 0U); // industry
  auto missing_parent = config(1); missing_parent.parents.pop_back();
  const auto no_industry = build(missing_parent, {Row{}}); ASSERT_TRUE(no_industry);
  EXPECT_FALSE(extract(*no_industry, 0)); // typed values alone cannot supply absent parent provenance
}

TEST(ExposurePanel, PartialWarmupDoesNotBanCompleteLaterDateAndUnknownMembersStayVisible) {
  Row warmup, ready;
  for (auto& value : warmup.raw) value.available_at_ns = 1000;
  const auto panel = build(config(), {warmup, ready}); ASSERT_TRUE(panel);
  const auto first = panel->date_summary(0), second = panel->date_summary(1); ASSERT_TRUE(first); ASSERT_TRUE(second);
  EXPECT_EQ(first->completeness, data::ExposureCompleteness::Partial);
  EXPECT_EQ(second->completeness, data::ExposureCompleteness::Complete);
  EXPECT_FALSE(extract(*panel, 0)); EXPECT_TRUE(extract(*panel, 1));
  Row unknown; unknown.known[0] = 3000;
  Row changed = unknown; changed.member[0] = 0; changed.present[0] = 0;
  const auto a = build(config(1), {unknown}), b = build(config(1), {changed}); ASSERT_TRUE(a); ASSERT_TRUE(b);
  EXPECT_EQ(a->content_sha256(), b->content_sha256()); EXPECT_FALSE(extract(*a, 0));
  data::ExposureExtractConfig drop; drop.missing = data::MissingExposurePolicy::DropUnavailableNames;
  const auto section = extract(*a, 0, drop); ASSERT_TRUE(section);
  EXPECT_EQ(section->known_member_names, 7U); EXPECT_EQ(section->unknown_membership_names, 1U);
  EXPECT_EQ(section->completeness, data::ExposureCompleteness::Partial);
  const auto synthetic = data::extract_exposure_date(*panel, 1, panel->axis_sha256(), panel->content_sha256(), 2000);
  ASSERT_FALSE(synthetic); EXPECT_EQ(synthetic.error().code(), core::ErrorCode::Unavailable);
}

TEST(ExposurePanel, RawWinsorizationDoesNotPromiseFinalZBoundAndFlagsDegenerateColumns) {
  constexpr usize n = 40; Row row(n);
  for (usize i = 0; i < n; ++i) {
    row.caps[i].value = i + 1 == n ? 1e15 : 1;
    row.raw[i].value = i + 1 == n ? 100 : 0;
    row.raw[n + i].value = 1; // identical Amihud values -> explicit degenerate zero
  }
  const auto panel = build(config(1, n), {row}); ASSERT_TRUE(panel);
  const auto result = extract(*panel, 0); ASSERT_TRUE(result);
  const auto summary = panel->date_summary(0); ASSERT_TRUE(summary);
  EXPECT_EQ(summary->columns[1].winsor_passes, 16U);
  EXPECT_TRUE(summary->columns[2].degenerate);
  EXPECT_NE(std::find(result->degenerate_columns.begin(), result->degenerate_columns.end(),
                      data::ExposureDescriptor::Amihud63), result->degenerate_columns.end());
  f64 max_abs = 0, weighted = 0, weight_sum = 0, mean = 0;
  for (usize i = 0; i < n; ++i) {
    const auto z = result->values[i * kFields + 1]; max_abs = std::max(max_abs, std::abs(z));
    const auto w = result->market_cap_usd[i] / 1e15; weighted += w * z; weight_sum += w; mean += z;
    EXPECT_DOUBLE_EQ(result->values[i * kFields + 2], 0);
  }
  EXPECT_GT(max_abs, 3); EXPECT_NEAR(weighted / weight_sum, 0, 1e-12);
  mean /= static_cast<f64>(n); f64 ss = 0;
  for (usize i = 0; i < n; ++i) {
    const auto delta = result->values[i * kFields + 1] - mean; ss += delta * delta;
  }
  EXPECT_NEAR(std::sqrt(ss / static_cast<f64>(n)), 1, 1e-12);
}

TEST(ExposurePanel, CheckedIdentityGeometryBudgetsAndClockFailuresDoNotPublishBadRows) {
  auto cfg = config(1); cfg.max_working_bytes = 1;
  const auto too_small = data::ExposurePanelBuilder::create(cfg); ASSERT_FALSE(too_small);
  EXPECT_EQ(too_small.error().code(), core::ErrorCode::OutOfRange);
  cfg = config(1); cfg.parents.push_back(cfg.parents.front()); EXPECT_FALSE(data::ExposurePanelBuilder::create(cfg));
  cfg = config(1); cfg.instrument_ids[1] = cfg.instrument_ids[0]; EXPECT_FALSE(data::ExposurePanelBuilder::create(cfg));
  cfg = config(1); auto builder = data::ExposurePanelBuilder::create(cfg); ASSERT_TRUE(builder);
  Row row; auto bad = row.view(); bad.raw_descriptors = bad.raw_descriptors.first(1);
  EXPECT_FALSE(builder->append_date(0, bad)); EXPECT_FALSE(builder->finish());
  ASSERT_TRUE(builder->append_date(0, row.view())); // invalid attempt retained append position
  const auto panel = builder->finish(); ASSERT_TRUE(panel);
  EXPECT_FALSE(builder->append_date(0, row.view())); EXPECT_FALSE(builder->finish());
  data::ExposureExtractConfig take; take.allow_synthetic = true;
  const auto wrong = data::extract_exposure_date(*panel, 0, std::string(64, 'b'), panel->content_sha256(), 1000, take);
  ASSERT_FALSE(wrong); EXPECT_EQ(wrong.error().code(), core::ErrorCode::InvalidArgument);
  EXPECT_FALSE(data::extract_exposure_date(*panel, 0, panel->axis_sha256(), panel->content_sha256(), 1001, take));
  take.descriptors.push_back(take.descriptors.back()); EXPECT_FALSE(extract(*panel, 0, take));
  take = {}; take.max_working_bytes = panel->bytes();
  const auto budget = extract(*panel, 0, take); ASSERT_FALSE(budget);
  EXPECT_EQ(budget.error().code(), core::ErrorCode::OutOfRange);
  row.caps[0].observed_through_ns = 30; row.caps[0].available_at_ns = 20;
  const auto impossible_clock = build(config(1), {row}); ASSERT_TRUE(impossible_clock);
  EXPECT_FALSE(extract(*impossible_clock, 0));
  data::ExposurePanel empty; EXPECT_EQ(empty.dates(), 0U); EXPECT_TRUE(empty.parents().empty());
  EXPECT_FALSE(data::extract_exposure_date(empty, 0, {}, {}, 0));
}
} // namespace
