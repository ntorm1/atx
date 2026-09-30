#include "strategy_live.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <new>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/book/target_shaping.hpp"
#include "build_provenance.hpp"
#include "stage_data_provenance.hpp"
#include "strategy_cost_v2.hpp"
#include "strategy_holdings.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_nav_replay_detail.hpp"
#include "strategy_orders.hpp"
#include "strategy_target_replay.hpp"
#include "strategy_target_replay_detail.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr u64 max_manifest_bytes = 1ULL << 20, max_fields_manifest_bytes = 16ULL << 20;
constexpr usize max_cadence = 4096;
constexpr u64 max_warm_start = 4096; // the nav verb's --warm-start-sessions bound
constexpr const char* targets_exe = "atx-equity-strategy-targets";
constexpr const char* ic_exe = "atx-equity-strategy-ic";
constexpr const char* tc_definition =
    "Pearson correlation over the decision's members with a finite signal and sigma > 0 of "
    "alpha_i / sigma_i^2 (alpha_i = the pinned combined signal at the as-of row, sigma_i = "
    "the member's daily-return SD over [d-w, d), the replay's execution liquidity "
    "definition) with the target weight (target) and with aim_leverage x desired (aim, "
    "effective rebalances only); NaN below 3 names or without dispersion; a diagnostic "
    "(Clarke-de Silva-Thorley 2002), never an input; the L3 definition (L3-F2), 1/sigma off "
    "from Grinold-Kahn";
constexpr const char* tc_replay_definition =
    "the NAV replay's own (cost_v2::transfer_coefficient; the transfer_coefficient column of "
    "v7_transfer_coefficient.csv at this session): corr over members with a finite sigma_i > 0 "
    "of desired_i / sigma_i (= alpha_i / sigma_i^2 with Grinold-Kahn alpha_i = IC sigma_i z_i, "
    "z = the neutralized rank aim) and the rule's plan before the locate block; NaN off an "
    "effective rebalance (desired all 0)";
constexpr const char* tc_target_definition =
    "as transfer_coefficient but on the final target weight (after the locate block): the "
    "book the orders build; the basis of the B9 band";
constexpr const char* decide_limitations =
    "TRAIN-only (role class train, sessions before the research seal); targets from the "
    "replay's own DECIDE functions at one as-of row (tiers, locate-in-aim mask, desired "
    "target with its neutralization guard, target rule, locate block); orders.csv holds "
    "weight deltas and decision-NAV dollars (target x NAV - held); orders_shares.csv the "
    "same deltas as lot-rounded shares at the as-of raw close sized to nav_dollars "
    "(one order type: moc, the replay's next-close fill; a name crossing zero is one "
    "order flagged short_sale, not split); working orders of earlier decisions are not "
    "modeled (every order is "
    "re-decided from the actual positions; the replay keeps unchanged members' working "
    "orders on non-rebalance days); the cadence phase is the blend's score_begin (pinned "
    "as nav.cadence_anchor when cadence > 1); rate "
    "per-name-v1 and monthly-budget-v2 are refused (book state positions do not carry); "
    "locates optional (without a file every short target is reported without a locate)";

bool hex_of(std::string_view s, usize length) {
  return s.size() == length && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool sha_value(const Json& v) {
  return v.is_string() && hex_of(v.get_ref<const std::string&>(), 64);
}
co::Result<i64> session_of(std::string_view text) {
  int year = 0; unsigned month = 0, day = 0;
  const auto part = [&](usize at, usize size, auto& out) {
    const auto parsed = std::from_chars(text.data() + at, text.data() + at + size, out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + at + size;
  };
  if (text.size() != 10 || text[4] != '-' || text[7] != '-' || !part(0, 4, year) ||
      !part(5, 2, month) || !part(8, 2, day))
    return co::Err(co::ErrorCode::InvalidArgument, "decide: session must be YYYY-MM-DD");
  const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month},
                                         std::chrono::day{day}};
  if (!date.ok())
    return co::Err(co::ErrorCode::InvalidArgument, "decide: not a calendar date");
  return co::Ok(static_cast<i64>(std::chrono::sys_days{date}.time_since_epoch().count()) * day_ns);
}
struct PinnedText {
  std::string text, sha256;
};
co::Result<PinnedText> read_text(const std::string& path, u64 cap, const char* what) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > cap)
    return co::Err(co::ErrorCode::InvalidArgument,
                   std::string("decide: ") + what + " missing or oversized: " + path);
  std::string text(static_cast<usize>(file.tellg()), '\0');
  file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) return co::Err(co::ErrorCode::IoError, std::string("decide: ") + what + " read");
  ATX_TRY(auto sha, co::sha256_hex(text));
  return co::Ok(PinnedText{std::move(text), std::move(sha)});
}
co::Status mismatch(const std::string& pin, const std::string& why = {}) {
  return co::Err(co::ErrorCode::InvalidArgument,
                 "deploy manifest: pin mismatch " + pin + (why.empty() ? "" : " (" + why + ")"));
}

// ---- atx.book-deploy/v1 ----
enum class PinKind : u8 { Sha, ShaOrNull, GitSha, Text };
struct PinSpec {
  const char* object; // "" for a top-level key
  const char* key;
  PinKind kind;
};
constexpr std::array<PinSpec, 22> pin_specs{{
    {"", "book", PinKind::Text},
    {"library", "sha256", PinKind::Sha},
    {"recipe", "sha256", PinKind::Sha},
    {"orientations", "sha256", PinKind::Sha},
    {"composition", "scheme", PinKind::Text},
    {"composition", "weights_sha256", PinKind::ShaOrNull},
    {"combined", "path", PinKind::Text},
    {"combined", "sha256", PinKind::Sha},
    {"role", "path", PinKind::Text},
    {"role", "sha256", PinKind::Sha},
    {"role", "class", PinKind::Text},
    {"fields", "path", PinKind::Text},
    {"fields", "sha256", PinKind::Sha},
    {"fields", "code_sha256", PinKind::Sha},
    {"", "data_source_sha256", PinKind::Sha},
    {"universe", "member_semantics", PinKind::Text},
    {"nav", "recipe_sha256", PinKind::Sha},
    {"executables", targets_exe, PinKind::Sha},
    {"executables", ic_exe, PinKind::Sha},
    {"source", "git_sha", PinKind::GitSha},
    {"seal", "policy", PinKind::Text},
    {"seal", "exclusive_session", PinKind::Text},
}};
std::string pin_name(const PinSpec& p) {
  return *p.object ? std::string(p.object) + "." + p.key : std::string(p.key);
}
const Json* find_pin(const Json& m, const PinSpec& p) {
  const Json* parent = &m;
  if (*p.object) {
    const auto it = m.find(p.object);
    if (it == m.end() || !it->is_object()) return nullptr;
    parent = &*it;
  }
  const auto it = parent->find(p.key);
  return it == parent->end() ? nullptr : &*it;
}
bool pin_ok(const Json& v, PinKind kind) {
  switch (kind) {
  case PinKind::Sha: return sha_value(v);
  case PinKind::ShaOrNull: return v.is_null() || sha_value(v);
  case PinKind::GitSha: return v.is_string() && hex_of(v.get_ref<const std::string&>(), 40);
  case PinKind::Text: return v.is_string() && !v.get_ref<const std::string&>().empty();
  }
  return false;
}
bool number(const Json& o, const char* key, f64& out) {
  const auto it = o.find(key);
  if (it == o.end() || !it->is_number()) return false;
  out = it->get<f64>();
  return std::isfinite(out);
}
bool unsigned_integer(const Json& o, const char* key, u64& out) {
  const auto it = o.find(key);
  if (it == o.end() || !it->is_number_unsigned()) return false;
  out = it->get<u64>();
  return true;
}
bool boolean(const Json& o, const char* key, bool& out) {
  const auto it = o.find(key);
  if (it == o.end() || !it->is_boolean()) return false;
  out = it->get<bool>();
  return true;
}
bool text(const Json& o, const char* key, std::string& out) {
  const auto it = o.find(key);
  if (it == o.end() || !it->is_string()) return false;
  out = it->get<std::string>();
  return true;
}
struct Bands {
  f64 gross_lo{}, gross_hi{}, abs_net_max{}, turnover_max{};
  u64 no_locate_max{};
};
struct Deploy {
  Json m;
  std::string sha256;
  TargetReplayRunConfig run;
  NavReplayConfig base; // the primary book: base + nav_scenario_matrix(true)[primary]
  NavTurnoverLimits limits;
  NavFieldsPin fields;
  Bands bands;
  bool owner_gate{};
};
co::Status nav_missing(const char* key) {
  return co::Err(co::ErrorCode::InvalidArgument,
                 std::string("deploy manifest: nav.") + key + " missing or malformed");
}
co::Status parse_rule(const Json& nav, TargetReplayConfig& t) {
  std::string rule;
  if (!text(nav, "rule", rule)) return nav_missing("rule");
  if (rule == "baseline-v1") t.rule = TargetReplayRule::BaselineTargetV1;
  else if (rule == "monthly-budget-v2") t.rule = TargetReplayRule::MonthlyTargetBudgetV2;
  else if (rule == "aim-partial-v5") t.rule = TargetReplayRule::AimPartialV5;
  else return nav_missing("rule");
  u64 cadence = 0;
  if (!unsigned_integer(nav, "cadence", cadence) || !cadence || cadence > max_cadence)
    return nav_missing("cadence");
  t.cadence = static_cast<usize>(cadence);
  const std::array<std::pair<const char*, f64*>, 6> reals{{
      {"trade_fraction", &t.trade_fraction}, {"dust_multiple", &t.dust_multiple},
      {"aim_leverage", &t.aim_leverage}, {"band_multiple", &t.band_multiple},
      {"monthly_budget", &t.monthly_budget}, {"exit_rate", &t.exit_rate}}};
  for (const auto& [key, out] : reals)
    if (!number(nav, key, *out)) return nav_missing(key);
  std::string neutralize;
  if (!text(nav, "neutralize", neutralize) || !detail::parse_neutralize(neutralize, t))
    return nav_missing("neutralize");
  u64 bytes = 0;
  if (!unsigned_integer(nav, "max_working_bytes", bytes) || !bytes)
    return nav_missing("max_working_bytes");
  t.max_working_bytes = bytes;
  return co::Ok();
}
// Optional nav keys added after atx.book-deploy/v1 was frozen (v8). Absent means off (0),
// and each enters the recomputed NAV recipe only when on, so a manifest without them pins the
// recipe exactly as before; present, the key must be well formed and becomes part of the pin.
co::Status parse_optional_nav(const Json& nav, Deploy& out) {
  if (nav.contains("warm_start_sessions")) { // v8 D-0: nav --warm-start-sessions K
    u64 sessions = 0;
    if (!unsigned_integer(nav, "warm_start_sessions", sessions) || sessions > max_warm_start)
      return nav_missing("warm_start_sessions");
    out.base.warm_start_sessions = static_cast<usize>(sessions);
  }
  if (nav.contains("hold_band")) { // v8 R-4: nav --hold-band B (range: the replay's validation)
    f64 band = 0;
    if (!number(nav, "hold_band", band)) return nav_missing("hold_band");
    out.run.target.hold_band = band;
  }
  if (nav.contains("adv_hold_q")) { // v8 R-5: nav --adv-hold-q Q (0 = off hashes as absent)
    if (!number(nav, "adv_hold_q", out.run.target.adv_hold_q)) return nav_missing("adv_hold_q");
  }
  return co::Ok();
}
// nav: the construction and NAV flags of the deployed (primary) book, as the nav verb
// takes them; the scenario is the NAV run's primary (S2 x swap-fin-v1 with the fields).
co::Status parse_nav(const Json& m, Deploy& out) {
  const auto it = m.find("nav");
  if (it == m.end() || !it->is_object())
    return co::Err(co::ErrorCode::InvalidArgument, "deploy manifest: missing pin nav");
  const Json& nav = *it;
  ATX_TRY_VOID(parse_rule(nav, out.run.target));
  std::string basis, rate;
  if (!text(nav, "order_basis", basis) || (basis != "target" && basis != "delta"))
    return nav_missing("order_basis");
  if (!boolean(nav, "locate_in_aim", out.base.locate_in_aim)) return nav_missing("locate_in_aim");
  if (!boolean(nav, "liquidity_cache", out.base.liquidity_cache))
    return nav_missing("liquidity_cache");
  if (!text(nav, "rate", rate)) return nav_missing("rate");
  if (rate != "fixed")
    return co::Err(co::ErrorCode::InvalidArgument,
                   "deploy manifest: nav.rate must be fixed (per-name-v1 needs the pre-trade "
                   "NAV, which positions do not carry)");
  if (!number(nav, "daily_turnover_mean_max", out.limits.daily_mean_max))
    return nav_missing("daily_turnover_mean_max");
  if (!number(nav, "daily_turnover_p95_max", out.limits.daily_p95_max))
    return nav_missing("daily_turnover_p95_max");
  ATX_TRY_VOID(parse_optional_nav(nav, out));
  out.base.order_basis = basis == "delta" ? NavOrderBasis::Delta : NavOrderBasis::Target;
  out.base.target = out.run.target;
  out.base.scenario = nav_scenario_matrix(true)[nav_primary_scenario_index];
  return co::Ok();
}
co::Status parse_bands(const Json& m, Bands& out) {
  const auto it = m.find("health");
  const auto bad = co::Err(co::ErrorCode::InvalidArgument,
                           "deploy manifest: health bands missing or malformed (gross_leverage "
                           "[lo, hi], abs_net_leverage_max, planned_turnover_max, "
                           "names_without_locate_max)");
  if (it == m.end() || !it->is_object()) return bad;
  const auto gross = it->find("gross_leverage");
  if (gross == it->end() || !gross->is_array() || gross->size() != 2 ||
      !(*gross)[0].is_number() || !(*gross)[1].is_number())
    return bad;
  out.gross_lo = (*gross)[0].get<f64>(); out.gross_hi = (*gross)[1].get<f64>();
  if (!(out.gross_lo >= 0 && out.gross_lo <= out.gross_hi && std::isfinite(out.gross_hi)) ||
      !number(*it, "abs_net_leverage_max", out.abs_net_max) || !(out.abs_net_max >= 0) ||
      !number(*it, "planned_turnover_max", out.turnover_max) || !(out.turnover_max >= 0) ||
      !unsigned_integer(*it, "names_without_locate_max", out.no_locate_max))
    return bad;
  return co::Ok();
}
// owner_gate: required key; null, or {owner, ruling, date YYYY-MM-DD}.
co::Result<bool> parse_owner_gate(const Json& m) {
  const auto it = m.find("owner_gate");
  if (it == m.end())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "deploy manifest: missing pin owner_gate (null or a record)");
  if (it->is_null()) return co::Ok(false);
  std::string owner, ruling, date;
  if (!it->is_object() || !text(*it, "owner", owner) || owner.empty() ||
      !text(*it, "ruling", ruling) || ruling.empty() || !text(*it, "date", date) ||
      !session_of(date))
    return co::Err(co::ErrorCode::InvalidArgument, "deploy manifest: malformed owner_gate");
  return co::Ok(true);
}
co::Result<Deploy> read_deploy(const std::string& path) {
  ATX_TRY(auto file, read_text(path, max_manifest_bytes, "deploy manifest"));
  Deploy out;
  out.sha256 = file.sha256;
  out.m = Json::parse(file.text);
  const auto& m = out.m;
  if (!m.is_object() || m.value("schema", std::string{}) != book_deploy_schema)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "deploy manifest: schema must be atx.book-deploy/v1");
  for (const auto& p : pin_specs) {
    const Json* v = find_pin(m, p);
    const char* problem = !v ? "missing" : !pin_ok(*v, p.kind) ? "malformed" : nullptr;
    if (problem)
      return co::Err(co::ErrorCode::InvalidArgument,
                     std::string("deploy manifest: ") + problem + " pin " + pin_name(p));
  }
  const auto names = m.at("fields").find("names");
  if (names == m.at("fields").end() || !names->is_array() || names->empty())
    return co::Err(co::ErrorCode::InvalidArgument, "deploy manifest: missing pin fields.names");
  if (m.at("role").at("class") != "train")
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: role class must be train in this build (TRAIN-only)");
  ATX_TRY_VOID(parse_nav(m, out));
  ATX_TRY_VOID(parse_bands(m, out.bands));
  ATX_TRY(out.owner_gate, parse_owner_gate(m));
  out.run.combined_path = m.at("combined").at("path").get<std::string>();
  out.run.combined_sha256 = m.at("combined").at("sha256").get<std::string>();
  out.run.role_path = m.at("role").at("path").get<std::string>();
  out.run.role_sha256 = m.at("role").at("sha256").get<std::string>();
  out.fields.manifest_path = m.at("fields").at("path").get<std::string>();
  out.fields.manifest_sha256 = m.at("fields").at("sha256").get<std::string>();
  return co::Ok(std::move(out));
}
// The seal block must be this build's policy; a session at or past the seal is refused,
// with or without an owner gate (live sessions are not enabled here; the seal is unchanged).
co::Status check_seal(const Deploy& deploy, i64 asof) {
  const auto& seal = deploy.m.at("seal");
  const std::string window =
      std::string(research_seal_policy) + ", exclusive " + std::string(research_seal_session);
  if (seal.at("policy") != research_seal_policy ||
      seal.at("exclusive_session") != research_seal_session)
    return mismatch("seal", "this build: " + window);
  if (asof < research_seal_exclusive_ns) return co::Ok();
  if (!deploy.owner_gate)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: the as-of session is at or past the research seal (" + window +
                       ") and the manifest carries no owner_gate");
  if constexpr (!live_sessions_enabled)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: owner_gate recorded, but live sessions past the research seal are "
                   "not enabled in this build (the seal is unchanged)");
  return co::Ok();
}
co::Status check_executable(const Deploy& deploy, const std::string& running) {
  const auto& pinned = deploy.m.at("executables").at(targets_exe).get_ref<const std::string&>();
  if (running.empty())
    return co::Err(co::ErrorCode::Unavailable, "decide: cannot hash the running executable");
  if (pinned != running) return mismatch(std::string("executables.") + targets_exe, running);
  return co::Ok();
}
// The pinned combined manifest's own bindings against the deploy pins.
co::Status check_combined(const Deploy& deploy) {
  ATX_TRY(auto file, read_text(deploy.run.combined_path, max_manifest_bytes, "combined manifest"));
  if (file.sha256 != deploy.run.combined_sha256) return mismatch("combined.sha256", file.sha256);
  const auto c = Json::parse(file.text);
  const auto& m = deploy.m;
  const auto same = [&c](const char* key, const Json& pinned) {
    const auto it = c.find(key);
    return it != c.end() && *it == pinned;
  };
  const std::array<std::pair<const char*, std::pair<const char*, const Json*>>, 7> bindings{{
      {"library_sha256", {"library.sha256", &m.at("library").at("sha256")}},
      {"run_recipe_sha256", {"recipe.sha256", &m.at("recipe").at("sha256")}},
      {"orientation_candidates_sha256",
       {"orientations.sha256", &m.at("orientations").at("sha256")}},
      {"source_sha256", {"data_source_sha256", &m.at("data_source_sha256")}},
      {"member_semantics", {"universe.member_semantics", &m.at("universe").at("member_semantics")}},
      {"role", {"role.class", &m.at("role").at("class")}},
      {"role_manifest_sha256", {"role.sha256", &m.at("role").at("sha256")}}}};
  for (const auto& [key, pin] : bindings)
    if (!same(key, *pin.second)) return mismatch(pin.first);
  // Equal-weight blends carry no weights key (null pin); pinned-weight blends their SHA.
  const Json weights = c.contains("composition_weights_sha256")
      ? c.at("composition_weights_sha256") : Json(nullptr);
  if (weights != m.at("composition").at("weights_sha256"))
    return mismatch("composition.weights_sha256");
  const auto fields = c.find("research_fields_manifest_sha256");
  if (fields != c.end() && fields->is_string() && *fields != m.at("fields").at("sha256"))
    return mismatch("fields.sha256", "the blend's research_fields_manifest_sha256");
  return co::Ok();
}
co::Status check_fields(const Deploy& deploy) {
  ATX_TRY(auto file, read_text(deploy.fields.manifest_path, max_fields_manifest_bytes,
                               "fields manifest"));
  if (file.sha256 != deploy.fields.manifest_sha256) return mismatch("fields.sha256", file.sha256);
  const auto f = Json::parse(file.text);
  const auto& pins = deploy.m.at("fields");
  const auto code = f.find("code_sha256");
  if (code == f.end() || *code != pins.at("code_sha256")) return mismatch("fields.code_sha256");
  std::set<std::string> present;
  for (const auto& entry : f.at("fields")) present.insert(entry.at("name").get<std::string>());
  for (const auto& name : pins.at("names"))
    if (!name.is_string() || !present.count(name.get<std::string>()))
      return mismatch("fields.names", name.dump() + " not in the fields manifest");
  return co::Ok();
}

// ---- positions and locates ----
std::vector<std::string_view> split(std::string_view line) {
  std::vector<std::string_view> out;
  for (usize at = 0;;) {
    const usize comma = line.find(',', at);
    out.push_back(line.substr(at, comma == std::string_view::npos ? line.npos : comma - at));
    if (comma == std::string_view::npos) return out;
    at = comma + 1;
  }
}
// Cell `index` of a CSV line without splitting the rest (empty when absent).
std::string_view field(std::string_view line, usize index) {
  usize at = 0;
  for (usize k = 0; k < index; ++k) {
    const usize comma = line.find(',', at);
    if (comma == std::string_view::npos) return {};
    at = comma + 1;
  }
  const usize end = line.find(',', at);
  return line.substr(at, end == std::string_view::npos ? line.npos : end - at);
}
template<class T> bool parse_cell(std::string_view s, T& out) {
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size();
}
struct Csv {
  std::ifstream file;
  std::vector<std::string> columns;
  std::string line;
  [[nodiscard]] usize column(std::string_view name) const {
    const auto it = std::find(columns.begin(), columns.end(), name);
    return it == columns.end() ? columns.size() : static_cast<usize>(it - columns.begin());
  }
  bool next() {
    if (!std::getline(file, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
  }
};
co::Status open_csv(const std::string& path, Csv& csv, const char* what) {
  csv.file.open(path, std::ios::binary);
  if (!csv.file || !csv.next())
    return co::Err(co::ErrorCode::InvalidArgument, std::string("decide: ") + what + " unreadable");
  for (const auto cell : split(csv.line)) csv.columns.emplace_back(cell);
  return co::Ok();
}
co::Result<usize> name_index(std::span<const u64> ids, std::string_view cell, const char* what) {
  u64 id = 0;
  const auto it = parse_cell(cell, id) ? std::lower_bound(ids.begin(), ids.end(), id) : ids.end();
  if (it == ids.end() || *it != id)
    return co::Err(co::ErrorCode::InvalidArgument,
                   std::string("decide: ") + what + " instrument outside the role: " +
                       std::string(cell));
  return co::Ok(static_cast<usize>(it - ids.begin()));
}
struct Positions {
  std::vector<f64> held;     // per name; +0 when absent
  std::vector<f64> expected; // target_weight per name (+0 when absent); has_expected only
  std::vector<f64> shares;   // broker shares per name (+0 when absent); has_shares only
  // v8 R-4 hold-band-v1 state per name (NaN when absent: unset); has_hold_state only.
  std::vector<f64> rank_set, desired_prev;
  std::vector<u8> seen;
  bool has_nav{}, has_expected{}, has_shares{}, has_hold_state{};
  f64 nav{};
  usize rows{};
  const char* layout{"csv"};
};
// Column positions of a positions CSV (the width when absent).
struct PositionColumns {
  usize id{}, held{}, nav{}, target{}, shares{}, rank_set{}, desired_prev{};
};
co::Status read_position_row(const Csv& csv, std::span<const u64> ids,
                             const PositionColumns& at, Positions& out) {
  const auto cells = split(csv.line);
  const usize width = csv.columns.size();
  const auto bad = co::Err(co::ErrorCode::InvalidArgument,
                           "decide: positions row malformed: " + csv.line.substr(0, 120));
  if (cells.size() != width) return bad;
  ATX_TRY(const usize i, name_index(ids, cells[at.id], "positions"));
  f64 held = 0;
  if (!parse_cell(cells[at.held], held) || !std::isfinite(held)) return bad;
  if (out.seen[i]) return co::Err(co::ErrorCode::InvalidArgument, "decide: duplicate position");
  out.seen[i] = 1; out.held[i] = held; ++out.rows;
  if (at.nav < width) {
    f64 nav = 0;
    if (!parse_cell(cells[at.nav], nav) || !std::isfinite(nav) ||
        (out.has_nav && std::bit_cast<u64>(nav) != std::bit_cast<u64>(out.nav)))
      return co::Err(co::ErrorCode::InvalidArgument, "decide: positions nav_post differs by row");
    out.has_nav = true; out.nav = nav;
  }
  if (at.target < width) {
    f64 target = 0;
    if (!parse_cell(cells[at.target], target)) return bad;
    out.expected[i] = target;
    if (!std::isfinite(target)) out.has_expected = false; // not a decision session
  }
  if (at.shares < width) {
    f64 shares = 0;
    if (!parse_cell(cells[at.shares], shares) || !std::isfinite(shares)) return bad;
    out.shares[i] = shares;
  }
  if (at.rank_set < width) { // both columns (read_positions); "nan" = unset
    if (!parse_cell(cells[at.rank_set], out.rank_set[i]) ||
        !parse_cell(cells[at.desired_prev], out.desired_prev[i]))
      return bad;
  }
  return co::Ok();
}
Positions empty_positions(usize names) {
  Positions out;
  out.held.assign(names, 0.0); out.seen.assign(names, u8{0});
  out.expected.assign(names, 0.0); out.shares.assign(names, 0.0);
  out.rank_set.assign(names, nan); out.desired_prev.assign(names, nan);
  return out;
}
// The f64 holdings layout (v7 W4): the as-of session's rows, every file SHA verified. A
// hold-band book's export (v8 E-16) also carries the band's state DECIDE read at the as-of
// (every name with a set rank has a row; a name without a row is unset).
co::Result<Positions> read_f64_positions(const std::string& path, std::span<const u64> ids,
                                         i64 asof) {
  ATX_TRY(const auto read, holdings::read_session(path, asof));
  Positions out = empty_positions(ids.size());
  out.layout = "f64";
  out.has_nav = true; out.nav = read.session.nav_post;
  out.has_expected = true;
  out.has_hold_state = read.hold_state;
  for (const auto& h : read.names) {
    ATX_TRY(const usize i, name_index(ids, std::to_string(h.instrument_id), "positions"));
    if (out.seen[i]) return co::Err(co::ErrorCode::InvalidArgument, "decide: duplicate position");
    if (!std::isfinite(h.held_dollars))
      return co::Err(co::ErrorCode::InvalidArgument, "decide: positions held_dollars not finite");
    out.seen[i] = 1; out.held[i] = h.held_dollars; ++out.rows;
    out.expected[i] = h.target_weight;
    if (!std::isfinite(h.target_weight)) out.has_expected = false; // execution-only session
    if (read.hold_state) { out.rank_set[i] = h.rank_set; out.desired_prev[i] = h.desired_prev; }
  }
  if (!out.rows) out.has_expected = false;
  return co::Ok(std::move(out));
}
co::Result<Positions> read_positions(const std::string& path, std::span<const u64> ids, i64 asof) {
  if (holdings::is_f64_layout(path)) return read_f64_positions(path, ids, asof);
  Csv csv;
  ATX_TRY_VOID(open_csv(path, csv, "positions"));
  const PositionColumns at{csv.column("instrument_id"), csv.column("held_dollars"),
                           csv.column("nav_post"), csv.column("target_weight"),
                           csv.column("shares"), csv.column("rank_set"),
                           csv.column("desired_prev")};
  const usize session = csv.column("session_ns"), width = csv.columns.size();
  if (at.id == width || at.held == width)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: positions need instrument_id and held_dollars columns");
  if ((at.rank_set < width) != (at.desired_prev < width))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: positions rank_set and desired_prev columns go together");
  Positions out = empty_positions(ids.size());
  out.has_expected = at.target < width;
  out.has_shares = at.shares < width;
  out.has_hold_state = at.rank_set < width;
  while (csv.next()) {
    if (csv.line.empty()) continue;
    if (session < width) {
      i64 key = 0;
      if (!parse_cell(field(csv.line, session), key))
        return co::Err(co::ErrorCode::InvalidArgument, "decide: positions session_ns malformed");
      if (key != asof) continue;
    }
    ATX_TRY_VOID(read_position_row(csv, ids, at, out));
  }
  if (!out.rows) out.has_expected = false;
  return co::Ok(std::move(out));
}
struct Locates {
  std::vector<u8> no_locate; // one per name: 1 = no locate (not listed, or listed 0)
  usize listed{}, without{};
};
co::Result<Locates> read_locates(const std::string& path, std::span<const u64> ids) {
  Csv csv;
  ATX_TRY_VOID(open_csv(path, csv, "locates"));
  const usize id = csv.column("instrument_id"), flag = csv.column("locate");
  const usize width = csv.columns.size();
  if (id == width || flag == width)
    return co::Err(co::ErrorCode::InvalidArgument, "decide: locates need instrument_id,locate");
  Locates out;
  out.no_locate.assign(ids.size(), u8{1});
  std::vector<u8> seen(ids.size());
  while (csv.next()) {
    if (csv.line.empty()) continue;
    const auto cells = split(csv.line);
    unsigned locate = 2;
    if (cells.size() != width || !parse_cell(cells[flag], locate) || locate > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "decide: locates row malformed");
    ATX_TRY(const usize i, name_index(ids, cells[id], "locates"));
    if (seen[i]) return co::Err(co::ErrorCode::InvalidArgument, "decide: duplicate locate");
    seen[i] = 1; ++out.listed;
    out.no_locate[i] = locate ? u8{0} : u8{1};
  }
  for (const u8 v : out.no_locate) out.without += v;
  return co::Ok(std::move(out));
}
co::Result<usize> asof_row(const TargetReplayInput& x, i64 asof) {
  const auto it = std::lower_bound(x.session_keys.begin(), x.session_keys.end(), asof);
  if (it == x.session_keys.end() || *it != asof)
    return co::Err(co::ErrorCode::InvalidArgument, "decide: the as-of is not a role session");
  const auto d = static_cast<usize>(it - x.session_keys.begin());
  if (d < x.decision_begin || d >= x.decision_end)
    return co::Err(co::ErrorCode::InvalidArgument, "decide: the as-of is outside the score window");
  return co::Ok(d);
}
std::string date_text(i64 session_ns) {
  const std::chrono::year_month_day ymd{
      std::chrono::sys_days{std::chrono::days{session_ns / day_ns}}};
  const auto two = [](unsigned v) {
    return std::string{static_cast<char>('0' + v / 10), static_cast<char>('0' + v % 10)};
  };
  std::string out = std::to_string(static_cast<int>(ymd.year()));
  while (out.size() < 4) out.insert(out.begin(), '0');
  return out + '-' + two(static_cast<unsigned>(ymd.month())) + '-' +
         two(static_cast<unsigned>(ymd.day()));
}
// B9 freshness (v7 W4): the as-of must be the role's last session; --allow-stale decides
// anyway and the health check records the lag.
struct Freshness {
  i64 last_session{};
  usize sessions_behind{};
};
co::Result<Freshness> check_freshness(const TargetReplayInput& x, usize d, bool allow_stale) {
  const Freshness out{x.session_keys[x.dates - 1], x.dates - 1 - d};
  if (out.sessions_behind && !allow_stale)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: stale data: the as-of " + date_text(x.session_keys[d]) +
                       " is not the role's last session " + date_text(out.last_session) + " (" +
                       std::to_string(out.sessions_behind) +
                       " sessions later); --allow-stale decides anyway with a health warning");
  return co::Ok(out);
}
// Review R1 m-14: the cadence phase is the role's decision_begin session. With cadence > 1
// the deploy manifest must pin it (nav.cadence_anchor, YYYY-MM-DD); when present it is
// always checked, so a role with another start cannot shift the rebalance days.
co::Status check_cadence_anchor(const Deploy& deploy, const TargetReplayInput& x) {
  const auto& nav = deploy.m.at("nav");
  const auto it = nav.find("cadence_anchor");
  if (it == nav.end() || it->is_null()) {
    if (deploy.run.target.cadence == 1) return co::Ok();
    return co::Err(co::ErrorCode::InvalidArgument,
                   "deploy manifest: nav.cadence_anchor (YYYY-MM-DD) is required with cadence > 1");
  }
  const auto anchor = it->is_string() ? session_of(it->get<std::string>())
                                      : co::Result<i64>(co::Err(co::ErrorCode::InvalidArgument));
  if (!anchor || *anchor != x.session_keys[x.decision_begin])
    return mismatch("nav.cadence_anchor", "the role's decision_begin session is " +
                                              date_text(x.session_keys[x.decision_begin]));
  return co::Ok();
}

// ---- diagnostics and health ----
f64 pearson(std::span<const f64> a, std::span<const f64> b) {
  const usize n = a.size();
  if (n < 3) return nan;
  f64 ma = 0, mb = 0;
  for (usize k = 0; k < n; ++k) { ma += a[k]; mb += b[k]; }
  ma /= static_cast<f64>(n); mb /= static_cast<f64>(n);
  f64 sab = 0, saa = 0, sbb = 0;
  for (usize k = 0; k < n; ++k) {
    const f64 u = a[k] - ma, v = b[k] - mb;
    sab += u * v; saa += u * u; sbb += v * v;
  }
  return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : nan;
}
// Review R1 M-3: two transfer-coefficient definitions, both reported under explicit keys.
// transfer_coefficient: the NAV replay's own (cost_v2::transfer_coefficient, the column of
// v7_transfer_coefficient.csv): corr(desired_i / sigma_i, the rule plan before the locate
// block), desired_i sigma_i being the Grinold-Kahn alpha; desired_over_sigma_target: the
// same alpha on the final target (after the block; the B9 band's basis); the L3
// definition signal_over_variance_{target,aim}: corr(signal_i / sigma_i^2, w) (the
// L3-F2 values), 1/sigma off from Grinold-Kahn.
struct Transfer {
  f64 replay{nan}, desired_target{nan}, signal_target{nan}, signal_aim{nan};
  usize signal_names{};
};
Transfer transfer_coefficient(const TargetReplayInput& x, usize d,
                              const detail::NavDecision& dec, f64 leverage) {
  const usize n = x.instruments;
  const auto member = x.member.subspan(d * n, n);
  Transfer out;
  out.replay = cost_v2::transfer_coefficient(member, dec.desired, dec.sigma, dec.rule);
  out.desired_target = cost_v2::transfer_coefficient(member, dec.desired, dec.sigma, dec.target);
  std::vector<f64> score, weight, aim;
  for (usize i = 0; i < n; ++i) {
    const f64 alpha = x.signal[d * n + i], sigma = dec.sigma[i];
    if (!member[i] || !std::isfinite(alpha) || !std::isfinite(sigma) || !(sigma > 0)) continue;
    score.push_back(alpha / (sigma * sigma));
    weight.push_back(dec.target[i]); aim.push_back(leverage * dec.desired[i]);
  }
  out.signal_target = pearson(score, weight);
  out.signal_aim = dec.rebalance ? pearson(score, aim) : nan;
  out.signal_names = score.size();
  return out;
}
const char* health_label(DecideHealth h) {
  switch (h) {
  case DecideHealth::Ok: return "ok";
  case DecideHealth::Warn: return "warn";
  case DecideHealth::Error: return "error";
  }
  return "error";
}
struct Health {
  Json checks = Json::array();
  DecideHealth worst{DecideHealth::Ok};
  void add(Json check, DecideHealth status) {
    check["status"] = health_label(status);
    checks.push_back(std::move(check));
    if (static_cast<u8>(status) > static_cast<u8>(worst)) worst = status;
  }
};
Json finite_or_null(f64 v) { return std::isfinite(v) ? Json(v) : Json(nullptr); }
// The facts every health check and output reads.
struct Facts {
  const TargetReplayInput& x;
  usize d{};
  const detail::NavDecision& dec;
  const Positions& positions;
  const Locates* locates; // null without --locates
  f64 nav{};
};
usize short_targets_without_locate(const Facts& f) {
  usize count = 0;
  for (usize i = 0; i < f.dec.target.size(); ++i)
    if (f.dec.target[i] < 0 && (!f.locates || f.locates->no_locate[i])) ++count;
  return count;
}
usize held_absent(const Facts& f) {
  const usize n = f.x.instruments;
  usize count = 0;
  for (usize i = 0; i < n; ++i)
    if (f.positions.held[i] != 0 && !f.x.present[f.d * n + i]) ++count;
  return count;
}
// The book the orders build (after the locate block): review R1 m-8 bands these, not the
// rule plan. Summed in index order as the replay sums its plan.
struct TargetStats {
  f64 gross{}, net{}, turnover{};
};
TargetStats target_stats(const detail::NavDecision& dec) {
  TargetStats s;
  for (usize i = 0; i < dec.target.size(); ++i) {
    s.turnover += std::abs(dec.target[i] - dec.current[i]);
    s.gross += std::abs(dec.target[i]); s.net += dec.target[i];
  }
  return s;
}
DecideHealth warn_if(bool out) { return out ? DecideHealth::Warn : DecideHealth::Ok; }
// B9: leverage and turnover of the final target against the manifest's TRAIN bands (warn),
// a neutralization skip (ERROR: the replay would silently keep the book), names without a
// locate, stale holdings, and the source pin against the build provenance (warn).
Health health_checks(const Facts& f, const Bands& bands, const std::string& manifest_git,
                     const std::string& build_git) {
  Health h;
  const auto book = target_stats(f.dec);
  const std::string basis = "the final target (after the locate block)";
  h.add({{"check", "gross_leverage"}, {"value", book.gross}, {"basis", basis},
         {"band", Json::array({bands.gross_lo, bands.gross_hi})}},
        warn_if(!(book.gross >= bands.gross_lo && book.gross <= bands.gross_hi)));
  h.add({{"check", "abs_net_leverage"}, {"value", std::abs(book.net)}, {"basis", basis},
         {"max", bands.abs_net_max}}, warn_if(!(std::abs(book.net) <= bands.abs_net_max)));
  h.add({{"check", "planned_turnover"}, {"value", book.turnover}, {"basis", basis},
         {"max", bands.turnover_max}}, warn_if(!(book.turnover <= bands.turnover_max)));
  const auto outcome = f.dec.construction.neutralize;
  const bool skipped = outcome != NeutralizeOutcome::NotAttempted &&
                       outcome != NeutralizeOutcome::Applied;
  h.add({{"check", "neutralization"}, {"value", detail::neutralize_outcome_label(outcome)},
         {"cadence_decision", f.dec.cadence},
         {"meaning", "a skipped neutralization skips the rebalance: only forced exits trade"}},
        skipped ? DecideHealth::Error : DecideHealth::Ok);
  const usize without = short_targets_without_locate(f);
  h.add({{"check", "names_without_locate"}, {"value", without},
         {"max", bands.no_locate_max}, {"locates", f.locates ? "supplied" : "not-supplied"}},
        warn_if(!f.locates || without > bands.no_locate_max));
  const usize stale = held_absent(f);
  h.add({{"check", "held_absent_names"}, {"value", stale},
         {"meaning", "held names absent at the as-of: priced stale, orders blocked"}},
        warn_if(stale > 0));
  h.add({{"check", "source_git_sha"}, {"manifest", manifest_git}, {"build", build_git},
         {"meaning", "configure-time provenance (review C4); the executable SHA binds the code"}},
        warn_if(build_git != manifest_git));
  return h;
}

// ---- B9 transfer-coefficient band (v7 W4) ----
struct PriorTc {
  i64 asof{};
  f64 tc{nan}; // desired_over_sigma_target; NaN when null or absent (an older decision)
};
struct TcBand {
  bool supplied{};
  usize read{}, used{}, consecutive_below{};
};
constexpr usize max_prior_entries = 100'000;
// A prior decision.json of the same book strictly before the as-of, or nothing.
co::Result<std::optional<PriorTc>> prior_decision(const std::filesystem::path& path,
                                                  const std::string& book, i64 asof) {
  ATX_TRY(const auto file, read_text(path.string(), max_fields_manifest_bytes, "prior decision"));
  Json j;
  try {
    j = Json::parse(file.text);
  } catch (const Json::exception&) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: prior decision unreadable: " + path.string());
  }
  if (!j.is_object() || j.value("schema", std::string{}) != book_decision_schema ||
      j.value("status", std::string{}) != "complete" || j.value("book", std::string{}) != book)
    return co::Ok(std::optional<PriorTc>{});
  const auto at = j.find("asof_session_ns");
  if (at == j.end() || !at->is_number_integer() || at->get<i64>() >= asof)
    return co::Ok(std::optional<PriorTc>{});
  PriorTc out{at->get<i64>(), nan};
  const auto tc = j.find("transfer_coefficient");
  if (tc != j.end() && tc->is_object()) {
    const auto v = tc->find("desired_over_sigma_target");
    if (v != tc->end() && v->is_number()) out.tc = v->get<f64>();
  }
  return co::Ok(std::optional<PriorTc>{out});
}
// Every DIR/<entry>/decision.json and DIR/<file>.json of this book before the as-of,
// newest first; two records of one session must agree.
co::Result<std::vector<PriorTc>> prior_decisions(const std::string& dir, const std::string& book,
                                                 i64 asof, usize& read) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec))
    return co::Err(co::ErrorCode::InvalidArgument, "decide: --prior-decisions must be a directory");
  std::vector<PriorTc> out;
  usize entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (++entries > max_prior_entries)
      return co::Err(co::ErrorCode::OutOfRange, "decide: --prior-decisions has too many entries");
    auto path = entry.path();
    if (entry.is_directory()) path /= "decision.json";
    else if (path.extension() != ".json") continue;
    if (!std::filesystem::is_regular_file(path, ec)) continue;
    ++read;
    ATX_TRY(const auto prior, prior_decision(path, book, asof));
    if (prior) out.push_back(*prior);
  }
  std::sort(out.begin(), out.end(), [](const PriorTc& a, const PriorTc& b) {
    return a.asof > b.asof;
  });
  for (usize k = 1; k < out.size(); ++k)
    if (out[k].asof == out[k - 1].asof &&
        std::bit_cast<u64>(out[k].tc) != std::bit_cast<u64>(out[k - 1].tc))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "decide: prior decisions disagree on the transfer coefficient at " +
                         date_text(out[k].asof));
  out.erase(std::unique(out.begin(), out.end(),
                        [](const PriorTc& a, const PriorTc& b) { return a.asof == b.asof; }),
            out.end());
  return co::Ok(std::move(out));
}
// The as-of's run of low TCs over the priors (tc_band_run); a null TC ends the run.
co::Result<TcBand> tc_band(const DecideConfig& cfg, const std::string& book, i64 asof, f64 tc) {
  TcBand band;
  band.supplied = !cfg.prior_decisions_directory.empty();
  std::vector<PriorTc> priors;
  if (band.supplied) {
    ATX_TRY(priors, prior_decisions(cfg.prior_decisions_directory, book, asof, band.read));
  }
  band.used = priors.size();
  std::vector<f64> values;
  values.reserve(priors.size());
  for (const auto& p : priors) values.push_back(p.tc);
  band.consecutive_below = tc_band_run(tc, values);
  return co::Ok(band);
}
void add_w4_checks(Health& h, const TcBand& band, f64 tc, const Freshness& fresh,
                   const DecideConfig& cfg) {
  h.add({{"check", "transfer_coefficient_band"}, {"value", finite_or_null(tc)},
         {"basis", "transfer_coefficient.desired_over_sigma_target"}, {"min", tc_band_min},
         {"consecutive_required", tc_band_consecutive},
         {"consecutive_below", band.consecutive_below},
         {"prior_decisions", band.supplied ? "supplied" : "not-supplied"},
         {"prior_files_read", band.read}, {"prior_decisions_used", band.used},
         {"meaning", "TC below the band for consecutive decisions: constraints (locates, ADV cap, "
                     "neutrality, partial aim) are eating the alpha (review L3-F2, P2 R2.5)"}},
        warn_if(!band.supplied || band.consecutive_below >= tc_band_consecutive));
  h.add({{"check", "data_freshness"}, {"asof", cfg.asof},
         {"role_last_session", date_text(fresh.last_session)},
         {"sessions_behind", fresh.sessions_behind}, {"allow_stale", cfg.allow_stale}},
        warn_if(fresh.sessions_behind > 0));
}
const char* reason(const Facts& f, usize i) {
  const auto& dec = f.dec;
  const bool member = f.x.member[f.d * f.x.instruments + i] != 0;
  if (dec.target[i] != dec.rule[i]) return "locate-block";
  if (!member)
    return dec.target[i] != 0 ? "decay" : dec.current[i] != 0 ? "exit" : "flat";
  return dec.target[i] == dec.current[i] ? "hold" : "move";
}
bool nonzero_bits(f64 v) { return std::bit_cast<u64>(v) != 0; }
void write_value(std::ostream& out, f64 v) {
  if (std::isnan(v)) out << "nan"; else out << v;
}
// hold_columns (v8 R-4, hold band declared): two more columns, rank_set and desired_prev, the
// hold-band state after this decision, and a row for every name holding a set rank (a name
// missing for a day keeps its state in the file). targets.csv is then a positions file whose
// state columns carry the book into the next decide.
co::Status write_targets(const std::filesystem::path& path, const Facts& f, f64 leverage,
                         bool hold_columns) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "decide: targets output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "instrument_id,member,tier,tier_missing,no_short,no_locate,held_dollars,"
          "current_weight,desired_weight,aim_weight,rule_weight,target_weight,trade_weight,"
          "order_dollars,reason";
  if (hold_columns) file << ",rank_set,desired_prev";
  file << '\n';
  const auto& dec = f.dec; const usize n = f.x.instruments;
  for (usize i = 0; i < n; ++i) {
    const bool member = f.x.member[f.d * n + i] != 0;
    const bool state = hold_columns && std::isfinite(dec.hold.rank_set[i]);
    if (!member && !state && !nonzero_bits(f.positions.held[i]) &&
        !nonzero_bits(dec.target[i]))
      continue;
    const bool tiered = !dec.tier.empty();
    file << f.x.instrument_ids[i] << ',' << member << ','
         << detail::borrow_tier_label(tiered ? dec.tier[i] : u8{0}) << ','
         << (tiered ? unsigned{dec.tier_missing[i]} : 0U) << ','
         << (dec.no_short.empty() ? 0U : unsigned{dec.no_short[i]}) << ','
         << (f.locates ? unsigned{f.locates->no_locate[i]} : 0U) << ',' << f.positions.held[i]
         << ',' << dec.current[i] << ',';
    write_value(file, dec.rebalance ? dec.desired[i] : nan); file << ',';
    write_value(file, dec.rebalance ? leverage * dec.desired[i] : nan);
    file << ',' << dec.rule[i] << ',' << dec.target[i] << ',' << dec.target[i] - dec.current[i]
         << ',' << dec.target[i] * f.nav - f.positions.held[i] << ',' << reason(f, i);
    if (hold_columns) {
      file << ',';
      write_value(file, dec.hold.rank_set[i]); file << ',';
      write_value(file, dec.hold.desired_prev[i]);
    }
    file << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "decide: targets close"));
}
struct OrderTotals {
  usize orders{};
  f64 buy_dollars{}, sell_dollars{};
};
co::Result<OrderTotals> write_orders(const std::filesystem::path& path, const Facts& f) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "decide: orders output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "instrument_id,side,weight_delta,order_dollars,reason\n";
  OrderTotals totals;
  const auto& dec = f.dec;
  for (usize i = 0; i < f.x.instruments; ++i) {
    if (dec.target[i] == dec.current[i]) continue;
    const f64 delta = dec.target[i] - dec.current[i];
    const f64 dollars = dec.target[i] * f.nav - f.positions.held[i];
    ++totals.orders;
    (delta > 0 ? totals.buy_dollars : totals.sell_dollars) += std::abs(dollars);
    file << f.x.instrument_ids[i] << ',' << (delta > 0 ? "buy" : "sell") << ',' << delta << ','
         << dollars << ',' << reason(f, i) << '\n';
  }
  file.close();
  if (!file) return co::Err(co::ErrorCode::IoError, "decide: orders close");
  return co::Ok(totals);
}
struct Parity {
  bool checked{};
  usize names{}, mismatches{};
  u64 first_mismatch{};
};
Parity replay_parity(const Facts& f) {
  Parity p;
  p.checked = true; p.names = f.dec.target.size();
  for (usize i = 0; i < p.names; ++i) {
    if (std::bit_cast<u64>(f.dec.target[i]) == std::bit_cast<u64>(f.positions.expected[i]))
      continue;
    if (!p.mismatches) p.first_mismatch = f.x.instrument_ids[i];
    ++p.mismatches;
  }
  return p;
}
Json decision_json(const Facts& f, const detail::NavDecision& dec) {
  const auto& c = dec.construction;
  return Json{{"cadence_decision", dec.cadence}, {"rebalance", dec.rebalance},
      {"neutralize", detail::neutralize_outcome_label(c.neutralize)},
      {"neutralize_used", c.neutralize_used}, {"neutralize_excluded", c.neutralize_excluded},
      {"neutralize_excluded_share", finite_or_null(c.neutralize_excluded_share)},
      {"neutralize_amplification", finite_or_null(c.neutralize_amplification)},
      {"locate_zeroed", c.locate_zeroed}, {"banded_names", c.banded_names},
      {"members", dec.members}, {"planned_turnover", dec.plan.turnover},
      {"planned_forced", dec.plan.forced_turnover},
      {"planned_discretionary", dec.plan.discretionary_turnover},
      {"planned_gross", dec.plan.gross}, {"planned_net", dec.plan.net},
      {"planned_held_names", dec.plan.held_names},
      {"blocked_short_names", dec.blocked_short_names},
      {"blocked_short_dollars", dec.blocked_short_dollars}, {"nav_post", f.nav},
      {"member_tiers", {{"gc", dec.member_tiers[0]}, {"warm", dec.member_tiers[1]},
                        {"special", dec.member_tiers[2]},
                        {"missing_predictors", dec.member_missing_predictors}}}};
}
// ---- B7 share orders (v7 W4) ----
constexpr const char* price_definition =
    "close: the as-of session's raw (unadjusted) close of the pinned role, the price the "
    "replay's MARK uses for raw dollars; a name absent at the as-of has no valid close and "
    "gets no order (refused_no_close, B7); held names convert dollars to shares at their "
    "last present raw close (mark)";
constexpr const char* order_tag =
    "moc: market-on-close at the next session, the replay's EXECUTE (fills at the close of "
    "d+1, capped at the scenario's participation of the ADV that session reads)";
// Reference prices, marks and the filling session's ADV, then the rule of strategy_orders.
co::Result<orders::Book> share_orders(const DecideConfig& cfg, const NavReplayInput& in,
                                      const NavReplayConfig& base, usize d,
                                      const detail::NavDecision& dec, const Positions& p,
                                      f64 nav, f64 nav_dollars) {
  const auto& x = in.target;
  const usize n = x.instruments;
  std::vector<f64> price(n, nan), mark(n, nan);
  for (usize i = 0; i < n; ++i) {
    if (x.present[d * n + i]) price[i] = x.raw_close[d * n + i];
    if (p.held[i] == 0) continue;
    for (usize t = d + 1; t-- > 0;) {
      if (!x.present[t * n + i]) continue;
      mark[i] = x.raw_close[t * n + i];
      break;
    }
  }
  ATX_TRY(const auto adv, detail::execution_adv(in, base, d + 1));
  const orders::Inputs inputs{dec.target, dec.current, p.held,
      p.has_shares ? std::span<const f64>(p.shares) : std::span<const f64>{}, price, mark, adv,
      nav, nav_dollars, cfg.min_notional, base.scenario.max_participation, cfg.lot_size};
  return orders::build(inputs);
}
co::Status write_share_orders(const std::filesystem::path& path, const Facts& f,
                              const orders::Book& book) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "decide: share orders output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "instrument_id,side,shares,notional,reference_price,adv_dollars,participation,"
          "residual_shares,residual_notional,target_weight,current_weight,exit,short_sale,"
          "below_min_notional,reason,tag\n";
  for (const auto& o : book.orders) {
    file << f.x.instrument_ids[o.index] << ',' << (o.shares > 0 ? "buy" : "sell") << ','
         << o.shares << ',' << o.notional << ',' << o.price << ',';
    write_value(file, o.adv); file << ',';
    write_value(file, o.participation);
    file << ',' << o.residual_shares << ',' << o.residual_shares * o.price << ','
         << f.dec.target[o.index] << ',' << f.dec.current[o.index] << ',' << o.exit << ','
         << o.short_sale << ',' << o.below_min_kept << ',' << reason(f, o.index) << ",moc\n";
  }
  file.close();
  return file ? co::Ok()
              : co::Status(co::Err(co::ErrorCode::IoError, "decide: share orders close"));
}
co::Status write_expected(const std::filesystem::path& path, const Facts& f,
                          const orders::Book& book) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "decide: expected holdings output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "instrument_id,shares,current_shares,order_shares,reference_price,notional\n";
  for (const auto& e : book.expected) {
    file << f.x.instrument_ids[e.index] << ',';
    write_value(file, e.shares); file << ',';
    write_value(file, e.current_shares); file << ',' << e.order_shares << ',';
    write_value(file, e.price); file << ',';
    write_value(file, e.shares * e.price); file << '\n';
  }
  file.close();
  return file ? co::Ok()
              : co::Status(co::Err(co::ErrorCode::IoError, "decide: expected holdings close"));
}
Json share_orders_json(const orders::Book& book, const DecideConfig& cfg, f64 nav,
                       f64 nav_dollars, f64 cap, bool shares_column) {
  const auto& s = book.summary;
  return Json{{"nav_dollars", nav_dollars},
      {"nav_dollars_source", cfg.nav_dollars ? "--nav-dollars" : "the book NAV"},
      {"scale", nav_dollars / nav}, {"lot_size", cfg.lot_size},
      {"min_notional", cfg.min_notional}, {"price_source", cfg.price_source},
      {"price_definition", price_definition}, {"rounding_rule", orders::rounding_rule},
      {"tag", order_tag},
      {"current_shares_source", shares_column ? "positions shares column"
                                              : "held_dollars / mark (fractional)"},
      {"orders", s.orders}, {"buys", s.buys}, {"sells", s.sells},
      {"buy_notional", s.buy_notional}, {"sell_notional", s.sell_notional},
      {"short_sales", s.short_sales}, {"exits", s.exits},
      {"exits_below_min_notional_kept", s.exits_below_min_kept},
      {"dropped_min_notional", {{"count", s.dropped_min_notional},
                                {"notional", s.dropped_notional}}},
      {"rounded_to_zero", {{"count", s.rounded_to_zero},
                           {"raw_notional", s.rounded_to_zero_notional}}},
      {"refused_no_close", s.refused_no_close},
      {"residual_notional", {{"net", s.residual_notional_net}, {"abs", s.residual_notional_abs},
                             {"max_abs", s.residual_notional_max_abs}}},
      {"participation", {{"max", s.participation_max}, {"scenario_cap", cap},
                         {"above_cap", s.above_participation_cap}, {"no_adv", s.no_adv}}},
      {"expected_holdings_rows", book.expected.size()}};
}

struct Published {
  std::string targets_sha, orders_sha, shares_sha, expected_sha;
  OrderTotals totals;
};
co::Result<Published> write_tables(const std::filesystem::path& dir, const Facts& f,
                                   f64 leverage, const orders::Book& book, bool hold_columns) {
  if (!std::filesystem::create_directory(dir))
    return co::Err(co::ErrorCode::AlreadyExists, "decide: output must not exist");
  ATX_TRY_VOID(write_targets(dir / "targets.csv", f, leverage, hold_columns));
  Published out;
  ATX_TRY(out.totals, write_orders(dir / "orders.csv", f));
  ATX_TRY_VOID(write_share_orders(dir / "orders_shares.csv", f, book));
  ATX_TRY_VOID(write_expected(dir / "expected_holdings.csv", f, book));
  ATX_TRY(out.targets_sha, co::sha256_file((dir / "targets.csv").string()));
  ATX_TRY(out.orders_sha, co::sha256_file((dir / "orders.csv").string()));
  ATX_TRY(out.shares_sha, co::sha256_file((dir / "orders_shares.csv").string()));
  ATX_TRY(out.expected_sha, co::sha256_file((dir / "expected_holdings.csv").string()));
  return co::Ok(std::move(out));
}
co::Status write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "decide: JSON output");
  file << j.dump(2) << '\n';
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "decide: JSON close"));
}
// Positions and locates of the as-of against the loaded role, and the book NAV.
struct Inputs {
  Positions positions;
  Locates locates;
  bool locates_supplied{};
  f64 nav{};
  const char* nav_source{};
  // Review R1 M-2: the bytes behind the decision (an f64 directory is pinned by its
  // manifest.json, which binds the index, which binds holdings.f64).
  std::string positions_sha, positions_sha_of, locates_sha;
};
Json pins_json(const Deploy& deploy, const std::string& recipe_sha, const DecideConfig& cfg,
               const Inputs& in) {
  const auto& m = deploy.m;
  return Json{{"deploy_manifest_sha256", deploy.sha256},
      {"combined_sha256", deploy.run.combined_sha256}, {"role_sha256", deploy.run.role_sha256},
      {"fields_sha256", deploy.fields.manifest_sha256},
      {"fields_code_sha256", m.at("fields").at("code_sha256")},
      {"library_sha256", m.at("library").at("sha256")},
      {"run_recipe_sha256", m.at("recipe").at("sha256")},
      {"orientations_sha256", m.at("orientations").at("sha256")},
      {"composition_weights_sha256", m.at("composition").at("weights_sha256")},
      {"data_source_sha256", m.at("data_source_sha256")},
      {"nav_recipe_sha256", recipe_sha}, {"executable_sha256", cfg.executable_sha256},
      {"positions", {{"path", cfg.positions_path}, {"layout", in.positions.layout},
                     {"sha256", in.positions_sha}, {"sha256_of", in.positions_sha_of}}},
      {"locates", in.locates_supplied
           ? Json{{"path", cfg.locates_path}, {"sha256", in.locates_sha}} : Json(nullptr)},
      {"nav", {{"source", in.nav_source}, {"value", in.nav},
               {"literal", cfg.nav_text.empty() ? Json(nullptr) : Json(cfg.nav_text)}}}};
}
co::Status pin_inputs(const DecideConfig& cfg, Inputs& in) {
  if (std::string_view{in.positions.layout} == "f64") {
    std::error_code ec;
    const bool dir = std::filesystem::is_directory(cfg.positions_path, ec);
    const auto file = dir ? std::filesystem::path(cfg.positions_path) / holdings::manifest_file
                          : std::filesystem::path(cfg.positions_path);
    ATX_TRY(in.positions_sha, co::sha256_file(file.string()));
    in.positions_sha_of = dir ? holdings::manifest_file : "file";
  } else {
    ATX_TRY(in.positions_sha, co::sha256_file(cfg.positions_path));
    in.positions_sha_of = "file";
  }
  if (in.locates_supplied) { ATX_TRY(in.locates_sha, co::sha256_file(cfg.locates_path)); }
  return co::Ok();
}
co::Result<Inputs> read_inputs(const DecideConfig& cfg, const TargetReplayInput& x, i64 asof) {
  Inputs in;
  ATX_TRY(in.positions, read_positions(cfg.positions_path, x.instrument_ids, asof));
  const auto& p = in.positions;
  // Review R1 M-1: a selection with no row is a data break (a wrong date, a truncated
  // file), never silently a flat book.
  if (!p.rows && !cfg.flat_book)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: the positions select no row at the as-of (a missing row is a data "
                   "break); --flat-book decides from a genuinely flat book");
  if (cfg.nav && p.has_nav && std::bit_cast<u64>(*cfg.nav) != std::bit_cast<u64>(p.nav))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: --nav differs from the positions' nav_post");
  if (!cfg.nav && !p.has_nav)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: book NAV unknown (--nav, or a nav_post column at the as-of)");
  in.nav = cfg.nav ? *cfg.nav : p.nav;
  in.nav_source = cfg.nav ? "--nav" : "positions nav_post";
  if (!std::isfinite(in.nav) || !(in.nav > 0))
    return co::Err(co::ErrorCode::InvalidArgument, "decide: book NAV must be finite and > 0");
  if (cfg.check_replay && !p.has_expected)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: --check-replay needs a finite target_weight column at the as-of "
                   "(a replay holdings.csv decision session)");
  if (!cfg.locates_path.empty()) {
    ATX_TRY(in.locates, read_locates(cfg.locates_path, x.instrument_ids));
    in.locates_supplied = true;
  }
  ATX_TRY_VOID(pin_inputs(cfg, in));
  return co::Ok(std::move(in));
}
// Every manifest, seal, executable and file pin, then the pinned load and the recomputed
// NAV recipe; nothing is written.
co::Result<detail::NavDeployLoad> verified_load(const Deploy& deploy, const DecideConfig& cfg,
                                                i64 asof) {
  ATX_TRY_VOID(check_seal(deploy, asof));
  ATX_TRY_VOID(check_executable(deploy, cfg.executable_sha256));
  ATX_TRY_VOID(check_combined(deploy));
  ATX_TRY_VOID(check_fields(deploy));
  ATX_TRY(auto loaded, detail::load_nav_deploy(deploy.run, deploy.base, deploy.limits,
                                               deploy.fields));
  const auto& pinned = deploy.m.at("nav").at("recipe_sha256").get_ref<const std::string&>();
  if (loaded.recipe_sha256 != pinned)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "deploy manifest: pin mismatch nav.recipe_sha256 (recomputed " +
                       loaded.recipe_sha256 + ")");
  return co::Ok(std::move(loaded));
}
// Arguments checked before anything is read.
co::Status validate_config(const DecideConfig& cfg) {
  if (cfg.deploy_path.empty() || cfg.positions_path.empty() || cfg.output_directory.empty() ||
      cfg.asof.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "decide: --deploy, --asof, --positions and --output are required");
  if (std::filesystem::exists(cfg.output_directory))
    return co::Err(co::ErrorCode::AlreadyExists, "decide: output must not exist");
  if (cfg.price_source != "close")
    return co::Err(co::ErrorCode::InvalidArgument, "decide: --price-source must be close");
  if (cfg.nav_dollars && (!std::isfinite(*cfg.nav_dollars) || !(*cfg.nav_dollars > 0)))
    return co::Err(co::ErrorCode::InvalidArgument, "decide: --nav-dollars must be finite > 0");
  if (!std::isfinite(cfg.min_notional) || cfg.min_notional < 0)
    return co::Err(co::ErrorCode::InvalidArgument, "decide: --min-notional must be finite >= 0");
  if (cfg.lot_size < 1 || cfg.lot_size > orders::max_lot_size)
    return co::Err(co::ErrorCode::InvalidArgument, "decide: --lot-size in [1, 1000000]");
  return co::Ok();
}
// Everything decision.json records, gathered once the tables are written.
struct Record {
  const DecideConfig& cfg;
  const Deploy& deploy;
  const std::string& recipe_sha;
  i64 asof{};
  usize d{};
  const Inputs& inputs;
  const Facts& facts;
  const Transfer& tc;
  const Health& health;
  const Parity& parity;
  const orders::Book& book;
  f64 nav_dollars{};
  const Published& published;
};
Json transfer_json(const Transfer& tc) {
  return Json{{"transfer_coefficient", finite_or_null(tc.replay)},
      {"desired_over_sigma_target", finite_or_null(tc.desired_target)},
      {"signal_over_variance_target", finite_or_null(tc.signal_target)},
      {"signal_over_variance_aim", finite_or_null(tc.signal_aim)},
      {"signal_names", tc.signal_names},
      {"definitions", {{"transfer_coefficient", tc_replay_definition},
                       {"desired_over_sigma_target", tc_target_definition},
                       {"signal_over_variance", tc_definition}}}};
}
// v8 R-4: the hold-band state entering and leaving the decision and what the band did there.
Json hold_band_json(const Record& r) {
  const auto& p = r.inputs.positions; const auto& dec = r.facts.dec;
  const auto& c = dec.construction;
  const auto count_set = [](const std::vector<f64>& ranks) {
    return static_cast<usize>(
        std::count_if(ranks.begin(), ranks.end(), [](f64 v) { return std::isfinite(v); }));
  };
  const char* state_in = !p.has_hold_state ? "none (no state columns): every name unset"
      : std::string_view{p.layout} == "f64" ? "holdings.f64 rank_set,desired_prev columns"
                                            : "positions rank_set,desired_prev columns";
  return Json{{"band", *r.deploy.base.target.hold_band}, {"state_in", state_in},
      {"names_set_in", count_set(p.rank_set)}, {"names_set_out", count_set(dec.hold.rank_set)},
      {"moved", c.hold_moved}, {"kept", c.hold_kept}, {"first_set", c.hold_first_set},
      {"state_out", "targets.csv rank_set,desired_prev: the state after this decision, the "
                    "next decide's positions columns"}};
}
// v8 R-5: the decision's ADV holding cap pass (desired-weight units; 0 without a rebalance).
Json adv_hold_json(const Record& r) {
  const auto& c = r.facts.dec.construction;
  return Json{{"q", r.deploy.base.target.adv_hold_q}, {"nav", r.deploy.base.initial_nav},
      {"clipped_names", c.adv_clipped}, {"clipped_mass", c.adv_clipped_mass},
      {"unplaced_mass", c.adv_unplaced_mass}, {"residual_names", c.adv_residual_names},
      {"residual_mass", c.adv_residual_mass}, {"residual_max", c.adv_residual_max},
      {"rule", "the NAV run's recipe adv_hold_rule (NAV = its initial_nav)"}};
}
Json decision_summary(const Record& r) {
  const auto& in = r.inputs; const auto& published = r.published;
  Json summary{{"schema", book_decision_schema}, {"status", "complete"},
      {"book", r.deploy.m.at("book")}, {"asof", r.cfg.asof}, {"asof_session_ns", r.asof},
      {"role_row", r.d}, {"rule", detail::construction_rule_id(r.deploy.base.target)},
      {"pins_verified", pins_json(r.deploy, r.recipe_sha, r.cfg, in)},
      {"positions", {{"rows", in.positions.rows}, {"nav_post", in.nav},
                     {"nav_source", in.nav_source}, {"layout", in.positions.layout},
                     {"shares_column", in.positions.has_shares},
                     {"flat_book", r.cfg.flat_book}}},
      {"locates", {{"supplied", in.locates_supplied}, {"names_listed", in.locates.listed},
                   {"names_without_locate", in.locates_supplied
                       ? Json(in.locates.without) : Json(nullptr)}}},
      {"decision", decision_json(r.facts, r.facts.dec)},
      {"orders", {{"count", published.totals.orders},
                  {"buy_dollars", published.totals.buy_dollars},
                  {"sell_dollars", published.totals.sell_dollars}}},
      {"orders_shares", share_orders_json(r.book, r.cfg, in.nav, r.nav_dollars,
                                          r.deploy.base.scenario.max_participation,
                                          in.positions.has_shares)},
      {"transfer_coefficient", transfer_json(r.tc)},
      {"health", {{"status", health_label(r.health.worst)}, {"checks", r.health.checks}}},
      {"files", {{"targets.csv", published.targets_sha}, {"orders.csv", published.orders_sha},
                 {"orders_shares.csv", published.shares_sha},
                 {"expected_holdings.csv", published.expected_sha}}},
      {"limitations", decide_limitations}};
  if (r.parity.checked)
    summary["replay_parity"] = Json{{"names", r.parity.names},
        {"mismatches", r.parity.mismatches},
        {"first_mismatch_instrument_id",
         r.parity.mismatches ? Json(r.parity.first_mismatch) : Json(nullptr)},
        {"basis", "bitwise target weight vs the positions file's target_weight at the as-of "
                  "(an absent name is +0)"}};
  if (hold_band_declared(r.deploy.base.target)) summary["hold_band"] = hold_band_json(r);
  if (adv_hold_on(r.deploy.base.target)) summary["adv_hold"] = adv_hold_json(r);
  return summary;
}
void report(std::ostream& progress, const Record& r) {
  const auto book = target_stats(r.facts.dec);
  progress << "decide " << r.cfg.asof << ": " << r.published.totals.orders << " orders ("
           << r.book.summary.orders << " share orders), gross " << std::setprecision(6)
           << book.gross << ", turnover " << book.turnover << ", TC " << r.tc.desired_target
           << ", health " << health_label(r.health.worst);
  if (r.parity.checked) progress << ", replay parity mismatches " << r.parity.mismatches;
  progress << '\n';
}
// nav_decide at the as-of row with the book's hold-band state (v8 R-4): the positions' state
// columns (NaN = unset; every name unset without them), read only with the hold band on.
co::Result<detail::NavDecision> decide_row(const NavReplayInput& view, const Deploy& deploy,
                                           usize d, const Inputs& inputs) {
  const auto& p = inputs.positions;
  const auto no_locate = inputs.locates_supplied ? std::span<const u8>(inputs.locates.no_locate)
                                                 : std::span<const u8>{};
  const atx::engine::book::HoldBandState hold{p.rank_set, p.desired_prev};
  const bool holding = hold_band_on(deploy.base.target);
  ATX_TRY(auto dec, detail::nav_decide(view, deploy.base, d, p.held, inputs.nav, no_locate,
                                       holding ? &hold : nullptr));
  if (hold_band_declared(deploy.base.target) && dec.hold.rank_set.size() != p.held.size())
    return co::Err(co::ErrorCode::Internal, "decide: hold-band state lost");
  return co::Ok(std::move(dec));
}
// The as-of decision from verified pins and inputs; decision.json is written last.
co::Result<DecideOutcome> decide(const DecideConfig& cfg, std::ostream& progress) {
  ATX_TRY_VOID(validate_config(cfg));
  ATX_TRY(const i64 asof, session_of(cfg.asof));
  ATX_TRY(const auto deploy, read_deploy(cfg.deploy_path));
  ATX_TRY(const auto loaded, verified_load(deploy, cfg, asof));
  const auto view = loaded.view();
  const auto& x = view.target;
  ATX_TRY(const usize d, asof_row(x, asof));
  ATX_TRY_VOID(check_cadence_anchor(deploy, x));
  ATX_TRY(const auto fresh, check_freshness(x, d, cfg.allow_stale));
  ATX_TRY(const auto inputs, read_inputs(cfg, x, asof));
  ATX_TRY(const auto dec, decide_row(view, deploy, d, inputs));
  const Facts facts{x, d, dec, inputs.positions,
                    inputs.locates_supplied ? &inputs.locates : nullptr, inputs.nav};
  const f64 nav_dollars = cfg.nav_dollars.value_or(inputs.nav);
  ATX_TRY(const auto book, share_orders(cfg, view, deploy.base, d, dec, inputs.positions,
                                        inputs.nav, nav_dollars));
  const f64 leverage = deploy.base.target.aim_leverage;
  const auto tc = transfer_coefficient(x, d, dec, leverage);
  const auto book_id = deploy.m.at("book").get<std::string>();
  ATX_TRY(const auto band, tc_band(cfg, book_id, asof, tc.desired_target));
  const auto manifest_git = deploy.m.at("source").at("git_sha").get<std::string>();
  auto health = health_checks(facts, deploy.bands, manifest_git, cfg.build_source_sha);
  add_w4_checks(health, band, tc.desired_target, fresh, cfg);
  const Parity parity = cfg.check_replay ? replay_parity(facts) : Parity{};
  const auto dir = std::filesystem::path(cfg.output_directory);
  ATX_TRY(const auto published, write_tables(dir, facts, leverage, book,
                                             hold_band_declared(deploy.base.target)));
  const Record record{cfg, deploy, loaded.recipe_sha256, asof, d, inputs, facts, tc, health,
                      parity, book, nav_dollars, published};
  ATX_TRY_VOID(write_json(dir / "decision.json", decision_summary(record)));
  report(progress, record);
  return co::Ok(DecideOutcome{health.worst, parity.checked, parity.names, parity.mismatches});
}
} // namespace

usize tc_band_run(f64 asof_tc, std::span<const f64> priors_newest_first) noexcept {
  const auto below = [](f64 v) { return std::isfinite(v) && v < tc_band_min; };
  if (!below(asof_tc)) return 0;
  usize run = 1;
  for (const f64 v : priors_newest_first) {
    if (run >= tc_band_consecutive || !below(v)) break;
    ++run;
  }
  return run;
}

co::Result<DecideOutcome> run_decide(const DecideConfig& cfg, std::ostream& progress) {
  try {
    return decide(cfg, progress);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "decide: allocation failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("decide: ") + e.what());
  }
}

namespace {
// Valueless decide switches.
bool decide_switch(const std::string& key, DecideConfig& cfg) {
  if (key == "--check-replay") cfg.check_replay = true;
  else if (key == "--allow-stale") cfg.allow_stale = true;
  else if (key == "--flat-book") cfg.flat_book = true;
  else return false;
  return true;
}
void decide_value(const std::string& key, const std::string& value, DecideConfig& cfg) {
  const auto real = [&value, &key]() {
    f64 x = 0;
    if (!parse_cell(std::string_view{value}, x)) throw std::invalid_argument("invalid " + key);
    return x;
  };
  if (key == "--deploy") cfg.deploy_path = value;
  else if (key == "--asof") cfg.asof = value;
  else if (key == "--positions") cfg.positions_path = value;
  else if (key == "--locates") cfg.locates_path = value;
  else if (key == "--output") cfg.output_directory = value;
  else if (key == "--nav") { cfg.nav = real(); cfg.nav_text = value; }
  else if (key == "--nav-dollars") cfg.nav_dollars = real();
  else if (key == "--min-notional") cfg.min_notional = real();
  else if (key == "--price-source") cfg.price_source = value;
  else if (key == "--prior-decisions") cfg.prior_decisions_directory = value;
  else if (key == "--lot-size") {
    if (!parse_cell(std::string_view{value}, cfg.lot_size))
      throw std::invalid_argument("invalid --lot-size");
  } else throw std::invalid_argument("unknown flag: " + key);
}
} // namespace

int dispatch_decide(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    DecideConfig cfg;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "decide --deploy MANIFEST.json --asof YYYY-MM-DD --positions CSV|HOLDINGS_DIR "
               "--output NEWDIR [--locates CSV] [--nav DOLLARS] [--check-replay] "
               "[--flat-book] [--nav-dollars D (default: the book NAV)] [--lot-size 1] "
               "[--min-notional 500] [--price-source close] [--prior-decisions DIR] "
               "[--allow-stale]\n"
               "Targets and orders of the deployed book (atx.book-deploy/v1) at the as-of from "
               "the actual positions, by the NAV replay's own DECIDE functions, plus "
               "lot-rounded share orders and the expected book after they fill. Exit 0 "
               "decided, 1 refused, 2 usage, 3 health ERROR, 4 replay parity mismatch.\n";
        return 0;
      }
      if (!seen.insert(key).second) throw std::invalid_argument("duplicate flag: " + key);
      if (decide_switch(key, cfg)) continue;
      if (i + 1 >= argc) throw std::invalid_argument("missing value: " + key);
      decide_value(key, argv[++i], cfg);
    }
    const auto exe = current_executable_sha256();
    cfg.executable_sha256 = exe ? *exe : std::string{};
    cfg.build_source_sha = std::string(build_engine_git_sha());
    const auto outcome = run_decide(cfg, out);
    if (!outcome) { err << outcome.error().to_string() << '\n'; return 1; }
    if (outcome->parity_checked && outcome->parity_mismatches) {
      err << "decide: replay parity mismatch on " << outcome->parity_mismatches << " names\n";
      return 4;
    }
    if (outcome->health == DecideHealth::Error) {
      err << "decide: health ERROR (see decision.json); review before any order\n";
      return 3;
    }
    return 0;
  } catch (const std::exception& e) { err << "decide: " << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
