#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/server/research.hpp"

namespace atx::server::research_detail {
using Json = nlohmann::json;
using Progress = std::function<void(double, std::string_view)>;

struct Day {
  std::int64_t session_ns{};
  std::string date;
  double nav{};
  double net_return{};
  bool return_observation{};
};

struct Evidence {
  std::vector<Day> days;
  Json provenance;
  Json trades = Json::array();
  Json positions = Json::array();
  Json portfolio_allocations = Json::array();
};

void checkpoint(std::stop_token stop, const Progress& progress, double fraction,
                std::string_view message);
[[nodiscard]] std::string date_text(std::chrono::sys_days date);
[[nodiscard]] std::chrono::sys_days parse_date(std::string_view text);
[[nodiscard]] Json parse_json(std::string_view text);
[[nodiscard]] std::filesystem::path artifact_path(std::string_view requested,
                                                 const ResearchOptions& options);
[[nodiscard]] std::string read_artifact(const std::filesystem::path& path,
                                       std::size_t max_bytes, std::stop_token stop);
[[nodiscard]] bool forbidden_path(std::string_view name);
[[nodiscard]] Evidence import_nav(const Json& request, const ResearchOptions& options,
                                  std::stop_token stop, const Progress& progress);
[[nodiscard]] Evidence synthetic_demo(const Json& request, const ResearchOptions& options,
                                      std::stop_token stop, const Progress& progress);
[[nodiscard]] Evidence real_nav(const Json& request, const ResearchOptions& options,
                                std::stop_token stop, const Progress& progress);
[[nodiscard]] Json summarize(const Evidence& evidence, const ResearchOptions& options,
                             std::stop_token stop, const Progress& progress);
} // namespace atx::server::research_detail
