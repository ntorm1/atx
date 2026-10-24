// atx-vol-risk-bench — intraday risk-engine benchmark over the SpiderRock store
// layout (design 2026-10-24 §5.8; CLI and report schema in docs/risk-bench.md).
// argv -> RiskBenchArgs -> run_risk_bench -> JSON (+ markdown). Exit codes: 0 ok,
// 1 runtime error, 2 usage error.

#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "risk_bench.hpp"

int main(int argc, char **argv) {
  namespace rb = atx::vol::riskbench;
  // argc >= 0 by the language; the copy gives argv the const-correct element type.
  const std::vector<const char *> args_in(argv, argv + argc);
  const auto args = rb::parse_risk_bench_args(std::span<const char *const>(args_in));
  if (!args.has_value()) {
    std::cerr << "atx-vol-risk-bench: " << args.error().to_string() << "\n"
              << rb::risk_bench_usage();
    return 2;
  }
  if (args->help) {
    std::cout << rb::risk_bench_usage();
    return 0;
  }
  const auto report = rb::run_risk_bench(*args, std::cerr);
  if (!report.has_value()) {
    std::cerr << "atx-vol-risk-bench: " << report.error().to_string() << "\n";
    return 1;
  }
  const std::string json = rb::report_to_json(*report);
  if (args->out_json.empty()) {
    std::cout << json << "\n";
  } else if (const auto st = rb::write_text_file(args->out_json, json); !st.has_value()) {
    std::cerr << "atx-vol-risk-bench: " << st.error().to_string() << "\n";
    return 1;
  }
  if (!args->out_md.empty()) {
    if (const auto st = rb::write_text_file(args->out_md, rb::report_to_markdown(*report));
        !st.has_value()) {
      std::cerr << "atx-vol-risk-bench: " << st.error().to_string() << "\n";
      return 1;
    }
  }
  return 0;
}
