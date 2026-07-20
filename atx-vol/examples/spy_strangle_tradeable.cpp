// spy_strangle_tradeable.cpp — VALIDATION harness: does the interpolated-from-fits
// short 40Δ/6M SPY strangle backtest match a TRADEABLE backtest built from the raw
// OPRA mid marks (actual listed contracts)?
//
// The production backtest (spy_strangle_backtest) prices the strategy on the FITTED
// surfaces: it solves a FRACTIONAL 40Δ strike at exactly T=0.5 and marks it at the
// surface fair_value (a re-Americanized model theo). This tool re-runs the SAME
// strategy a second way — picking the ACTUAL listed expiry nearest 6M and the actual
// listed strikes nearest 40Δ, filled/MTM'd at the OPRA quote MID = ½(bid+ask) — and
// compares the two P&L series day by day.
//
// Three series per date-pair (t → t+1), each a short 40Δ/6M strangle sized to the
// same $/day book theta:
//   * INTERPOLATED — the engine's strategy: fractional 40Δ strike @ T=0.5, priced by
//     surf.fair_value (== what spy_strangle_backtest / pnl_explain produce). Uses
//     resolve_spec() so the strikes + qty are BIT-IDENTICAL to the engine.
//   * TRADEABLE     — actual listed expiry≈0.5y, listed strikes≈40Δ, MTM at OPRA mids.
//   * BRIDGE        — the SAME listed contract + qty as TRADEABLE, but priced on the
//     fits (surf.fair_value). It shares TRADEABLE's strike/tenor/qty and INTERPOLATED's
//     pricing model, so it splits the interp↔trade gap into two independent parts:
//        gap_granularity = INTERPOLATED − BRIDGE   (fractional 40Δ/0.5y vs listed)
//        gap_fit_fidelity = BRIDGE     − TRADEABLE  (model fair_value vs market mid)
//
// The fit-fidelity gap is the load-bearing one: it is exactly the question "does the
// fitted surface reproduce the market mids at the contracts we actually trade?" We
// also report the raw per-contract fit residual (fair_value − mid) at the traded
// wings, independent of any P&L.
//
//   spy_strangle_tradeable --manifest DIR/manifest.tsv --opra DIR/opra
//       [--start YYYY-MM-DD] [--end YYYY-MM-DD] [--theta-per-day X] [--r RATE]
//       [--csv OUT.csv]
//
// OFF by default (ATX_BUILD_EXAMPLES). Correctness gate lives in the test suite.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/vol/american.hpp"          // AmericanGreeks
#include "atx/vol/backtest.hpp"          // MarketSnapshot
#include "atx/vol/corpus.hpp"            // CorpusManifest, read_manifest_file, CorpusFitStatus
#include "atx/vol/data.hpp"             // QuoteFrame/Row, year_fraction
#include "atx/vol/opra_batch.hpp"        // OpraBatchSpec, load_opra_daterange
#include "atx/vol/portfolio_pricer.hpp"  // kNsPerYear
#include "atx/vol/priced_surface.hpp"    // PricedSurface
#include "atx/vol/strategy.hpp"          // StrategySpec, resolve_spec, SizedLeg
#include "atx/vol/types.hpp"             // Side, Result

using namespace atx::vol;

namespace {

constexpr double kMult = 100.0;
constexpr double kTenorT = 0.5;                // 6-month strangle
constexpr double kCalendarDaysPerYear = 365.25;

// ── Per-date raw OPRA mid lookup ────────────────────────────────────────────

// One listed contract's mid + its listed strike, keyed for O(1) day-over-day match.
struct ContractKey {
  char side;             // 'C' / 'P'
  std::string expiry;    // "YYYY-MM-DD"
  std::int64_t k_milli;  // strike * 1000, integer (exact match across days)
  bool operator==(const ContractKey& o) const noexcept {
    return side == o.side && k_milli == o.k_milli && expiry == o.expiry;
  }
};
struct ContractKeyHash {
  std::size_t operator()(const ContractKey& k) const noexcept {
    std::size_t h = std::hash<std::string>{}(k.expiry);
    h ^= std::hash<std::int64_t>{}(k.k_milli) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= static_cast<std::size_t>(k.side) + 0x9e3779b9U + (h << 6) + (h >> 2);
    return h;
  }
};

[[nodiscard]] std::int64_t k_milli_of(double strike) noexcept {
  return std::llround(strike * 1000.0);
}
[[nodiscard]] char side_char(Side s) noexcept { return s == Side::Call ? 'C' : 'P'; }

// A date's raw chain indexed for (side,expiry,strike) mid lookup, plus the distinct
// listed (expiry, strike) rows per side for 40Δ scanning.
struct DayChain {
  std::string snapshot_iso;
  std::unordered_map<ContractKey, double, ContractKeyHash> mid;  // -> ½(bid+ask), >0 only
  // side -> expiry -> sorted list of listed strikes carrying a positive mid.
  std::map<std::string, std::vector<double>> call_strikes;  // by expiry
  std::map<std::string, std::vector<double>> put_strikes;
};

[[nodiscard]] DayChain index_chain(const QuoteFrame& f) {
  DayChain d;
  d.snapshot_iso = f.snapshot_iso;
  std::map<std::string, std::vector<double>>* dst = nullptr;
  for (const QuoteRow& r : f.rows) {
    const double m = 0.5 * (r.bid + r.ask);
    if (!(m > 0.0)) {
      continue;  // no tradeable mid (one-sided / zero) — skip
    }
    d.mid[ContractKey{side_char(r.side), r.expiry_iso, k_milli_of(r.strike)}] = m;
    dst = (r.side == Side::Call) ? &d.call_strikes : &d.put_strikes;
    (*dst)[r.expiry_iso].push_back(r.strike);
  }
  for (auto* m : {&d.call_strikes, &d.put_strikes}) {
    for (auto& [exp, ks] : *m) {
      std::sort(ks.begin(), ks.end());
    }
  }
  return d;
}

[[nodiscard]] std::optional<double> lookup_mid(const DayChain& d, Side side,
                                               const std::string& expiry, double strike) {
  const auto it = d.mid.find(ContractKey{side_char(side), expiry, k_milli_of(strike)});
  return it == d.mid.end() ? std::nullopt : std::optional<double>(it->second);
}

// The listed expiry whose year-fraction is closest to `target_T` (must be > a floor
// so 0DTE noise cannot win). Scans the CALL expiries (calls+puts co-list).
[[nodiscard]] std::optional<std::string> nearest_expiry(const DayChain& d, double target_T) {
  std::optional<std::string> best;
  double best_gap = 1e18;
  for (const auto& [exp, ks] : d.call_strikes) {
    const double T = year_fraction(d.snapshot_iso, exp);
    if (!(T > 0.02) || ks.empty()) {
      continue;
    }
    const double gap = std::fabs(T - target_T);
    if (gap < best_gap) {
      best_gap = gap;
      best = exp;
    }
  }
  return best;
}

// The listed strike at (expiry, side) whose |American delta| on `surf` is closest to
// `target_abs_delta`. Both a positive mid AND finite model delta are required.
[[nodiscard]] std::optional<double> nearest_delta_strike(const DayChain& d, const PricedSurface& surf,
                                                         Side side, const std::string& expiry,
                                                         double T, double target_abs_delta) {
  const auto& by_exp = (side == Side::Call) ? d.call_strikes : d.put_strikes;
  const auto it = by_exp.find(expiry);
  if (it == by_exp.end()) {
    return std::nullopt;
  }
  std::optional<double> best;
  double best_gap = 1e18;
  for (const double K : it->second) {
    const Result<double> dl = surf.delta(K, T, side);
    if (!dl.has_value() || !std::isfinite(*dl)) {
      continue;
    }
    const double gap = std::fabs(std::fabs(*dl) - target_abs_delta);
    if (gap < best_gap) {
      best_gap = gap;
      best = K;
    }
  }
  return best;
}

[[nodiscard]] StrategySpec make_strangle_spec(double target_theta_per_day) {
  StrategySpec spec;
  spec.name = "spy-short-40d-6m-strangle-daily-restrike";
  LegSpec leg;
  leg.symbol = "SPY";
  leg.tenor.target_T = kTenorT;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.size = SizeSpec{SizeSpec::Kind::TargetTheta, target_theta_per_day, -1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 1.0;
  return spec;
}

// Running mean / rms / correlation accumulator over paired (a, b) daily P&L.
struct PairStats {
  std::size_t n = 0;
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;  // sums for corr
  double sdiff = 0, sdiff2 = 0;                       // a-b diff moments
  void add(double a, double b) {
    ++n;
    sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
    const double d = a - b;
    sdiff += d; sdiff2 += d * d;
  }
  [[nodiscard]] double corr() const {
    if (n < 2) return 0.0;
    const double cov = sab / n - (sa / n) * (sb / n);
    const double va = saa / n - (sa / n) * (sa / n);
    const double vb = sbb / n - (sb / n) * (sb / n);
    const double den = std::sqrt(va * vb);
    return den > 0 ? cov / den : 0.0;
  }
  [[nodiscard]] double mean_diff() const { return n ? sdiff / n : 0.0; }
  [[nodiscard]] double rms_diff() const { return n ? std::sqrt(sdiff2 / n) : 0.0; }
};

}  // namespace

int main(int argc, char** argv) {
  std::string manifest_path;
  std::string opra_root = "data/spy_ytd/opra";
  std::string start = "2026-01-02";
  std::string end = "2026-07-02";
  std::string csv_out;
  double target_theta_per_day = 10'000.0;
  double r = 0.043;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--manifest") manifest_path = nv();
    else if (a == "--opra") opra_root = nv();
    else if (a == "--start") start = nv();
    else if (a == "--end") end = nv();
    else if (a == "--csv") csv_out = nv();
    else if (a == "--theta-per-day") target_theta_per_day = std::strtod(nv(), nullptr);
    else if (a == "--r") r = std::strtod(nv(), nullptr);
    else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
  }
  if (manifest_path.empty()) {
    std::fprintf(stderr, "usage: spy_strangle_tradeable --manifest M.tsv --opra DIR [...]\n");
    return 2;
  }

  // ── 1. Fitted corpus (interpolated + bridge pricing) ──────────────────────
  const Result<CorpusManifest> man = read_manifest_file(manifest_path);
  if (!man) {
    std::fprintf(stderr, "read_manifest_file(%s): %s\n", manifest_path.c_str(),
                 man.error().to_string().c_str());
    return 1;
  }
  std::vector<std::string> dates;
  std::map<std::string, std::string> archive_of;  // date -> archive path (first Ok)
  for (const CorpusEntry& e : man->entries) {
    if (e.status != CorpusFitStatus::Ok) continue;
    if (archive_of.emplace(e.date, e.archive_path).second) dates.push_back(e.date);
  }
  std::sort(dates.begin(), dates.end());
  if (dates.size() < 2) {
    std::fprintf(stderr, "need >=2 fitted dates, have %zu\n", dates.size());
    return 1;
  }

  // ── 2. Raw OPRA mids per date ─────────────────────────────────────────────
  OpraBatchSpec bspec;
  bspec.symbols = {"SPY"};
  bspec.date_lo = start;
  bspec.date_hi = end;
  bspec.root_dir = opra_root;
  bspec.r = r;
  const Result<OpraBatchResult> batch = load_opra_daterange(bspec);
  if (!batch) {
    std::fprintf(stderr, "load_opra_daterange: %s\n", batch.error().to_string().c_str());
    return 1;
  }
  std::map<std::string, DayChain> chain_of;
  for (const OpraBatchEntry& e : batch->entries) {
    if (e.panel.has_value()) chain_of.emplace(e.date, index_chain(e.panel->frame));
  }
  std::printf("[data] fitted dates=%zu  raw OPRA days=%zu  (%s..%s)\n", dates.size(),
              chain_of.size(), dates.front().c_str(), dates.back().c_str());

  const StrategySpec spec = make_strangle_spec(target_theta_per_day);
  const double target_theta_annual = target_theta_per_day * kCalendarDaysPerYear;

  // ── 3. Walk consecutive date-pairs, build all three P&L series ────────────
  struct DayRow {
    std::string date;
    double kc_i = 0, kp_i = 0;      // interpolated fractional strikes
    std::string expiry_l;          // listed expiry chosen
    double kc_l = 0, kp_l = 0;      // listed strikes chosen
    double pnl_interp = 0, pnl_trade = 0, pnl_bridge = 0;
    double resid_c = 0, resid_p = 0;  // fair_value - mid at entry (per share)
    double mid_c = 0, mid_p = 0;
    bool trade_ok = false;
  };
  std::vector<DayRow> out;
  PairStats interp_vs_trade, bridge_vs_trade, interp_vs_bridge;
  double tot_interp = 0, tot_trade = 0, tot_bridge = 0;
  double tot_interp_on_ok = 0;
  std::size_t n_skip = 0;
  // fit residual accumulators (per traded contract, at entry)
  double resid_abs_sum = 0, resid_pct_sum = 0;
  double resid_signed_sum = 0;
  std::size_t resid_n = 0;

  for (std::size_t i = 0; i + 1 < dates.size(); ++i) {
    const std::string& dt0 = dates[i];
    const std::string& dt1 = dates[i + 1];
    auto a0 = archive_of.find(dt0), a1 = archive_of.find(dt1);
    if (a0 == archive_of.end() || a1 == archive_of.end()) continue;
    Result<MarketSnapshot> s0 = MarketSnapshot::load(a0->second);
    Result<MarketSnapshot> s1 = MarketSnapshot::load(a1->second);
    if (!s0 || !s1) continue;
    const std::optional<std::uint32_t> uid = s0->uid_of("SPY");
    if (!uid) continue;
    // WS-ZC1: `find` returns a SurfaceRef handle — the surface may be OWNED or a
    // zero-copy BORROW of the mapped record. Pointer-style use below is unchanged.
    const SurfaceRef surf0 = s0->find(*uid);
    const SurfaceRef surf1 = s1->find(*uid);
    if (surf0 == nullptr || surf1 == nullptr) continue;
    const double dt = static_cast<double>(s1->ts_ns() - s0->ts_ns()) / kNsPerYear;
    if (!(dt > 0.0)) continue;

    DayRow row;
    row.date = dt0;

    // (a) INTERPOLATED — exact engine strikes + qty via resolve_spec.
    const Result<std::vector<SizedLeg>> sized = resolve_spec(*s0, spec);
    if (!sized) continue;
    for (const SizedLeg& sl : *sized) {
      const Result<double> base = surf0->fair_value(sl.leg.K, sl.leg.T, sl.leg.side);
      const Result<double> tgt = surf1->fair_value(sl.leg.K, sl.leg.T - dt, sl.leg.side);
      if (!base || !tgt) continue;
      row.pnl_interp += sl.qty * sl.multiplier * (*tgt - *base);
      (sl.leg.side == Side::Call ? row.kc_i : row.kp_i) = sl.leg.K;
    }

    // (b) TRADEABLE + (c) BRIDGE — listed 40Δ/≈6M contracts, mids vs fits.
    const auto ch0 = chain_of.find(dt0);
    const auto ch1 = chain_of.find(dt1);
    if (ch0 != chain_of.end() && ch1 != chain_of.end()) {
      const DayChain& c0 = ch0->second;
      const DayChain& c1 = ch1->second;
      const std::optional<std::string> exp = nearest_expiry(c0, kTenorT);
      if (exp) {
        const double Tstar = year_fraction(c0.snapshot_iso, *exp);
        const std::optional<double> Kc =
            nearest_delta_strike(c0, *surf0, Side::Call, *exp, Tstar, 0.40);
        const std::optional<double> Kp =
            nearest_delta_strike(c0, *surf0, Side::Put, *exp, Tstar, 0.40);
        if (Kc && Kp) {
          // Size the tradeable strangle to the same $/day book theta from the fits'
          // per-share theta at the LISTED (K,Tstar) — mirrors resolve_spec exactly.
          const Result<AmericanGreeks> gc = surf0->greeks(*Kc, Tstar, Side::Call);
          const Result<AmericanGreeks> gp = surf0->greeks(*Kp, Tstar, Side::Put);
          const std::optional<double> m0c = lookup_mid(c0, Side::Call, *exp, *Kc);
          const std::optional<double> m0p = lookup_mid(c0, Side::Put, *exp, *Kp);
          const std::optional<double> m1c = lookup_mid(c1, Side::Call, *exp, *Kc);
          const std::optional<double> m1p = lookup_mid(c1, Side::Put, *exp, *Kp);
          if (gc && gp && m0c && m0p && m1c && m1p) {
            const double struct_theta = gc->theta + gp->theta;
            if (std::isfinite(struct_theta) && std::fabs(struct_theta) > 0.0) {
              const double qty = -1.0 * target_theta_annual / (std::fabs(struct_theta) * kMult);
              // tradeable MTM (mids)
              row.pnl_trade = qty * kMult * ((*m1c - *m0c) + (*m1p - *m0p));
              // bridge MTM (same contract + qty, priced on fits)
              const Result<double> b0c = surf0->fair_value(*Kc, Tstar, Side::Call);
              const Result<double> b0p = surf0->fair_value(*Kp, Tstar, Side::Put);
              const Result<double> b1c = surf1->fair_value(*Kc, Tstar - dt, Side::Call);
              const Result<double> b1p = surf1->fair_value(*Kp, Tstar - dt, Side::Put);
              if (b0c && b0p && b1c && b1p) {
                row.pnl_bridge = qty * kMult * ((*b1c - *b0c) + (*b1p - *b0p));
                row.expiry_l = *exp;
                row.kc_l = *Kc; row.kp_l = *Kp;
                row.mid_c = *m0c; row.mid_p = *m0p;
                row.resid_c = *b0c - *m0c;  // fair_value - mid (per share)
                row.resid_p = *b0p - *m0p;
                row.trade_ok = true;
              }
            }
          }
        }
      }
    }

    tot_interp += row.pnl_interp;
    if (row.trade_ok) {
      tot_trade += row.pnl_trade;
      tot_bridge += row.pnl_bridge;
      tot_interp_on_ok += row.pnl_interp;
      interp_vs_trade.add(row.pnl_interp, row.pnl_trade);
      bridge_vs_trade.add(row.pnl_bridge, row.pnl_trade);
      interp_vs_bridge.add(row.pnl_interp, row.pnl_bridge);
      for (const auto& [fv_minus_mid, mid] :
           {std::pair{row.resid_c, row.mid_c}, std::pair{row.resid_p, row.mid_p}}) {
        resid_signed_sum += fv_minus_mid;
        resid_abs_sum += std::fabs(fv_minus_mid);
        if (mid > 0) resid_pct_sum += std::fabs(fv_minus_mid) / mid;
        ++resid_n;
      }
    } else {
      ++n_skip;
    }
    out.push_back(std::move(row));
  }

  // ── 4. Report ─────────────────────────────────────────────────────────────
  const std::size_t n_ok = interp_vs_trade.n;
  std::printf("\n=== TRADEABLE vs INTERPOLATED — short 40Δ 6M SPY strangle, daily restrike ===\n");
  std::printf("date-pairs: %zu total, %zu tradeable-complete, %zu skipped (missing listed/next-day mid)\n",
              out.size(), n_ok, n_skip);
  std::printf("\n[cumulative P&L, $, book theta $%.0f/day]\n", target_theta_per_day);
  std::printf("  INTERPOLATED (fits, fractional 40Δ @0.5y) : %14.2f  (all pairs)\n", tot_interp);
  std::printf("  INTERPOLATED (same tradeable-complete days): %14.2f\n", tot_interp_on_ok);
  std::printf("  BRIDGE       (listed contract, fits price) : %14.2f\n", tot_bridge);
  std::printf("  TRADEABLE    (listed contract, OPRA mids)  : %14.2f\n", tot_trade);
  std::printf("\n[decomposition of the interp↔trade gap on complete days]\n");
  std::printf("  gap_granularity = INTERP − BRIDGE = %14.2f  (fractional 40Δ/0.5y vs listed strike/expiry)\n",
              tot_interp_on_ok - tot_bridge);
  std::printf("  gap_fit_fidelity = BRIDGE − TRADE = %14.2f  (model fair_value vs market mid)\n",
              tot_bridge - tot_trade);
  std::printf("\n[daily P&L agreement]\n");
  std::printf("  corr(INTERP, TRADE)  = %.4f   mean_daily_diff = %10.2f   rms_daily_diff = %10.2f\n",
              interp_vs_trade.corr(), interp_vs_trade.mean_diff(), interp_vs_trade.rms_diff());
  std::printf("  corr(BRIDGE, TRADE)  = %.4f   mean_daily_diff = %10.2f   rms_daily_diff = %10.2f\n",
              bridge_vs_trade.corr(), bridge_vs_trade.mean_diff(), bridge_vs_trade.rms_diff());
  std::printf("  corr(INTERP, BRIDGE) = %.4f   mean_daily_diff = %10.2f   rms_daily_diff = %10.2f\n",
              interp_vs_bridge.corr(), interp_vs_bridge.mean_diff(), interp_vs_bridge.rms_diff());
  std::printf("\n[fit residual at traded 40Δ wings, entry (fair_value − mid), n=%zu contracts]\n", resid_n);
  if (resid_n > 0) {
    std::printf("  mean signed = %+.4f $/sh   mean |resid| = %.4f $/sh   mean |resid|/mid = %.3f%%\n",
                resid_signed_sum / resid_n, resid_abs_sum / resid_n,
                100.0 * resid_pct_sum / resid_n);
  }

  // head/tail table
  std::printf("\n  %-12s %8s %8s  %-12s %8s %8s %12s %12s %12s\n", "date", "Kc_i", "Kp_i",
              "listed_exp", "Kc_l", "Kp_l", "pnl_interp", "pnl_bridge", "pnl_trade");
  const auto prow = [&](const DayRow& rr) {
    std::printf("  %-12s %8.2f %8.2f  %-12s %8.2f %8.2f %12.2f %12.2f %12.2f%s\n", rr.date.c_str(),
                rr.kc_i, rr.kp_i, rr.expiry_l.c_str(), rr.kc_l, rr.kp_l, rr.pnl_interp,
                rr.pnl_bridge, rr.pnl_trade, rr.trade_ok ? "" : "  [skip]");
  };
  for (std::size_t i = 0; i < out.size() && i < 6; ++i) prow(out[i]);
  if (out.size() > 12) {
    std::printf("  ...\n");
    for (std::size_t i = out.size() - 6; i < out.size(); ++i) prow(out[i]);
  }

  // ── 5. Optional CSV ───────────────────────────────────────────────────────
  if (!csv_out.empty()) {
    std::ofstream os(csv_out, std::ios::binary | std::ios::trunc);
    os.setf(std::ios::fmtflags(0), std::ios::floatfield);
    os.precision(10);
    os << "# strategy=Short 40D 6M SPY Strangle, Daily Restrike\n"
       << "# target_theta_daily=" << target_theta_per_day << "\n"
       << "# total_interp=" << tot_interp << "\n# total_bridge=" << tot_bridge
       << "\n# total_trade=" << tot_trade << "\n"
       << "# corr_interp_trade=" << interp_vs_trade.corr()
       << "\n# corr_bridge_trade=" << bridge_vs_trade.corr() << "\n"
       << "date,kc_interp,kp_interp,listed_expiry,kc_listed,kp_listed,"
          "pnl_interp,pnl_bridge,pnl_trade,resid_call,resid_put,mid_call,mid_put,trade_ok\n";
    for (const DayRow& rr : out) {
      os << rr.date << ',' << rr.kc_i << ',' << rr.kp_i << ',' << rr.expiry_l << ',' << rr.kc_l
         << ',' << rr.kp_l << ',' << rr.pnl_interp << ',' << rr.pnl_bridge << ',' << rr.pnl_trade
         << ',' << rr.resid_c << ',' << rr.resid_p << ',' << rr.mid_c << ',' << rr.mid_p << ','
         << (rr.trade_ok ? 1 : 0) << '\n';
    }
    std::printf("\n[wrote] %s (%zu rows)\n", csv_out.c_str(), out.size());
  }
  return 0;
}
