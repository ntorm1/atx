// vol_breadth_bench — one library call, many underlyings. Proof of BREADTH:
// the self-contained PricerFitter, given only a chain + MarketEnv and NO curve
// config, auto-selects the right curve family per board and serves an accurate
// surface — whether the board is a penny-dense index (SPY) or a sparse, wide-
// spread single name (XOM).
//
// Real Databento OPRA cbbo-1m slices, same 2026-06-05T19:55Z snapshot:
//   * SPY — index, dense strikes, penny-wide markets  => expect ConvexDense.
//   * XOM — single name, fewer strikes, wider markets  => the CurveSelector
//           decides out-of-sample (a dense curve would overfit the noise).
//
// For each symbol it reports the board shape, the auto-selected curve with the
// per-candidate out-of-sample scores, the served price-in-band, and the fit time.
// Skips a symbol whose parquet fixture is absent.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>

#include "atx/vol/api/fitting/pricer_fitter.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"

#include "../tests/support/opra_fixture.hpp"

using namespace atx::vol;

namespace {

double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void run_symbol(const std::string &sym_lc, const std::string &sym_uc) {
  auto board = testkit::load_opra_board(sym_lc, sym_uc);
  if (!board.has_value()) {
    std::printf("\n%s: fixture not found (skipped)\n", sym_uc.c_str());
    return;
  }
  const Underlying &U = board->underlying();
  std::size_t n_strikes = 0;
  for (const auto &c : U.chains) {
    n_strikes += c.n_strikes();
  }

  auto chain = OptionChain::from_frame(board->panel.frame, board->env());
  if (!chain.has_value()) {
    std::printf("\n%s: chain build failed: %s\n", sym_uc.c_str(),
                chain.error().to_string().c_str());
    return;
  }

  PricerConfig cfg;
  cfg.preset = FitPreset::Fast; // cfg.curve unset => auto-select
  PricerFitter fitter{cfg};
  const double t0 = now_ms();
  const auto st = fitter.fit(*chain);
  const double fit_ms = now_ms() - t0;
  if (!st.has_value()) {
    std::printf("\n%s: fit failed: %s\n", sym_uc.c_str(), st.error().to_string().c_str());
    return;
  }

  std::printf("\n== %s ==  spot %.2f  |  %zu expiries  %zu strikes  (%zu legs)\n", sym_uc.c_str(),
              board->spot(), U.chains.size(), n_strikes, chain->size());

  if (fitter.selection().has_value()) {
    const auto &sel = *fitter.selection();
    std::printf("  auto-selected curve: %s   (fit %.0f ms)\n", to_string(sel.chosen.kind), fit_ms);
    for (const auto &s : sel.scores) {
      std::printf("    %-13s  OOS in-band %6.2f%%   vw %6.2f%%   avg-dof %5.1f   "
                  "(%zu held-out, %zu slices)\n",
                  to_string(s.kind), 100.0 * s.oos_in_band, 100.0 * s.oos_vw,
                  s.n_slices ? static_cast<double>(s.dof_sum) / static_cast<double>(s.n_slices)
                             : 0.0,
                  s.n_holdout, s.n_slices);
    }
  }
  if (fitter.decision().has_value() && !fitter.selection().has_value()) {
    std::printf("  policy profile=%u curve=%s preset=%u (direct)\n",
                static_cast<unsigned>(fitter.decision()->profile.kind),
                to_string(fitter.decision()->curve.kind),
                static_cast<unsigned>(fitter.decision()->preset));
  }

  const auto sc = testkit::price_in_band(fitter.surface()->session(), U, board->spot(), board->r);
  std::printf("  served price-in-band: pxCLN %.2f%% (%zu/%zu)   pxALL %.2f%%\n", sc.px_clean,
              sc.n_clean_in, sc.n_clean, sc.px_all);
}

} // namespace

int main() {
  std::printf("vol_breadth_bench — auto-select curve per underlying (real OPRA, "
              "2026-06-05T19:55Z)\n");
  run_symbol("spy", "SPY");
  run_symbol("xom", "XOM");
  std::printf("\nOne PricerFitter, no per-symbol config: the library picks the curve that "
              "generalizes best out-of-sample for each board.\n");
  return 0;
}
