#include "strategy_target_replay.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
using Json = nlohmann::json;
using Ranked = std::pair<f64, usize>;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr u64 metadata_bytes = 64ULL << 20;
constexpr usize max_dates = 4096, max_names = 20000;
struct Budget {
  u64 limit{}, used{};
  bool add(u64 count, u64 width) {
    if (width && count > (limit - used) / width) return false;
    used += count * width; return true;
  }
};
bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
u32 calendar_month(i64 session) {
  const std::chrono::year_month_day date{
      std::chrono::sys_days{std::chrono::days{session / day_ns}}};
  return static_cast<u32>(static_cast<int>(date.year())) * 100U +
         static_cast<u32>(static_cast<unsigned>(date.month()));
}
co::Status validate_config(const TargetReplayConfig& cfg) {
  if ((cfg.rule != TargetReplayRule::BaselineTargetV1 &&
       cfg.rule != TargetReplayRule::MonthlyTargetBudgetV2) || !cfg.cadence ||
      cfg.cadence > max_dates || !std::isfinite(cfg.trade_fraction) ||
      cfg.trade_fraction <= 0 || cfg.trade_fraction > 1 ||
      !std::isfinite(cfg.monthly_budget) || cfg.monthly_budget <= 0 ||
      cfg.monthly_budget > 100 || !std::isfinite(cfg.one_way_bps) ||
      cfg.one_way_bps < 0 || cfg.one_way_bps > 10000 ||
      !std::isfinite(cfg.annual_borrow_bps) || cfg.annual_borrow_bps < 0 ||
      cfg.annual_borrow_bps > 100000)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: invalid recipe");
  return co::Ok();
}
co::Status validate_input(const TargetReplayInput& in, const TargetReplayConfig& cfg) {
  ATX_TRY_VOID(validate_config(cfg));
  if (!in.dates || in.dates > max_dates || !in.instruments || in.instruments > max_names ||
      in.decision_begin >= in.decision_end || in.decision_end > in.dates)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: bounded dimensions/window");
  const auto cells = in.dates * in.instruments;
  Budget budget{cfg.max_working_bytes};
  if (!budget.add(1, 65536) || !budget.add(in.instruments, sizeof(Ranked) + 2 * sizeof(f64)) ||
      !budget.add(in.decision_end - in.decision_begin, sizeof(TargetReplayDay)))
    return co::Err(co::ErrorCode::OutOfRange, "target replay: workspace budget");
  const bool prices = !in.close.empty() || !in.raw_close.empty() || !in.present.empty();
  if (in.signal.size() != cells || in.member.size() != cells ||
      in.session_keys.size() != in.dates || in.instrument_ids.size() != in.instruments ||
      (prices && (in.close.size() != cells || in.raw_close.size() != cells ||
                  in.present.size() != cells)))
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: span geometry");
  if (in.session_keys.front() <= 0 || in.session_keys.back() >= 4'102'444'800'000'000'000LL ||
      std::adjacent_find(in.session_keys.begin(), in.session_keys.end(),
                        std::greater_equal<i64>{}) != in.session_keys.end() ||
      in.instrument_ids.front() == 0 ||
      std::adjacent_find(in.instrument_ids.begin(), in.instrument_ids.end(),
                        std::greater_equal<u64>{}) != in.instrument_ids.end())
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: ordered axes");
  for (const auto s : in.session_keys) if (s % day_ns != 0)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: session must be UTC midnight");
  for (usize k = 0; k < cells; ++k) {
    if (in.member[k] > 1 || (in.member[k] ? !std::isfinite(in.signal[k]) :
                                         !std::isnan(in.signal[k])) ||
        (prices && in.present[k] > 1))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: support/value contract");
  }
  return co::Ok();
}
// Same operations and order as IcComposition::finish. In particular a tied
// all-zero blend stays flat, and no daily renormalization changes partial fills.
void desired_target(std::span<const f64> signal, std::span<const u8> member,
                    std::vector<Ranked>& row, std::vector<f64>& target) {
  std::fill(target.begin(), target.end(), 0); row.clear();
  for (usize i = 0; i < signal.size(); ++i) if (member[i]) row.emplace_back(signal[i], i);
  std::sort(row.begin(), row.end());
  if (row.size() >= 2) for (usize b = 0; b < row.size();) {
    usize e = b + 1;
    while (e < row.size() && row[e].first == row[b].first) ++e;
    const f64 r = (static_cast<f64>(b) + static_cast<f64>(e - 1)) /
                 (2.0 * static_cast<f64>(row.size() - 1)) - 0.5;
    for (usize k = b; k < e; ++k) target[row[k].second] = r;
    b = e;
  }
  f64 sum = 0;
  for (const auto& value : row) sum += target[value.second];
  const f64 mean = row.empty() ? 0 : sum / static_cast<f64>(row.size());
  f64 gross = 0;
  for (const auto& value : row) {
    target[value.second] -= mean; gross += std::abs(target[value.second]);
  }
  if (gross > 0) for (const auto& value : row) target[value.second] /= gross;
}
void update_weights(const TargetReplayInput& in, const TargetReplayConfig& cfg, usize d,
                    bool rebalance, f64 spent, const std::vector<f64>& desired,
                    std::vector<f64>& current, TargetReplayDay& out) {
  const auto offset = d * in.instruments;
  f64 forced = 0, distance = 0;
  for (usize i = 0; i < in.instruments; ++i) {
    if (!in.member[offset + i]) forced += std::abs(current[i]);
    else if (rebalance) distance += std::abs(desired[i] - current[i]);
  }
  f64 fraction = rebalance ? cfg.trade_fraction : 0;
  if (cfg.rule == TargetReplayRule::MonthlyTargetBudgetV2 && distance > 0)
    fraction = std::min(fraction, std::max(0.0, cfg.monthly_budget - spent - forced) / distance);
  out.applied_fraction = fraction;
  f64 squared = 0;
  for (usize i = 0; i < in.instruments; ++i) {
    const bool live = in.member[offset + i] != 0;
    const f64 next = !live ? 0 : rebalance
      ? current[i] + fraction * (desired[i] - current[i]) : current[i];
    const f64 trade = std::abs(next - current[i]);
    out.turnover += trade;
    if (!live) out.forced_turnover += trade; else out.discretionary_turnover += trade;
    current[i] = next;
    out.gross += std::abs(next); out.net += next;
    out.long_weight += std::max(0.0, next); out.short_weight += std::max(0.0, -next);
    out.max_abs_weight = std::max(out.max_abs_weight, std::abs(next));
    out.held_names += next != 0 ? 1U : 0U; squared += next * next;
  }
  out.effective_names = squared > 0 ? out.gross * out.gross / squared : 0;
}
void rough_return(const TargetReplayInput& in, const TargetReplayConfig& cfg,
                  std::span<const f64> weights, TargetReplayDay& out) {
  out.complete_gross_return = nan; out.complete_net_return = nan;
  out.modeled_trade_cost = out.turnover * cfg.one_way_bps / 10000;
  out.modeled_borrow_cost = out.short_weight * cfg.annual_borrow_bps / (10000 * 252);
  if (in.close.empty() || out.endpoint >= in.decision_end) return;
  out.return_mature = true;
  for (usize i = 0; i < in.instruments; ++i) {
    const f64 w = weights[i]; if (w == 0) continue;
    const auto a = out.entry * in.instruments + i, b = out.endpoint * in.instruments + i;
    const bool priced = in.present[a] && in.present[b] &&
        std::isfinite(in.close[a]) && std::isfinite(in.close[b]) &&
        std::isfinite(in.raw_close[a]) && std::isfinite(in.raw_close[b]) &&
        in.close[a] > 0 && in.close[b] > 0 && in.raw_close[a] > 0 && in.raw_close[b] > 0;
    const f64 r = priced ? in.close[b] / in.close[a] - 1 : nan;
    const f64 log_return = priced ? std::log(in.close[b]) - std::log(in.close[a]) : nan;
    const f64 raw_log_return = priced ? std::log(in.raw_close[b]) - std::log(in.raw_close[a]) : nan;
    const bool guarded = priced && (!std::isfinite(r) || std::abs(log_return) > 1.5 ||
                                    std::abs(log_return) > std::abs(raw_log_return) + .10);
    if (!priced || guarded) {
      ++out.missing_names; out.guarded_names += guarded ? 1U : 0U;
      out.missing_long += std::max(0.0, w); out.missing_short += std::max(0.0, -w);
    } else out.observed_return_component += w * r;
  }
  out.missing_gross = out.missing_long + out.missing_short;
  out.return_complete = out.missing_names == 0;
  if (out.return_complete) {
    out.complete_gross_return = out.observed_return_component;
    out.complete_net_return = out.complete_gross_return - out.modeled_trade_cost -
                              out.modeled_borrow_cost;
  }
}
} // namespace

co::Result<TargetReplayResult> replay_targets(const TargetReplayInput& in,
                                            const TargetReplayConfig& cfg) {
  ATX_TRY_VOID(validate_input(in, cfg));
  try {
    TargetReplayResult result; result.deployment_date = in.dates;
    result.days.reserve(in.decision_end - in.decision_begin);
    std::vector<f64> current(in.instruments), desired(in.instruments);
    std::vector<Ranked> row; row.reserve(in.instruments);
    u32 month = 0; f64 spent = 0;
    for (usize d = in.decision_begin; d < in.decision_end; ++d) {
      TargetReplayDay day; day.decision = d; day.session = in.session_keys[d];
      day.entry = std::min(in.dates, d + 1); day.endpoint = std::min(in.dates, d + 2);
      day.calendar_month = calendar_month(day.session);
      if (day.calendar_month != month) { month = day.calendar_month; spent = 0; }
      const bool rebalance = (d - in.decision_begin) % cfg.cadence == 0;
      const auto offset = d * in.instruments;
      if (rebalance) desired_target(in.signal.subspan(offset, in.instruments),
          in.member.subspan(offset, in.instruments), row, desired);
      update_weights(in, cfg, d, rebalance, spent, desired, current, day);
      spent += day.turnover; day.month_turnover = spent;
      if (cfg.rule == TargetReplayRule::MonthlyTargetBudgetV2)
        day.budget_excess = std::max(0.0, spent - cfg.monthly_budget);
      if (result.deployment_date == in.dates && day.gross > 0) {
        result.deployment_date = d; result.deployment_turnover = day.turnover;
        day.deployment_turnover = day.turnover;
      }
      result.total_turnover += day.turnover; result.forced_turnover += day.forced_turnover;
      result.discretionary_turnover += day.discretionary_turnover;
      rough_return(in, cfg, current, day); result.days.push_back(day);
    }
    return co::Ok(std::move(result));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "target replay: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "target replay: allocation extent");
  }
}

namespace {
struct SavedBlend {
  Json manifest;
  usize dates{}, names{}, begin{}, end{};
  std::vector<f64> signal, close, raw;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  TargetReplayInput view() const {
    return {dates, names, begin, end, signal, member, sessions, ids, close, raw, present};
  }
};
co::Result<Json> pinned_json(const std::string& path, const std::string& pin) {
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument, "target replay: external pin");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > (1ULL << 20))
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: manifest missing/oversized");
  std::string text(static_cast<usize>(file.tellg()), '\0'); file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file || file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, "target replay: manifest changed extent");
  ATX_TRY(auto digest, co::sha256_hex(text));
  if (digest != pin) return co::Err(co::ErrorCode::InvalidArgument, "target replay: manifest SHA");
  return co::Ok(Json::parse(text));
}
std::string hex(const std::array<std::byte, 32>& bytes) {
  constexpr char digits[] = "0123456789abcdef"; std::string result(64, '0');
  for (usize i = 0; i < bytes.size(); ++i) {
    const auto b = std::to_integer<unsigned>(bytes[i]);
    result[2 * i] = digits[b >> 4]; result[2 * i + 1] = digits[b & 15];
  }
  return result;
}
template<class T> co::Status payload(const std::filesystem::path& base, const Json& files,
                                    const std::string& name, std::vector<T>& out, usize count) {
  const auto bytes = static_cast<u64>(count) * sizeof(T);
  const auto& receipt = files.at(name);
  const auto expected = receipt.at("sha256").get<std::string>();
  if (receipt.at("bytes").get<u64>() != bytes || !hash_valid(expected))
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: invalid payload receipt");
  std::ifstream file(base / name, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || static_cast<u64>(file.tellg()) != bytes)
    return co::Err(co::ErrorCode::IoError, "target replay: payload extent: " + name);
  file.seekg(0); out.resize(count);
  auto destination = std::as_writable_bytes(std::span(out)); co::Sha256 digest;
  for (usize offset = 0; offset < destination.size();) {
    const auto size = std::min(usize{65536}, destination.size() - offset);
    auto chunk = destination.subspan(offset, size);
    // SAFETY: char accesses object representations of trivially copyable numeric payloads.
    file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(size));
    if (!file) return co::Err(co::ErrorCode::IoError, "target replay: truncated payload");
    ATX_TRY_VOID(digest.update(chunk)); offset += size;
  }
  if (file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, "target replay: payload changed extent");
  ATX_TRY(auto actual, digest.finalize());
  if (hex(actual) != expected)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: payload SHA: " + name);
  return co::Ok();
}
co::Status admit_saved(const TargetReplayRunConfig& cfg, SavedBlend& out) {
  ATX_TRY(out.manifest, pinned_json(cfg.combined_path, cfg.combined_sha256));
  const auto& j = out.manifest;
  const auto d = j.at("dates").get<u64>(), n = j.at("instruments").get<u64>();
  if (j.at("schema") != "atx.dsl-combined-signal/v1" || j.at("status") != "complete" ||
      j.at("layout") != "date-major-little-endian" || j.at("role_window_required") != true ||
      j.at("actual_trades_or_returns") != false ||
      j.at("signal_semantics") != "exact-pre-target-composition;equal-family/equal-within;missing-or-unoriented-neutral-fixed-denominator" ||
      j.at("member_semantics") != "decision-member-and-source-present-and-finite-positive-close;independent-of-component-coverage" ||
      j.at("finite_semantics") != "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal" ||
      !d || d > max_dates || !n || n > max_names)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved blend contract");
  const auto role = j.at("role").get<std::string>();
  if (role != "train" && role != "validation" && role != "holdout")
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved role identifier");
  for (const auto* key : {"role_manifest_sha256", "source_sha256", "library_sha256",
                          "train_manifest_sha256", "run_recipe_sha256",
                          "orientation_candidates_sha256"})
    if (!hash_valid(j.at(key).get<std::string>()))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: provenance pin");
  out.dates = static_cast<usize>(d); out.names = static_cast<usize>(n);
  out.begin = j.at("score_begin").get<usize>(); out.end = j.at("score_end").get<usize>();
  if (out.begin >= out.end || out.end > out.dates)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved score window");
  Budget budget{cfg.target.max_working_bytes}; const auto cells = d * n;
  // Includes metadata parse/output overlap, finite-mask validation, optional
  // close/raw/presence and temporary role membership/axis verification vectors.
  if (!budget.add(1, metadata_bytes) || !budget.add(cells, cfg.role_path.empty() ? 10 : 28) ||
      !budget.add(d, 2 * sizeof(i64) + sizeof(TargetReplayDay)) ||
      !budget.add(n, 2 * sizeof(u64) + sizeof(Ranked) + 2 * sizeof(f64)))
    return co::Err(co::ErrorCode::OutOfRange, "target replay: aggregate input/workspace budget");
  const auto base = std::filesystem::path(cfg.combined_path).parent_path();
  const auto prefix = role + "_combined"; const auto& files = j.at("files");
  if (files.size() != 5)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved file set");
  ATX_TRY_VOID(payload(base, files, prefix + ".f64", out.signal, static_cast<usize>(cells)));
  ATX_TRY_VOID(payload(base, files, prefix + "_member.u8", out.member, static_cast<usize>(cells)));
  ATX_TRY_VOID(payload(base, files, prefix + "_sessions.i64", out.sessions, out.dates));
  ATX_TRY_VOID(payload(base, files, prefix + "_ids.u64", out.ids, out.names));
  std::vector<u8> finite;
  ATX_TRY_VOID(payload(base, files, prefix + "_finite.u8", finite, static_cast<usize>(cells)));
  u64 finite_count = 0, members = 0;
  for (usize k = 0; k < finite.size(); ++k) {
    if (finite[k] > 1 || finite[k] != static_cast<u8>(std::isfinite(out.signal[k])))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: finite-mask mismatch");
    finite_count += finite[k]; members += out.member[k];
  }
  if (j.at("finite_cells").get<u64>() != finite_count ||
      j.at("member_cells").get<u64>() != members)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: support count mismatch");
  return validate_input(out.view(), cfg.target);
}
co::Status load_prices(const TargetReplayRunConfig& cfg, SavedBlend& out) {
  if (cfg.role_path.empty()) return co::Ok();
  ATX_TRY(auto j, pinned_json(cfg.role_path, cfg.role_sha256));
  if (cfg.role_sha256 != out.manifest.at("role_manifest_sha256").get<std::string>() ||
      j.at("schema") != "atx.recent-research-role/v1" || j.at("status") != "complete" ||
      j.at("source_sha256") != out.manifest.at("source_sha256") ||
      j.at("instrument_namespace") != "spiderrock.securityID" ||
      j.at("close_basis") != "f64(raw-f32-close)*f64-cumulReturnFactor" ||
      j.at("clock_recipe") != "modeled-session+22h-mark+23h-decision-v1" ||
      j.at("common_stock_verified") != false || j.at("historical_vintage_verified") != false ||
      j.at("dates").get<usize>() != out.dates || j.at("instruments").get<usize>() != out.names ||
      j.at("score_begin").get<usize>() != out.begin || j.at("score_end").get<usize>() != out.end)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role recipe/identity");
  const auto base = std::filesystem::path(cfg.role_path).parent_path(); const auto& files = j.at("files");
  std::vector<i64> sessions; std::vector<u64> ids;
  ATX_TRY_VOID(payload(base, files, "sessions.i64", sessions, out.dates));
  ATX_TRY_VOID(payload(base, files, "ids.u64", ids, out.names));
  if (sessions != out.sessions || ids != out.ids)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role axes");
  const auto cells = out.dates * out.names;
  ATX_TRY_VOID(payload(base, files, "close.f64", out.close, cells));
  ATX_TRY_VOID(payload(base, files, "raw_close.f64", out.raw, cells));
  ATX_TRY_VOID(payload(base, files, "present.u8", out.present, cells));
  std::vector<u8> member; ATX_TRY_VOID(payload(base, files, "member.u8", member, cells));
  for (usize k = 0; k < cells; ++k) {
    if (member[k] > 1 || out.present[k] > 1 ||
        (out.present[k] ? (!std::isfinite(out.close[k]) || out.close[k] <= 0 ||
                          !std::isfinite(out.raw[k]) || out.raw[k] <= 0)
                        : (!std::isnan(out.close[k]) || !std::isnan(out.raw[k]))) ||
        out.member[k] != static_cast<u8>(member[k] && out.present[k] &&
                                        std::isfinite(out.close[k]) && out.close[k] > 0))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role presence/support");
  }
  return co::Ok();
}
const char* rule_name(TargetReplayRule rule) {
  return rule == TargetReplayRule::BaselineTargetV1 ? "baseline-target-v1" : "monthly-target-budget-v2";
}
Json recipe(const TargetReplayRunConfig& cfg) {
  return Json{{"schema", "atx.dsl-target-replay/v1"}, {"rule", rule_name(cfg.target.rule)},
      {"combined_sha256", cfg.combined_sha256}, {"role_sha256", cfg.role_sha256},
      {"cadence", cfg.target.cadence}, {"trade_fraction", cfg.target.trade_fraction},
      {"monthly_budget", cfg.target.monthly_budget}, {"calendar_basis", "decision-session-month"},
      {"deployment_included", true}, {"forced_exits", "immediate;charged-before-discretionary;may-breach"},
      {"target", "tied-rank;neutral-desired-gross1;partial-no-drift-no-renormalization"},
      {"rough_returns", !cfg.role_path.empty()}, {"entry_delay", 1}, {"return_horizon", 1},
      {"missing_return", "complete-day-undefined;observed-component-and-missing-exposures"},
      {"return_guard", "observed-adjacent-log1.5;adjusted-log-vs-raw+.10;no-missing-zero-fill"},
      {"one_way_bps", cfg.target.one_way_bps}, {"annual_borrow_bps", cfg.target.annual_borrow_bps},
      {"cost_basis", "target-change-no-drift;short-target/252;not-fills"},
      {"max_working_bytes", cfg.target.max_working_bytes}};
}
co::Status write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "target replay: JSON output");
  file << j.dump(2) << '\n'; file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "target replay: JSON close"));
}
co::Status write_days(const std::filesystem::path& path, const TargetReplayResult& result) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "target replay: daily output");
  file << "decision,session_ns,month,entry,endpoint,turnover,forced,discretionary,deployment,"
          "month_turnover,budget_excess,applied_fraction,gross,net,long_weight,short_weight,"
          "max_abs_weight,effective_names,held_names,return_mature,return_complete,"
          "observed_return_component,missing_long,missing_short,missing_gross,missing_names,"
          "guarded_names,modeled_trade_cost,modeled_borrow_cost,complete_gross_return,complete_net_return\n";
  file << std::setprecision(17);
  for (const auto& d : result.days) {
    file << d.decision << ',' << d.session << ',' << d.calendar_month << ',' << d.entry << ','
         << d.endpoint << ',' << d.turnover << ',' << d.forced_turnover << ','
         << d.discretionary_turnover << ',' << d.deployment_turnover << ',' << d.month_turnover << ','
         << d.budget_excess << ',' << d.applied_fraction << ',' << d.gross << ',' << d.net << ','
         << d.long_weight << ',' << d.short_weight << ',' << d.max_abs_weight << ','
         << d.effective_names << ',' << d.held_names << ',' << d.return_mature << ','
         << d.return_complete << ',' << d.observed_return_component << ',' << d.missing_long << ','
         << d.missing_short << ',' << d.missing_gross << ',' << d.missing_names << ','
         << d.guarded_names << ',' << d.modeled_trade_cost << ',' << d.modeled_borrow_cost << ','
         << d.complete_gross_return << ',' << d.complete_net_return << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "target replay: daily close"));
}
Json summarize(const TargetReplayResult& result) {
  struct Month { usize days{}; f64 turnover{}, forced{}, discretionary{}, deployment{}, excess{}; };
  std::map<u32, Month> months;
  f64 gross = 0, max_net = 0, max_name = 0, missing = 0; usize mature = 0, complete = 0;
  f64 observed = 0, complete_gross = 0, complete_net = 0, trade_cost = 0, borrow_cost = 0;
  for (const auto& d : result.days) {
    auto& m = months[d.calendar_month]; ++m.days; m.turnover += d.turnover;
    m.forced += d.forced_turnover; m.discretionary += d.discretionary_turnover;
    m.deployment += d.deployment_turnover; m.excess = std::max(m.excess, d.budget_excess);
    gross += d.gross; max_net = std::max(max_net, std::abs(d.net));
    max_name = std::max(max_name, d.max_abs_weight); missing += d.missing_gross;
    mature += d.return_mature ? 1U : 0U; complete += d.return_complete ? 1U : 0U;
    if (d.return_mature) observed += d.observed_return_component;
    if (d.return_complete) { complete_gross += d.complete_gross_return; complete_net += d.complete_net_return; }
    trade_cost += d.modeled_trade_cost; borrow_cost += d.modeled_borrow_cost;
  }
  Json rows = Json::array(); f64 max_month = 0, total = 0;
  for (const auto& [id, m] : months) {
    rows.push_back({{"month", id}, {"observed_decision_sessions", m.days}, {"turnover", m.turnover},
        {"forced", m.forced}, {"discretionary", m.discretionary}, {"deployment", m.deployment},
        {"budget_excess", m.excess}});
    max_month = std::max(max_month, m.turnover); total += m.turnover;
  }
  return Json{{"months", std::move(rows)}, {"total_turnover", result.total_turnover},
      {"monthly_reconciled_total", total}, {"mean_monthly_turnover", total / static_cast<f64>(months.size())},
      {"max_monthly_turnover", max_month}, {"forced_turnover", result.forced_turnover},
      {"discretionary_turnover", result.discretionary_turnover},
      {"deployment_turnover", result.deployment_turnover}, {"deployment_date_index", result.deployment_date},
      {"mean_gross", gross / static_cast<f64>(result.days.size())}, {"max_abs_net", max_net},
      {"max_abs_name_weight", max_name}, {"mature_return_days", mature}, {"complete_return_days", complete},
      {"incomplete_return_days", mature - complete}, {"summed_missing_gross_exposure", missing},
      {"observed_component_sum_not_portfolio_return", observed},
      {"complete_days_gross_sum_not_total_period_return", complete_gross},
      {"complete_days_net_sum_not_total_period_return", complete_net},
      {"modeled_trade_cost_all_decisions", trade_cost}, {"modeled_borrow_cost_all_decisions", borrow_cost},
      {"net_sharpe", nullptr}, {"self_financing_nav", false}, {"capacity_qualified", false}};
}
} // namespace

co::Status run_target_replay(const TargetReplayRunConfig& cfg, std::ostream& progress) {
  try {
    ATX_TRY_VOID(validate_config(cfg.target));
    if constexpr (std::endian::native != std::endian::little)
      return co::Err(co::ErrorCode::Unavailable, "target replay: little-endian host required");
    if (!std::numeric_limits<f64>::is_iec559 || cfg.output_directory.empty() ||
        cfg.role_path.empty() != cfg.role_sha256.empty() ||
        cfg.target.max_working_bytes < metadata_bytes)
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: output/role/budget contract");
    SavedBlend blend; ATX_TRY_VOID(admit_saved(cfg, blend)); ATX_TRY_VOID(load_prices(cfg, blend));
    ATX_TRY(auto result, replay_targets(blend.view(), cfg.target));
    const auto dir = std::filesystem::path(cfg.output_directory);
    if (!std::filesystem::create_directory(dir))
      return co::Err(co::ErrorCode::AlreadyExists, "target replay: output must not exist");
    const auto method = recipe(cfg); ATX_TRY(auto method_sha, co::sha256_hex(method.dump()));
    ATX_TRY_VOID(write_json(dir / "recipe.json", method));
    ATX_TRY_VOID(write_days(dir / "daily.csv", result));
    ATX_TRY(auto daily_sha, co::sha256_file((dir / "daily.csv").string()));
    auto summary = summarize(result);
    summary["schema"] = "atx.dsl-target-replay-summary/v1"; summary["status"] = "complete";
    summary["recipe_sha256"] = method_sha; summary["combined_sha256"] = cfg.combined_sha256;
    summary["source_bindings"] = blend.manifest; summary["daily_csv_sha256"] = daily_sha;
    summary["limitations"] = "planned weights without drift; decision-month turnover includes deployment; "
        "rough delayed observed returns only, incomplete days unavailable; no corporate-action accounting, "
        "fills, funding, realized fees, capacity, NAV or realistic Sharpe";
    ATX_TRY_VOID(write_json(dir / "summary.json", summary));
    progress << "target replay complete: " << result.days.size() << " decisions; total planned turnover "
             << std::setprecision(17) << result.total_turnover << '\n';
    return co::Ok();
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("target replay: ") + e.what());
  }
}
int dispatch_target_replay(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    TargetReplayRunConfig cfg; std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "--combined PATH --combined-sha256 SHA --output DIR "
               "[--rule baseline-v1|monthly-budget-v2] [--cadence 5] [--trade-fraction .25] "
               "[--monthly-budget .30] [--role PATH --role-sha256 SHA] "
               "[--one-way-bps 0 --annual-borrow-bps 0] [--max-bytes 536870912]\n";
        return 0;
      }
      if (!seen.insert(key).second || i + 1 >= argc) throw std::invalid_argument("duplicate/missing flag");
      const std::string value = argv[++i];
      const auto real = [&]() {
        usize used = 0; const f64 x = std::stod(value, &used);
        if (used != value.size()) throw std::invalid_argument("invalid number");
        return x;
      };
      const auto integer = [&]() {
        u64 x = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
          throw std::invalid_argument("invalid integer");
        return x;
      };
      if (key == "--combined") cfg.combined_path = value;
      else if (key == "--combined-sha256") cfg.combined_sha256 = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--role") cfg.role_path = value;
      else if (key == "--role-sha256") cfg.role_sha256 = value;
      else if (key == "--cadence") {
        const auto x = integer(); if (x > max_dates) throw std::invalid_argument("cadence exceeds bound");
        cfg.target.cadence = static_cast<usize>(x);
      } else if (key == "--trade-fraction") cfg.target.trade_fraction = real();
      else if (key == "--monthly-budget") cfg.target.monthly_budget = real();
      else if (key == "--one-way-bps") cfg.target.one_way_bps = real();
      else if (key == "--annual-borrow-bps") cfg.target.annual_borrow_bps = real();
      else if (key == "--max-bytes") cfg.target.max_working_bytes = integer();
      else if (key == "--rule") {
        if (value == "baseline-v1") cfg.target.rule = TargetReplayRule::BaselineTargetV1;
        else if (value == "monthly-budget-v2") cfg.target.rule = TargetReplayRule::MonthlyTargetBudgetV2;
        else throw std::invalid_argument("unknown target rule");
      } else throw std::invalid_argument("unknown flag: " + key);
    }
    const auto status = run_target_replay(cfg, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << "target replay: " << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
