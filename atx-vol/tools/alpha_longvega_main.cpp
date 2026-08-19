// atx-vol-longvega — the long single-name vega strategy, end to end on a panel.
//
//   atx-vol-longvega --panel <tsv> [--features PAT[,PAT...]] [--market SYM]
//                    [--names N] [--horizon H] [--max-hspread F]
//                    [--cost-liquid VP] [--cost-illiquid VP] [--crossings X]
//                    [--liquid-cut F] [--require-measured-liq]
//                    [--per-date FILE]
//
// Six stages, each of which can fail loudly rather than degrade quietly:
//
//   1. LOAD     panel by name (no schema version), per-column finite census.
//   2. COMPUTE  every selected feature from the panel's own series through the
//               registry -- including features the panel never emitted.
//   3. VERIFY   recomputed vs emitted, column by column, for every feature the
//               panel DOES carry. An independent reimplementation agreeing to
//               1e-9 is evidence about the emitter that reading either
//               implementation cannot supply. A disagreement stops the run.
//   4. ADJUDICATE  leaks and entry-mark channels against the money axis, plus
//               the decontaminated cross-read axis.
//   5. BLEND    equal-weight oriented within-date ranks. Orientation comes
//               from the catalogue's published sign priors; a feature with no
//               prior is refused, not fitted.
//   6. RUN      admission, selection, equal-vega book, paired excess over the
//               long-everything floor, raw / Newey-West / non-overlapping
//               phase-sweep significance.
//
// Exit 1 on a load failure, a verification mismatch, or a fatal audit finding.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/alpha/audit.hpp"
#include "atx/vol/alpha/compute.hpp"
#include "atx/vol/alpha/frame.hpp"
#include "atx/vol/alpha/registry.hpp"
#include "atx/vol/alpha/strategy.hpp"

namespace {

using namespace atx::vol::alpha; // NOLINT — a tool TU, not a header

struct Args {
  std::string panel;
  std::vector<std::string> features{"f5_hv_iv_gap", "f4_term_slope", "f16_iv_vov_21d",
                                    "f15_idio_share", "liq_hspread_frac"};
  std::string market{"@xsec"};
  std::string earnings; // empty = no calendar; f28..f30 evaluate to NaN
  double avoid_earn_days{0.0}; // >0: veto names within N days of their print
  bool keep_cheap_prints{false}; // exempt prints priced below the name's own history (f31 < 0)
  std::vector<std::string> flip; // features traded AGAINST their catalogue prior
  // "dh" = the delta-hedged money axis. "rv" = the decontaminated cross-read:
  // forward realized vol alone, no implied leg, not a P&L.
  std::string axis{"dh"};
  std::string per_date;
  std::size_t names{20};
  std::size_t horizon{21};
  bool horizon_explicit{false};
  double max_hspread{0.20};
  double liquid_cut{0.05};
  double cost_liquid{0.10};
  double cost_illiquid{0.25};
  double crossings{1.0};
  bool require_measured_liq{false};
  double verify_tol{1.0e-9};
};

void usage() {
  std::fprintf(
      stderr,
      "usage: atx-vol-longvega --panel <tsv> [options]\n"
      "  --features PAT[,PAT...]  catalogue globs (default: the five-signal long-vega set)\n"
      "  --market SYM|@xsec       market proxy for f15/f27: a panel symbol, or @xsec\n"
"                           for the cross-sectional mean return (default @xsec —\n"
"                           a single-symbol proxy with gaps starves both features)\n"
"  --earnings TSV           earnings calendar (fetch_earnings_calendar.py output);\n"
"                           enables f28_days_to_earn/f29_earn_n_21d/f30_earn_sigma_e\n"
"  --avoid-earn-days N      veto names within N calendar days of their next print\n"
"                           from BOTH books (needs --earnings). Measured r11: held\n"
"                           through the print the event premium costs -4.47 vp.\n"
"  --keep-cheap-prints      exempt from that veto the prints priced BELOW the\n"
"                           name's own delivered history (f31 < 0) — the long-vega\n"
"                           side of the event premium. Zero is the principled\n"
"                           threshold, not a tuned one.\n"
"  --flip NAME[,NAME...]    trade the named feature(s) AGAINST their catalogue\n"
"                           prior. ONLY for a published axis-specific direction\n"
"                           (Vasquez buys steep-slope FRONT month, Campasano-Linn\n"
"                           buy the inverted name's BACK month — both are right);\n"
"                           flipping because the backtest liked it is sign-mining.\n"
      "  --axis ... dhev          dhev = the EVENT sleeve (needs --earnings): 21d-tenor\n"
      "                           straddle P&L entered 2 sessions before the print\n"
      "                           anchor, exited 2 after (horizon auto-set to 4). NaN off\n"
      "                           the schedule, so the FLOOR is the unconditional\n"
      "                           pre-print straddle (Gao-Xing-Zhang) and the EXCESS is\n"
      "                           print selection (Milian).\n"
      "  --axis ... dh63          dh63 = 100*(rv_fwd_63d - iv_fair_63d), the BACK-month\n"
      "                           money axis, 63-session hold (horizon auto-set to 63).\n"
      "                           The back mark is re-marked late (Campasano-Linn), so a\n"
      "                           forecast the 21d mark absorbed can still pay here. The\n"
      "                           forward leg is computed from the panel's spot under the\n"
      "                           full contiguity + corporate-action gate.\n"
      "  --axis dh|rv|volchg      volchg = 100*(rv_fwd - rv_trail), the VOL-CHANGE\n"
    "                           axis: no implied leg AND no vol level, so it is\n"
    "                           immune to both the entry-mark channel and to\n"
    "                           volatility persistence. It carries its own\n"
    "                           -rv_trail leg, which the adjudicator names.\n"
    "  --axis dh|rv             dh = delta-hedged straddle P&L, 100*(rv_fwd - iv_entry),\n"
      "                           the money axis (default). rv = 100*rv_fwd alone, the\n"
      "                           DECONTAMINATED cross-read: no implied leg, so a selection\n"
      "                           excess that survives it is forecasting skill rather than\n"
      "                           the entry-mark channel. Not a P&L; never headline it.\n"
      "  --names N                names held per date (default 20)\n"
      "  --horizon H              holding sessions; sets the NW lag and phase count (21)\n"
      "  --max-hspread F          admission cap on measured ATM half-spread (0.20)\n"
      "  --liquid-cut F           half-spread at or below which the liquid tier applies (0.05)\n"
      "  --cost-liquid VP         vol points charged per crossing, liquid tier (0.10)\n"
      "  --cost-illiquid VP       ... illiquid tier (0.25)\n"
      "  --crossings X            crossings charged per position (1.0)\n"
      "  --require-measured-liq   exclude a name whose width was never measured\n"
      "  --per-date FILE          write the per-date series as TSV\n");
}

std::vector<std::string> split_commas(const std::string &s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == ',') {
      if (i > start) {
        out.push_back(s.substr(start, i - start));
      }
      start = i + 1;
    }
  }
  return out;
}

bool parse_args(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    std::string v;
    const auto next = [&]() {
      if (i + 1 >= argc) {
        return false;
      }
      v = argv[++i];
      return true;
    };
    if (flag == "--panel" && next()) {
      a.panel = v;
    } else if (flag == "--features" && next()) {
      a.features = split_commas(v);
    } else if (flag == "--market" && next()) {
      a.market = v;
    } else if (flag == "--earnings" && next()) {
      a.earnings = v;
    } else if (flag == "--avoid-earn-days" && next()) {
      a.avoid_earn_days = std::stod(v);
    } else if (flag == "--keep-cheap-prints") {
      a.keep_cheap_prints = true;
    } else if (flag == "--flip" && next()) {
      for (const std::string &n : split_commas(v)) {
        a.flip.push_back(n);
      }
    } else if (flag == "--axis" && next()) {
      if (v != "dh" && v != "rv" && v != "volchg" && v != "dh63" && v != "dhev") {
        std::fprintf(stderr, "--axis must be 'dh', 'rv', 'volchg', 'dh63' or 'dhev' (got '%s')\n",
                     v.c_str());
        return false;
      }
      a.axis = v;
    } else if (flag == "--per-date" && next()) {
      a.per_date = v;
    } else if (flag == "--names" && next()) {
      a.names = static_cast<std::size_t>(std::stoull(v));
    } else if (flag == "--horizon" && next()) {
      a.horizon = static_cast<std::size_t>(std::stoull(v));
      a.horizon_explicit = true;
    } else if (flag == "--max-hspread" && next()) {
      a.max_hspread = std::stod(v);
    } else if (flag == "--liquid-cut" && next()) {
      a.liquid_cut = std::stod(v);
    } else if (flag == "--cost-liquid" && next()) {
      a.cost_liquid = std::stod(v);
    } else if (flag == "--cost-illiquid" && next()) {
      a.cost_illiquid = std::stod(v);
    } else if (flag == "--crossings" && next()) {
      a.crossings = std::stod(v);
    } else if (flag == "--require-measured-liq") {
      a.require_measured_liq = true;
    } else {
      std::fprintf(stderr, "unknown or incomplete flag: %s\n", flag.c_str());
      return false;
    }
  }
  if (a.panel.empty()) {
    std::fprintf(stderr, "--panel is required\n");
    return false;
  }
  return true;
}

// Recomputed vs emitted, for a column the panel carries. Compares only rows
// where BOTH are finite, and separately counts the rows where one is finite
// and the other is not -- a missingness disagreement is a real finding even
// when every shared row matches to the last bit.
struct VerifyResult {
  std::size_t n_both{0};
  std::size_t n_only_emitted{0};
  std::size_t n_only_computed{0};
  double max_abs_diff{0.0};
  double max_rel_diff{0.0};
};

VerifyResult verify_column(std::span<const double> emitted, const std::vector<double> &computed) {
  VerifyResult v;
  for (std::size_t r = 0; r < emitted.size() && r < computed.size(); ++r) {
    const bool fe = std::isfinite(emitted[r]);
    const bool fc = std::isfinite(computed[r]);
    if (fe && fc) {
      ++v.n_both;
      const double d = std::fabs(emitted[r] - computed[r]);
      v.max_abs_diff = std::max(v.max_abs_diff, d);
      const double scale = std::max(1.0, std::fabs(emitted[r]));
      v.max_rel_diff = std::max(v.max_rel_diff, d / scale);
    } else if (fe) {
      ++v.n_only_emitted;
    } else if (fc) {
      ++v.n_only_computed;
    }
  }
  return v;
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage();
    return 2;
  }

  // A 63-session hold gets a 63-session overlap correction and phase split; a
  // silent 21 would overstate the t by ~sqrt(3).
  if (args.axis == "dh63" && !args.horizon_explicit) {
    args.horizon = 63;
    std::printf("NOTE  --axis dh63: horizon set to 63 sessions (pass --horizon to override)\n");
  }
  if (args.axis == "dhev") {
    if (args.earnings.empty()) {
      std::fprintf(stderr, "--axis dhev needs --earnings (the entry schedule IS the calendar)\n");
      return 2;
    }
    if (!args.horizon_explicit) {
      args.horizon = 4;
      std::printf("NOTE  --axis dhev: horizon set to 4 sessions (pass --horizon to override)\n");
    }
  }

  const FeatureRegistry freg = builtin_features();
  const TargetRegistry treg = builtin_targets();

  // ── 1. LOAD ───────────────────────────────────────────────────────────────
  std::ifstream in(args.panel, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open panel '%s'\n", args.panel.c_str());
    return 1;
  }
  auto loaded = PanelFrame::read_tsv(in);
  if (!loaded) {
    std::fprintf(stderr, "panel load failed: %s\n", loaded.error().to_string().c_str());
    return 1;
  }
  const PanelFrame &frame = *loaded;
  std::printf("PANEL   %s\n  rows=%zu cols=%zu fingerprint=%016llx\n", args.panel.c_str(),
              frame.rows(), frame.cols(),
              static_cast<unsigned long long>(frame.schema().fingerprint()));

  auto dates = group_by_date(frame);
  if (!dates) {
    std::fprintf(stderr, "date grouping failed: %s\n", dates.error().to_string().c_str());
    return 1;
  }
  std::printf("  sessions=%zu  %s .. %s\n", dates->size(),
              dates->empty() ? "-" : dates->front().date.c_str(),
              dates->empty() ? "-" : dates->back().date.c_str());

  const auto selected = freg.select(args.features);
  if (!selected) {
    std::fprintf(stderr, "--features: %s\n", selected.error().to_string().c_str());
    return 2;
  }

  // ── 2. COMPUTE ────────────────────────────────────────────────────────────
  auto series = group_by_symbol(frame);
  if (!series) {
    std::fprintf(stderr, "symbol grouping failed: %s\n", series.error().to_string().c_str());
    return 1;
  }
  MarketSeries market;
  bool have_market = false;
  if (auto m = args.market == "@xsec"
                   ? market_from_cross_section(*series, dates->size())
                   : market_from(*series, args.market, dates->size())) {
    market = std::move(*m);
    have_market = true;
    // The proxy obeys the same row policy as every other symbol, and
    // `f15_idio_share` needs it present on EVERY date of a 63-session window.
    // At coverage c that is roughly c^63, so a proxy at 86% yields the feature
    // on essentially no row. Say so here rather than let an all-NaN column be
    // discovered three stages later.
    const double cov = market.coverage_fraction();
    std::printf("\nMARKET  proxy %s covers %zu / %zu sessions (%.1f%%); "
                "P(63-session window intact) ~ %.2g\n",
                market.symbol.c_str(), market.date.size(), dates->size(), 100.0 * cov,
                std::pow(cov, 63.0));
    if (std::pow(cov, 63.0) < 0.01) {
      std::printf("        f15_idio_share will be near-empty on this panel — it needs a "
                  "gap-free market series.\n");
    }
  } else {
    std::printf("\nNOTE  market proxy '%s' is not in this panel; f15_idio_share will be NaN.\n",
                args.market.c_str());
  }

  EarningsCalendar earnings;
  if (!args.earnings.empty()) {
    std::ifstream ein(args.earnings, std::ios::binary);
    if (!ein) {
      std::fprintf(stderr, "cannot open earnings calendar '%s'\n", args.earnings.c_str());
      return 1;
    }
    std::string text((std::istreambuf_iterator<char>(ein)), std::istreambuf_iterator<char>());
    auto cal = earnings_from_tsv(text);
    if (!cal) {
      std::fprintf(stderr, "earnings load failed: %s\n", cal.error().to_string().c_str());
      return 1;
    }
    earnings = std::move(*cal);
    // Coverage against the PANEL's names is the number that matters: a rich
    // calendar that misses this panel's universe still starves f28..f30.
    std::size_t covered = 0;
    for (const SymbolSeries &s : *series) {
      if (earnings.find(s.symbol) != nullptr) {
        ++covered;
      }
    }
    std::printf("\nEARNINGS  %zu events across %zu names; calendar covers %zu / %zu panel "
                "names (%.1f%%)\n",
                earnings.n_events, earnings.by_symbol.size(), covered, series->size(),
                series->empty() ? 0.0
                                : 100.0 * static_cast<double>(covered) /
                                      static_cast<double>(series->size()));
  }

  auto computed = evaluate(frame, *selected, have_market ? &market : nullptr,
                           earnings.empty() ? nullptr : &earnings);
  if (!computed) {
    std::fprintf(stderr, "feature evaluation failed: %s\n", computed.error().to_string().c_str());
    return 1;
  }
  std::printf("\nFEATURES  %zu selected, %zu evaluated from panel series, %zu need the surface DB\n",
              selected->size(), computed->values.size(), computed->needs_surface.size());
  for (const std::string &n : computed->needs_surface) {
    std::printf("  needs-surface  %s\n", n.c_str());
  }

  // ── 3. VERIFY ─────────────────────────────────────────────────────────────
  std::printf("\nVERIFY  recomputed vs emitted (rows where both are finite)\n");
  bool verify_failed = false;
  std::size_t n_checked = 0;
  for (const FeatureSpec *spec : *selected) {
    const auto it = computed->values.find(spec->name);
    if (it == computed->values.end() || !frame.schema().has(spec->name)) {
      continue;
    }
    const auto emitted = frame.numbers(spec->name);
    if (!emitted) {
      continue;
    }
    const VerifyResult v = verify_column(*emitted, it->second);
    ++n_checked;
    // The pass criterion is EXACT agreement wherever both are finite, plus no
    // row where this reimplementation produced a value the emitter did not.
    // `only_emitted > 0` is EXPECTED and is not a failure: the emitter owns the
    // full bar axis and this reimplementation only sees the emitted subset, so
    // it deliberately declines every window that spans a dropped session. That
    // is coverage loss, not disagreement.
    const bool ok = v.max_abs_diff <= args.verify_tol && v.n_only_computed == 0;
    std::printf("  %-20s n=%-6zu max|diff|=%.3e  only-emitted=%zu only-computed=%zu  %s\n",
                spec->name.c_str(), v.n_both, v.max_abs_diff, v.n_only_emitted, v.n_only_computed,
                ok ? "OK" : "MISMATCH");
    if (!ok) {
      verify_failed = true;
    }
  }
  if (n_checked == 0) {
    std::printf("  (no selected feature is present in the panel to check against)\n");
  }
  if (verify_failed) {
    std::fprintf(stderr,
                 "\nRESULT  VERIFY FAILED — the independent reimplementation disagrees with the\n"
                 "        emitted panel. One of the two is wrong; do not fit until it is settled.\n");
    return 1;
  }

  // ── 4. ADJUDICATE ─────────────────────────────────────────────────────────
  const char *axis_target = args.axis == "rv"       ? "rv_fwd_21d"
                            : args.axis == "volchg" ? "vol_chg_21d"
                            : args.axis == "dh63"   ? "dh_straddle_pnl_63d"
                            : args.axis == "dhev"   ? "event_straddle_pnl_4d"
                                                    : "dh_straddle_pnl_21d";
  const TargetSpec *money = treg.find(axis_target);
  if (money == nullptr) {
    std::fprintf(stderr, "catalogue is missing %s\n", axis_target);
    return 1;
  }
  AuditConfig acfg;
  // Only a tradeable axis may sit in the headline seat; asking the adjudicator
  // about a forecast axis in that seat is how the reminder gets printed.
  acfg.target_is_headline = true;
  const AuditReport report = audit(*selected, *money, acfg);
  std::printf("\nADJUDICATION  target=%s (%s)\n", money->name.c_str(),
              money->tradeable ? "TRADEABLE" : "forecast-only, NOT a P&L");
  if (report.clean()) {
    std::printf("  CLEAN\n");
  } else {
    for (const std::string &line : format_report(report)) {
      std::printf("  %s\n", line.c_str());
    }
  }
  if (report.has_fatal()) {
    std::fprintf(stderr, "\nRESULT  FATAL audit finding — refusing to run the book.\n");
    return 1;
  }
  if (report.count(FindingKind::EntryMarkChannel) > 0) {
    const std::string axis = cross_read_axis(*selected, treg.all(), *money, 0);
    std::printf("  cross-read axis: %s\n", axis.empty() ? "(none clean for this set)" : axis.c_str());
  }

  // ── 5. BLEND ──────────────────────────────────────────────────────────────
  BlendConfig bcfg;
  bcfg.flip = args.flip;
  auto blended = blend(frame, *dates, *selected, computed->values, bcfg);
  if (!blended) {
    std::fprintf(stderr, "\nblend failed: %s\n", blended.error().to_string().c_str());
    return 1;
  }
  std::printf("\nBLEND   equal-weight oriented within-date ranks, %zu signal(s), %zu rows scored "
              "(>= %zu finite feature(s)/row)\n",
              blended->used.size(), blended->n_scored, blended->required_per_row);
  for (std::size_t k = 0; k < blended->used.size(); ++k) {
    const std::string &n = blended->used[k];
    const FeatureSpec *sp = freg.find(n);
    const bool flipped = std::find(blended->flipped.begin(), blended->flipped.end(), n) !=
                         blended->flipped.end();
    std::printf("  use     %-20s %-9s coverage %6zu / %zu rows%s\n", n.c_str(),
                sp == nullptr ? "" : std::string(to_string(sp->prior)).c_str(),
                blended->used_coverage[k], frame.rows(),
                flipped ? "   FLIPPED — trading AGAINST the catalogue prior" : "");
  }
  for (const std::string &n : blended->refused) {
    std::printf("  REFUSE  %-20s no published sign prior — not fitted\n", n.c_str());
  }
  for (const std::string &n : blended->missing) {
    std::printf("  absent  %-20s no values available\n", n.c_str());
  }

  if (blended->n_scored == 0) {
    std::fprintf(stderr,
                 "\nRESULT  NO ROW SCORED. The blend needs %zu finite feature(s) per row; the\n"
                 "        coverage line above names the binding input. A zero-coverage feature is\n"
                 "        not a defect in the blend -- it is a feature this panel cannot support.\n",
                 blended->required_per_row);
    return 1;
  }

  // ── 6. RUN ────────────────────────────────────────────────────────────────
  auto pnl = args.axis == "rv"       ? forward_rv_vol_points(frame)
             : args.axis == "volchg" ? vol_change_vol_points(frame)
             : args.axis == "dh63"   ? dh63_straddle_pnl_vol_points(frame)
             : args.axis == "dhev"   ? event_straddle_pnl_vol_points(frame, earnings)
                                     : dh_straddle_pnl_vol_points(frame);
  if (!pnl) {
    std::fprintf(stderr, "\nP&L construction failed: %s\n", pnl.error().to_string().c_str());
    return 1;
  }
  StrategyConfig scfg;
  scfg.horizon_sessions = args.horizon;
  scfg.max_names = args.names;
  scfg.max_hspread_frac = args.max_hspread;
  scfg.liquid_hspread_cut = args.liquid_cut;
  scfg.cost_vp_liquid = args.cost_liquid;
  scfg.cost_vp_illiquid = args.cost_illiquid;
  scfg.crossings = args.crossings;
  scfg.require_measured_liquidity = args.require_measured_liq;

  std::vector<double> veto;
  if (args.avoid_earn_days > 0.0) {
    if (earnings.empty()) {
      std::fprintf(stderr, "--avoid-earn-days needs --earnings\n");
      return 2;
    }
    std::vector<std::string> vpats{"f28_days_to_earn"};
    if (args.keep_cheap_prints) {
      vpats.push_back("f31_earn_move_rich");
    }
    const auto vsel = freg.select(vpats);
    if (!vsel) {
      std::fprintf(stderr, "--avoid-earn-days: %s\n", vsel.error().to_string().c_str());
      return 1;
    }
    auto vcomp = evaluate(frame, *vsel, nullptr, &earnings);
    if (!vcomp) {
      std::fprintf(stderr, "--avoid-earn-days: %s\n", vcomp.error().to_string().c_str());
      return 1;
    }
    const std::vector<double> &d2e = vcomp->values.at("f28_days_to_earn");
    const std::vector<double> *rich =
        args.keep_cheap_prints ? &vcomp->values.at("f31_earn_move_rich") : nullptr;
    veto.resize(frame.rows(), 0.0);
    std::size_t vetoed = 0;
    std::size_t kept_cheap = 0;
    for (std::size_t r = 0; r < frame.rows(); ++r) {
      // NaN days = no calendar information; an unknown print date is not an
      // imminent one, so it does not veto.
      if (!std::isfinite(d2e[r]) || d2e[r] >= args.avoid_earn_days) {
        continue;
      }
      // A print priced BELOW the name's own delivered history is the event
      // the long-vega book exists to own; NaN richness (no history) does NOT
      // exempt -- an unmeasured premium is not a cheap one.
      if (rich != nullptr && std::isfinite((*rich)[r]) && (*rich)[r] < 0.0) {
        ++kept_cheap;
        continue;
      }
      veto[r] = 1.0;
      ++vetoed;
    }
    std::printf("\nVETO    %zu / %zu rows within %.0f days of their print — excluded from both "
                "books\n",
                vetoed, frame.rows(), args.avoid_earn_days);
    if (args.keep_cheap_prints) {
      std::printf("        %zu imminent-print rows KEPT: priced below the name's own history "
                  "(f31 < 0)\n",
                  kept_cheap);
    }
  }

  auto card = run(frame, *dates, blended->score, *pnl, scfg,
                  veto.empty() ? std::span<const double>{} : std::span<const double>(veto));
  if (!card) {
    std::fprintf(stderr, "\nbook failed: %s\n", card.error().to_string().c_str());
    return 1;
  }

  std::printf("\nBOOK    long %zu names/date, equal vega, horizon %zu, cost %.2f/%.2f vp x %.1f "
              "crossing(s)\n",
              args.names, args.horizon, args.cost_liquid, args.cost_illiquid, args.crossings);
  std::printf("  dates formed        %zu\n", card->n_dates);
  std::printf("  selected  net       %+8.4f vol pts   t_nw %+6.2f\n", card->mean_selected_net,
              card->t_nw_selected_net);
  std::printf("  floor     net       %+8.4f vol pts   (long every admitted name)\n",
              card->mean_floor_net);
  std::printf("  EXCESS    net       %+8.4f vol pts   t_raw %+6.2f   t_nw %+6.2f\n",
              card->mean_excess_net, card->t_raw_excess_net, card->t_nw_excess_net);
  std::printf("  excess    gross     %+8.4f vol pts\n", card->mean_excess_gross);
  std::printf("  phase sweep (%zu disjoint non-overlapping sub-series, no HAC needed)\n",
              card->phase_mean_excess_net.size());
  std::printf("    positive phases   %.0f%%   min %+.4f   max %+.4f\n",
              100.0 * card->phase_positive_fraction, card->phase_min_mean, card->phase_max_mean);

  // Goyal-Saretto's 5% FDR bound for this literature. Stated so a reader does
  // not have to remember which hurdle applies.
  constexpr double kFdrHurdle = 2.44;
  std::printf("\nVERDICT  hurdle t = %.2f (Goyal-Saretto 5%% FDR). ", kFdrHurdle);
  if (!std::isfinite(card->t_nw_excess_net)) {
    std::printf("t_nw is undefined.\n");
  } else if (card->t_nw_excess_net >= kFdrHurdle && card->phase_positive_fraction >= 0.6) {
    std::printf("CLEARS on both the HAC t and the phase sweep.\n");
  } else if (card->t_nw_excess_net >= kFdrHurdle) {
    std::printf("Clears the HAC t but only %.0f%% of non-overlapping phases are positive —\n"
                "         believe the phase sweep.\n",
                100.0 * card->phase_positive_fraction);
  } else {
    std::printf("DOES NOT CLEAR.\n");
  }

  if (!args.per_date.empty()) {
    std::ofstream out(args.per_date, std::ios::binary);
    if (!out) {
      std::fprintf(stderr, "cannot write '%s'\n", args.per_date.c_str());
      return 1;
    }
    out << "date\tn_rows\tn_admitted\tn_selected\tselected_gross\tselected_net\tfloor_gross\t"
           "floor_net\texcess_gross\texcess_net\n";
    for (const DateResult &r : card->per_date) {
      out << r.date << '\t' << r.n_rows << '\t' << r.n_admitted << '\t' << r.n_selected << '\t'
          << r.selected_gross << '\t' << r.selected_net << '\t' << r.floor_gross << '\t'
          << r.floor_net << '\t' << r.excess_gross << '\t' << r.excess_net << '\n';
    }
    std::printf("\nwrote %s\n", args.per_date.c_str());
  }
  return 0;
}
