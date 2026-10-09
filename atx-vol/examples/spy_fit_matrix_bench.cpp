// Cold-start SPY fit benchmark over the ten-slice real-OPRA accuracy corpus.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "../tests/support/spy_fit_fixture.hpp"
#include "atx/vol/api/core/chain.hpp"
#include "atx/vol/api/fitting/pricer_fitter.hpp"

namespace {

using namespace atx::vol;
using atx::vol::testkit::kSpyFitFixtures;
using atx::vol::testkit::load_spy_fit_fixture;
using atx::vol::testkit::price_in_band;

double percentile(std::vector<double> xs, double p) {
  if (xs.empty()) {
    return 0.0;
  }
  std::sort(xs.begin(), xs.end());
  const double pos = p * static_cast<double>(xs.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(pos));
  const auto hi = static_cast<std::size_t>(std::ceil(pos));
  const double a = pos - static_cast<double>(lo);
  return xs[lo] * (1.0 - a) + xs[hi] * a;
}

PricerConfig config_for(std::string_view mode) {
  PricerConfig cfg;
  if (mode == "auto") {
    return cfg;
  }
  cfg.preset = FitPreset::Hft;
  if (mode == "linear-hft" || mode == "linear-hft-nocap" || mode == "linear-hft-cap128" ||
      mode == "linear-hft-cap160") {
    CurveConfig curve;
    curve.kind = VolCurveKind::LinearVariance;
    cfg.curve = curve;
    if (mode == "linear-hft-nocap") {
      cfg.max_obs_per_slice = 0;
    } else if (mode == "linear-hft-cap128") {
      cfg.max_obs_per_slice = 128;
    } else if (mode == "linear-hft-cap160") {
      cfg.max_obs_per_slice = 160;
    }
  } else if (mode == "hft-nocap") {
    cfg.max_obs_per_slice = 0;
  } else if (mode == "hft-cachefit") {
    cfg.use_correction_cache = true;
    cfg.use_deam_cache_for_fit = true;
  } else if (mode == "fast-full" || mode == "fast-cap96" || mode == "linear-full" ||
             mode == "linear-fast-cap96") {
    cfg.preset = FitPreset::Fast;
    CurveConfig curve;
    curve.kind = (mode == "linear-full" || mode == "linear-fast-cap96")
                     ? VolCurveKind::LinearVariance
                     : VolCurveKind::ConvexDense;
    curve.convex.node_cap = 40;
    cfg.curve = curve;
    cfg.use_correction_cache = false;
    cfg.score_parity = false;
    cfg.enforce_calendar_floor = false;
    if (mode == "fast-cap96" || mode == "linear-fast-cap96") {
      cfg.max_obs_per_slice = 96;
    }
  }
  return cfg;
}

} // namespace

int main(int argc, char **argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "hft";
  const int repeats = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5;
  if (mode != "auto" && mode != "hft" && mode != "hft-nocap" && mode != "hft-cachefit" &&
      mode != "fast-full" && mode != "fast-cap96" && mode != "linear-full" &&
      mode != "linear-hft" && mode != "linear-hft-nocap" && mode != "linear-hft-cap128" &&
      mode != "linear-hft-cap160" && mode != "linear-fast-cap96") {
    std::fprintf(stderr, "usage: spy_fit_matrix_bench "
                         "[auto|hft|hft-nocap|hft-cachefit|fast-full|fast-cap96|linear-full|"
                         "linear-hft|linear-hft-nocap|linear-hft-cap128|"
                         "linear-hft-cap160|linear-fast-cap96] "
                         "[repeats]\n");
    return 2;
  }
  PricerConfig config = config_for(mode);
  if (argc > 3) {
    config.max_obs_per_slice = static_cast<std::uint32_t>(std::max(0, std::atoi(argv[3])));
  }
  if (argc > 4) {
    config.max_otm_shortcut_premium_spread_frac = std::atof(argv[4]);
  }
  std::vector<double> all_fit_ms;
  double min_accuracy = 100.0;
  std::size_t loaded = 0;
  bool quality_ok = true;

  const unsigned effective_cap =
      config.max_obs_per_slice.has_value()
          ? static_cast<unsigned>(*config.max_obs_per_slice)
          : ((mode == "auto" || config.preset == FitPreset::Hft) ? 48u : 0u);
  const double effective_shortcut = config.max_otm_shortcut_premium_spread_frac.value_or(
      (mode == "auto" || config.preset == FitPreset::Hft) ? 0.50 : 0.0);
  std::printf("mode=%.*s repeats=%d cap=%u shortcut=%.2f\n", static_cast<int>(mode.size()),
              mode.data(), repeats, effective_cap, effective_shortcut);
  std::printf("%-13s %-24s %8s %8s %8s %8s %10s\n", "fixture", "regime", "legs", "p50-ms",
              "best-ms", "pxCLN", "clean in/n");
  for (const auto &fixture : kSpyFitFixtures) {
    auto board = load_spy_fit_fixture(fixture);
    if (!board.has_value()) {
      std::printf("%-13s %-24s MISSING\n", fixture.id, fixture.regime);
      continue;
    }
    ++loaded;
    auto chain = OptionChain::from_frame(board->panel.frame, board->env());
    if (!chain.has_value()) {
      std::fprintf(stderr, "%s: chain build failed: %s\n", fixture.id,
                   chain.error().to_string().c_str());
      return 1;
    }

    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(repeats));
    PricerFitter scored{config};
    for (int rep = 0; rep < repeats; ++rep) {
      PricerFitter fitter{config};
      const auto t0 = std::chrono::steady_clock::now();
      const Status fit = fitter.fit(*chain);
      const auto t1 = std::chrono::steady_clock::now();
      if (!fit.has_value()) {
        std::fprintf(stderr, "%s: fit failed: %s\n", fixture.id, fit.error().to_string().c_str());
        return 1;
      }
      const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      times.push_back(ms);
      all_fit_ms.push_back(ms);
      if (rep + 1 == repeats) {
        scored = std::move(fitter);
      }
    }
    const auto score =
        price_in_band(scored.surface()->session(), chain->underlying(), board->spot(), board->r);
    const double med = percentile(times, 0.50);
    const double best = *std::min_element(times.begin(), times.end());
    min_accuracy = std::min(min_accuracy, score.px_clean);
    quality_ok = quality_ok && score.px_clean >= 98.0;
    std::printf("%-13s %-24s %8zu %8.2f %8.2f %7.2f%% %5zu/%-4zu\n", fixture.id, fixture.regime,
                chain->size(), med, best, score.px_clean, score.n_clean_in, score.n_clean);
  }

  if (loaded == 0) {
    std::fprintf(stderr, "No SPY fixtures found under data/spy_fit_slices\n");
    return 2;
  }
  std::printf("\nfit distribution (%zu cold fits, %zu fixtures): "
              "p50=%.2fms p95=%.2fms max=%.2fms; worst pxCLN=%.2f%%\n",
              all_fit_ms.size(), loaded, percentile(all_fit_ms, 0.50), percentile(all_fit_ms, 0.95),
              *std::max_element(all_fit_ms.begin(), all_fit_ms.end()), min_accuracy);
  std::printf("98%% accuracy gate: %s\n", quality_ok ? "PASS" : "FAIL");
  return loaded == kSpyFitFixtures.size() && quality_ok ? 0 : 3;
}
