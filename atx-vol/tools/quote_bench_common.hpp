#pragma once

#include "quote_bench_data.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace quote_bench {

struct CommonRun {
  std::vector<Prediction> predictions;
  nlohmann::json report;
};

// Offline orchestration; one immutable fitted process per exact stock snapshot.
// Every source row is returned, including whole-snapshot refusals. Throws on
// malformed configuration; expected market/fit refusals are reported as data.
[[nodiscard]] CommonRun run_common_markov(const std::vector<InputRow> &rows,
                                         const std::string &policy,
                                         const std::string &dividends);

} // namespace quote_bench
