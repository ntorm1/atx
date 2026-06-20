// atx-impl — single hand-crafted alpha, capacity-aware backtest.
//
// GOAL (research): hand-build ONE alpha-DSL expression (no search engine) that,
// on the real ORATS liquid panel, clears all of:
//   * Sharpe > 1
//   * daily turnover < 30%   (mean over rebalance days of sum_i |w_i(d)-w_i(d-1)|)
//   * high capacity          (stocks only, close > $1, 20-day dollar ADV >= $50M)
//   * economically sensible  (a documented anomaly, not a degenerate fit)
//
// This TU evaluates a curated set of economically-motivated candidate alphas in a
// SINGLE panel load and prints Sharpe/turnover/capacity diagnostics for each, so
// the slow-signal / capacity trade-off is visible. The book is the same daily
// dollar-neutral, gross-1, L1-normalized construction the Alpha101 harness uses
// (weighting_score) — but the universe each day is the CAPACITY universe
// (in_universe AND close>min_price AND adv{W} >= min_adv), not the raw panel
// universe. All conditioning (rank/zscore) lives IN the DSL expression, so the
// expression alone fully determines the signal.
//
// Real numbers require ATX_ALPHA101_PANEL (the serialized liquid panel). On the
// synthetic random-walk panel the Sharpes are ~0 by construction, so the test
// GTEST_SKIPs the verdict there and only proves the path runs.

#define _CRT_SECURE_NO_WARNINGS 1

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/eval/perf_metrics.hpp"

#include "serialize_panel.hpp"   // atx::impl::read_panel
#include "alpha101_support.hpp"  // augment_for_alpha101, make_synth_orats_panel, FixtureAlpha

namespace atxtest_single_alpha_capacity {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::SignalSet;

inline constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Process-lifetime Library: parsed Asts borrow OpSig pointers from it.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// ---- Candidate alphas (economically motivated; slow unless noted) ----------
struct Candidate {
  const char *name;
  const char *dsl;
  const char *thesis; // one-line economic rationale
};

// Each is dollar-neutral cross-sectional. rank() inside the DSL keeps weights
// bounded and the book persistent (rank of a slow quantity moves slowly ->
// low turnover). The two *_FAST entries are controls: real anomalies but fast
// signals, included to show the turnover gate doing its job.
inline const std::vector<Candidate> &candidates() {
  static const std::vector<Candidate> c = {
      // CONCENTRATION-POWER SWEEP of sector-neutral zscore low-vol. If the
      // Sharpe rises smoothly with the power (and both sample halves agree), the
      // tail premium is real; if it SPIKES at one power, it's an artifact.
      {"lv_z_p1.0", "group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),1.0),sector)",
       "zscore low-vol, linear (p=1.0) — the clean baseline (0.868)"},
      {"lv_z_p1.5", "group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),1.5),sector)",
       "p=1.5"},
      {"lv_z_p2.0", "group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),2.0),sector)",
       "p=2.0"},
      {"lv_z_p2.5", "group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),2.5),sector)",
       "p=2.5"},
      {"lv_z_p3.0", "group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),3.0),sector)",
       "p=3.0 — round-5 winner (1.855)"},
      {"lv_z_p4.0", "group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),4.0),sector)",
       "p=4.0 — even more extreme"},
      // robustness: does the winning recipe hold at a neighbouring vol window?
      {"lv25_z_p3.0", "group_neutralize(signedpower(zscore(-1*ts_std(returns,25)),3.0),sector)",
       "p=3.0 on 25-day vol (window robustness)"},
      {"lv15_z_p3.0", "group_neutralize(signedpower(zscore(-1*ts_std(returns,15)),3.0),sector)",
       "p=3.0 on 15-day vol (window robustness)"},
      // robustness: is sector-neutralization load-bearing or incidental?
      {"lv_z_p3.0_raw", "signedpower(zscore(-1*ts_std(returns,20)),3.0)",
       "p=3.0 WITHOUT sector-neutral (is neutralization needed?)"},
  };
  return c;
}

// ---- Capacity-aware book statistics ---------------------------------------
struct BookStats {
  bool evaluated = false;
  std::string err;
  atx::f64 sharpe = kNaN;
  atx::f64 sharpe_h1 = kNaN; // Sharpe over first half of the sample
  atx::f64 sharpe_h2 = kNaN; // Sharpe over second half (robustness / not-degenerate check)
  atx::f64 turnover = kNaN; // mean over rebalance days of sum_i |w_i(d)-w_i(d-1)|
  atx::f64 ann_ret = kNaN;  // 252 * mean(daily pnl over active days)
  atx::f64 ann_vol = kNaN;  // sqrt(252) * pop std(daily pnl over active days)
  atx::f64 max_dd = kNaN;
  double avg_names = 0.0;   // mean # of nonzero weights per active day
  double finite_frac = 0.0; // mean (finite signal cells / capacity cells) over active days
  int active_days = 0;      // days that produced a non-degenerate book
};

// Build the per-cell capacity universe: in_universe(d,i) AND close>min_price AND
// adv{advWindow} >= min_adv. Computed once, reused across candidates.
[[nodiscard]] std::vector<std::uint8_t>
capacity_universe(const Panel &panel, const std::string &adv_field, atx::f64 min_price,
                  atx::f64 min_adv, atx::usize &kept_cells_out) {
  const atx::usize D = panel.dates();
  const atx::usize I = panel.instruments();
  const auto close = panel.field_all(*panel.field_id("close"));
  auto adv_id = panel.field_id(adv_field);
  const std::span<const atx::f64> adv =
      adv_id ? panel.field_all(*adv_id) : std::span<const atx::f64>{};

  std::vector<std::uint8_t> univ(D * I, 0);
  atx::usize kept = 0;
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize i = 0; i < I; ++i) {
      const atx::usize idx = d * I + i;
      if (!panel.in_universe(d, i)) {
        continue;
      }
      const atx::f64 px = close[idx];
      const atx::f64 dv = (idx < adv.size()) ? adv[idx] : kNaN;
      if (std::isfinite(px) && px > min_price && std::isfinite(dv) && dv >= min_adv) {
        univ[idx] = 1;
        ++kept;
      }
    }
  }
  kept_cells_out = kept;
  return univ;
}

// Daily dollar-neutral, gross-1, L1-normalized book restricted to `univ`.
// Conditioning lives in the DSL; here we only demean + L1-normalize the raw
// signal (Cond::Identity), exactly like the Alpha101 weighting_score path.
[[nodiscard]] BookStats run_book(const SignalSet &ss, const Panel &panel,
                                 const std::vector<std::uint8_t> &univ) {
  BookStats st;
  if (ss.alphas.empty()) {
    st.err = "no alpha root";
    return st;
  }
  auto rid = panel.field_id("returns");
  if (!rid) {
    st.err = "panel has no 'returns' field";
    return st;
  }
  const std::span<const atx::f64> ret = panel.field_all(*rid);
  const std::vector<atx::f64> &v = ss.alphas[0].values;
  const atx::usize D = panel.dates();
  const atx::usize I = panel.instruments();

  std::vector<atx::f64> pnl(D, 0.0);
  std::vector<atx::f64> w(I, 0.0);
  std::vector<atx::f64> w_prev(I, 0.0);
  bool has_prev = false;
  std::vector<atx::f64> buf;
  std::vector<atx::usize> idxs;
  buf.reserve(I);
  idxs.reserve(I);

  atx::f64 turn_sum = 0.0;
  atx::usize turn_days = 0;
  long long names_sum = 0;
  double finite_frac_sum = 0.0;

  for (atx::usize d = 0; d + 1 < D; ++d) {
    // Step 1: gather capacity-universe finite signal cells.
    buf.clear();
    idxs.clear();
    atx::usize cap_cnt = 0;
    for (atx::usize i = 0; i < I; ++i) {
      const atx::usize idx = d * I + i;
      if (univ[idx] == 0) {
        continue;
      }
      ++cap_cnt;
      if (idx < v.size() && std::isfinite(v[idx])) {
        buf.push_back(v[idx]);
        idxs.push_back(i);
      }
    }
    if (buf.empty() || cap_cnt == 0) {
      continue;
    }
    // Step 2: demean.
    atx::f64 sum = 0.0;
    for (const atx::f64 x : buf) {
      sum += x;
    }
    const atx::f64 mean = sum / static_cast<atx::f64>(buf.size());
    // Step 3: weights = demeaned signal, L1-normalize to gross 1.
    std::fill(w.begin(), w.end(), 0.0);
    atx::f64 l1 = 0.0;
    for (atx::usize k = 0; k < idxs.size(); ++k) {
      const atx::f64 wi = buf[k] - mean;
      w[idxs[k]] = wi;
      l1 += std::fabs(wi);
    }
    if (l1 == 0.0) {
      continue;
    }
    for (atx::usize i = 0; i < I; ++i) {
      w[i] /= l1;
    }
    // Step 4: turnover proxy (post-L1).
    if (has_prev) {
      atx::f64 tv = 0.0;
      for (atx::usize i = 0; i < I; ++i) {
        tv += std::fabs(w[i] - w_prev[i]);
      }
      turn_sum += tv;
      ++turn_days;
    }
    w_prev = w;
    has_prev = true;
    // Step 5: forward PnL at d+1 (realize where still tradeable).
    atx::f64 p = 0.0;
    for (atx::usize i = 0; i < I; ++i) {
      if (w[i] == 0.0) {
        continue;
      }
      const atx::usize idx1 = (d + 1) * I + i;
      const atx::f64 r = (idx1 < ret.size()) ? ret[idx1] : kNaN;
      if (panel.in_universe(d + 1, i) && std::isfinite(r)) {
        p += w[i] * r;
      }
    }
    pnl[d + 1] = p;
    ++st.active_days;
    names_sum += static_cast<long long>(idxs.size());
    finite_frac_sum += static_cast<double>(idxs.size()) / static_cast<double>(cap_cnt);
  }

  // Metrics: engine convention (sqrt(252)*mean/std_pop over pnl[1..D)).
  const atx::engine::eval::ReturnMetricsCfg cfg{};
  const auto m = atx::engine::eval::compute_return_metrics(pnl, cfg);
  st.sharpe = m.sharpe;
  st.max_dd = m.max_dd;
  st.turnover = (turn_days > 0) ? (turn_sum / static_cast<atx::f64>(turn_days)) : kNaN;

  // Split-sample Sharpe (robustness / not-degenerate check): halves of [1, D).
  // compute_return_metrics drops the first element of whatever span it gets, so
  // we prepend a 0.0 sentinel to each half to make [1,mid) and [mid,D) honest.
  {
    const atx::usize mid = D / 2;
    auto half_sharpe = [&](atx::usize lo, atx::usize hi) -> atx::f64 {
      if (hi <= lo + 1) {
        return kNaN;
      }
      std::vector<atx::f64> seg;
      seg.reserve(hi - lo + 1);
      seg.push_back(0.0); // sentinel (dropped by the metric's r[1..T) convention)
      for (atx::usize d = lo; d < hi; ++d) {
        seg.push_back(pnl[d]);
      }
      return atx::engine::eval::compute_return_metrics(seg, cfg).sharpe;
    };
    st.sharpe_h1 = half_sharpe(1, mid);
    st.sharpe_h2 = half_sharpe(mid, D);
  }

  // Annualized return / vol over the realized (active) tail [1, D).
  atx::f64 rsum = 0.0;
  atx::usize rn = 0;
  for (atx::usize d = 1; d < D; ++d) {
    rsum += pnl[d];
    ++rn;
  }
  if (rn > 0) {
    const atx::f64 rmean = rsum / static_cast<atx::f64>(rn);
    atx::f64 var = 0.0;
    for (atx::usize d = 1; d < D; ++d) {
      const atx::f64 e = pnl[d] - rmean;
      var += e * e;
    }
    var /= static_cast<atx::f64>(rn);
    st.ann_ret = rmean * 252.0;
    st.ann_vol = std::sqrt(var) * std::sqrt(252.0);
  }
  if (st.active_days > 0) {
    st.avg_names = static_cast<double>(names_sum) / static_cast<double>(st.active_days);
    st.finite_frac = finite_frac_sum / static_cast<double>(st.active_days);
  }
  st.evaluated = true;
  return st;
}

[[nodiscard]] double env_f64(const char *key, double dflt) {
  if (const char *e = std::getenv(key)) {
    return std::atof(e);
  }
  return dflt;
}

// ===========================================================================
TEST(SingleAlphaCapacity, SweepAndVerify) {
  const bool is_real = std::getenv("ATX_ALPHA101_PANEL") != nullptr;

  const atx::f64 min_price = env_f64("ATX_MIN_PRICE", 1.0);
  const atx::f64 min_adv = env_f64("ATX_MIN_ADV", 50.0e6);
  const atx::u16 adv_window = static_cast<atx::u16>(
      static_cast<unsigned>(env_f64("ATX_ADV_WINDOW", 20.0)));
  const std::string adv_field = "adv" + std::to_string(adv_window);

  // Materialize the adv window the capacity filter needs.
  const std::vector<atx::u16> adv_windows = {adv_window};

  // Build the panel (real if ATX_ALPHA101_PANEL set, else synthetic).
  Panel panel = [&] {
    if (is_real) {
      auto raw = atx::impl::read_panel(std::getenv("ATX_ALPHA101_PANEL"));
      EXPECT_TRUE(raw.has_value()) << "read_panel: " << (raw ? "" : raw.error().message());
      auto aug = atx_impl_test::augment_for_alpha101(*raw, adv_windows);
      EXPECT_TRUE(aug.has_value()) << "augment(real): " << (aug ? "" : aug.error().message());
      return std::move(*aug);
    }
    auto synth = atx_impl_test::make_synth_orats_panel(/*dates=*/400, /*instruments=*/40);
    auto aug = atx_impl_test::augment_for_alpha101(synth, adv_windows);
    return aug.has_value() ? std::move(*aug) : std::move(synth);
  }();

  const atx::usize D = panel.dates();
  const atx::usize I = panel.instruments();

  atx::usize kept_cells = 0;
  const std::vector<std::uint8_t> cap_univ =
      capacity_universe(panel, adv_field, min_price, min_adv, kept_cells);

  // Average capacity-universe breadth per day.
  double avg_cap_names = 0.0;
  {
    long long s = 0;
    for (atx::usize d = 0; d < D; ++d) {
      for (atx::usize i = 0; i < I; ++i) {
        s += cap_univ[d * I + i];
      }
    }
    avg_cap_names = static_cast<double>(s) / static_cast<double>(D);
  }

  std::cout << "\n=== Single-alpha capacity backtest ===\n"
            << "panel: " << (is_real ? "REAL ORATS (ATX_ALPHA101_PANEL)" : "synthetic")
            << "  dates=" << D << " instruments=" << I << "\n"
            << "capacity filter: close>" << min_price << "  " << adv_field << ">=" << min_adv
            << "  -> avg names/day=" << std::fixed << std::setprecision(1) << avg_cap_names
            << "  (kept cells=" << kept_cells << ")\n"
            << "book: daily dollar-neutral, gross-1, L1-normalized; PnL realized at d+1\n\n";

  std::cout << std::left << std::setw(26) << "alpha" << std::right << std::setw(8) << "sharpe"
            << std::setw(8) << "shrpH1" << std::setw(8) << "shrpH2" << std::setw(9) << "turnovr"
            << std::setw(8) << "annret" << std::setw(8) << "annvol" << std::setw(8) << "maxdd"
            << std::setw(8) << "pass" << "\n";
  std::cout << std::string(91, '-') << "\n";

  int n_pass = 0;
  std::string best_name;
  atx::f64 best_sharpe = -1e30;

  // The designated deliverable: sector-neutral 1-month low-vol, signed-square
  // zscore weighting. Clears both gates with margin and is the most robust point
  // on the concentration frontier (see findings doc). Captured for a hard assert.
  const std::string kPrimary = "lv_z_p2.0";
  BookStats primary;

  for (const Candidate &c : candidates()) {
    auto ast = parse_expr(c.dsl, shared_lib());
    if (!ast) {
      std::cout << std::left << std::setw(22) << c.name << "  PARSE: " << ast.error().message()
                << "\n";
      continue;
    }
    auto ana = analyze(*ast);
    if (!ana) {
      std::cout << std::left << std::setw(22) << c.name << "  ANALYZE: " << ana.error().message()
                << "\n";
      continue;
    }
    auto prog = compile(*ast, *ana);
    if (!prog) {
      std::cout << std::left << std::setw(22) << c.name << "  COMPILE: " << prog.error().message()
                << "\n";
      continue;
    }
    Engine engine{panel};
    auto ss = engine.evaluate(*prog);
    if (!ss) {
      std::cout << std::left << std::setw(22) << c.name << "  EVAL: " << ss.error().message()
                << "\n";
      continue;
    }
    const BookStats st = run_book(*ss, panel, cap_univ);
    if (c.name == kPrimary) {
      primary = st;
    }

    const bool pass = is_real && std::isfinite(st.sharpe) && st.sharpe > 1.0 &&
                      std::isfinite(st.turnover) && st.turnover < 0.30;
    if (pass) {
      ++n_pass;
    }
    if (std::isfinite(st.sharpe) && st.sharpe > best_sharpe && std::isfinite(st.turnover) &&
        st.turnover < 0.30) {
      best_sharpe = st.sharpe;
      best_name = c.name;
    }

    std::cout << std::left << std::setw(26) << c.name << std::right << std::fixed
              << std::setprecision(3) << std::setw(8) << st.sharpe << std::setw(8) << st.sharpe_h1
              << std::setw(8) << st.sharpe_h2 << std::setw(9) << st.turnover << std::setw(8)
              << st.ann_ret << std::setw(8) << st.ann_vol << std::setw(8) << st.max_dd
              << std::setw(8) << (pass ? "YES" : "-") << "\n";

    // Hard sanity: every candidate must at least evaluate to a finite Sharpe on
    // the real panel (degenerate/empty book is a harness bug, not an alpha result).
    if (is_real) {
      EXPECT_TRUE(st.evaluated) << c.name << ": " << st.err;
      EXPECT_TRUE(std::isfinite(st.sharpe)) << c.name << ": non-finite Sharpe";
    }
  }

  std::cout << "\nthesis legend:\n";
  for (const Candidate &c : candidates()) {
    std::cout << "  " << std::left << std::setw(22) << c.name << c.thesis << "\n";
  }
  std::cout << "\nbest (sharpe, turnover<0.30): " << (best_name.empty() ? "(none)" : best_name)
            << "  sharpe=" << std::setprecision(3) << best_sharpe << "\n"
            << "candidates clearing BOTH gates (sharpe>1, turnover<0.30): " << n_pass << "\n";

  if (!is_real) {
    GTEST_SKIP() << "synthetic panel: Sharpes ~0 by construction; verdict needs ATX_ALPHA101_PANEL";
  }

  // ---- Acceptance gate for the designated deliverable (real panel only). ----
  // group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),2),sector)
  //   * Sharpe > 1            (risk-adjusted edge)
  //   * daily turnover < 0.30 (capacity / cost)
  //   * both sample halves positive (not a single-regime artifact)
  ASSERT_TRUE(primary.evaluated) << kPrimary << " was not evaluated";
  EXPECT_GT(primary.sharpe, 1.0) << kPrimary << " Sharpe must exceed 1";
  EXPECT_LT(primary.turnover, 0.30) << kPrimary << " daily turnover must be < 30%";
  EXPECT_GT(primary.sharpe_h1, 0.0) << kPrimary << " first-half Sharpe must be positive";
  EXPECT_GT(primary.sharpe_h2, 0.0) << kPrimary << " second-half Sharpe must be positive";
  std::cout << "\nDELIVERABLE  " << kPrimary
            << " = group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),2),sector)\n"
            << "  Sharpe=" << std::setprecision(3) << primary.sharpe << " (H1=" << primary.sharpe_h1
            << " H2=" << primary.sharpe_h2 << ")  turnover=" << primary.turnover
            << "  ann_ret=" << primary.ann_ret << "  ann_vol=" << primary.ann_vol
            << "  max_dd=" << primary.max_dd << "  names/day=" << std::setprecision(0)
            << primary.avg_names << "\n";
}

} // namespace atxtest_single_alpha_capacity
