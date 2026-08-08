#include "atx/vol/realized_vol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

// 0 < low <= min(open,close) <= max(open,close) <= high, all finite.
[[nodiscard]] bool bar_is_valid(const OhlcBar &b) noexcept {
  if (!std::isfinite(b.open) || !std::isfinite(b.high) || !std::isfinite(b.low) ||
      !std::isfinite(b.close))
    return false;
  if (!(b.low > 0.0))
    return false;
  const double mn = std::min(b.open, b.close);
  const double mx = std::max(b.open, b.close);
  return b.low <= mn && mn <= mx && mx <= b.high;
}

[[nodiscard]] Status validate_bars(std::span<const OhlcBar> bars) {
  for (const auto &b : bars) {
    if (!bar_is_valid(b))
      return Err(ErrorCode::InvalidArgument,
                 "realized_vol: bar violates 0 < low <= min(o,c) <= max(o,c) <= high");
  }
  return Ok();
}

// Per-bar log-return decomposition (Yang-Zhang / Garman-Klass / Rogers-
// Satchell terms), relative to the PRIOR bar's close for the overnight term:
//   o = ln(O_t / C_{t-1})   overnight (close-to-open) return
//   u = ln(H_t / O_t)       up-wick from the open
//   d = ln(L_t / O_t)       down-wick from the open (<= 0)
//   c = ln(C_t / O_t)       open-to-close return
struct BarTerms {
  double o, u, d, c;
};

} // namespace

Result<double> realized_vol(std::span<const OhlcBar> bars, RvEstimator est,
                            double annualization) {
  if (bars.size() < 2 || !(annualization > 0.0))
    return Err(ErrorCode::InvalidArgument, "realized_vol: need >=2 bars, annualization>0");
  const std::size_t n = bars.size() - 1; // return terms use the previous close
  if (est == RvEstimator::YangZhang && n < 3)
    return Err(ErrorCode::InvalidArgument, "realized_vol: yang-zhang needs >=3 terms");
  ATX_TRY_VOID(validate_bars(bars));

  double sum = 0.0, sum_o = 0.0, sum_o2 = 0.0, sum_c = 0.0, sum_c2 = 0.0, sum_rs = 0.0;
  for (std::size_t i = 1; i < bars.size(); ++i) {
    const BarTerms t{std::log(bars[i].open / bars[i - 1].close),
                     std::log(bars[i].high / bars[i].open),
                     std::log(bars[i].low / bars[i].open),
                     std::log(bars[i].close / bars[i].open)};
    switch (est) {
    case RvEstimator::CloseToClose: {
      const double r = t.o + t.c;
      sum += r * r;
    } break;
    case RvEstimator::Parkinson: {
      const double hl = t.u - t.d;
      sum += hl * hl / (4.0 * std::log(2.0));
    } break;
    case RvEstimator::GarmanKlass: {
      const double hl = t.u - t.d;
      sum += 0.5 * hl * hl - (2.0 * std::log(2.0) - 1.0) * t.c * t.c;
    } break;
    case RvEstimator::RogersSatchell:
      sum += t.u * (t.u - t.c) + t.d * (t.d - t.c);
      break;
    case RvEstimator::YangZhang:
      sum_o += t.o;
      sum_o2 += t.o * t.o;
      sum_c += t.c;
      sum_c2 += t.c * t.c;
      sum_rs += t.u * (t.u - t.c) + t.d * (t.d - t.c);
      break;
    }
  }

  double var_per_bar = 0.0;
  if (est == RvEstimator::YangZhang) {
    const double nn = static_cast<double>(n);
    const double vo = (sum_o2 - sum_o * sum_o / nn) / (nn - 1.0);
    const double vc = (sum_c2 - sum_c * sum_c / nn) / (nn - 1.0);
    const double vrs = sum_rs / nn;
    const double k = 0.34 / (1.34 + (nn + 1.0) / (nn - 1.0));
    var_per_bar = vo + k * vc + (1.0 - k) * vrs;
  } else {
    var_per_bar = sum / static_cast<double>(n);
  }
  if (!(var_per_bar >= 0.0) || !std::isfinite(var_per_bar))
    return Err(ErrorCode::InvalidArgument, "realized_vol: non-finite variance");
  return Ok(std::sqrt(var_per_bar * annualization));
}

Result<RvPanel> realized_vol_panel(std::span<const OhlcBar> bars, RvEstimator est,
                                   double annualization) {
  if (bars.empty() || !(annualization > 0.0))
    return Err(ErrorCode::InvalidArgument,
               "realized_vol_panel: need >=1 bar, annualization>0");

  RvPanel panel{};
  for (std::size_t slot = 0; slot < panel.window.size(); ++slot) {
    // Trailing window ending at the last bar; a window longer than the
    // available history falls back to the whole span.
    const std::size_t want = panel.window[slot];
    const std::size_t len = std::min<std::size_t>(want, bars.size());
    if (len < 2) {
      // Fewer than 2 bars in the slice cannot form a return term: flag the
      // slot rather than failing the whole panel (plan note).
      panel.vol[slot] = std::numeric_limits<double>::quiet_NaN();
      continue;
    }
    const auto slice = bars.subspan(bars.size() - len, len);
    ATX_TRY(const double v, realized_vol(slice, est, annualization));
    panel.vol[slot] = v;
  }
  return Ok(panel);
}

} // namespace atx::vol
