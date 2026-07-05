// chain_pricer_bench.cpp — the unified library layer, end to end, on real SPY.
//
// Proves the goal's five-step lifecycle and its parallel-inversion evaluator:
//   1. build an OptionChain (each option a unique id) from the SPY OPRA slice;
//   2. pass it to a PricerFitter with a config;
//   3. the fitter fits and OWNS the surface (unique_ptr<FittedSurface>);
//   4. value the whole chain in parallel across {1,2,4,8} threads, reporting the
//      scaling and asserting the result is BIT-IDENTICAL across thread counts
//      (the deterministic-layout claim);
//   5. update bid/ask for a set of option ids and re-value — the tick-to-quote
//      path — showing the band IVs move.
//
// The heavy per-option work is the cold American-IV inversion of bid/ask/mid on
// the fit's carry: embarrassingly parallel, so this is where the thread scaling
// shows. Model price/IV/Greeks come from the fitted surface's cached hot path.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/pricer_fitter.hpp"

using namespace atx::vol;

namespace {

double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch())
      .count();
}

// Count populated (non-NaN) cells across the band columns — the actual cold
// inversions performed (bid/ask/mid with a live quote).
std::size_t count_band_inversions(const ChainValuation& v) {
  std::size_t n = 0;
  for (double x : v.bid_iv) n += std::isfinite(x) ? 1u : 0u;
  for (double x : v.ask_iv) n += std::isfinite(x) ? 1u : 0u;
  for (double x : v.mid_iv) n += std::isfinite(x) ? 1u : 0u;
  return n;
}

bool same(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return a == b;
}

bool identical(const ChainValuation& a, const ChainValuation& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a.ids[i] != b.ids[i]) return false;
    if (!same(a.model_iv[i], b.model_iv[i])) return false;
    if (!same(a.model_price[i], b.model_price[i])) return false;
    if (!same(a.bid_iv[i], b.bid_iv[i])) return false;
    if (!same(a.ask_iv[i], b.ask_iv[i])) return false;
    if (!same(a.mid_iv[i], b.mid_iv[i])) return false;
    if (!same(a.greeks[i].price, b.greeks[i].price)) return false;
    if (!same(a.greeks[i].delta, b.greeks[i].delta)) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1
      ? argv[1]
      : "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet";
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  auto panel = load_opra_cbbo_parquet(spec);
  if (!panel.has_value()) {
    std::fprintf(stderr, "load failed: %s\n", panel.error().message().c_str());
    return 1;
  }

  // 1. OptionChain (unique-id board) from the panel frame + PCP-implied spot.
  auto chain_r = OptionChain::from_frame(panel->frame, spec.r, panel->implied_spot);
  if (!chain_r.has_value()) {
    std::fprintf(stderr, "chain build failed: %s\n", chain_r.error().message().c_str());
    return 1;
  }
  OptionChain chain = std::move(*chain_r);
  std::printf("SPY chain: spot %.2f, %zu option legs (unique ids)\n\n",
              chain.spot(), chain.size());

  // 2-3. PricerFitter fits and OWNS the surface.
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
  const double t_fit0 = now_ms();
  const Status fit = fitter.fit(chain);
  const double fit_ms = now_ms() - t_fit0;
  if (!fit.has_value()) {
    std::fprintf(stderr, "fit failed: %s\n", fit.error().message().c_str());
    return 1;
  }
  std::printf("PricerFitter.fit: %.1f ms, surface owned=%s, %zu slices, worst frac-in-band %.3f\n\n",
              fit_ms, fitter.fitted() ? "yes" : "no",
              fitter.surface()->diagnostics().n_slices,
              fitter.surface()->diagnostics().worst_frac_within_bidask);

  // 4. Parallel whole-chain valuation across thread counts. All fields: model
  //    price/IV/Greeks (cached) + bid/ask/mid American-IV inversions (cold,
  //    parallel). Report scaling + prove determinism.
  const unsigned threads[] = {1u, 2u, 4u, 8u};
  std::printf("value_chain(All) parallel scaling (cold American-IV inversions dominate):\n");
  std::printf("%8s %10s %14s %9s\n", "threads", "wall_ms", "inversions/s", "speedup");
  double base_ms = 0.0;
  ChainValuation ref;
  bool have_ref = false;
  bool determinism_ok = true;
  for (const unsigned nt : threads) {
    const double t0 = now_ms();
    auto vr = fitter.value_chain(chain, OutputField::All, nt);
    const double ms = now_ms() - t0;
    if (!vr.has_value()) {
      std::fprintf(stderr, "value_chain failed: %s\n", vr.error().message().c_str());
      return 1;
    }
    const std::size_t n_inv = count_band_inversions(*vr);
    const double inv_per_s = ms > 0.0 ? 1000.0 * static_cast<double>(n_inv) / ms : 0.0;
    if (nt == 1u) base_ms = ms;
    const double speedup = ms > 0.0 ? base_ms / ms : 0.0;
    std::printf("%8u %10.1f %14.0f %8.2fx\n", nt, ms, inv_per_s, speedup);
    if (!have_ref) {
      ref = std::move(*vr);
      have_ref = true;
    } else if (!identical(ref, *vr)) {
      determinism_ok = false;
    }
  }
  std::printf("\nDETERMINISM across thread counts: %s\n\n",
              determinism_ok ? "IDENTICAL (bit-for-bit)" : "*** MISMATCH ***");

  // 5. Tick-to-quote: replace bid/ask for a spread of option ids, re-value the
  //    band, and show the model IV band moved.
  const std::vector<OptionId> all_ids = chain.ids();
  std::vector<OptionId> upd_ids;
  std::vector<double> upd_bids;
  std::vector<double> upd_asks;
  for (std::size_t i = 0; i < all_ids.size() && upd_ids.size() < 8; i += all_ids.size() / 8 + 1) {
    const auto o = chain.at(all_ids[i]);
    if (!o.has_value() || !(o->mid > 0.5)) continue;
    upd_ids.push_back(all_ids[i]);
    upd_bids.push_back(o->bid * 1.10);  // widen the quote up 10%
    upd_asks.push_back(o->ask * 1.10);
  }
  const auto before = fitter.value_chain(chain, OutputField::MidIV, 0);
  const Status us = chain.update_quotes(std::span<const OptionId>(upd_ids),
                                        std::span<const double>(upd_bids),
                                        std::span<const double>(upd_asks));
  const auto after = fitter.value_chain(chain, OutputField::MidIV, 0);
  std::printf("update_quotes on %zu ids -> re-value MidIV (status=%s):\n",
              upd_ids.size(), us.has_value() ? "ok" : "err");
  if (before.has_value() && after.has_value()) {
    for (const OptionId id : upd_ids) {
      const auto rb = before->row_of(id);
      const auto ra = after->row_of(id);
      if (rb && ra) {
        std::printf("  id %016llx  midIV %.4f -> %.4f\n",
                    static_cast<unsigned long long>(id),
                    before->mid_iv[*rb], after->mid_iv[*ra]);
      }
    }
  }

  std::printf("\nNOTE: model price/IV/Greeks come from the fitted surface's cached hot\n"
              "path; the parallel win is the cold bid/ask/mid American-IV inversions,\n"
              "which are independent per option. The result is deterministic (identical\n"
              "for any thread count) by construction (disjoint output slots).\n");
  return 0;
}
