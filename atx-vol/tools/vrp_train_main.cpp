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
            "                         [--min-train-sessions <n>] (default 252)\n"
            "                         [--test-sessions <n>]      (default 63)\n"
            "                         [--step-sessions <n>]      (default 63)\n"
            "                         [--lambda <f>]             (default 1e-3)\n"
            "                         [--alpha <f>]              (default 0.5)\n"
            "\n"
            "Trains the log-HAR elastic-net baseline ({f0,f1,f2}, log space,\n"
            "exp(s^2/2) retransform + insanity clip) and the pooled 10-feature\n"
            "GBT over a purged/embargoed walk-forward, then writes\n"
            "vrp_signal.tsv (frozen vrp_signal_v1), vrp_metrics.tsv, and the\n"
            "serialized schema-2 model files into --out-dir.");
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
    } else if (arg == "--test-sessions") {
      ok = parse_number(value, cfg.walk.test_sessions);
    } else if (arg == "--step-sessions") {
      ok = parse_number(value, cfg.walk.step_sessions);
    } else if (arg == "--lambda") {
      ok = parse_number(value, cfg.en_lambda);
    } else if (arg == "--alpha") {
      ok = parse_number(value, cfg.en_alpha);
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

  const auto report = atx::vol::vrp::run_vrp_train(cfg);
  if (!report.has_value()) {
    std::fprintf(stderr, "atx-vol-vrp-train: %s\n", report.error().to_string().c_str());
    return 1;
  }

  std::printf("fold\tn_train\tn_test\tqlike_baseline\tqlike_gbt\tqlike_mean\tic_baseline\t"
              "ic_gbt\n");
  for (const auto &fold : report->folds) {
    std::printf("%u\t%zu\t%zu\t%.6g\t%.6g\t%.6g\t%.4f\t%.4f\n", fold.fold_id, fold.n_train,
                fold.n_test, fold.qlike_baseline, fold.qlike_gbt, fold.qlike_mean_forecast,
                fold.ic_baseline, fold.ic_gbt);
  }
  std::printf("signal:   %s\n", report->signal_path.string().c_str());
  std::printf("metrics:  %s\n", report->metrics_path.string().c_str());
  std::printf("gbt:      %s\n", report->gbt_model_path.string().c_str());
  std::printf("baseline: %s\n", report->baseline_model_path.string().c_str());
  return 0;
}
