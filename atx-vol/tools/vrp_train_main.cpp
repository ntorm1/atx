// atx-vol-vrp-train — thin argv shell over tools/vrp_train.hpp (lane
// vrp-model). All logic (frozen vrp_panel_v1 parsing, purged/embargoed
// walk-forward, log-HAR elastic-net baseline, pooled GBT, frozen
// vrp_signal_v1 output, schema-2 model serialization) lives in the logic
// header, gate-tested by VrpTrain* in tests/vrp_model_test.cpp; this TU only
// parses flags, calls run_vrp_train, and prints the fold metrics table.

#include "vrp_train.hpp"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
  std::puts("usage: atx-vol-vrp-train --panel <vrp_panel_v1.tsv> --out-dir <dir>\n"
            "                         [--master-seed <u64>]      (default 42)\n"
            "                         [--min-train-sessions <n>]\n"
            "                         [--test-sessions <n>]\n"
            "                         [--step-sessions <n>]\n"
            "                         [--max-label-span <n>]     (default 42; >= 21)\n"
            "                         [--lambda <f>]             (default 1e-3)\n"
            "                         [--alpha <f>]              (default 0.5)\n"
            "                         [--recalibrate <off|isotonic>] (default off)\n"
            "                         [--recalib-window <n>]     (default 63; >= 1)\n"
            "                         [--retransform <jensen|smearing>] (default jensen)\n"
            "                         [--edge-norm <cross-section|per-symbol>]\n"
            "                                                    (default cross-section)\n"
            "                         [--feature-lag <n>]        (default 0; <= 21)\n"
            "                         [--short-vega-haircut <f>] (default 0.50; in [0,1])\n"
            "                         [--cost-crossing-fraction <f>] (default 0.55; (0,1])\n"
            "                         [--eiv-target-entry-lag <n>] (default 0; <= 21)\n"
            "                         [--corpus <label>]         (default: panel file stem)\n"
            "\n"
            "--panel accepts vrp_panel_v1 (18 columns) and vrp_panel_v2 (v1 then\n"
            "iv_atmf_21d, the ATM-FORWARD implied vol a straddle is struck and\n"
            "marked at). The round-5 iv-change axes and the VEGA book need v2; on a\n"
            "v1 panel they are undefined rather than silently built on the\n"
            "variance-swap strip strike iv_fair_21d, which sits ~4.24 vol points\n"
            "above the ATM-forward point on 99.5% of rows.\n"
            "\n"
            "--short-vega-haircut discounts a POSITIVE short-vega edge in the\n"
            "asymmetric objective and NEVER discounts a short-vega loss. Short vol\n"
            "carries unbounded single-name gap risk, negative skew and left-tail\n"
            "correlation across names, and its Sharpe is exactly the payoff\n"
            "Goetzmann-Ingersoll-Spiegel-Welch (RFS 2007) prove is maximised by\n"
            "option selling that adds no value; Faias-Santa-Clara (JFQA 2017)\n"
            "exhibit a Sharpe-0.47 option book with a certainty equivalent of\n"
            "-100%. 0.50 is a STATED JUDGEMENT, not a measurement: it makes a\n"
            "short-vega edge count half. 0 prints the undiscounted book.\n"
            "\n"
            "--edge-norm picks the basis pred_edge_norm standardizes on -- the column\n"
            "the VolEdge book RANKS on. cross-section (the round-4 DEFAULT)\n"
            "standardizes pred_label WITHIN each date across symbols, an\n"
            "order-preserving map, so the book ranks on the axis the IC is measured\n"
            "on. per-symbol is the round-1..3 z-score (pred_label - label_mean[sym])\n"
            "/ label_sd[sym], which demeans away each name's own variance premium --\n"
            "the quantity being harvested -- and reproduces round-3 artifacts byte\n"
            "for byte. Measured on the same rows, dates and 21d horizon through an\n"
            "equal-weighted decile long/short book: per-symbol -1.63 vol pts/cycle\n"
            "(29% of phase offsets positive), cross-section +1.74 (76% positive).\n"
            "\n"
            "--feature-lag N reads every FEATURE from the row's N-th same-symbol\n"
            "predecessor (targets untouched, so the fold plan is identical at every\n"
            "lag). 2 is the recommended round-4 setting: it removes the channel by\n"
            "which a stale same-session quote can manufacture IC. 0 reproduces the\n"
            "round-1..3 behaviour. Rows without an N-th predecessor get NaN features\n"
            "and are counted in feature_lag_rows_unavailable.\n"
            "\n"
            "--cost-crossing-fraction is the fraction of the DERIVED QUOTED option\n"
            "spread the vega book crosses. The measured input is an EFFECTIVE spread\n"
            "(Christoffersen et al. RFS 2018, ATM call 6.41% of premium, halved\n"
            "one-way), so the crossing discount is ALREADY INSIDE IT; the quoted\n"
            "width is derived by dividing it by Zhan-Han-Cao-Tong (RFS 2022) realized\n"
            "effective/quoted of 0.55, measured on actual OPRA prints 2003-2016. The\n"
            "default therefore returns the measured effective charge bit-exactly. Do\n"
            "NOT apply the ORATS complex-order 0.53 on top of it -- that counts the\n"
            "same discount twice and hands the book a free 47% cost cut. 0.38 is\n"
            "Muravyev-Pearson patient-algo, 1.00 the full-quoted stress corner; below\n"
            "0.30 is unsupported by any published measurement.\n"
            "\n"
            "--eiv-target-entry-lag N rebuilds the iv-change TARGET with its ENTRY leg\n"
            "read N same-symbol sessions earlier, exit leg unmoved. It is a\n"
            "DIAGNOSTIC, not a tradeable configuration (N>0 is a 21+N session hold):\n"
            "whichever surface read sits inside the target is the one that scores, so\n"
            "a column whose edge survives moving the target's entry read away from its\n"
            "own has a forecast, and one whose edge migrates does not. Run it against\n"
            "every column you intend to gate.\n"
            "\n"
            "--corpus labels every gate statistic in vrp_metrics.tsv. Rounds 1-3\n"
            "quoted a clean-25 IC beside an SP100 book because no artifact recorded\n"
            "which corpus a number came from.\n"
            "\n"
            "EVERY run grades every score column on THREE targets and prints a\n"
            "PASS/FAIL verdict. The gating target is rv_fwd_21d, the label's\n"
            "REALIZED leg; ln(rv_fwd_21d/rv_trail_21d) is reported as the one axis\n"
            "with no iv_fair in it; the composite label is reported and LABELLED\n"
            "CONTAMINATED. The gate PASSES only when the model beats EVERY\n"
            "zero-parameter benchmark (f5_hv_iv_gap, the Goyal-Saretto HV-IV\n"
            "classic) on the rv_fwd_21d mean per-date Pearson IC (the Grinold\n"
            "sizing IC), the rv_fwd_21d mean per-date Spearman IC, AND the\n"
            "IV-quintile-neutralised P&L measured in EXCESS OF THE\n"
            "SHORT-EVERYTHING FLOOR -- which the model's own excess must also\n"
            "clear outright. It fails closed: an unmeasurable comparison is a FAIL.\n"
            "\n"
            "-iv_fair_21d IS NOT A BENCHMARK and must never be reinstated as one.\n"
            "The label is (rv_fwd_21d^2 - iv_fair_21d^2)*21/252 and iv_fair_21d > 0\n"
            "on every labeled row, so -iv_fair_21d is a PERFECT rank transform of\n"
            "the label's own implied leg: rank IC exactly +1.0000, by algebra, on\n"
            "any dataset, forever. Against rv_fwd_21d it is an ANTI-forecaster at\n"
            "-0.6128 (t_nw -22.93). It is still scored and published, as\n"
            "contaminated_neg_iv_fair_21d, and cannot decide a verdict. The defect\n"
            "generalises to ANY score shaped (variance_forecast - iv_fair^2). See\n"
            ".superpowers/sdd/2026-08-15-vrp-ml/audit-benchmark-contamination.md\n"
            "\n"
            "All P&L is vol points per 1 unit of GROSS vega per cycle, from\n"
            "ppv = 100*(rv_fwd_21d^2 - iv_fair_21d^2)/(2*iv_fair_21d), |ppv| capped\n"
            "at 60 with the capped-row count published. EVERY P&L figure is printed\n"
            "beside its excess over the short-everything floor (shorting the whole\n"
            "cross-section blind, computed from this run's own rows): raw carry is\n"
            "mostly short-vol beta, and quoting it alone is how that beta was read\n"
            "as selection skill for three rounds.\n"
            "\n"
            "ROUND 5 adds the TRADEABLE vol-change axes and a second, equally\n"
            "strict VEGA GATE. iv_chg_21d_raw = 100*(iv_atmf_21d[t+21] -\n"
            "iv_atmf_21d[t]) vol points. That RAW axis IS REPORTED AND NEVER GATED:\n"
            "iv_atmf_21d is a CONSTANT-MATURITY index, the 21d option bought at t\n"
            "has expired by t+21, and the strongest free predictor of the raw axis\n"
            "is the term-structure roll a real constant-maturity position pays\n"
            "(f4_term_slope scores rank IC +0.4764, t_nw +17.51, against the raw\n"
            "axis and +0.2038 against the roll-adjusted one). iv_chg_21d_roll\n"
            "subtracts that roll, 100*(iv_fair_63d[t]-iv_fair_21d[t])/2, and is the\n"
            "ONLY iv-change axis the money is graded on.\n"
            "\n"
            "BOTH LEGS OF THE IV-CHANGE TARGET ARE FITTED SURFACE READS, so any\n"
            "predictor built from the same ENTRY read inherits its error with the\n"
            "target's own sign. Measured: -iv_atmf_21d scores +0.2537 read at t and\n"
            "+0.1182 read at t-2; rebuilding the TARGET with a t-2 entry leg moves\n"
            "the advantage to -iv_atmf_21d[t-2] (+0.2535) from -iv_atmf_21d[t]\n"
            "(+0.1068). Whichever surface read sits inside the target is the one\n"
            "that scores. Run --feature-lag 2 before trusting any level-ranked\n"
            "column on these axes (Duarte-Jones-Wang errors-in-variables).\n"
            "\n"
            "VEGA P&L is vol points per 1u GROSS vega per cycle, NET OF COSTS:\n"
            "a one-way charge of 3.205% of premium (Christoffersen-Goyenko-Jacobs-\n"
            "Karoui RFS 2018 ATM effective relative spread, halved) times 100*iv,\n"
            "charged at entry AND exit. Its floor is NOT short-everything -- a\n"
            "vega book is not structurally short vol -- but the better of LONG\n"
            "everything and SHORT everything, chosen once from this run's own rows\n"
            "and published with both. Every vega figure carries its long-leg and\n"
            "short-leg split, each with its own t_nw and its own excess over the\n"
            "SAME-DIRECTION zero-selection alternative, because a net number can\n"
            "hide a dead long leg.\n"
            "\n"
            "--recalibrate isotonic fits, per fold, a deterministic PAVA isotonic\n"
            "map from raw GBT forecast to realized label on the trailing\n"
            "--recalib-window ADMITTED train sessions (capped at half the fold's\n"
            "train sessions; out-of-sample via a temporal-holdout calibration\n"
            "model) and applies it to the fold's raw test forecasts. Monotone =>\n"
            "rank-preserving; fit rows are admitted train rows, so the fit uses\n"
            "only data strictly before the fold's test start and the fold plan is\n"
            "untouched. Behind the flag the recalibrated values flow through the\n"
            "EXISTING pred_label/pred_edge_norm columns (schema unchanged; value\n"
            "semantics in CHANGELOG). QLIKE + Mincer-Zarnowitz slope/intercept +\n"
            "rank IC are reported before AND after per fold (meta lines + the\n"
            "recalibration table below the fold table).\n"
            "\n"
            "--retransform smearing swaps the baseline's exp(s^2/2) lognormal\n"
            "retransform for the Duan smearing factor mean(exp(resid))\n"
            "(trainer-side only; the sidecar contract stays jensen-based).\n"
            "\n"
            "--max-label-span caps each labeled row's label-window span in POOLED\n"
            "sessions (decision row -> its own 21st emitted successor). Rows past\n"
            "the cap are rejected and counted (rejected_rows_span_cap), so one\n"
            "sparse symbol cannot stretch the global embargo and purge the corpus;\n"
            "label_end itself stays the CONSERVATIVE emitted-axis end (never\n"
            "understates a bar-holey symbol's true window).\n"
            "\n"
            "Walk-forward defaults AUTO-SCALE to the panel's labeled session\n"
            "depth (252/63/63 when history allows; floors 84/21/21, so fewer\n"
            "than 105 labeled sessions fails closed). Passing ANY of the three\n"
            "walk flags disables auto-scaling; an omitted flag then falls back\n"
            "to its 252/63/63 default.\n"
            "\n"
            "Trains the log-HAR elastic-net baseline ({f0,f1,f2}, log space,\n"
            "exp(s^2/2) retransform + insanity clip) and the pooled 10-feature\n"
            "GBT over a purged/embargoed walk-forward, then writes\n"
            "vrp_signal.tsv (frozen vrp_signal_v1; vov_63d always finite --\n"
            "a NaN f9 imputes the scoring fold's per-asset train mean),\n"
            "vrp_metrics.tsv (meta lines: t+21 + span-cap rejection counters,\n"
            "per-fold purge/embargo-removed train counts, per-fold GBT clip\n"
            "count + post-clip extrema), vrp_fold_stats.tsv (per-fold\n"
            "standardization/label/retransform sidecar), and the serialized\n"
            "schema-2 model files into --out-dir.");
}

template <typename T>
[[nodiscard]] bool parse_number(std::string_view text, T &out) {
  const char *first = text.data();
  const char *last = text.data() + text.size();
  const auto r = std::from_chars(first, last, out);
  return r.ec == std::errc{} && r.ptr == last;
}

} // namespace

int main(int argc, char **argv) {
  atx::vol::vrp::VrpTrainConfig cfg;
  cfg.walk.min_train_sessions = 252;
  cfg.walk.test_sessions = 63;
  cfg.walk.step_sessions = 63;
  bool any_walk_flag = false;

  const std::vector<std::string_view> args(argv + 1, argv + argc);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    const bool has_value = i + 1 < args.size();
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (!has_value) {
      std::fprintf(stderr, "error: flag '%.*s' needs a value\n", static_cast<int>(arg.size()),
                   arg.data());
      print_usage();
      return 2;
    }
    const std::string_view value = args[++i];
    bool ok = true;
    if (arg == "--panel") {
      cfg.panel_path = std::string(value);
    } else if (arg == "--out-dir") {
      cfg.out_dir = std::string(value);
    } else if (arg == "--master-seed") {
      ok = parse_number(value, cfg.master_seed);
    } else if (arg == "--min-train-sessions") {
      ok = parse_number(value, cfg.walk.min_train_sessions);
      any_walk_flag = true;
    } else if (arg == "--test-sessions") {
      ok = parse_number(value, cfg.walk.test_sessions);
      any_walk_flag = true;
    } else if (arg == "--step-sessions") {
      ok = parse_number(value, cfg.walk.step_sessions);
      any_walk_flag = true;
    } else if (arg == "--max-label-span") {
      ok = parse_number(value, cfg.max_label_span_sessions);
      // Fail closed at the boundary: every label window spans >= the
      // 21-session horizon, so a smaller cap would reject every row.
      if (ok && cfg.max_label_span_sessions < atx::vol::vrp::kVrpHorizonSessions) {
        std::fprintf(stderr, "error: --max-label-span %zu is below the %zu-session horizon\n",
                     cfg.max_label_span_sessions, atx::vol::vrp::kVrpHorizonSessions);
        return 2;
      }
    } else if (arg == "--lambda") {
      ok = parse_number(value, cfg.en_lambda);
    } else if (arg == "--alpha") {
      ok = parse_number(value, cfg.en_alpha);
    } else if (arg == "--recalibrate") {
      if (value == "off") {
        cfg.recalibrate = atx::vol::vrp::VrpRecalMode::Off;
      } else if (value == "isotonic") {
        cfg.recalibrate = atx::vol::vrp::VrpRecalMode::Isotonic;
      } else {
        std::fprintf(stderr, "error: --recalibrate must be 'off' or 'isotonic', got '%.*s'\n",
                     static_cast<int>(value.size()), value.data());
        return 2;
      }
    } else if (arg == "--recalib-window") {
      ok = parse_number(value, cfg.recalib_window_sessions);
      // Fail closed at the boundary: a zero window cannot calibrate.
      if (ok && cfg.recalib_window_sessions == 0) {
        std::fprintf(stderr, "error: --recalib-window must be >= 1\n");
        return 2;
      }
    } else if (arg == "--edge-norm") {
      if (value == "cross-section") {
        cfg.edge_norm = atx::vol::vrp::VrpEdgeNormMode::CrossSection;
      } else if (value == "per-symbol") {
        cfg.edge_norm = atx::vol::vrp::VrpEdgeNormMode::PerSymbol;
      } else {
        std::fprintf(stderr,
                     "error: --edge-norm must be 'cross-section' or 'per-symbol', got '%.*s'\n",
                     static_cast<int>(value.size()), value.data());
        return 2;
      }
    } else if (arg == "--feature-lag") {
      ok = parse_number(value, cfg.feature_lag);
      // Fail closed at the boundary: a lag past the cap blanks whole symbols.
      if (ok && cfg.feature_lag > atx::vol::vrp::kVrpMaxFeatureLag) {
        std::fprintf(stderr, "error: --feature-lag %zu exceeds the %zu-session cap\n",
                     cfg.feature_lag, atx::vol::vrp::kVrpMaxFeatureLag);
        return 2;
      }
    } else if (arg == "--short-vega-haircut") {
      ok = parse_number(value, cfg.short_vega_haircut);
      // Fail closed at the boundary: a haircut outside [0,1] is not a discount.
      if (ok && !(cfg.short_vega_haircut >= 0.0 && cfg.short_vega_haircut <= 1.0)) {
        std::fprintf(stderr, "error: --short-vega-haircut must be in [0, 1]\n");
        return 2;
      }
    } else if (arg == "--cost-crossing-fraction") {
      ok = parse_number(value, cfg.cost_crossing_fraction);
      // Fail closed at the boundary: 0 is not "costs off", it is a free lunch.
      if (ok && !(cfg.cost_crossing_fraction > 0.0 && cfg.cost_crossing_fraction <= 1.0)) {
        std::fprintf(stderr, "error: --cost-crossing-fraction must be in (0, 1]\n");
        return 2;
      }
    } else if (arg == "--eiv-target-entry-lag") {
      ok = parse_number(value, cfg.eiv_target_entry_lag);
      if (ok && cfg.eiv_target_entry_lag > atx::vol::vrp::kVrpMaxFeatureLag) {
        std::fprintf(stderr, "error: --eiv-target-entry-lag %zu exceeds the %zu-session cap\n",
                     cfg.eiv_target_entry_lag, atx::vol::vrp::kVrpMaxFeatureLag);
        return 2;
      }
    } else if (arg == "--corpus") {
      cfg.corpus = std::string(value);
    } else if (arg == "--retransform") {
      if (value == "jensen") {
        cfg.retransform = atx::vol::vrp::VrpRetransformMode::Jensen;
      } else if (value == "smearing") {
        cfg.retransform = atx::vol::vrp::VrpRetransformMode::Smearing;
      } else {
        std::fprintf(stderr,
                     "error: --retransform must be 'jensen' or 'smearing', got '%.*s'\n",
                     static_cast<int>(value.size()), value.data());
        return 2;
      }
    } else {
      std::fprintf(stderr, "error: unknown flag '%.*s'\n", static_cast<int>(arg.size()),
                   arg.data());
      print_usage();
      return 2;
    }
    if (!ok) {
      std::fprintf(stderr, "error: unparseable value '%.*s' for '%.*s'\n",
                   static_cast<int>(value.size()), value.data(), static_cast<int>(arg.size()),
                   arg.data());
      return 2;
    }
  }

  if (cfg.panel_path.empty() || cfg.out_dir.empty()) {
    print_usage();
    return 2;
  }
  // No explicit walk flag => derive the fold plan from the panel's labeled
  // session depth (see derive_vrp_walk_forward); any explicit flag pins the
  // requested plan and fails closed when the history cannot carry it.
  cfg.walk_auto = !any_walk_flag;

  const auto report = atx::vol::vrp::run_vrp_train(cfg);
  if (!report.has_value()) {
    std::fprintf(stderr, "atx-vol-vrp-train: %s\n", report.error().to_string().c_str());
    return 1;
  }

  // Rejection accounting -- the same counters the metrics meta lines carry,
  // surfaced on stderr so an attrition regression is visible in logs.
  std::fprintf(stderr,
               "[vrp-train] labeled_rows=%zu rejected_rows_no_t21=%zu "
               "rejected_rows_span_cap=%zu symbols_fully_rejected=%zu\n",
               report->observations.n_labeled_rows,
               report->observations.n_rows_rejected_no_t21,
               report->observations.n_rows_rejected_span_cap,
               report->observations.n_symbols_fully_rejected);

  // gbt_clipped + post-clip extrema and the purge/embargo losses mirror the
  // metrics meta lines (round-2 review majors 2 + 3) -- a saturated clip or
  // a purge-dominated fold is visible from the summary alone.
  std::printf("fold\tn_train\tn_test\tpurged\tembargoed\tqlike_baseline\tqlike_gbt\t"
              "qlike_mean\tic_baseline\tic_gbt\tgbt_clipped\tgbt_fcast_min\tgbt_fcast_max\n");
  for (const auto &fold : report->folds) {
    std::printf("%u\t%zu\t%zu\t%zu\t%zu\t%.6g\t%.6g\t%.6g\t%.4f\t%.4f\t%zu\t%.6g\t%.6g\n",
                fold.fold_id, fold.n_train, fold.n_test, fold.n_train_purged,
                fold.n_train_embargoed, fold.qlike_baseline, fold.qlike_gbt,
                fold.qlike_mean_forecast, fold.ic_baseline, fold.ic_gbt,
                fold.n_gbt_forecast_clipped, fold.gbt_test_forecast_min,
                fold.gbt_test_forecast_max);
  }
  // Round-3 metrics honesty (research digest Q4): Mincer-Zarnowitz level
  // diagnostics per fold -- raw always; the before/after recalibration table
  // only when --recalibrate is on. QLIKE alone can favor positively biased
  // forecasts, so slope/intercept sit beside it in every mode.
  std::printf("fold\tmz_slope_raw\tmz_intercept_raw\tsmear_factor\n");
  for (const auto &fold : report->folds) {
    std::printf("%u\t%.4f\t%.6g\t%.6g\n", fold.fold_id, fold.mz_slope_raw,
                fold.mz_intercept_raw, fold.smear_factor);
  }
  if (cfg.recalibrate == atx::vol::vrp::VrpRecalMode::Isotonic) {
    std::printf("recal\tfold\tqlike_raw\tqlike_recal\tmz_slope_raw\tmz_slope_recal\t"
                "mz_int_raw\tmz_int_recal\tic_raw\tic_recal\tn_fit\twindow\tapplied\n");
    for (const auto &fold : report->folds) {
      std::printf("recal\t%u\t%.6g\t%.6g\t%.4f\t%.4f\t%.6g\t%.6g\t%.4f\t%.4f\t%zu\t%zu\t%d\n",
                  fold.fold_id, fold.qlike_gbt, fold.qlike_gbt_recal, fold.mz_slope_raw,
                  fold.mz_slope_recal, fold.mz_intercept_raw, fold.mz_intercept_recal,
                  fold.ic_gbt, fold.ic_gbt_recal, fold.recal_n_fit,
                  fold.recal_window_effective, fold.recal_applied ? 1 : 0);
    }
  }
  // ── Round-4 benchmark gate (F2) + honest metrics (F3) ─────────────────────
  // Printed for EVERY run, never behind a flag: this block exists because
  // three rounds shipped on an IC no free rule was asked to beat. `qlike`
  // above is retained only for round-3 comparability -- it is undefined on a
  // signed variance spread and the gate never reads it.
  const auto &gate = report->gate;
  const auto score_row = [](const char *scope, const atx::vol::vrp::VrpScoreReport &s) {
    std::printf("gate\t%s\t%s\t%s\t%.4f\t%.2f\t%.2f\t%.4f\t%.2f\t%.2f\t%.4f\t%.2f\t%.6g\t%.2f\t"
                "%.4f\t%zu\t%zu\n",
                scope, s.name.c_str(),
                std::string{atx::vol::vrp::vrp_target_axis_key(s.target)}.c_str(),
                s.ic_pearson, s.ic_pearson_t, s.ic_pearson_t_nw, s.ic_spearman,
                s.ic_spearman_t, s.ic_spearman_t_nw, s.ic_pearson_traded,
                s.ic_pearson_traded_t_nw, s.decile_spread, s.decile_spread_t_nw,
                s.decile_rho, s.n_dates, s.n_rows);
  };
  // Every IC names its TARGET. A bare ic_spearman is the key that let a
  // +1.0000 algebraic identity read as a forecast for a whole round.
  std::printf("gate\tscope\tscore\ttarget\tic_pearson\tt\tt_nw\tic_spearman\tt\tt_nw\t"
              "ic_pear_traded\tt_nw\tdecile_spread\tt_nw\tdecile_rho\tn_dates\tn_rows\n");
  for (const auto &f : gate.per_fold) {
    const std::string scope = "fold_" + std::to_string(f.fold_id);
    for (const auto &s : f.scores) {
      score_row(scope.c_str(), s);
    }
  }
  for (const auto &s : gate.pooled) {
    score_row("pooled", s);
  }
  // Money, in vol points per 1u GROSS VEGA per cycle. The raw figure is never
  // printed without its excess over the short-everything floor beside it:
  // short-vol beta was read as selection skill for three rounds.
  std::printf("pnl\tscope\tscore\tbook\tvol_pts_gross_vega\tt\tt_nw\texcess_over_floor\tt\t"
              "t_nw\tn_dates\n");
  const auto pnl_row = [](const char *scope, const char *name, const char *book,
                          const atx::vol::vrp::VrpPnlAgg &a) {
    std::printf("pnl\t%s\t%s\t%s\t%+.3f\t%.2f\t%.2f\t%+.3f\t%.2f\t%.2f\t%zu\n", scope, name,
                book, a.mean, a.t_iid, a.t_nw, a.excess, a.excess_t, a.excess_t_nw,
                a.n_dates);
  };
  std::printf("pnl\tpooled\tfloor_short_everything\t--\t%+.3f\t%.2f\t%.2f\t%+.3f\t%.2f\t%.2f\t"
              "%zu\n",
              gate.pooled_floor.mean, gate.pooled_floor.t_iid, gate.pooled_floor.t_nw, 0.0,
              0.0, 0.0, gate.pooled_floor.n_dates);
  for (const auto &p : gate.pooled_pnl) {
    pnl_row("pooled", p.name.c_str(), "decile", p.decile);
    pnl_row("pooled", p.name.c_str(), "iv_neutral", p.iv_neutral);
  }
  std::printf("pnl\tppv_winsor_abs=%.1f\trows_priced=%zu\trows_winsorized=%zu\n",
              atx::vol::vrp::kVrpPpvWinsorAbs, gate.n_ppv_rows_priced,
              gate.n_ppv_rows_winsorized);
  std::printf("gate\tcorpus=%s\tsignal_rows=%zu\tunlabeled_tail=%zu (%.1f%% never validated)\n",
              gate.corpus.c_str(), gate.n_signal_rows, gate.n_signal_rows_unlabeled,
              100.0 * gate.frac_unlabeled());
  // The verdict goes to BOTH streams and says the word out loud. A model that
  // loses to a zero-parameter rule has produced no evidence it should ship.
  const char *verdict = gate.verdict.pass ? "PASS" : "FAIL";
  std::printf("GATE VERDICT: %s -- model '%s' vs rv_fwd_21d ic_pearson=%.4f ic_spearman=%.4f, "
              "iv-neutral P&L excess over the +%.3f short-everything floor = %+.3f; best free "
              "benchmark '%s' ic_pearson=%.4f ic_spearman=%.4f excess=%+.3f (%zu benchmarks)\n",
              verdict, gate.verdict.model.c_str(), gate.verdict.model_ic_pearson,
              gate.verdict.model_ic_spearman, gate.verdict.pnl_floor,
              gate.verdict.model_pnl_excess, gate.verdict.best_benchmark.c_str(),
              gate.verdict.best_benchmark_ic_pearson, gate.verdict.best_benchmark_ic_spearman,
              gate.verdict.best_benchmark_pnl_excess, gate.verdict.n_benchmarks);
  if (!gate.verdict.pass) {
    std::fprintf(stderr,
                 "[vrp-train] GATE VERDICT: FAIL -- '%s' does not beat the zero-parameter "
                 "benchmark '%s' on rv_fwd_21d Pearson AND Spearman IC AND IV-neutralised P&L "
                 "in excess of the short-everything floor. The free rule wins; this model is "
                 "not evidence of skill.\n",
                 gate.verdict.model.c_str(), gate.verdict.best_benchmark.c_str());
  } else {
    std::fprintf(stderr,
                 "[vrp-train] GATE VERDICT: PASS -- '%s' beats all %zu zero-parameter "
                 "benchmarks on rv_fwd_21d Pearson and Spearman IC and on IV-neutralised P&L, "
                 "and clears the short-everything floor by %+.3f vol pts / 1u gross vega.\n",
                 gate.verdict.model.c_str(), gate.verdict.n_benchmarks,
                 gate.verdict.model_pnl_excess);
  }
  std::fprintf(stderr,
               "[vrp-train] the composite label is CONTAMINATED as a ranking target "
               "(-iv_fair_21d scores +1.0000 against its implied leg by algebra and -0.6128 "
               "against rv_fwd_21d); it is reported, never gated. See "
               ".superpowers/sdd/2026-08-15-vrp-ml/audit-benchmark-contamination.md\n");
  std::fprintf(stderr, "[vrp-train] edge_norm=%s feature_lag=%zu lag_rows_unavailable=%zu\n",
               cfg.edge_norm == atx::vol::vrp::VrpEdgeNormMode::PerSymbol ? "per_symbol"
                                                                          : "cross_section",
               cfg.feature_lag, report->feature_lag_rows_unavailable);

  std::printf("signal:     %s\n", report->signal_path.string().c_str());
  std::printf("metrics:    %s\n", report->metrics_path.string().c_str());
  std::printf("gbt:        %s\n", report->gbt_model_path.string().c_str());
  std::printf("baseline:   %s\n", report->baseline_model_path.string().c_str());
  std::printf("fold_stats: %s\n", report->fold_stats_path.string().c_str());
  return 0;
}
