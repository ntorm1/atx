// risk_model_real_scorecard_bench.cpp — L7: REAL-DATA risk-model validation harness.
//
// Not a timing bench: BM_L7RealScorecard runs once and writes the walk-forward
// validation scorecards (model_validation.hpp) of the candidate hybrid-model configs
// fitted on a real equity panel. It is inert (SkipWithError) unless both
//   ATX_L7_REAL_DIR  — a directory written by bench/tools/l7_panel_to_raw.py
//   ATX_L7_OUT_DIR   — where l7_scorecards.json is written
// are set. Run from the Release bench build only, e.g.
//   atx-engine-bench.exe --benchmark_filter=BM_L7RealScorecard
//
// Inputs are PRICE styles only (the context panels carry no fundamentals): Size,
// Volatility, Liquidity, ResidVol, STReversal, the market intercept and sector
// dummies.
//
// Point in time. Caps are PER ROW (cap_tn.f64 -> build_panel_series_pit_caps): style
// row t's Size exposure and the date-t regression weights use only the cap known at
// that exposure row. The sector of a name is its FIRST in-universe sector id inside
// the series span (oldest row first). A name enters a regression or a model only at
// rows where it is in the universe, and those rows are never older than that first
// observation, so no sector value from a later date is ever used. (An earlier revision
// used each name's LAST cap and sector. That leaked the scored return into Size and
// the weights; its 2014_t1000 scorecard is superseded.)

#include <cmath>
#include <cstdio>  // std::fprintf
#include <cstdlib> // std::getenv, free
#include <cstddef> // std::size_t
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/combine/cov_targets.hpp"
#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/risk/hybrid_factor_model.hpp"
#include "atx/engine/risk/model_validation.hpp"

namespace atx_bench_l7_riskmodel_real {

using atx::f64;
using atx::u32;
using atx::usize;
using namespace atx::engine::risk; // NOLINT(google-build-using-namespace) bench-local

// Environment variable or "" (MSVC CRT: _dupenv_s, the non-deprecated form).
std::string env_or_empty(const char *name) {
  std::string v;
#if defined(_MSC_VER) || defined(_WIN32)
  char *e = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&e, &len, name) == 0 && e != nullptr) {
    v = e;
  }
  free(e); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s heap copy
#else
  if (const char *e = std::getenv(name); e != nullptr) { // NOLINT(concurrency-mt-unsafe)
    v = e;
  }
#endif
  return v;
}

template <typename T> std::vector<T> read_raw(const std::string &path, usize count) {
  std::vector<T> out(count);
  std::ifstream in(path, std::ios::binary);
  // SAFETY: out holds exactly `count` trivially-copyable T; the read is bounded.
  if (!in || !in.read(reinterpret_cast<char *>(out.data()), // NOLINT(*-reinterpret-cast)
                      static_cast<std::streamsize>(count * sizeof(T)))) {
    out.clear();
  }
  return out;
}

// Ring-buffer backing for a PanelView over date-ascending raw arrays: the newest
// `rows` dates ending at (exclusive) `end`. Physical rows run oldest-first so that
// head == rows-1 (the PanelView addressing contract; mirrors stage_riskmodel.cpp).
struct RingPanel {
  usize n = 0U;
  usize rows = 0U;
  usize cap = 1U;
  usize words = 0U;
  std::vector<atx::engine::InstrumentId> universe;
  std::vector<f64> fields;
  std::vector<atx::u64> mask;

  RingPanel(const std::vector<f64> &close, const std::vector<f64> &vol, usize n_inst, usize end,
            usize n_rows)
      : n(n_inst), rows(n_rows) {
    while (cap < rows) {
      cap <<= 1U;
    }
    words = (n + 63U) / 64U;
    for (usize i = 0; i < n; ++i) {
      universe.push_back(atx::engine::InstrumentId{static_cast<u32>(i + 1U)});
    }
    fields.assign(atx::engine::kPanelFieldCount * cap * n, std::numeric_limits<f64>::quiet_NaN());
    mask.assign(cap * words, 0ULL);
    for (usize r = 0; r < rows; ++r) {
      const usize date = end - 1U - r;
      const usize phys = rows - 1U - r;
      for (usize i = 0; i < n; ++i) {
        const f64 c = close[date * n + i];
        const f64 v = vol[date * n + i];
        for (const auto f : {atx::engine::PanelField::Open, atx::engine::PanelField::High,
                             atx::engine::PanelField::Low, atx::engine::PanelField::Close}) {
          fields[static_cast<usize>(f) * cap * n + phys * n + i] = c;
        }
        fields[static_cast<usize>(atx::engine::PanelField::Volume) * cap * n + phys * n + i] = v;
        if (!std::isnan(c)) {
          mask[phys * words + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }
  [[nodiscard]] atx::engine::PanelView view() const noexcept {
    return atx::engine::PanelView{fields.data(), mask.data(),
                                  std::span<const atx::engine::InstrumentId>{universe},
                                  cap, rows - 1U, rows, words};
  }
};

void BM_L7RealScorecard(benchmark::State &state) {
  const std::string dir = env_or_empty("ATX_L7_REAL_DIR");
  const std::string out_dir = env_or_empty("ATX_L7_OUT_DIR");
  if (dir.empty() || out_dir.empty()) {
    state.SkipWithError("set ATX_L7_REAL_DIR and ATX_L7_OUT_DIR to run the real-data scorecard");
    return;
  }
  usize t_all = 0U;
  usize n = 0U;
  {
    std::ifstream meta(dir + "/meta.txt");
    meta >> t_all >> n;
  }
  const auto close = read_raw<f64>(dir + "/close.f64", t_all * n);
  const auto vol = read_raw<f64>(dir + "/volume.f64", t_all * n);
  const auto cap_tn = read_raw<f64>(dir + "/cap_tn.f64", t_all * n);
  const auto sector_tn = read_raw<f64>(dir + "/sector_tn.f64", t_all * n);
  const auto days = read_raw<atx::i64>(dir + "/dates.i64", t_all);
  if (t_all == 0U || close.empty() || vol.empty() || cap_tn.empty() || sector_tn.empty() ||
      days.empty()) {
    state.SkipWithError("cannot read the raw panel");
    return;
  }
  const usize window = static_cast<usize>(state.range(0));  // estimation window W
  const usize periods = static_cast<usize>(state.range(1)); // forecasts scored P
  constexpr usize kLookback = 64U;                           // ResidVol 60 (+ slack)
  const usize series_window = window + periods + 1U;
  const usize rows = series_window + 1U + kLookback;
  if (rows > t_all) {
    state.SkipWithError("panel too short for window + periods + lookback");
    return;
  }
  // Newest-first (series_window+1)×N cap rows: row r = date t_all−1−r (NaN ⇒ the name
  // has no cap there and drops out of that row's Size exposure and regression).
  std::vector<f64> cap_rows((series_window + 1U) * n);
  for (usize r = 0; r <= series_window; ++r) {
    for (usize i = 0; i < n; ++i) {
      cap_rows[r * n + i] = cap_tn[(t_all - 1U - r) * n + i];
    }
  }
  // Static sector = first in-universe observation inside the series span (see header).
  constexpr u32 kUnknownSector = 0xFFFFFFFEU;
  std::vector<u32> sector(n, kUnknownSector);
  for (usize i = 0; i < n; ++i) {
    for (usize r = series_window + 1U; r-- > 0U;) {
      const f64 v = sector_tn[(t_all - 1U - r) * n + i];
      if (std::isfinite(v)) {
        sector[i] = static_cast<u32>(v);
        break;
      }
    }
  }
  FundamentalCfg fcfg;
  for (const StyleFactor sf : {StyleFactor::Size, StyleFactor::Volatility, StyleFactor::Liquidity,
                               StyleFactor::ResidVol, StyleFactor::STReversal,
                               StyleFactor::Market}) {
    fcfg.mask.set(static_cast<usize>(sf));
  }
  fcfg.sector_factors = true;
  std::vector<atx::i64> row_dates(rows);
  for (usize r = 0; r < rows; ++r) {
    row_dates[r] = days[t_all - 1U - r];
  }
  std::string json;
  for (auto _ : state) {
    const RingPanel ring(close, vol, n, t_all, rows);
    auto ps = build_panel_series_pit_caps(ring.view(), nullptr, fcfg, row_dates, cap_rows, sector,
                                          series_window);
    if (!ps) {
      state.SkipWithError(ps.error().message().c_str());
      return;
    }
    HybridCfg fund;
    fund.window = window;
    fund.select = StatFactorSelect::Fixed;
    fund.n_stat_fixed = 0U;
    HybridCfg hyb = fund;
    hyb.select = StatFactorSelect::BaiNgIc2;
    HybridCfg hyb_mp = fund;
    hyb_mp.select = StatFactorSelect::MarchenkoPastur;
    HybridCfg hyb_ewma = hyb;
    hyb_ewma.vol_halflife = 42U;
    hyb_ewma.corr_halflife = 126U;
    hyb_ewma.spec_halflife = 42U;
    HybridCfg hyb_lw = hyb;
    hyb_lw.spec_halflife = 42U;
    hyb_lw.factor_cov_target = atx::engine::combine::CovTarget::LwNonlinear;
    const std::vector<std::pair<std::string, HybridCfg>> configs = {
        {"fundamental_price_styles", fund},
        {"hybrid_baing", hyb},
        {"hybrid_mp", hyb_mp},
        {"hybrid_baing_ewma", hyb_ewma},
        {"hybrid_baing_lw2020_spec_ewma", hyb_lw}};
    json = "{\"panel_dir\":" + json_quote(dir) + ",\"pit_caps\":true,\"n_assets\":" +
           std::to_string(n) +
           ",\"window\":" + std::to_string(window) + ",\"periods\":" + std::to_string(periods) +
           ",\"newest_day\":" + std::to_string(row_dates[0]) + ",\"scorecards\":[";
    bool first = true;
    for (const auto &[label, hc] : configs) {
      ValidationCfg vc;
      vc.label = label;
      vc.first_as_of = 1U;
      vc.n_periods = periods;
      vc.n_random = 100U;
      vc.n_optimized = 20U;
      const auto sc = validate_risk_model(hybrid_model_factory(ps->returns, ps->exposures, hc),
                                          ps->returns, vc);
      if (!sc) {
        std::fprintf(stderr, "%s: %s\n", label.c_str(), sc.error().message().c_str());
        continue;
      }
      std::fprintf(stderr,
                   "%-30s K=%5.1f EW b=%.3f MinVar b=%.3f rand b=%.3f inband=%.2f opt b=%.3f "
                   "opt inband=%.2f asset b=%.3f MRAD=%.3f excl=%.1f\n",
                   label.c_str(), sc->mean_factors, sc->equal_weight.bias, sc->min_variance.bias,
                   sc->random_bias_mean, sc->random_in_band_frac, sc->optimized_bias_mean,
                   sc->optimized_in_band_frac, sc->asset_bias_mean, sc->asset_mrad,
                   sc->mean_excluded);
      json += (first ? "" : ",") + sc->to_json();
      first = false;
    }
    json += "]}";
  }
  const std::string out = out_dir + "/l7_scorecards.json";
  std::ofstream(out, std::ios::binary) << json << "\n";
}
BENCHMARK(BM_L7RealScorecard)->Args({252, 120})->Iterations(1)->Unit(benchmark::kSecond);

} // namespace atx_bench_l7_riskmodel_real
