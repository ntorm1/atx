// atx-engine/tests/eval/eval_w0e0a_frozen_v1_test.cpp
//
// W0-E0a acceptance "Frozen streams reproduce under V1" (E-02, E-09).
//
// The digest constants below were MEASURED on the W0 base commit 458d0bef (before any
// W0-E0a change) by running this exact panel through `compute_cross_section_ic` with the
// base code. After W0-E0a, the same panel under `BlockLenRule::HalfHorizonV1` and
// `execution_delay = 0` must hash to the identical value: every per-date point, every
// summary and every bootstrap interval (lo/hi bits included) is folded into the digest,
// so one moved stream or one moved forward-return window changes it.
//
// Synthetic panel only: no file I/O, no clock.

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/cross_section_ic.hpp"

namespace atx_test_w0_e0a_frozen_v1 {

using namespace atx::engine::eval;

constexpr atx::i64 kDayNs = kNanosPerDay;
constexpr atx::i64 k2013Jan02Ns = 1'357'084'800'000'000'000; // 2013-01-02T00:00:00Z
constexpr atx::f64 kNaNForTest = std::numeric_limits<atx::f64>::quiet_NaN();

struct Panel {
  atx::usize dates{};
  atx::usize instruments{};
  std::vector<atx::f64> signal, price, raw_price, terminal_value;
  std::vector<atx::u8> mask, terminal, terminal_evidenced, excluded_audited;
  std::vector<atx::i64> session_keys;

  [[nodiscard]] CrossSectionIcInput view() const {
    CrossSectionIcInput in{};
    in.dates = dates;
    in.instruments = instruments;
    in.signal = signal;
    in.price = price;
    in.raw_price = raw_price;
    in.mask = mask;
    in.terminal = terminal;
    in.terminal_evidenced = terminal_evidenced;
    in.terminal_value = terminal_value;
    in.excluded_audited = excluded_audited;
    in.session_keys = session_keys;
    return in;
  }
};

// A seeded random-walk panel whose signal carries a weak edge on the next-day return,
// with some masked, missing and audited-terminal cells so every branch contributes.
[[nodiscard]] Panel make_panel() {
  Panel p{};
  p.dates = 260U;
  p.instruments = 40U;
  const atx::usize cells = p.dates * p.instruments;
  p.signal.assign(cells, 0.0);
  p.price.assign(cells, 0.0);
  p.raw_price.assign(cells, 0.0);
  p.terminal_value.assign(cells, 0.0);
  p.mask.assign(cells, atx::u8{1});
  p.terminal.assign(cells, atx::u8{0});
  p.terminal_evidenced.assign(cells, atx::u8{0});
  p.excluded_audited.assign(cells, atx::u8{0});
  p.session_keys.resize(p.dates);
  atx::core::Xoshiro256pp rng{0xE0A'2026'0924ULL};
  std::vector<atx::f64> level(p.instruments, 50.0);
  std::vector<atx::f64> edge(p.instruments, 0.0);
  for (atx::usize t = 0U; t < p.dates; ++t) {
    // Weekday-ish calendar: every fifth step skips two extra days.
    const atx::i64 gap = static_cast<atx::i64>(t + (t / 5U) * 2U);
    p.session_keys[t] = k2013Jan02Ns + kDayNs * gap;
    for (atx::usize i = 0U; i < p.instruments; ++i) {
      const atx::usize c = t * p.instruments + i;
      const atx::f64 ret = 0.4 * edge[i] + 0.02 * rng.normal();
      level[i] *= (1.0 + ret);
      edge[i] = 0.01 * rng.normal();
      p.price[c] = level[i];
      p.raw_price[c] = level[i] * 0.97;
      p.signal[c] = edge[i] + 0.002 * rng.normal();
      if ((i * 7U + t) % 53U == 0U) {
        p.mask[c] = atx::u8{0};
      }
      if ((i * 3U + t * 5U) % 97U == 0U) {
        p.price[c] = kNaNForTest;
      }
      if (i % 13U == 5U) {
        p.excluded_audited[c] = atx::u8{1};
      }
      if (i == 11U && t > 200U) {
        p.price[c] = kNaNForTest; // delisted: terminal coverage from t = 190
      }
      if (i == 11U && t >= 190U) {
        p.terminal[c] = atx::u8{1};
        p.terminal_evidenced[c] = (t % 2U == 0U) ? atx::u8{1} : atx::u8{0};
        p.terminal_value[c] = 40.0;
      }
    }
  }
  return p;
}

// FNV-1a over 64-bit words.
struct Digest {
  std::uint64_t h{14695981039346656037ULL};
  void word(std::uint64_t v) noexcept {
    for (int b = 0; b < 8; ++b) {
      h ^= (v >> (8 * b)) & 0xFFULL;
      h *= 1099511628211ULL;
    }
  }
  void f(atx::f64 v) noexcept { word(std::bit_cast<std::uint64_t>(v)); }
  void u(atx::usize v) noexcept { word(static_cast<std::uint64_t>(v)); }
  void i(atx::i64 v) noexcept { word(static_cast<std::uint64_t>(v)); }
};

void fold(Digest &d, const BootstrapInterval &iv) {
  d.f(iv.point);
  d.f(iv.lo);
  d.f(iv.hi);
  d.u(iv.draws);
  d.u(iv.block_len);
  d.u(iv.blocks);
  d.u(iv.series_len);
  d.u(iv.modulo_fallbacks);
  d.u(iv.reportable);
  d.u(iv.unreportable_reason);
}

void fold(Digest &d, const IcSampleStats &s) {
  d.u(s.dates_emitted);
  d.f(s.ic_mean);
  d.f(s.ic_sd);
  d.f(s.icir);
  d.f(s.naive_t);
  d.f(s.rank_ic_mean);
  d.f(s.rank_ic_sd);
  d.f(s.rank_icir);
  d.f(s.rank_naive_t);
  d.f(s.spread_gross_mean);
  d.f(s.spread_gross_sd);
  d.f(s.spread_net_mean);
  d.f(s.spread_net_sd);
  fold(d, s.ic_mean_ci);
  fold(d, s.icir_ci);
  fold(d, s.rank_ic_mean_ci);
  fold(d, s.rank_icir_ci);
  fold(d, s.spread_gross_ci);
  fold(d, s.spread_net_ci);
  d.u(s.common_prefix_gaps);
  d.u(s.summary_reportable);
  d.u(s.unreportable_reason);
}

[[nodiscard]] std::uint64_t digest(const CrossSectionIcResult &r) {
  Digest d{};
  for (const IcHorizonSummary &h : r.horizons) {
    d.u(h.horizon);
    d.u(h.block_len);
    d.u(h.dates_below_min_names);
    d.u(h.dates_below_quantile_count);
    fold(d, h.full);
    fold(d, h.common);
    d.f(h.decile_one_way_turnover);
    d.f(h.trade_drag);
    d.f(h.mean_borrow_drag);
    d.f(h.mean_cost_drag);
    d.f(h.mean_days_forward);
    d.u(h.spread_reportable);
    d.u(h.spread_unreportable_reason);
    for (const QuantileBucketStat &b : h.buckets) {
      d.u(b.quantile);
      d.u(b.n_dates);
      d.f(b.mean_forward_return);
      d.f(b.mean_names);
    }
    for (const IcDatePoint &p : h.series) {
      d.u(p.date);
      d.i(p.session_key);
      d.u(p.n_eligible);
      d.u(p.n_signal_finite);
      d.u(p.n_with_forward);
      d.u(p.n_dropped_missing_forward);
      d.u(p.n_terminal_applied);
      d.u(p.n_terminal_unevidenced);
      d.u(p.n_excluded_audited);
      d.u(p.n_used);
      d.f(p.pearson_ic);
      d.f(p.rank_ic);
      d.f(p.spread_gross);
      d.f(p.spread_net);
      d.i(p.days_forward);
      d.f(p.borrow_drag);
      d.f(p.cost_drag);
      d.u(p.emitted);
      d.u(p.spread_emitted);
      d.u(p.in_common_sample);
    }
  }
  d.u(r.autocorr.lag);
  d.u(r.autocorr.pairs_emitted);
  d.f(r.autocorr.rho_pearson);
  d.f(r.autocorr.rho_rank);
  d.f(r.autocorr.implied_one_way_turnover);
  return d.h;
}

// The pre-registered recipe of the frozen streams: floor 5 and the checkpoint-14 horizon
// set; `variant` / `restriction` select the four configurations digested below.
[[nodiscard]] CrossSectionIcConfig frozen_config(std::span<const atx::usize> horizons,
                                                 ForwardReturnVariant variant,
                                                 atx::u64 restriction) {
  CrossSectionIcConfig cfg{};
  cfg.horizons = horizons;
  cfg.quantiles = 10U;
  cfg.min_names_per_date = 2U;
  cfg.bootstrap_draws = 400U;
  cfg.block_len_floor = 5U;
  cfg.bootstrap_seed = 20260920ULL;
  cfg.stream_signal_index = 3U;
  cfg.stream_variant_id =
      (variant == ForwardReturnVariant::IncludeAuditedTerminalV1) ? 1U : 0U;
  cfg.stream_restriction_id = restriction;
  cfg.trade_bps = 5.0;
  cfg.annual_borrow_bps = 50.0;
  cfg.short_leg_gross = 1.0;
  cfg.day_basis = 365;
  cfg.common_sample_dates = 150U;
  cfg.forward_variant = variant;
  cfg.ties = IcTieHandling::AverageRanksV1;
  return cfg;
}

struct Case {
  ForwardReturnVariant variant;
  atx::u64 restriction;
  std::uint64_t base_digest; // measured on 458d0bef
};

TEST(EvalHac, FrozenStreams_ReproduceUnderV1) {
  const Panel p = make_panel();
  const std::vector<atx::usize> horizons{1U, 5U, 10U, 21U, 63U};
  const Case cases[] = {
      {ForwardReturnVariant::DropMissingForward, 0U, 0x3eff877612745386ULL},
      {ForwardReturnVariant::DropMissingForward, 1U, 0xdee7f78c1999af44ULL},
      {ForwardReturnVariant::IncludeAuditedTerminalV1, 0U, 0x5f2c2f0e33647808ULL},
      {ForwardReturnVariant::IncludeAuditedTerminalV1, 1U, 0x0d8febe9dd429f91ULL},
  };
  for (const Case &c : cases) {
    const CrossSectionIcConfig cfg = frozen_config(horizons, c.variant, c.restriction);
    auto scratch = plan_cross_section_ic(p.view(), cfg);
    ASSERT_TRUE(scratch.has_value()) << scratch.error().message();
    auto res = compute_cross_section_ic(p.view(), cfg, *scratch);
    ASSERT_TRUE(res.has_value()) << res.error().message();
    const std::uint64_t got = digest(*res);
    std::printf("W0E0A_FROZEN_DIGEST variant=%u restriction=%llu digest=0x%016llx\n",
                static_cast<unsigned>(c.variant), static_cast<unsigned long long>(c.restriction),
                static_cast<unsigned long long>(got));
    EXPECT_EQ(got, c.base_digest);
  }
}

} // namespace atx_test_w0_e0a_frozen_v1
