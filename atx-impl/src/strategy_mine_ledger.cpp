// The mining campaign's cycle-ledger line (platform v8 H-3, Ruling E-33; review MINE-1).
// Contracts in strategy_mine_ledger.hpp; the Python twin is backtest_integrity.campaign_line.
#include "strategy_mine_ledger.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
using Json = nlohmann::json;

constexpr std::string_view kLedgerSchema = "atx.trial-ledger/v1";
constexpr std::string_view kLedgerKind = "mining-campaign";
constexpr std::string_view kLedgerRule = "mined-v1";
constexpr atx::usize kTrialIdDigits = 16;

bool lower_hex(std::string_view text, atx::usize digits) {
  return text.size() == digits && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool iso_date(std::string_view text) {
  if (text.size() != 10U || text[4] != '-' || text[7] != '-') return false;
  for (atx::usize i = 0; i < text.size(); ++i)
    if (i != 4U && i != 7U && (text[i] < '0' || text[i] > '9')) return false;
  return true;
}

// campaign_line's refusals, in its order and words.
std::string field_problem(const MineLedgerLine &line) {
  if (line.campaign_id.empty()) return "a mining campaign needs a name";
  if (!lower_hex(line.registry_head, 64U))
    return "a mining campaign names its registry's chain head (a SHA-256 hex digest)";
  if (line.registry_count < 1U) return "a mining campaign's registry count is a positive integer";
  if (line.registry_total < line.registry_count)
    return "a mining campaign's registry total is at least its count (Ruling E-33a)";
  if (line.budget < line.registry_count)
    return "a mining campaign's budget is fixed in advance and covers its registry count "
           "(pre-registration rule 10)";
  if (line.registry_bytes < 1U) return "a mining campaign's registry byte count is a positive integer";
  if (!lower_hex(line.recipe_sha256, 64U))
    return "a mining campaign names its trial recipe (a SHA-256 hex digest)";
  if (!iso_date(line.confirm_begin) || !iso_date(line.confirm_end) ||
      !(line.confirm_begin < line.confirm_end))
    return "a mining campaign names its confirm window (YYYY-MM-DD dates, begin before end)";
  if (line.window_id.empty()) return "a mining campaign names its research window";
  return {};
}

// The text campaign_line would write for `line` (fields already checked).
co::Result<std::string> line_text(const MineLedgerLine &line) {
  ATX_TRY(const std::string ident,
          co::sha256_hex(Json::array({std::string(kLedgerKind), line.recipe_sha256,
                                      line.registry_head})
                             .dump()));
  std::string path = line.registry_path;
  std::replace(path.begin(), path.end(), '\\', '/');
  const Json out{{"schema", std::string(kLedgerSchema)},
                 {"kind", std::string(kLedgerKind)},
                 {"count", 0},
                 {"campaign", line.campaign_id},
                 {"origin", "mined"},
                 {"rule", std::string(kLedgerRule)},
                 {"budget", line.budget},
                 {"recipe_sha256", line.recipe_sha256},
                 {"confirm", {{"begin", line.confirm_begin}, {"end", line.confirm_end}}},
                 {"window_id", line.window_id},
                 {"registry",
                  {{"path", path},
                   {"chain_head", line.registry_head},
                   {"bytes", line.registry_bytes},
                   {"count", line.registry_count},
                   {"total", line.registry_total}}},
                 {"trial_id", ident.substr(0, kTrialIdDigits)}};
  return co::Ok(out.dump());
}

std::string text_at(const Json &j, const char *key) {
  return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>()
                                                                   : std::string{};
}

atx::u64 unsigned_at(const Json &j, const char *key) {
  return j.is_object() && j.contains(key) && j.at(key).is_number_unsigned()
             ? j.at(key).get<atx::u64>()
             : atx::u64{0};
}
} // namespace

co::Result<std::string> mine_ledger_line(const MineLedgerLine &line) {
  if (const std::string problem = field_problem(line); !problem.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "mine ledger line: " + problem);
  return line_text(line);
}

std::string mine_ledger_line_problem(std::string_view text) {
  try {
    std::string_view body = text;
    if (!body.empty() && body.back() == '\n') body.remove_suffix(1U);
    const Json j = Json::parse(std::string(body), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "not a JSON object";
    const Json registry = j.contains("registry") ? j.at("registry") : Json::object();
    const Json confirm = j.contains("confirm") ? j.at("confirm") : Json::object();
    if (text_at(j, "rule") != kLedgerRule) return "a mining campaign's rule is mined-v1";
    MineLedgerLine line;
    line.campaign_id = text_at(j, "campaign");
    line.registry_path = text_at(registry, "path");
    line.registry_head = text_at(registry, "chain_head");
    line.registry_bytes = unsigned_at(registry, "bytes");
    line.registry_count = unsigned_at(registry, "count");
    line.registry_total = unsigned_at(registry, "total");
    line.budget = unsigned_at(j, "budget");
    line.recipe_sha256 = text_at(j, "recipe_sha256");
    line.confirm_begin = text_at(confirm, "begin");
    line.confirm_end = text_at(confirm, "end");
    line.window_id = text_at(j, "window_id");
    if (const std::string problem = field_problem(line); !problem.empty()) return problem;
    const auto expected = line_text(line);
    if (!expected) return expected.error().to_string();
    return *expected == body ? std::string{} : "differs from campaign_line: " + *expected;
  } catch (const std::exception &e) {
    return e.what();
  }
}

} // namespace atx::impl::strategy
