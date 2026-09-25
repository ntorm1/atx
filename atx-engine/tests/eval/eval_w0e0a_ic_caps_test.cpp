// atx-engine/tests/eval/eval_w0e0a_ic_caps_test.cpp
//
// W0-E0a / E-08: the 4,096 date / instrument caps became runtime sizing with a preflight.
//
// Acceptance: "A panel of 6,624 ids x 1,750 dates is accepted" (the 2013-2019 t3000 union),
// plus the plan's "supports >= 16384". Synthetic panels only. The 6,624 x 1,750 panel keeps
// its footprint near 120 MB by aliasing the read-only spans the engine never distinguishes
// (one f64 buffer serves signal/price/raw_price/terminal_value, one u8 buffer of ones is the
// mask and one of zeros serves the three flag spans) — the spans are borrowed and const.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/cross_section_ic.hpp"

namespace atx_test_w0_e0a_ic_caps {

using namespace atx::engine::eval;
using atx::core::ErrorCode;

constexpr atx::i64 kBaseNs = 1'357'084'800'000'000'000; // 2013-01-02T00:00:00Z

// A shared-buffer panel: every value span aliases `values`, every flag span `zeros`.
struct AliasedPanel {
  std::size_t dates{};
  std::size_t instruments{};
  std::vector<double> values;
  std::vector<atx::u8> ones;
  std::vector<atx::u8> zeros;
  std::vector<atx::i64> keys;

  AliasedPanel(std::size_t d, std::size_t n) : dates{d}, instruments{n} {
    const std::size_t cells = d * n;
    values.resize(cells);
    for (std::size_t t = 0U; t < d; ++t) {
      for (std::size_t i = 0U; i < n; ++i) {
        // Positive, varying closes: a slow per-name drift plus a deterministic wiggle.
        const double wiggle = static_cast<double>((t * 7919U + i * 104'729U) % 97U) - 48.0;
        values[t * n + i] = 100.0 + 0.01 * static_cast<double>(t) * static_cast<double>(i % 7U) +
                            0.05 * wiggle;
      }
    }
    ones.assign(cells, atx::u8{1});
    zeros.assign(cells, atx::u8{0});
    keys.resize(d);
    for (std::size_t t = 0U; t < d; ++t) {
      keys[t] = kBaseNs + kNanosPerDay * static_cast<atx::i64>(t);
    }
  }

  [[nodiscard]] CrossSectionIcInput view() const {
    CrossSectionIcInput in{};
    in.dates = dates;
    in.instruments = instruments;
    in.signal = values;
    in.price = values;
    in.raw_price = values;
    in.terminal_value = values;
    in.mask = ones;
    in.terminal = zeros;
    in.terminal_evidenced = zeros;
    in.excluded_audited = zeros;
    in.session_keys = keys;
    return in;
  }
};

[[nodiscard]] CrossSectionIcConfig config(std::span<const std::size_t> horizons,
                                          std::size_t draws) {
  CrossSectionIcConfig cfg{}; // W0-E0a defaults: delay 1, TwoHorizonV2, HansenHodrickV1
  cfg.horizons = horizons;
  cfg.bootstrap_draws = draws;
  cfg.forward_variant = ForwardReturnVariant::DropMissingForward;
  cfg.ties = IcTieHandling::AverageRanksV1;
  return cfg;
}

[[nodiscard]] std::uint64_t scratch_bytes(const CrossSectionIcScratch &s) {
  const std::uint64_t f64s = s.x.size() + s.r.size() + s.rx.size() + s.rr.size() +
                             s.buf.size() + s.w_prev.size() + s.w_curr.size() +
                             s.draw.size() + s.stat.size();
  const std::uint64_t idx = s.order.size() + s.perm.size();
  return f64s * sizeof(double) + idx * sizeof(std::size_t);
}

// ===========================================================================
//  Acceptance: the 2013-2019 t3000 union, 6,624 ids x 1,750 dates, end to end.
// ===========================================================================
TEST(EvalIcCaps, T3000Union_6624IdsBy1750Dates_IsAcceptedAndComputed) {
  constexpr std::size_t kI = 6624U;
  constexpr std::size_t kT = 1750U;
  static_assert(kI > 4096U, "the pre-W0 cap rejected this width");
  const AliasedPanel p{kT, kI};
  const std::vector<std::size_t> h{21U};
  const CrossSectionIcConfig cfg = config(h, 200U);

  auto sizing = preflight_cross_section_ic(kT, kI, cfg);
  ASSERT_TRUE(sizing.has_value()) << sizing.error().message();
  std::printf("[EvalIcCaps] 6624 x 1750: cells %zu, input %llu B, scratch %llu B, result %llu "
              "B, working %llu B (budget %llu B)\n",
              sizing->cells, static_cast<unsigned long long>(sizing->input_bytes),
              static_cast<unsigned long long>(sizing->scratch_bytes),
              static_cast<unsigned long long>(sizing->result_bytes),
              static_cast<unsigned long long>(sizing->working_bytes),
              static_cast<unsigned long long>(cfg.max_working_bytes));
  EXPECT_EQ(sizing->cells, kI * kT);

  auto scratch = plan_cross_section_ic(p.view(), cfg);
  ASSERT_TRUE(scratch.has_value()) << scratch.error().message();
  EXPECT_EQ(scratch_bytes(*scratch), sizing->scratch_bytes);
  auto res = compute_cross_section_ic(p.view(), cfg, *scratch);
  ASSERT_TRUE(res.has_value()) << res.error().message();
  ASSERT_EQ(res->horizons.size(), 1U);
  const IcHorizonSummary &hs = res->horizons[0];
  EXPECT_EQ(hs.series.size(), kT - 21U - 1U); // T - h - delay signal rows
  EXPECT_EQ(hs.full.dates_emitted, kT - 21U - 1U);
  EXPECT_EQ(hs.series.front().n_used, kI);
  EXPECT_EQ(hs.block_len, 42U);
  EXPECT_EQ(hs.full.ic_mean_ci.reportable, 1U); // 1728 / 42 = 41 blocks >= 10
  EXPECT_EQ(hs.full.ic_mean_hac.reportable, 1U);
  std::printf("[EvalIcCaps] 6624 x 1750 computed: dates emitted %zu, n_used/date %zu, IC mean "
              "%.5f, HAC t %.3f\n",
              hs.full.dates_emitted, hs.series.front().n_used, hs.full.ic_mean,
              hs.full.ic_mean_hac.t);
}

// ===========================================================================
//  ">= 16384": both extents at 16,384 pass the preflight, and each is actually computed.
// ===========================================================================
TEST(EvalIcCaps, SixteenK_PreflightAcceptsAndBothExtentsCompute) {
  const std::vector<std::size_t> h{1U};
  const CrossSectionIcConfig cfg = config(h, 64U);
  auto square = preflight_cross_section_ic(16'384U, 16'384U, cfg);
  ASSERT_TRUE(square.has_value()) << square.error().message();
  // The engine's own working set is small; the caller-owned panel is what is large.
  EXPECT_LT(square->working_bytes, cfg.max_working_bytes);
  EXPECT_GT(square->input_bytes, std::uint64_t{9} * 1'000'000'000U);
  std::printf("[EvalIcCaps] 16384 x 16384 preflight: working %llu B, caller input %llu B\n",
              static_cast<unsigned long long>(square->working_bytes),
              static_cast<unsigned long long>(square->input_bytes));

  {
    const AliasedPanel wide{30U, 16'384U};
    auto s = plan_cross_section_ic(wide.view(), cfg);
    ASSERT_TRUE(s.has_value()) << s.error().message();
    auto r = compute_cross_section_ic(wide.view(), cfg, *s);
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(r->horizons[0].series.front().n_used, 16'384U);
  }
  {
    const AliasedPanel tall{16'384U, 3U};
    auto s = plan_cross_section_ic(tall.view(), cfg);
    ASSERT_TRUE(s.has_value()) << s.error().message();
    auto r = compute_cross_section_ic(tall.view(), cfg, *s);
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(r->horizons[0].series.size(), 16'384U - 2U);
  }
}

// ===========================================================================
//  The runtime budget is the binding limit.
// ===========================================================================
TEST(EvalIcCaps, WorkingSetBudget_IsExactAndBinding) {
  const std::vector<std::size_t> h{1U, 5U};
  CrossSectionIcConfig cfg = config(h, 100U);
  auto sizing = preflight_cross_section_ic(200U, 50U, cfg);
  ASSERT_TRUE(sizing.has_value());
  EXPECT_EQ(sizing->working_bytes, sizing->scratch_bytes + sizing->result_bytes);
  EXPECT_EQ(sizing->input_bytes, std::uint64_t{200U * 50U} * 36U + 200U * 8U);

  cfg.max_working_bytes = sizing->working_bytes; // exactly enough
  EXPECT_TRUE(preflight_cross_section_ic(200U, 50U, cfg).has_value());
  cfg.max_working_bytes = sizing->working_bytes - 1U; // one byte short
  auto over = preflight_cross_section_ic(200U, 50U, cfg);
  ASSERT_FALSE(over.has_value());
  EXPECT_EQ(over.error().code(), ErrorCode::OutOfRange);
  cfg.max_working_bytes = 0U;
  auto zero = preflight_cross_section_ic(200U, 50U, cfg);
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(zero.error().code(), ErrorCode::InvalidArgument);

  // The plan runs the same preflight, before any span check or sizing.
  const AliasedPanel p{200U, 50U};
  cfg.max_working_bytes = 1024U;
  auto planned = plan_cross_section_ic(p.view(), cfg);
  ASSERT_FALSE(planned.has_value());
  EXPECT_EQ(planned.error().code(), ErrorCode::OutOfRange);
}

TEST(EvalIcCaps, SanityBoundsStillRejectAndZeroExtentsAreInvalid) {
  const std::vector<std::size_t> h{1U};
  const CrossSectionIcConfig cfg = config(h, 64U);
  EXPECT_GE(kMaxIcDates, 16'384U);
  EXPECT_GE(kMaxIcInstruments, 16'384U);
  auto tall = preflight_cross_section_ic(kMaxIcDates + 1U, 1U, cfg);
  ASSERT_FALSE(tall.has_value());
  EXPECT_EQ(tall.error().code(), ErrorCode::OutOfRange);
  auto wide = preflight_cross_section_ic(2U, kMaxIcInstruments + 1U, cfg);
  ASSERT_FALSE(wide.has_value());
  EXPECT_EQ(wide.error().code(), ErrorCode::OutOfRange);
  auto none = preflight_cross_section_ic(0U, 10U, cfg);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().code(), ErrorCode::InvalidArgument);
  EXPECT_TRUE(preflight_cross_section_ic(kMaxIcDates, 1U, cfg).has_value());
}

// Review fix pass 1: the preflight's documented horizon-count and bootstrap-draw maxima
// are enforced by a standalone preflight, not only by plan_cross_section_ic. The budget
// is raised so only the maxima can reject; both sides of each boundary are pinned.
TEST(EvalIcCaps, StandalonePreflight_EnforcesHorizonCountAndDrawMaxima) {
  std::vector<std::size_t> at_max;
  for (std::size_t k = 0U; k < kMaxIcHorizons; ++k) {
    at_max.push_back(k + 1U);
  }
  std::vector<std::size_t> too_many = at_max;
  too_many.push_back(kMaxIcHorizons + 1U);
  CrossSectionIcConfig cfg = config(at_max, kMaxBootstrapDraws);
  cfg.max_working_bytes = std::uint64_t{1} << 40U;
  EXPECT_TRUE(preflight_cross_section_ic(200U, 50U, cfg).has_value());

  cfg.horizons = too_many;
  auto horizons = preflight_cross_section_ic(200U, 50U, cfg);
  ASSERT_FALSE(horizons.has_value());
  EXPECT_EQ(horizons.error().code(), ErrorCode::InvalidArgument);

  cfg.horizons = at_max;
  cfg.bootstrap_draws = kMaxBootstrapDraws + 1U;
  auto draws = preflight_cross_section_ic(200U, 50U, cfg);
  ASSERT_FALSE(draws.has_value());
  EXPECT_EQ(draws.error().code(), ErrorCode::OutOfRange);
}

} // namespace atx_test_w0_e0a_ic_caps
