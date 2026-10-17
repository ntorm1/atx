#include "strategy_sign_verb.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/admission/screen.hpp"
#include "atx/engine/research/admission/sign_rule.hpp"

namespace atx::impl::strategy {
namespace {
namespace fs = std::filesystem;
namespace adm = atx::engine::research::admission;
namespace co = atx::core;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr u64 kRowsLimit = 16ULL << 20;
constexpr usize kMaxRows = 20000U;

// A wave manifest's sign_rule id and the predicate's rule it applies. The verb holds no decision
// of its own: every verdict is adm::pm7_35's.
struct WaveSignRule {
  std::string_view id;
  adm::SignRule predicate{adm::SignRule::Unknown};
};
constexpr std::array<WaveSignRule, 1> kWaveSignRules{{{"pm7-35", adm::kPm735SignRule}}};

// wave_rules.py ADD / REPLACE.
struct KindName {
  std::string_view name;
  adm::CandidateKind kind{adm::CandidateKind::Addition};
};
constexpr std::array<KindName, 2> kKindNames{
    {{"add", adm::CandidateKind::Addition}, {"replace", adm::CandidateKind::Replacement}}};

// One screened candidate as the rows file lists it (validated).
struct SignRow {
  std::string id;
  KindName kind;
  i32 prior_sign{};
  std::string status;
  i32 runner_sign{};
};

struct SignConfig {
  WaveSignRule rule;
  std::string rows_path, output_path;
};

struct SignCounts {
  usize rows{}, kept{}, dropped{};
};

[[nodiscard]] co::Error refused(const std::string& message) {
  return co::Error(co::ErrorCode::InvalidArgument, "sign: " + message);
}

[[nodiscard]] std::optional<WaveSignRule> find_rule(std::string_view id) noexcept {
  for (const WaveSignRule& rule : kWaveSignRules) {
    if (rule.id == id) return rule;
  }
  return std::nullopt;
}

[[nodiscard]] std::string known_rules() {
  std::string out;
  for (const WaveSignRule& rule : kWaveSignRules) {
    out += (out.empty() ? "" : ", ") + std::string(rule.id);
  }
  return out;
}

[[nodiscard]] std::optional<KindName> find_kind(std::string_view name) noexcept {
  for (const KindName& kind : kKindNames) {
    if (kind.name == name) return kind;
  }
  return std::nullopt;
}

// The integer at row[key] when it lies in [lowest, highest]: not a boolean, a float or a string
// (nlohmann's is_number_integer), and an unsigned value is range-checked before any narrowing.
[[nodiscard]] std::optional<i32> small_integer(const Json& row, const char* key, i64 lowest,
                                               i64 highest) {
  if (!row.contains(key) || !row.at(key).is_number_integer()) return std::nullopt;
  const Json& value = row.at(key);
  if (value.is_number_unsigned() && value.get<u64>() > static_cast<u64>(highest)) {
    return std::nullopt;
  }
  const i64 v = value.get<i64>();
  if (v < lowest || v > highest) return std::nullopt;
  return static_cast<i32>(v);
}

[[nodiscard]] bool non_empty_string(const Json& row, const char* key) {
  return row.contains(key) && row.at(key).is_string() &&
         !row.at(key).get_ref<const std::string&>().empty();
}

// Row `index` (0-based) of the rows array. A row without its admission keys is a string the
// screen did not score: refused, never a drop (wave_rules.sign_pm7_35's None row).
[[nodiscard]] co::Result<SignRow> parse_row(const Json& row, usize index) {
  const std::string where = "row " + std::to_string(index + 1U);
  if (!row.is_object()) return co::Err(refused(where + " is not an object"));
  if (!non_empty_string(row, "id")) return co::Err(refused(where + " needs a non-empty string id"));
  SignRow out;
  out.id = row.at("id").get<std::string>();
  const std::string named = where + " (" + out.id + ")";
  if (!row.contains("kind") || !row.at("kind").is_string()) {
    return co::Err(refused(named + " needs a string kind (add | replace)"));
  }
  const std::optional<KindName> kind = find_kind(row.at("kind").get_ref<const std::string&>());
  if (!kind) {
    return co::Err(refused(named + ": unknown kind \"" + row.at("kind").get<std::string>() +
                           "\" (add | replace)"));
  }
  out.kind = *kind;
  const std::optional<i32> prior = small_integer(row, "prior_sign", -1, 1);
  if (!prior || *prior == 0) {
    return co::Err(refused(named + " needs an integer prior_sign 1 or -1"));
  }
  out.prior_sign = *prior;
  if (!row.contains("status") || !row.contains("runner_sign")) {
    return co::Err(refused(named + " has no admission row (status, runner_sign): the screen did "
                                   "not score this string (a stop, not a drop)"));
  }
  if (!non_empty_string(row, "status")) {
    return co::Err(refused(named + " needs a non-empty string status"));
  }
  out.status = row.at("status").get<std::string>();
  const std::optional<i32> runner = small_integer(row, "runner_sign", -1, 1);
  if (!runner) return co::Err(refused(named + " needs an integer runner_sign -1, 0 or 1"));
  out.runner_sign = *runner;
  return co::Ok(std::move(out));
}

[[nodiscard]] co::Result<std::vector<SignRow>> parse_rows(std::string_view text) {
  const Json doc = Json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    return co::Err(refused("the rows file is not a JSON object"));
  }
  if (!doc.contains("rows") || !doc.at("rows").is_array() || doc.at("rows").empty()) {
    return co::Err(refused("the rows file needs a non-empty rows array"));
  }
  const Json& rows = doc.at("rows");
  if (rows.size() > kMaxRows) {
    return co::Err(refused("more than " + std::to_string(kMaxRows) + " rows"));
  }
  std::vector<SignRow> out;
  out.reserve(rows.size());
  std::set<std::string> seen;
  for (usize k = 0; k < rows.size(); ++k) {
    ATX_TRY(SignRow row, parse_row(rows[k], k));
    if (!seen.insert(row.id).second) return co::Err(refused("duplicate id " + row.id));
    out.push_back(std::move(row));
  }
  return co::Ok(std::move(out));
}

// Every byte of the rows file, refused above kRowsLimit.
[[nodiscard]] co::Result<std::string> read_rows_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return co::Err(co::ErrorCode::IoError, "sign: cannot open the rows file " + path.string());
  }
  const std::streamoff size = in.tellg();
  if (size < 0 || static_cast<u64>(size) > kRowsLimit) {
    return co::Err(refused("the rows file " + path.string() + " is unreadable or over 16 MiB"));
  }
  std::string out(static_cast<usize>(size), '\0');
  in.seekg(0);
  in.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (!in) {
    return co::Err(co::ErrorCode::IoError, "sign: cannot read the rows file " + path.string());
  }
  return co::Ok(std::move(out));
}

[[nodiscard]] bool admitted(const SignRow& row) noexcept {
  return row.status == adm::status_name(adm::Status::Admitted);
}

[[nodiscard]] std::string_view verdict_name(adm::SignVerdict verdict) noexcept {
  switch (verdict) {
  case adm::SignVerdict::Keep:
    return "keep";
  case adm::SignVerdict::Drop:
    return "drop";
  }
  return "";
}

// Python's f"{x:+d}".
[[nodiscard]] std::string signed_text(i32 x) {
  return (x >= 0 ? "+" : "") + std::to_string(x);
}

// The words of wave_rules.sign_pm7_35 for the predicate's verdict: a description of the decision,
// keyed by the verdict, never a second decision.
[[nodiscard]] std::string reason_of(const SignRow& row, adm::SignVerdict verdict) {
  const bool keep = verdict == adm::SignVerdict::Keep;
  const std::string runner = std::to_string(row.runner_sign);
  const std::string prior = signed_text(row.prior_sign);
  if (row.kind.kind == adm::CandidateKind::Replacement) {
    return keep ? std::string("replacement admitted with the prior sign")
                : "replacement not kept (status " + row.status + ", runner sign " + runner +
                      ", prior " + prior + "): the replaced member keeps its string at 0 trials";
  }
  if (!admitted(row)) {
    return "addition not admitted (status " + row.status + "): " +
           (keep ? "stays at weight 0" : "dropped from the wave");
  }
  if (!keep) {
    return "addition admitted with runner sign " + runner + " against prior " + prior +
           ": dropped from the wave";
  }
  return row.runner_sign == row.prior_sign
             ? std::string("addition admitted with the prior sign")
             : "addition admitted, runner sign " + runner + " (R-2 precedent)";
}

[[nodiscard]] OrderedJson receipt_json(const SignConfig& cfg, const std::string& rows_sha256,
                                       std::span<const SignRow> rows) {
  OrderedJson listed = OrderedJson::array();
  OrderedJson kept = OrderedJson::array();
  OrderedJson dropped = OrderedJson::array();
  for (const SignRow& row : rows) {
    const adm::SignVerdict verdict = adm::pm7_35(cfg.rule.predicate, row.kind.kind, admitted(row),
                                                 row.prior_sign, row.runner_sign);
    listed.push_back(OrderedJson{{"id", row.id},
                                 {"kind", std::string(row.kind.name)},
                                 {"prior_sign", row.prior_sign},
                                 {"status", row.status},
                                 {"runner_sign", row.runner_sign},
                                 {"verdict", std::string(verdict_name(verdict))},
                                 {"reason", reason_of(row, verdict)}});
    (verdict == adm::SignVerdict::Keep ? kept : dropped).push_back(row.id);
  }
  return OrderedJson{
      {"schema", std::string(kSignReceiptSchema)},
      {"rule", std::string(cfg.rule.id)},
      {"sign_rule", std::string(adm::sign_rule_name(cfg.rule.predicate))},
      {"predicate", "pm7_35(sign_rule, kind, status == \"admitted\", prior_sign, runner_sign)"},
      {"rows_file", cfg.rows_path},
      {"rows_sha256", rows_sha256},
      {"rows", std::move(listed)},
      {"kept", std::move(kept)},
      {"dropped", std::move(dropped)}};
}

// The receipt, written once every check has passed (a failed write leaves no file behind).
[[nodiscard]] co::Status write_receipt(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  if (!out) {
    std::error_code ec;
    fs::remove(path, ec);
    return co::Err(co::ErrorCode::IoError, "sign: cannot write " + path.string());
  }
  return co::Ok();
}

[[nodiscard]] co::Result<SignCounts> run_sign(const SignConfig& cfg) {
  const fs::path output(cfg.output_path);
  std::error_code ec;
  if (fs::exists(output, ec) || ec) {
    return co::Err(co::ErrorCode::AlreadyExists,
                   "sign: the output must not exist (never overwritten): " + cfg.output_path);
  }
  ATX_TRY(const std::string text, read_rows_file(fs::path(cfg.rows_path)));
  ATX_TRY(const std::string digest, co::sha256_hex(std::string_view(text)));
  ATX_TRY(const std::vector<SignRow> rows, parse_rows(text));
  const OrderedJson receipt = receipt_json(cfg, digest, rows);
  ATX_TRY_VOID(write_receipt(output, receipt.dump(2) + "\n"));
  return co::Ok(SignCounts{rows.size(), receipt.at("kept").size(), receipt.at("dropped").size()});
}

[[nodiscard]] std::string usage() {
  return "usage: atx-equity-strategy-ic sign --rule " + known_rules() +
         " --rows FILE --output NEWFILE\n"
         "Applies the wave's sign rule (the engine's research/admission pm7_35) to FILE's rows "
         "[{id, kind add|replace, prior_sign, status, runner_sign}] and writes the receipt "
         "(atx.sign-receipt/v1: each row's verdict keep|drop and reason, FILE's SHA-256).\n";
}

struct Parsed {
  SignConfig cfg;
  std::string problem; // the first usage error; empty when none
  bool help{};
};

[[nodiscard]] Parsed parse_args(int argc, char** argv) {
  Parsed p;
  std::set<std::string_view> seen;
  for (int i = 1; i < argc; i += 2) {
    const std::string_view key = argv[i];
    if (key == "--help") {
      p.help = true;
      return p;
    }
    if (i + 1 >= argc || !seen.insert(key).second) {
      p.problem = "missing value or repeated flag " + std::string(key);
      return p;
    }
    const std::string_view value = argv[i + 1];
    if (key == "--rule") {
      const std::optional<WaveSignRule> rule = find_rule(value);
      if (!rule) {
        p.problem = "unknown --rule " + std::string(value) + " (known: " + known_rules() + ")";
        return p;
      }
      p.cfg.rule = *rule;
    } else if (key == "--rows") {
      p.cfg.rows_path = value;
    } else if (key == "--output") {
      p.cfg.output_path = value;
    } else {
      p.problem = "unknown flag " + std::string(key);
      return p;
    }
  }
  if (p.cfg.rule.id.empty() || p.cfg.rows_path.empty() || p.cfg.output_path.empty()) {
    p.problem = "--rule, --rows and --output are required";
  }
  return p;
}
} // namespace

int dispatch_sign(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    const Parsed p = parse_args(argc, argv);
    if (p.help) {
      out << usage();
      return 0;
    }
    if (!p.problem.empty()) {
      err << "sign: " << p.problem << '\n' << usage();
      return 2;
    }
    const co::Result<SignCounts> done = run_sign(p.cfg);
    if (!done) {
      err << done.error().to_string() << '\n';
      return 1;
    }
    out << "sign: " << p.cfg.rule.id << " (" << adm::sign_rule_name(p.cfg.rule.predicate)
        << "): " << done->rows << " rows, kept " << done->kept << ", dropped " << done->dropped
        << " -> " << p.cfg.output_path << '\n';
    return 0;
  } catch (const std::exception& e) {
    err << "sign: " << e.what() << '\n'; // allocation or JSON failure: nothing was written
    return 1;
  }
}
} // namespace atx::impl::strategy
