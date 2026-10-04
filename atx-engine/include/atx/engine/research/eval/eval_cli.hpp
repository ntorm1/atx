#pragma once

// atx::engine::research::eval -- the `atx-research-eval` executable (platform P9 B2, K-P9-5):
// the statistics of record of one or more daily net series, as one JSON document
// (eval_report.hpp) or its text print.
//
//   atx-research-eval --daily [LABEL=]CSV [--daily ...]
//       [--paired CSV] [--paired-of LABEL=CSV ...] [--one-sided]
//       [--protocol v7|v8] [--draws N] [--block N] [--seed N]
//       [--dsr-n N] [--psr-benchmarks A,B,...] [--psr-alpha A]
//       [--onc] [--onc-series [LABEL=]CSV ...] [--onc-init N] [--onc-seed N]
//       [--pbo] [--pbo-series [LABEL=]CSV ...] [--pbo-blocks N]
//       [--ledger PATH] [--ledger-missing empty|refuse] [--window-id ID]
//       [--trial LABEL=TRIAL_ID ...] [--history LABEL ...] [--hand TRIAL_ID=CSV ...]
//       [--step LABEL=DSR,SE ...] [--threshold C] [--conditional-alpha A]
//       [--json] [--out PATH]
//
// CSV: session_ns,net_return (eval_series.hpp). A label defaults to the path. --protocol v8 sets
// the v8 bootstrap (4,999 draws, seed 20260929) unless --draws / --seed are given; v7 keeps
// 2,000 draws and seed 20260927 (nav_summ.py's defaults). --json prints the document to `out`;
// --out writes it to PATH (the text print still goes to `out` unless --json). The ledger is
// read and verified, never written.
//
// --trial and --history are trusted-caller flags (review B2 S4): the + 1 of house_v1 / tot follows
// them as given (a cell named by --trial whose id is ledgered, or by --history, adds no trial), and
// the verb cannot check them against the series. nav_summ derives both from the NAV dir (trial_id
// of the daily CSV's SHA-256; a history read is a pool scored before TRAIN, Ruling E-41); any other
// caller must do the same.
//
// Exit codes: 0 evaluated; 1 refused (message on `err`); 2 usage.

#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/engine/research/eval/eval_report.hpp"

namespace atx::engine::research::eval {

struct EvalCli {
  EvalRequest request;
  bool json{false};
  std::string out_path;
};

// argv without the program name. InvalidArgument on an unknown flag, a missing or malformed
// value; the request's own checks run in evaluate().
[[nodiscard]] core::Result<EvalCli> parse_eval_args(std::span<const std::string_view> args);

// The JSON text the verb writes (indent 1, sorted keys, NaN as null, trailing newline).
[[nodiscard]] std::string eval_json_text(const nlohmann::json &doc);

// The verb: argv as main() receives it.
[[nodiscard]] int research_eval_main(std::span<const std::string_view> args, std::ostream &out,
                                     std::ostream &err);

} // namespace atx::engine::research::eval
