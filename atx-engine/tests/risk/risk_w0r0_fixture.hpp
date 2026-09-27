#pragma once

// risk_w0r0_fixture.hpp — shared synthetic-panel fixture for the W0-R0 risk-estimator
// tests (risk_w0r0_*_test.cpp). Header-only; no real data.
//
// StorePanel owns a PanelView's ring storage for an n_rows × n_inst newest-first grid
// (storage row 0 = the NEWEST date). view_at(k) returns the view a live RollingPanel
// would hand out at the date of storage row k: rows NEWER than k (the "future" relative
// to that date) physically exist in the same buffer but are outside the view — exactly
// the situation a causality harness must prove invisible.

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)

namespace atx_test_w0_r0_fixture {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;

inline constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

using Grid = std::vector<std::vector<f64>>; // [row][instrument], row 0 = newest

class StorePanel {
public:
  StorePanel(const Grid &close, const Grid &volume)
      : n_rows_{close.size()}, n_inst_{close.empty() ? 0U : close[0].size()},
        cap_{pow2_ceil(close.size())}, mask_words_{(n_inst_ + 63U) / 64U} {
    universe_.reserve(n_inst_);
    for (usize i = 0; i < n_inst_; ++i) {
      universe_.push_back(atx::core::domain::Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(atx::engine::kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical ring row
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        const f64 v = volume[r][i];
        set(atx::engine::PanelField::Open, phys, i, c);
        set(atx::engine::PanelField::High, phys, i, c);
        set(atx::engine::PanelField::Low, phys, i, c);
        set(atx::engine::PanelField::Close, phys, i, c);
        set(atx::engine::PanelField::Volume, phys, i, v);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  // The view whose row 0 is storage row k (k newer rows are "the future").
  [[nodiscard]] atx::engine::PanelView view_at(usize k) const noexcept {
    return atx::engine::PanelView{fields_.data(),
                                  mask_.data(),
                                  std::span<const atx::engine::InstrumentId>{universe_},
                                  cap_,
                                  (n_rows_ - 1U) - k,
                                  n_rows_ - k,
                                  mask_words_};
  }
  [[nodiscard]] atx::engine::PanelView view() const noexcept { return view_at(0U); }
  [[nodiscard]] usize rows() const noexcept { return n_rows_; }
  [[nodiscard]] usize instruments() const noexcept { return n_inst_; }

private:
  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }
  void set(atx::engine::PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<atx::engine::InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<u64> mask_;
};

// Deterministic, platform-independent RNG (splitmix64 + Box-Muller) so every measured
// number in the lane report reproduces bit-for-bit on any toolchain.
class Rng {
public:
  explicit Rng(u64 seed) noexcept : s_{seed} {}
  [[nodiscard]] u64 next() noexcept {
    u64 z = (s_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  [[nodiscard]] f64 uniform() noexcept { // (0, 1)
    return (static_cast<f64>(next() >> 11U) + 0.5) * (1.0 / 9007199254740992.0);
  }
  [[nodiscard]] f64 normal() noexcept {
    const f64 u1 = uniform();
    const f64 u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

private:
  u64 s_;
};

// Closes from a newest-first return grid ret[s][i] (r_s = close(s)/close(s+1) − 1):
// the oldest row is 100, walking forward. NaN returns are not allowed here.
[[nodiscard]] inline Grid closes_from_returns(const Grid &ret) {
  const usize n_rows = ret.size() + 1U;
  const usize n_inst = ret.empty() ? 0U : ret[0].size();
  Grid close(n_rows, std::vector<f64>(n_inst, 100.0));
  for (usize s = ret.size(); s-- > 0U;) {
    for (usize i = 0; i < n_inst; ++i) {
      close[s][i] = close[s + 1U][i] * (1.0 + ret[s][i]);
    }
  }
  return close;
}

// Mean and t-statistic (mean / (sd/√n), sample sd) of a series.
struct MeanT {
  f64 mean = 0.0;
  f64 t = 0.0;
};
[[nodiscard]] inline MeanT mean_t(const std::vector<f64> &xs) {
  MeanT out{};
  const usize n = xs.size();
  if (n < 2U) {
    return out;
  }
  f64 sum = 0.0;
  for (const f64 v : xs) {
    sum += v;
  }
  out.mean = sum / static_cast<f64>(n);
  f64 ss = 0.0;
  for (const f64 v : xs) {
    ss += (v - out.mean) * (v - out.mean);
  }
  const f64 sd = std::sqrt(ss / static_cast<f64>(n - 1U));
  out.t = (sd > 0.0) ? out.mean / (sd / std::sqrt(static_cast<f64>(n))) : 0.0;
  return out;
}

} // namespace atx_test_w0_r0_fixture
