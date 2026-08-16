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
  std::printf("signal:     %s\n", report->signal_path.string().c_str());
  std::printf("metrics:    %s\n", report->metrics_path.string().c_str());
  std::printf("gbt:        %s\n", report->gbt_model_path.string().c_str());
  std::printf("baseline:   %s\n", report->baseline_model_path.string().c_str());
  std::printf("fold_stats: %s\n", report->fold_stats_path.string().c_str());
  return 0;
}
