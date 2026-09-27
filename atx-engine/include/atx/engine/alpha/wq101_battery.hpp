#pragma once

// atx::engine::alpha — WQ101 throughput / conformance battery (Lane 2).
//
// The "101 Formulaic Alphas" (Kakushadze 2016, arXiv:1601.00991) written in the
// atx DSL, plus a deterministic synthetic panel carrying every field they read.
// Used by the fusion bit-identity test, the subtree-cache / strategy-B tests and
// the release throughput bench (bench/alpha_wq101_battery_bench.cpp), so all three
// measure the SAME workload.
//
// Transcription rules (documented so the battery is reproducible):
//   * a comparison yields a Mask, which the typechecker keeps out of arithmetic;
//     the paper's `(a < b) * -1` is written `((a < b) ? -1 : 0)` (same values).
//   * `x ^ e` -> power(x, e); `sum` -> ts_sum; `min/max(x, d)` with a window ->
//     ts_min/ts_max; `Ts_ArgMax` -> ts_argmax; `IndClass.*` classifiers are the
//     panel's group columns.
//   * fractional windows (e.g. 7.23) are rounded to the nearest integer (the VM
//     truncates windows; rounding matches the paper's intent more closely).
//   * `returns`, `cap`, `vwap` and every `adv{d}` the battery references are panel
//     fields (make_wq101_panel builds them), so the alphas stay one-liners.
//   * alphas the paper defines with nested industry-neutral multi-stage blends that
//     need an op we do not ship are omitted; `wq101_alphas()` lists what is present
//     and the id comment on each line says which paper alpha it is.
//
// Header-only; every function is `inline`. Panel construction is a cold path.

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::alpha {

namespace detail {
inline constexpr std::array<std::string_view, 70> kWq101 = {{
    // #1
    "rank(ts_argmax(signedpower(((returns < 0) ? stddev(returns, 20) : close), 2), 5)) - 0.5",
    // #2
    "-1 * correlation(rank(delta(log(volume), 2)), rank(((close - open) / open)), 6)",
    // #3
    "-1 * correlation(rank(open), rank(volume), 10)",
    // #4
    "-1 * ts_rank(rank(low), 9)",
    // #5
    "rank((open - (ts_sum(vwap, 10) / 10))) * (-1 * abs(rank((close - vwap))))",
    // #6
    "-1 * correlation(open, volume, 10)",
    // #7
    "((adv20 < volume) ? ((-1 * ts_rank(abs(delta(close, 7)), 60)) * sign(delta(close, 7))) : "
    "(-1 * 1))",
    // #8
    "-1 * rank(((ts_sum(open, 5) * ts_sum(returns, 5)) - delay((ts_sum(open, 5) * "
    "ts_sum(returns, 5)), 10)))",
    // #9
    "((0 < ts_min(delta(close, 1), 5)) ? delta(close, 1) : ((ts_max(delta(close, 1), 5) < 0) ? "
    "delta(close, 1) : (-1 * delta(close, 1))))",
    // #10
    "rank(((0 < ts_min(delta(close, 1), 4)) ? delta(close, 1) : ((ts_max(delta(close, 1), 4) < "
    "0) ? delta(close, 1) : (-1 * delta(close, 1)))))",
    // #11
    "((rank(ts_max((vwap - close), 3)) + rank(ts_min((vwap - close), 3))) * "
    "rank(delta(volume, 3)))",
    // #12
    "sign(delta(volume, 1)) * (-1 * delta(close, 1))",
    // #13
    "-1 * rank(covariance(rank(close), rank(volume), 5))",
    // #14
    "((-1 * rank(delta(returns, 3))) * correlation(open, volume, 10))",
    // #15
    "(-1 * ts_sum(rank(correlation(rank(high), rank(volume), 3)), 3))",
    // #16
    "(-1 * rank(covariance(rank(high), rank(volume), 5)))",
    // #17
    "(((-1 * rank(ts_rank(close, 10))) * rank(delta(delta(close, 1), 1))) * "
    "rank(ts_rank((volume / adv20), 5)))",
    // #18
    "(-1 * rank(((stddev(abs((close - open)), 5) + (close - open)) + correlation(close, open, "
    "10))))",
    // #19
    "((-1 * sign(((close - delay(close, 7)) + delta(close, 7)))) * (1 + rank((1 + "
    "ts_sum(returns, 250)))))",
    // #20
    "(((-1 * rank((open - delay(high, 1)))) * rank((open - delay(close, 1)))) * rank((open - "
    "delay(low, 1))))",
    // #21
    "((((ts_sum(close, 8) / 8) + stddev(close, 8)) < (ts_sum(close, 2) / 2)) ? (-1 * 1) : "
    "(((ts_sum(close, 2) / 2) < ((ts_sum(close, 8) / 8) - stddev(close, 8))) ? 1 : (((1 < "
    "(volume / adv20)) || ((volume / adv20) == 1)) ? 1 : (-1 * 1))))",
    // #22
    "(-1 * (delta(correlation(high, volume, 5), 5) * rank(stddev(close, 20))))",
    // #23
    "(((ts_sum(high, 20) / 20) < high) ? (-1 * delta(high, 2)) : 0)",
    // #24
    "((((delta((ts_sum(close, 100) / 100), 100) / delay(close, 100)) < 0.05) || "
    "((delta((ts_sum(close, 100) / 100), 100) / delay(close, 100)) == 0.05)) ? (-1 * (close - "
    "ts_min(close, 100))) : (-1 * delta(close, 3)))",
    // #25
    "rank(((((-1 * returns) * adv20) * vwap) * (high - close)))",
    // #26
    "(-1 * ts_max(correlation(ts_rank(volume, 5), ts_rank(high, 5), 5), 3))",
    // #27
    "((0.5 < rank((ts_sum(correlation(rank(volume), rank(vwap), 6), 2) / 2.0))) ? (-1 * 1) : 1)",
    // #28
    "scale(((correlation(adv20, low, 5) + ((high + low) / 2)) - close))",
    // #29
    "(ts_min(product(rank(rank(scale(log(ts_sum(ts_min(rank(rank((-1 * rank(delta((close - 1), "
    "5))))), 2), 1))))), 1), 5) + ts_rank(delay((-1 * returns), 6), 5))",
    // #30
    "(((1.0 - rank(((sign((close - delay(close, 1))) + sign((delay(close, 1) - delay(close, "
    "2)))) + sign((delay(close, 2) - delay(close, 3)))))) * ts_sum(volume, 5)) / ts_sum(volume, "
    "20))",
    // #31
    "((rank(rank(rank(decay_linear((-1 * rank(rank(delta(close, 10)))), 10)))) + rank((-1 * "
    "delta(close, 3)))) + sign(scale(correlation(adv20, low, 12))))",
    // #32
    "(scale(((ts_sum(close, 7) / 7) - close)) + (20 * scale(correlation(vwap, delay(close, 5), "
    "230))))",
    // #33
    "rank((-1 * (1 - (open / close))))",
    // #34
    "rank(((1 - rank((stddev(returns, 2) / stddev(returns, 5)))) + (1 - rank(delta(close, 1)))))",
    // #35
    "((ts_rank(volume, 32) * (1 - ts_rank(((close + high) - low), 16))) * (1 - ts_rank(returns, "
    "32)))",
    // #36
    "(((((2.21 * rank(correlation((close - open), delay(volume, 1), 15))) + (0.7 * rank((open - "
    "close)))) + (0.73 * rank(ts_rank(delay((-1 * returns), 6), 5)))) + rank(abs(correlation(vwap, "
    "adv20, 6)))) + (0.6 * rank((((ts_sum(close, 200) / 200) - open) * (close - open)))))",
    // #37
    "(rank(correlation(delay((open - close), 1), close, 200)) + rank((open - close)))",
    // #38
    "((-1 * rank(ts_rank(close, 10))) * rank((close / open)))",
    // #39
    "((-1 * rank((delta(close, 7) * (1 - rank(decay_linear((volume / adv20), 9)))))) * (1 + "
    "rank(ts_sum(returns, 250))))",
    // #40
    "((-1 * rank(stddev(high, 10))) * correlation(high, volume, 10))",
    // #41
    "(power((high * low), 0.5) - vwap)",
    // #42
    "(rank((vwap - close)) / rank((vwap + close)))",
    // #43
    "(ts_rank((volume / adv20), 20) * ts_rank((-1 * delta(close, 7)), 8))",
    // #44
    "(-1 * correlation(high, rank(volume), 5))",
    // #45
    "(-1 * ((rank((ts_sum(delay(close, 5), 20) / 20)) * correlation(close, volume, 2)) * "
    "rank(correlation(ts_sum(close, 5), ts_sum(close, 20), 2))))",
    // #46
    "((0.25 < (((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10))) "
    "? (-1 * 1) : (((((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / "
    "10)) < 0) ? 1 : ((-1 * 1) * (close - delay(close, 1)))))",
    // #47
    "((((rank((1 / close)) * volume) / adv20) * ((high * rank((high - close))) / (ts_sum(high, 5) "
    "/ 5))) - rank((vwap - delay(vwap, 5))))",
    // #48
    "(indneutralize(((correlation(delta(close, 1), delta(delay(close, 1), 1), 250) * delta(close, "
    "1)) / close), IndClass.subindustry) / ts_sum(power((delta(close, 1) / delay(close, 1)), 2), "
    "250))",
    // #49
    "(((((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10)) < (-1 * "
    "0.1)) ? 1 : ((-1 * 1) * (close - delay(close, 1))))",
    // #50
    "(-1 * ts_max(rank(correlation(rank(volume), rank(vwap), 5)), 5))",
    // #51
    "(((((delay(close, 20) - delay(close, 10)) / 10) - ((delay(close, 10) - close) / 10)) < (-1 * "
    "0.05)) ? 1 : ((-1 * 1) * (close - delay(close, 1))))",
    // #52
    "((((-1 * ts_min(low, 5)) + delay(ts_min(low, 5), 5)) * rank(((ts_sum(returns, 240) - "
    "ts_sum(returns, 20)) / 220))) * ts_rank(volume, 5))",
    // #53
    "(-1 * delta((((close - low) - (high - close)) / (close - low)), 9))",
    // #54
    "((-1 * ((low - close) * power(open, 5))) / ((low - high) * power(close, 5)))",
    // #55
    "(-1 * correlation(rank(((close - ts_min(low, 12)) / (ts_max(high, 12) - ts_min(low, 12)))), "
    "rank(volume), 6))",
    // #56
    "(0 - (1 * (rank((ts_sum(returns, 10) / ts_sum(ts_sum(returns, 2), 3))) * rank((returns * "
    "cap)))))",
    // #57
    "(0 - (1 * ((close - vwap) / decay_linear(rank(ts_argmax(close, 30)), 2))))",
    // #58
    "(-1 * ts_rank(decay_linear(correlation(indneutralize(vwap, IndClass.sector), volume, 4), 8), "
    "6))",
    // #60
    "(0 - (1 * ((2 * scale(rank(((((close - low) - (high - close)) / (high - low)) * volume)))) - "
    "scale(rank(ts_argmax(close, 10))))))",
    // #61
    "(rank((vwap - ts_min(vwap, 16))) < rank(correlation(vwap, adv180, 18)))",
    // #62
    "((rank(correlation(vwap, ts_sum(adv20, 22), 10)) < rank((((rank(open) + rank(open)) < "
    "(rank(((high + low) / 2)) + rank(high))) ? 1 : 0))) ? -1 : 0)",
    // #64
    "((rank(correlation(ts_sum(((open * 0.178404) + (low * (1 - 0.178404))), 13), ts_sum(adv120, "
    "13), 17)) < rank(delta(((((high + low) / 2) * 0.178404) + (vwap * (1 - 0.178404))), 4))) ? "
    "-1 : 0)",
    // #65
    "((rank(correlation(((open * 0.00817205) + (vwap * (1 - 0.00817205))), ts_sum(adv60, 9), 6)) "
    "< rank((open - ts_min(open, 14)))) ? -1 : 0)",
    // #71
    "max(ts_rank(decay_linear(correlation(ts_rank(close, 3), ts_rank(adv180, 12), 18), 4), 16), "
    "ts_rank(decay_linear(power(rank(((low + open) - (vwap + vwap))), 2), 16), 4))",
    // #72
    "(rank(decay_linear(correlation(((high + low) / 2), adv40, 9), 10)) / "
    "rank(decay_linear(correlation(ts_rank(vwap, 4), ts_rank(volume, 19), 7), 3)))",
    // #74
    "((rank(correlation(close, ts_sum(adv30, 37), 15)) < rank(correlation(rank(((high * "
    "0.0261661) + (vwap * (1 - 0.0261661)))), rank(volume), 11))) ? -1 : 0)",
    // #75
    "(rank(correlation(vwap, volume, 4)) < rank(correlation(rank(low), rank(adv50), 12)))",
    // #78
    "power(rank(correlation(ts_sum(((low * 0.352233) + (vwap * (1 - 0.352233))), 20), "
    "ts_sum(adv40, 20), 7)), rank(correlation(rank(vwap), rank(volume), 6)))",
    // #83
    "((rank(delay(((high - low) / (ts_sum(close, 5) / 5)), 2)) * rank(rank(volume))) / (((high - "
    "low) / (ts_sum(close, 5) / 5)) / (vwap - close)))",
    // #101
    "((close - open) / ((high - low) + 0.001))",
}};
} // namespace detail

// The battery, in paper order. Each entry is one bare DSL expression.
[[nodiscard]] inline std::span<const std::string_view> wq101_alphas() noexcept {
  return detail::kWq101;
}

// adv windows the battery reads (plus adv20, the datafields default).
inline constexpr std::array<atx::u16, 7> kWq101AdvWindows = {20, 30, 40, 50, 60, 120, 180};

// Deterministic synthetic panel with every field the battery reads: OHLCV as a
// per-instrument geometric random walk, `returns` = close/prev_close - 1 (NaN on
// the first date), `cap`, three nested IndClass classifiers, derived vwap and
// adv{d}. About 1% of cells are out-of-universe (so the NaN paths run too).
[[nodiscard]] inline atx::core::Result<Panel> make_wq101_panel(atx::usize dates,
                                                               atx::usize instruments,
                                                               std::uint64_t seed = 0x5eed) {
  const atx::usize cells = dates * instruments;
  std::uint64_t s = seed | 1U;
  auto next = [&s]() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(s >> 11U) / static_cast<atx::f64>(1ULL << 53U);
  };
  enum Col : atx::usize { kC, kO, kH, kL, kV, kR, kCap, kSec, kInd, kSub, kNCols };
  std::vector<std::vector<atx::f64>> cols(kNCols, std::vector<atx::f64>(cells));
  std::vector<std::uint8_t> universe(cells, 1);
  std::vector<atx::f64> px(instruments);
  for (atx::usize j = 0; j < instruments; ++j) {
    px[j] = 20.0 + next() * 80.0;
  }
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      const atx::usize i = d * instruments + j;
      const atx::f64 prev = px[j];
      px[j] = prev * (1.0 + (next() - 0.5) * 0.04);
      const atx::f64 spread = px[j] * (0.002 + next() * 0.02);
      cols[kC][i] = px[j];
      cols[kO][i] = prev * (1.0 + (next() - 0.5) * 0.01);
      cols[kH][i] = std::fmax(px[j], cols[kO][i]) + spread;
      cols[kL][i] = std::fmin(px[j], cols[kO][i]) - spread;
      cols[kV][i] = 1.0e5 + next() * 9.0e6;
      cols[kR][i] = d == 0 ? std::nan("") : px[j] / prev - 1.0;
      cols[kCap][i] = px[j] * (1.0e6 + static_cast<atx::f64>(j) * 1.0e4);
      cols[kSec][i] = static_cast<atx::f64>(j % 11U);
      cols[kInd][i] = static_cast<atx::f64>(j % 37U);
      cols[kSub][i] = static_cast<atx::f64>(j % 71U);
      universe[i] = next() < 0.01 ? std::uint8_t{0} : std::uint8_t{1};
    }
  }
  std::vector<std::string> names = {"close",   "open", "high",           "low",
                                    "volume",  "returns", "cap",         "IndClass.sector",
                                    "IndClass.industry", "IndClass.subindustry"};
  return datafields::with_datafields(dates, instruments, std::move(names), std::move(cols),
                                     std::move(universe), kWq101AdvWindows,
                                     VwapRule::AdjustedTypicalV1); // frozen synthetic battery recipe
}

} // namespace atx::engine::alpha
