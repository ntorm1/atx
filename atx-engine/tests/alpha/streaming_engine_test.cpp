// atx::engine::alpha — StreamingEngine (Lane 1 / streaming_engine.hpp).
//
// Contract: warm() over dates [0, T-K) then step() over [T-K, T) reproduces the
// last K rows of the batch Engine::evaluate over [0, T) — bit-for-bit in
// AuditExact AND ResearchFast (the streaming states share / restate the batch
// kernels' exact operation order) — on a WQ101-style battery exercising every
// Ts routing class (lookback, running sum, deque extremes, order statistics,
// generic windowed, Welford / sliding lanes, recurrences) plus Cs ops, NaN holes
// and a moving universe.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streaming_engine.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_test_l1_kernels_streaming {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::CrossSection;
using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::StreamingEngine;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] bool same_bits(atx::f64 a, atx::f64 b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  atx::u64 ua = 0;
  atx::u64 ub = 0;
  std::memcpy(&ua, &a, sizeof(a));
  std::memcpy(&ub, &b, sizeof(b));
  return ua == ub;
}

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  if (!ast) {
    return Program{};
  }
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  if (!ana) {
    return Program{};
  }
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// WQ101 alphas expressible from the shipped fields (verbatim modulo the
// registry's names), plus operators the 101 do not use but the streaming engine
// must also carry (variance family, sliding lanes, order stats, recurrences).
[[nodiscard]] std::string_view battery_src() {
  return "a1 = rank(ts_argmax(signedpower((returns < 0) ? stddev(returns, 20) : close, 2), 5)) - "
         "0.5\n"
         "a2 = -1 * correlation(rank(delta(log(volume), 2)), rank((close - open) / open), 6)\n"
         "a3 = -1 * correlation(rank(open), rank(volume), 10)\n"
         "a4 = -1 * ts_rank(rank(low), 9)\n"
         "a5 = rank(open - ts_sum(vwap, 10) / 10) * (-1 * abs(rank(close - vwap)))\n"
         "a6 = -1 * correlation(open, volume, 10)\n"
         "a7 = (adv20 < volume) ? ((-1 * ts_rank(abs(delta(close, 7)), 60)) * "
         "sign(delta(close, 7))) : (-1 * 1)\n"
         "a8 = -1 * rank(ts_sum(open, 5) * ts_sum(returns, 5) - delay(ts_sum(open, 5) * "
         "ts_sum(returns, 5), 10))\n"
         "a9 = (0 < ts_min(delta(close, 1), 5)) ? delta(close, 1) : ((ts_max(delta(close, 1), 5) "
         "< 0) ? delta(close, 1) : (-1 * delta(close, 1)))\n"
         "a12 = sign(delta(volume, 1)) * (-1 * delta(close, 1))\n"
         "a13 = -1 * rank(covariance(rank(close), rank(volume), 5))\n"
         "a14 = (-1 * rank(delta(returns, 3))) * correlation(open, volume, 10)\n"
         "a15 = -1 * ts_sum(rank(correlation(rank(high), rank(volume), 3)), 3)\n"
         "a17 = ((-1 * rank(ts_rank(close, 10))) * rank(delta(delta(close, 1), 1))) * "
         "rank(ts_rank(volume / adv20, 5))\n"
         "a18 = -1 * rank(stddev(abs(close - open), 5) + (close - open) + correlation(close, "
         "open, 10))\n"
         "a19 = (-1 * sign((close - delay(close, 7)) + delta(close, 7))) * (1 + rank(1 + "
         "ts_sum(returns, 250)))\n"
         "a20 = ((-1 * rank(open - delay(high, 1))) * rank(open - delay(close, 1))) * rank(open - "
         "delay(low, 1))\n"
         "a22 = -1 * (delta(correlation(high, volume, 5), 5) * rank(stddev(close, 20)))\n"
         "a23 = (ts_sum(high, 20) / 20 < high) ? (-1 * delta(high, 2)) : 0\n"
         "a26 = -1 * ts_max(correlation(ts_rank(volume, 5), ts_rank(high, 5), 5), 3)\n"
         "a28 = scale(correlation(adv20, low, 5) + (high + low) / 2 - close, 1)\n"
         "a33 = rank(-1 * (1 - open / close))\n"
         "a34 = rank((1 - rank(stddev(returns, 2) / stddev(returns, 5))) + (1 - rank(delta(close, "
         "1))))\n"
         "a35 = (ts_rank(volume, 32) * (1 - ts_rank(close + high - low, 16))) * (1 - "
         "ts_rank(returns, 32))\n"
         "a37 = rank(correlation(delay(open - close, 1), close, 200)) + rank(open - close)\n"
         "a38 = (-1 * rank(ts_rank(close, 10))) * rank(close / open)\n"
         "a40 = (-1 * rank(stddev(high, 10))) * correlation(high, volume, 10)\n"
         "a41 = power(high * low, 0.5) - vwap\n"
         "a43 = ts_rank(volume / adv20, 20) * ts_rank(-1 * delta(close, 7), 8)\n"
         "a44 = -1 * correlation(high, rank(volume), 5)\n"
         "a45 = -1 * ((rank(ts_sum(delay(close, 5), 20) / 20) * correlation(close, volume, 2)) * "
         "rank(correlation(ts_sum(close, 5), ts_sum(close, 20), 2)))\n"
         "a53 = -1 * delta(((close - low) - (high - close)) / (close - low), 9)\n"
         "a54 = (-1 * ((low - close) * power(open, 5))) / ((low - high) * power(close, 5))\n"
         "a55 = -1 * correlation(rank((close - ts_min(low, 12)) / (ts_max(high, 12) - ts_min(low, "
         "12))), rank(volume), 6)\n"
         "a60 = -1 * (2 * scale(rank((((close - low) - (high - close)) / (high - low)) * volume), "
         "1) - scale(rank(ts_argmax(close, 10)), 1))\n"
         "a101 = (close - open) / ((high - low) + 0.001)\n"
         "x1 = decay_linear(rank(close), 4) + wma(close, 20)\n"
         "x2 = indneutralize(close, IndClass.sector) + group_rank(volume, IndClass.sector)\n"
         "x3 = ts_zscore(close, 20) + ts_av_diff(volume, 10) + ts_var(returns, 15)\n"
         "x4 = slope(close, 15) + rsquare(close, 30) + resid(vwap, 12)\n"
         "x5 = med(close, 11) + ts_quantile(volume, 7) + ts_scale(close, 9) + ts_min(low, 3)\n"
         "x6 = skew(returns, 20) + kurt(returns, 20) + mad(close, 8) + product(1 + returns, 5)\n"
         "x7 = ts_backfill(returns, 5) + ts_count_nans(returns, 10) + ema(close, 12)\n"
         "x8 = trade_when(volume > adv20, rank(returns), returns < -0.04)\n"
         "x9 = hump(rank(close), 0.05) + kalman_level(close, 0.01, 1.0)\n"
         "x10 = ts_regression(close, vwap, 20) + zscore(volume) + winsorize(returns, 3)\n";
}

struct Fixture {
  atx::usize dates{0};
  atx::usize inst{0};
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  std::vector<std::uint8_t> universe;
};

// OHLCV-ish random walks with NaN holes and a universe that drifts.
[[nodiscard]] Fixture make_fixture(atx::usize dates, atx::usize inst, std::uint64_t seed) {
  Fixture f;
  f.dates = dates;
  f.inst = inst;
  f.names = {"open", "high", "low", "close", "volume", "vwap", "returns", "adv20",
             "IndClass.sector"};
  const atx::usize cells = dates * inst;
  f.cols.assign(f.names.size(), std::vector<atx::f64>(cells));
  f.universe.assign(cells, 1);
  std::mt19937_64 rng{seed};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::uniform_real_distribution<atx::f64> u{0.0, 1.0};
  std::vector<atx::f64> px(inst, 50.0);
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize j = 0; j < inst; ++j) {
      const atx::usize i = t * inst + j;
      const atx::f64 prev = px[j];
      px[j] = std::max(1.0, px[j] * (1.0 + 0.02 * nd(rng)));
      const atx::f64 o = prev * (1.0 + 0.005 * nd(rng));
      const atx::f64 c = px[j];
      const atx::f64 h = std::max(o, c) * (1.0 + 0.01 * u(rng));
      const atx::f64 l = std::min(o, c) * (1.0 - 0.01 * u(rng));
      f.cols[0][i] = o;
      f.cols[1][i] = h;
      f.cols[2][i] = l;
      f.cols[3][i] = c;
      f.cols[4][i] = 1.0e5 * (0.5 + u(rng));
      f.cols[5][i] = (h + l + c) / 3.0;
      f.cols[6][i] = c / prev - 1.0;
      f.cols[7][i] = 1.0e5;
      f.cols[8][i] = static_cast<atx::f64>(j % 4);
      if (u(rng) < 0.01) {
        f.cols[6][i] = kNaN; // returns holes
      }
      if (u(rng) < 0.005) {
        f.cols[3][i] = kNaN; // close holes
      }
      f.universe[i] = (u(rng) < 0.03) ? 0 : 1;
    }
  }
  return f;
}

// A Panel over dates [0, upto).
[[nodiscard]] Panel prefix_panel(const Fixture &f, atx::usize upto) {
  const atx::usize cells = upto * f.inst;
  std::vector<std::vector<atx::f64>> cols;
  for (const auto &c : f.cols) {
    cols.emplace_back(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(cells));
  }
  std::vector<std::uint8_t> uni(f.universe.begin(),
                                f.universe.begin() + static_cast<std::ptrdiff_t>(cells));
  auto p = Panel::create(upto, f.inst, f.names, std::move(cols), std::move(uni));
  EXPECT_TRUE(p.has_value());
  return p.value();
}

// The CrossSection of date t, fields in Program::fields order.
[[nodiscard]] CrossSection row_of(const Fixture &f, const Program &prog, atx::usize t) {
  CrossSection cs;
  for (const std::string &name : prog.fields) {
    atx::usize k = 0;
    while (k < f.names.size() && f.names[k] != name) {
      ++k;
    }
    EXPECT_LT(k, f.names.size()) << name;
    cs.fields.emplace_back(f.cols[k].data() + t * f.inst, f.inst);
  }
  cs.universe = std::span<const std::uint8_t>{f.universe.data() + t * f.inst, f.inst};
  return cs;
}

void expect_stream_equals_batch(EvalMode mode) {
  constexpr atx::usize kDates = 300;
  constexpr atx::usize kInst = 24;
  constexpr atx::usize kStream = 25;
  const Fixture f = make_fixture(kDates, kInst, 0xA1FA101ULL);
  const Program prog = compile_ok(battery_src());
  ASSERT_FALSE(prog.roots.empty());

  const Panel full = prefix_panel(f, kDates);
  Engine eng{full};
  eng.set_eval_mode(mode);
  auto batch = eng.evaluate(prog);
  ASSERT_TRUE(batch.has_value()) << batch.error().message();

  auto se = StreamingEngine::create(prog, static_cast<atx::u32>(kInst), mode);
  ASSERT_TRUE(se.has_value()) << se.error().message();
  ASSERT_TRUE(se->warm(prefix_panel(f, kDates - kStream)).has_value());
  for (atx::usize t = kDates - kStream; t < kDates; ++t) {
    auto r = se->step(row_of(f, prog, t));
    ASSERT_TRUE(r.has_value()) << r.error().message();
    for (atx::usize a = 0; a < prog.roots.size(); ++a) {
      const std::span<const atx::f64> got = se->output(a);
      const std::vector<atx::f64> &want = batch->alphas[a].values;
      for (atx::usize j = 0; j < kInst; ++j) {
        ASSERT_TRUE(same_bits(got[j], want[t * kInst + j]))
            << prog.roots[a].name << " t=" << t << " j=" << j << " got=" << got[j]
            << " want=" << want[t * kInst + j] << " mode=" << static_cast<int>(mode);
      }
    }
  }
  EXPECT_EQ(se->dates_seen(), kDates);
}

TEST(StreamingEngine_Batch, Wq101BatteryBitExactAuditExact) {
  expect_stream_equals_batch(EvalMode::AuditExact);
}

TEST(StreamingEngine_Batch, Wq101BatteryBitExactResearchFast) {
  expect_stream_equals_batch(EvalMode::ResearchFast);
}

TEST(StreamingEngine_Batch, ColdStartStepOnlyEqualsBatch) {
  // No warm(): stepping every date from the start is the same computation.
  constexpr atx::usize kDates = 60;
  constexpr atx::usize kInst = 9;
  const Fixture f = make_fixture(kDates, kInst, 77ULL);
  const Program prog = compile_ok("a = ts_sum(close, 5) + ts_rank(volume, 7) + rank(returns)\n");
  const Panel full = prefix_panel(f, kDates);
  Engine eng{full};
  auto batch = eng.evaluate(prog);
  ASSERT_TRUE(batch.has_value());
  auto se = StreamingEngine::create(prog, static_cast<atx::u32>(kInst), EvalMode::AuditExact);
  ASSERT_TRUE(se.has_value());
  for (atx::usize t = 0; t < kDates; ++t) {
    auto r = se->step(row_of(f, prog, t));
    ASSERT_TRUE(r.has_value());
    for (atx::usize j = 0; j < kInst; ++j) {
      ASSERT_TRUE(same_bits((*r)[j], batch->alphas[0].values[t * kInst + j])) << t << "," << j;
    }
  }
}

TEST(StreamingEngine_Errors, ShapeMismatchesAreRejected) {
  const Program prog = compile_ok("a = ts_mean(close, 3)\n");
  EXPECT_FALSE(StreamingEngine::create(prog, 0, EvalMode::AuditExact).has_value());
  auto se = StreamingEngine::create(prog, 4, EvalMode::AuditExact);
  ASSERT_TRUE(se.has_value());
  const Fixture f = make_fixture(10, 5, 1ULL); // 5 instruments != 4
  EXPECT_FALSE(se->warm(prefix_panel(f, 10)).has_value());
  CrossSection bad;
  EXPECT_FALSE(se->step(bad).has_value()); // no field rows
}

} // namespace atx_test_l1_kernels_streaming
