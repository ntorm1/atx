#include "atx/engine/research/eval/eval_cli.hpp"

#include <charconv>
#include <fstream>
#include <ios>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace atx::engine::research::eval {
namespace {

constexpr std::string_view kUsage =
    "usage: atx-research-eval --daily [LABEL=]CSV [--daily ...] [--paired CSV]\n"
    "         [--paired-of LABEL=CSV ...] [--one-sided] [--protocol v7|v8] [--draws N]\n"
    "         [--block N] [--seed N] [--dsr-n N] [--psr-benchmarks A,B] [--psr-alpha A]\n"
    "         [--onc] [--onc-series [LABEL=]CSV ...] [--onc-init N] [--onc-seed N]\n"
    "         [--pbo] [--pbo-series [LABEL=]CSV ...] [--pbo-blocks N]\n"
    "         [--ledger PATH] [--ledger-missing empty|refuse] [--window-id ID]\n"
    "         [--trial LABEL=TRIAL_ID ...] [--history LABEL ...] [--hand TRIAL_ID=CSV ...]\n"
    "         [--step LABEL=DSR,SE ...] [--threshold C] [--conditional-alpha A]\n"
    "         [--json] [--out PATH] [--vol]\n";

[[nodiscard]] core::Error usage_error(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "atx-research-eval: " + std::move(message));
}

template <class T> [[nodiscard]] std::optional<T> parse_uint(std::string_view s) {
  T v{};
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || s.empty()) {
    return std::nullopt;
  }
  return v;
}

[[nodiscard]] std::optional<f64> parse_f64(std::string_view s) {
  f64 v{};
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || s.empty()) {
    return std::nullopt;
  }
  return v;
}

// LABEL=VALUE, or VALUE alone with the value as its label when `label_optional`.
[[nodiscard]] std::optional<LabeledPath> labeled(std::string_view s, bool label_optional) {
  const usize eq = s.find('=');
  if (eq == std::string_view::npos) {
    if (!label_optional || s.empty()) {
      return std::nullopt;
    }
    return LabeledPath{std::string(s), std::string(s)};
  }
  if (eq == 0U || eq + 1U == s.size()) {
    return std::nullopt;
  }
  return LabeledPath{std::string(s.substr(0, eq)), std::string(s.substr(eq + 1U))};
}

[[nodiscard]] std::optional<std::vector<f64>> parse_list(std::string_view s) {
  std::vector<f64> out;
  usize begin = 0;
  for (usize guard = 0; guard <= s.size(); ++guard) {
    const usize end = s.find(',', begin);
    const std::string_view item = s.substr(begin, end == std::string_view::npos ? s.npos
                                                                                : end - begin);
    if (item.find_first_not_of(" \t") != std::string_view::npos) {
      const std::optional<f64> v = parse_f64(item);
      if (!v) {
        return std::nullopt;
      }
      out.push_back(*v);
    }
    if (end == std::string_view::npos) {
      return out;
    }
    begin = end + 1U;
  }
  return out;
}

[[nodiscard]] std::optional<LineageStep> parse_step(std::string_view s) {
  const std::optional<LabeledPath> lp = labeled(s, false);
  if (!lp) {
    return std::nullopt;
  }
  const usize comma = lp->path.find(',');
  if (comma == std::string::npos) {
    return std::nullopt;
  }
  const std::optional<f64> raw = parse_f64(std::string_view(lp->path).substr(0, comma));
  const std::optional<f64> se = parse_f64(std::string_view(lp->path).substr(comma + 1U));
  if (!raw || !se) {
    return std::nullopt;
  }
  return LineageStep{lp->label, *raw, *se};
}

struct Parser {
  EvalCli cli;
  std::optional<usize> draws;
  std::optional<u64> seed;
  std::set<std::string_view> seen;

  // A flag that takes no value.
  [[nodiscard]] bool flag(std::string_view key) {
    if (key == "--one-sided") {
      cli.request.paired_config.one_sided = true;
    } else if (key == "--onc") {
      cli.request.onc = true;
    } else if (key == "--pbo") {
      cli.request.pbo = true;
    } else if (key == "--json") {
      cli.json = true;
    } else if (key == "--vol") { // P9 lane D3W (ruling D3W-EVAL): each cell's net_vol block
      cli.request.vol = true;
    } else {
      return false;
    }
    return true;
  }

  [[nodiscard]] core::Status repeated(std::string_view key, std::string_view value) {
    EvalRequest &rq = cli.request;
    if (key == "--daily" || key == "--onc-series" || key == "--pbo-series") {
      const std::optional<LabeledPath> lp = labeled(value, true);
      if (!lp) {
        return core::Err(usage_error("malformed " + std::string(key) + " " + std::string(value)));
      }
      (key == "--daily" ? rq.daily : key == "--onc-series" ? rq.onc_series : rq.pbo_series)
          .push_back(*lp);
    } else if (key == "--paired-of" || key == "--trial") {
      const std::optional<LabeledPath> lp = labeled(value, false);
      if (!lp) {
        return core::Err(usage_error("malformed " + std::string(key) + " " + std::string(value)));
      }
      (key == "--trial" ? rq.trials : rq.paired_of).push_back(*lp);
    } else if (key == "--hand") {
      const std::optional<LabeledPath> lp = labeled(value, false);
      if (!lp) {
        return core::Err(usage_error("malformed --hand " + std::string(value)));
      }
      rq.hands.push_back(HandBook{lp->label, lp->path});
    } else if (key == "--history") {
      rq.history.emplace_back(value);
    } else {
      const std::optional<LineageStep> step = parse_step(value);
      if (!step) {
        return core::Err(usage_error("malformed --step " + std::string(value)));
      }
      rq.steps.push_back(*step);
    }
    return core::Ok();
  }

  [[nodiscard]] bool is_repeated(std::string_view key) const {
    return key == "--daily" || key == "--onc-series" || key == "--pbo-series" ||
           key == "--paired-of" || key == "--trial" || key == "--hand" || key == "--history" ||
           key == "--step";
  }

  [[nodiscard]] core::Status number(std::string_view key, std::string_view value) {
    EvalRequest &rq = cli.request;
    bool ok = true;
    if (key == "--draws") {
      draws = parse_uint<usize>(value);
      ok = draws.has_value();
    } else if (key == "--seed") {
      seed = parse_uint<u64>(value);
      ok = seed.has_value();
    } else if (key == "--block" || key == "--dsr-n" || key == "--onc-init" ||
               key == "--pbo-blocks") {
      const std::optional<usize> v = parse_uint<usize>(value);
      ok = v.has_value();
      if (ok) {
        (key == "--block"      ? rq.paired_config.block
         : key == "--dsr-n"    ? rq.dsr_n
         : key == "--onc-init" ? rq.onc_init
                               : rq.pbo_blocks) = *v;
      }
    } else if (key == "--onc-seed") {
      const std::optional<u64> v = parse_uint<u64>(value);
      ok = v.has_value();
      rq.onc_seed = v.value_or(rq.onc_seed);
    } else {
      const std::optional<f64> v = parse_f64(value);
      ok = v.has_value();
      if (ok) {
        (key == "--psr-alpha" ? rq.psr_alpha
         : key == "--threshold" ? rq.threshold
                                : rq.conditional_alpha) = *v;
      }
    }
    if (!ok) {
      return core::Err(usage_error("malformed number " + std::string(key) + " " +
                                   std::string(value)));
    }
    return core::Ok();
  }

  [[nodiscard]] core::Status single(std::string_view key, std::string_view value) {
    EvalRequest &rq = cli.request;
    if (key == "--paired") {
      rq.paired = std::string(value);
    } else if (key == "--protocol") {
      if (value != "v7" && value != "v8") {
        return core::Err(usage_error("--protocol is v7 or v8"));
      }
      rq.protocol = std::string(value);
    } else if (key == "--psr-benchmarks") {
      std::optional<std::vector<f64>> list = parse_list(value);
      if (!list) {
        return core::Err(usage_error("malformed --psr-benchmarks " + std::string(value)));
      }
      rq.psr_benchmarks = std::move(*list);
    } else if (key == "--ledger") {
      rq.ledger = std::string(value);
    } else if (key == "--ledger-missing") {
      if (value != "empty" && value != "refuse") {
        return core::Err(usage_error("--ledger-missing is empty or refuse"));
      }
      rq.ledger_missing = value == "empty" ? ledger::MissingLedger::Empty
                                           : ledger::MissingLedger::Refuse;
    } else if (key == "--window-id") {
      rq.window_id = std::string(value);
    } else if (key == "--out") {
      cli.out_path = std::string(value);
    } else if (key == "--draws" || key == "--seed" || key == "--block" || key == "--dsr-n" ||
               key == "--onc-init" || key == "--onc-seed" || key == "--pbo-blocks" ||
               key == "--psr-alpha" || key == "--threshold" || key == "--conditional-alpha") {
      return number(key, value);
    } else {
      return core::Err(usage_error("unknown argument " + std::string(key)));
    }
    return core::Ok();
  }

  void finish() {
    PairedConfig &pc = cli.request.paired_config;
    const bool v8 = cli.request.protocol == "v8";
    pc.draws = draws.value_or(v8 ? kV8Draws : kDefaultDraws);
    pc.seed = seed.value_or(v8 ? kV8Seed : kDefaultSeed);
  }
};

[[nodiscard]] core::Status write_file(const std::string &path, const std::string &text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return core::Err(core::ErrorCode::IoError, "atx-research-eval: cannot write " + path);
  }
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) {
    return core::Err(core::ErrorCode::IoError, "atx-research-eval: write failed " + path);
  }
  return core::Ok();
}

} // namespace

core::Result<EvalCli> parse_eval_args(std::span<const std::string_view> args) {
  Parser p;
  for (usize i = 0; i < args.size(); ++i) {
    const std::string_view key = args[i];
    if (p.flag(key)) {
      continue;
    }
    if (i + 1U >= args.size()) {
      return core::Err(usage_error("missing value for " + std::string(key)));
    }
    const std::string_view value = args[++i];
    if (p.is_repeated(key)) {
      ATX_TRY_VOID(p.repeated(key, value));
      continue;
    }
    if (!p.seen.insert(key).second) {
      return core::Err(usage_error("repeated flag " + std::string(key)));
    }
    ATX_TRY_VOID(p.single(key, value));
  }
  p.finish();
  return core::Ok(std::move(p.cli));
}

std::string eval_json_text(const nlohmann::json &doc) {
  return doc.dump(1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

int research_eval_main(std::span<const std::string_view> args, std::ostream &out,
                       std::ostream &err) {
  const std::span<const std::string_view> rest =
      args.empty() ? args : args.subspan(1);
  if (rest.empty()) {
    err << kUsage;
    return 2;
  }
  const auto cli = parse_eval_args(rest);
  if (!cli) {
    err << cli.error().message() << "\n" << kUsage;
    return 2;
  }
  const auto doc = evaluate(cli->request);
  if (!doc) {
    err << doc.error().to_string() << "\n";
    return 1;
  }
  const std::string text = eval_json_text(*doc);
  if (!cli->out_path.empty()) {
    const core::Status written = write_file(cli->out_path, text);
    if (!written) {
      err << written.error().to_string() << "\n";
      return 1;
    }
  }
  if (cli->json) {
    out << text;
  } else {
    out << eval_text(*doc);
  }
  return 0;
}

} // namespace atx::engine::research::eval
