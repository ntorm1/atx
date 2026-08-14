// Mine one point-in-time BacktestDb signal with deterministic purged
// walk-forward validation and publish the OOS evidence to the companion
// ResearchDb.
//
// Example:
//   atx-vol-quant-research --db C:\data\backtests
//       --template long-40d-3m-strangle --symbol SPY
//       --signal implied_correlation --capital 1000000
//       --min-train 252 --test 63 --step 63 --lookbacks 5,20,60

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "storage/backtest_db.hpp"
#include "backtest/quant_pipeline.hpp"
#include "storage/research_db.hpp"

using namespace atx::vol;

namespace {

void usage(std::FILE *out) {
  std::fprintf(out, "usage: atx-vol-quant-research --db <backtest-root> --template <id>\n"
                    "       --symbol <symbol> --signal <name> --capital <positive-number>\n"
                    "       [--min-train N] [--test N] [--step N] [--embargo-ns N]\n"
                    "       [--nw-lag N] [--lookbacks 5,20,60] [--rolling-train N]\n");
}

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t &out) {
  if (text.empty() || text.front() == '-') {
    return false;
  }
  const std::string owned(text);
  char *end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(owned.c_str(), &end, 10);
  if (errno == ERANGE || end != owned.c_str() + owned.size()) {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

[[nodiscard]] bool parse_size(std::string_view text, std::size_t &out) {
  std::uint64_t parsed = 0u;
  if (!parse_u64(text, parsed) ||
      parsed > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    return false;
  }
  out = static_cast<std::size_t>(parsed);
  return true;
}

[[nodiscard]] bool parse_i64(std::string_view text, std::int64_t &out) {
  if (text.empty()) {
    return false;
  }
  const std::string owned(text);
  char *end = nullptr;
  errno = 0;
  const long long value = std::strtoll(owned.c_str(), &end, 10);
  if (errno == ERANGE || end != owned.c_str() + owned.size()) {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

[[nodiscard]] bool parse_double(std::string_view text, double &out) {
  if (text.empty()) {
    return false;
  }
  const std::string owned(text);
  char *end = nullptr;
  errno = 0;
  const double value = std::strtod(owned.c_str(), &end);
  if (errno == ERANGE || end != owned.c_str() + owned.size() || !std::isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_lookbacks(std::string_view text, std::vector<std::size_t> &out) {
  std::size_t first = 0u;
  while (first <= text.size()) {
    const std::size_t comma = text.find(',', first);
    const std::size_t last = comma == std::string_view::npos ? text.size() : comma;
    std::size_t value = 0u;
    if (!parse_size(text.substr(first, last - first), value) || value < 2u) {
      return false;
    }
    out.push_back(value);
    if (comma == std::string_view::npos) {
      return true;
    }
    first = comma + 1u;
  }
  return false;
}

[[nodiscard]] const ResearchCandidateEvaluation *
selected_evaluation(const ResearchMiningResult &mining) {
  for (const ResearchCandidateEvaluation &evaluation : mining.evaluations) {
    if (evaluation.candidate_identity == mining.selected_candidate_identity &&
        evaluation.candidate.id == mining.selected_candidate_id) {
      return &evaluation;
    }
  }
  return nullptr;
}

} // namespace

int main(int argc, char **argv) {
  std::string db_root;
  std::string template_id;
  std::string symbol;
  BacktestSignalResearchSpec research;
  research.validation.min_train_groups = 252u;
  research.validation.test_groups = 63u;
  research.validation.step_groups = 63u;
  research.newey_west_lag = 5u;
  research.lagged_capital = 0.0;
  std::vector<std::size_t> lookbacks{5u, 20u, 60u};

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto value = [&](std::string_view flag) -> const char * {
      if (arg != flag || i + 1 >= argc) {
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage(stdout);
      return 0;
    }
    if (const char *parsed = value("--db")) {
      db_root = parsed;
    } else if (const char *parsed = value("--template")) {
      template_id = parsed;
    } else if (const char *parsed = value("--symbol")) {
      symbol = parsed;
    } else if (const char *parsed = value("--signal")) {
      research.signal_name = parsed;
    } else if (const char *parsed = value("--capital")) {
      if (!parse_double(parsed, research.lagged_capital)) {
        usage(stderr);
        return 2;
      }
    } else if (const char *parsed = value("--min-train")) {
      if (!parse_size(parsed, research.validation.min_train_groups)) {
        usage(stderr);
        return 2;
      }
    } else if (const char *parsed = value("--test")) {
      if (!parse_size(parsed, research.validation.test_groups)) {
        usage(stderr);
        return 2;
      }
    } else if (const char *parsed = value("--step")) {
      if (!parse_size(parsed, research.validation.step_groups)) {
        usage(stderr);
        return 2;
      }
    } else if (const char *parsed = value("--rolling-train")) {
      research.validation.kind = ResearchWalkForwardKind::Rolling;
      if (!parse_size(parsed, research.validation.max_train_groups)) {
        usage(stderr);
        return 2;
      }
    } else if (const char *parsed = value("--embargo-ns")) {
      if (!parse_i64(parsed, research.validation.embargo_ns)) {
        usage(stderr);
        return 2;
      }
    } else if (const char *parsed = value("--nw-lag")) {
      if (!parse_size(parsed, research.newey_west_lag)) {
        usage(stderr);
        return 2;
      }
    } else if (const char *parsed = value("--lookbacks")) {
      lookbacks.clear();
      if (!parse_lookbacks(parsed, lookbacks)) {
        usage(stderr);
        return 2;
      }
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
      usage(stderr);
      return 2;
    }
  }

  if (db_root.empty() || template_id.empty() || symbol.empty() || research.signal_name.empty() ||
      research.lagged_capital <= 0.0) {
    usage(stderr);
    return 2;
  }

  research.candidates = {
      ResearchSignalCandidate{"level-long", ResearchSignalTransform::Identity, 0u, 0u,
                              ResearchSignalDirection::LongHigh},
      ResearchSignalCandidate{"level-short", ResearchSignalTransform::Identity, 0u, 0u,
                              ResearchSignalDirection::ShortHigh},
      ResearchSignalCandidate{"change-long", ResearchSignalTransform::Difference, 0u, 1u,
                              ResearchSignalDirection::LongHigh},
      ResearchSignalCandidate{"change-short", ResearchSignalTransform::Difference, 0u, 1u,
                              ResearchSignalDirection::ShortHigh},
  };
  for (const std::size_t lookback : lookbacks) {
    research.candidates.push_back(ResearchSignalCandidate{
        "zscore-" + std::to_string(lookback) + "-long", ResearchSignalTransform::RollingZScore, 0u,
        lookback, ResearchSignalDirection::LongHigh});
    research.candidates.push_back(ResearchSignalCandidate{
        "zscore-" + std::to_string(lookback) + "-short", ResearchSignalTransform::RollingZScore, 0u,
        lookback, ResearchSignalDirection::ShortHigh});
  }
  research.family = ResearchTrialFamily{true, research.candidates.size()};

  auto backtests = BacktestDb::open(db_root);
  if (!backtests) {
    std::fprintf(stderr, "%s\n", backtests.error().to_string().c_str());
    return 1;
  }
  auto info = backtests->find_series(template_id, symbol);
  if (!info) {
    std::fprintf(stderr, "%s\n", info.error().to_string().c_str());
    return 1;
  }
  auto series = backtests->load_series(template_id, symbol);
  if (!series) {
    std::fprintf(stderr, "%s\n", series.error().to_string().c_str());
    return 1;
  }
  auto result = mine_backtest_signal_series(*info, *series, research);
  if (!result) {
    std::fprintf(stderr, "%s\n", result.error().to_string().c_str());
    return 1;
  }

  auto research_db = ResearchDb::open(db_root);
  if (!research_db && research_db.error().code() == ErrorCode::NotFound) {
    research_db = ResearchDb::create(db_root);
  }
  if (!research_db) {
    std::fprintf(stderr, "%s\n", research_db.error().to_string().c_str());
    return 1;
  }

  const std::string logical_id =
      "trials/" + template_id + "/" + symbol + "/" + research.signal_name;
  ResearchTrialPublishSpec publication{logical_id, {}};
  auto current = research_db->find_head(ResearchArtifactKind::Trial, logical_id);
  if (current) {
    publication.expected_head_id = current->artifact_id;
  } else if (current.error().code() != ErrorCode::NotFound) {
    std::fprintf(stderr, "%s\n", current.error().to_string().c_str());
    return 1;
  }

  auto published =
      publish_backtest_signal_trial(*research_db, *info, research, *result, publication);
  if (!published) {
    std::fprintf(stderr, "%s\n", published.error().to_string().c_str());
    return 1;
  }
  const ResearchCandidateEvaluation *selected = selected_evaluation(result->mining);
  if (selected == nullptr) {
    std::fprintf(stderr, "internal: selected candidate not found\n");
    return 1;
  }
  std::printf("artifact_id=%s\nrevision=%u\ncandidate=%s\noos_rows=%zu\n"
              "oos_mean=%.17g\nhac_t=%.17g\ndsr_probability=%.17g\n"
              "max_drawdown=%.17g\n",
              published->artifact_id.c_str(), published->revision,
              result->mining.selected_candidate_id.c_str(), selected->oos_stats.n_observations,
              selected->oos_stats.mean, selected->oos_stats.hac_t_statistic,
              selected->oos_stats.deflated_sharpe_probability, selected->oos_stats.max_drawdown);
  return 0;
}
