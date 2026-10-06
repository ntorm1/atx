#include "book/nav_spec.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "strategy_cost_v2.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::nav_spec {
namespace {
namespace co = atx::core;
using Json = nlohmann::json;

// The choices of each typed flag (the parsers' own value sets).
constexpr std::array<std::string_view, 7> rules_values{
    "baseline-v1", "monthly-budget-v2", "aim-partial-v5", "aim-partial-v6",
    "spo-v1",      "spo-v2",            "spo-v3"};
constexpr std::array<std::string_view, 4> neutralize_values{
    "none", "price-risk-v1", "price-risk-ind-v1", "price-risk-ind-v2"};
constexpr std::array<std::string_view, 2> holdings_formats_values{"f64", "csv"};
constexpr std::array<std::string_view, 2> order_bases_values{"target", "delta"};
constexpr std::array<std::string_view, 1> inv_vol_values{"inv-vol-v1"};
constexpr std::array<std::string_view, 1> norm_score_values{"norm-score-v1"};
constexpr std::array<std::string_view, 1> two_speed_values{"two-speed-v1"};
constexpr std::array<std::string_view, 1> vol_target_values{"vol-target-v1"};
constexpr std::array<std::string_view, 2> on_off_values{"on", "off"};
constexpr std::array<std::string_view, 1> implied_aim_values{"implied-aim"};
constexpr std::array<std::string_view, 2> spo_books_values{"all", "primary"};
constexpr std::span<const std::string_view> no_choices{};

// Every flag of the nav verb that configures a run, in the order to_args renders them. The
// registry-valued flags (--rate, --cost-law, --target-rule) are texts: their parsers own the ids
// (nav_rules.hpp), so a rule appended there needs no edit here.
constexpr Key table[] = {
    // pinned inputs, outputs and the budget
    {"combined", "--combined", Type::Text, no_choices},
    {"combined_sha256", "--combined-sha256", Type::Text, no_choices},
    {"role", "--role", Type::Text, no_choices},
    {"role_sha256", "--role-sha256", Type::Text, no_choices},
    {"output", "--output", Type::Text, no_choices},
    {"fields", "--fields", Type::Text, no_choices},
    {"fields_sha256", "--fields-sha256", Type::Text, no_choices},
    {"label_role", "--label-role", Type::Text, no_choices},
    {"label_role_sha256", "--label-role-sha256", Type::Text, no_choices},
    {"risk_model", "--risk-model", Type::Text, no_choices},
    {"risk_model_sha256", "--risk-model-sha256", Type::Text, no_choices},
    {"construction_grid", "--construction-grid", Type::Text, no_choices},
    {"emit_holdings", "--emit-holdings", Type::Text, no_choices},
    {"holdings_format", "--holdings-format", Type::Choice, holdings_formats_values},
    {"max_bytes", "--max-bytes", Type::Count, no_choices},
    // construction
    {"rule", "--rule", Type::Choice, rules_values},
    {"cadence", "--cadence", Type::Count, no_choices},
    {"trade_fraction", "--trade-fraction", Type::Real, no_choices},
    {"monthly_budget", "--monthly-budget", Type::Real, no_choices},
    {"band_multiple", "--band-multiple", Type::Real, no_choices},
    {"dust_multiple", "--dust-multiple", Type::Real, no_choices},
    {"aim_leverage", "--aim-leverage", Type::Real, no_choices},
    {"exit_rate", "--exit-rate", Type::Real, no_choices},
    {"neutralize", "--neutralize", Type::Choice, neutralize_values},
    {"hold_band", "--hold-band", Type::Real, no_choices},
    {"adv_hold_q", "--adv-hold-q", Type::Real, no_choices},
    {"vol_scale", "--vol-scale", Type::Choice, inv_vol_values},
    {"rank_shape", "--rank-shape", Type::Choice, norm_score_values},
    {"two_speed", "--two-speed", Type::Choice, two_speed_values},
    {"target_rule", "--target-rule", Type::Text, no_choices},
    // the trade rate
    {"rate", "--rate", Type::Text, no_choices},
    {"rate_rra", "--rate-rra", Type::Real, no_choices},
    {"rate_lambda", "--rate-lambda", Type::Real, no_choices},
    {"rate_min", "--rate-min", Type::Real, no_choices},
    {"rate_max", "--rate-max", Type::Real, no_choices},
    // execution
    {"order_basis", "--order-basis", Type::Choice, order_bases_values},
    {"locate_in_aim", "--locate-in-aim", Type::Switch, no_choices},
    {"liquidity_cache", "--liquidity-cache", Type::Switch, no_choices},
    {"warm_start_sessions", "--warm-start-sessions", Type::Count, no_choices},
    {"book_workers", "--book-workers", Type::Count, no_choices},
    {"daily_turnover_mean_max", "--daily-turnover-mean-max", Type::Real, no_choices},
    {"daily_turnover_p95_max", "--daily-turnover-p95-max", Type::Real, no_choices},
    {"stage_timers", "--stage-timers", Type::Switch, no_choices},
    // P9 C3 (Rulings PRE-7 / PRE-7a): print-only, summary.json's scenarios gain ann_mean_gross
    {"ann_mean_gross", "--ann-mean-gross", Type::Switch, no_choices},
    // P9 C2: PM6-6 in one process
    {"calibrate_gross", "--calibrate-gross", Type::Real, no_choices},
    {"calibrate_tolerance", "--calibrate-tolerance", Type::Real, no_choices},
    // the v7 seam: cost laws, capacity, aim-partial-v6, leverage rules, spo
    {"cost_v2", "--cost-v2", Type::Switch, no_choices},
    {"cost_law", "--cost-law", Type::Text, no_choices},
    {"capacity_curve", "--capacity-curve", Type::Switch, no_choices},
    {"cost_shrink_kappa", "--cost-shrink-kappa", Type::Real, no_choices},
    {"band_b", "--band-b", Type::Real, no_choices},
    {"band_exponent", "--band-exponent", Type::Real, no_choices},
    {"rate_clip", "--rate-clip", Type::RealPair, no_choices},
    {"vol_target", "--vol-target", Type::Choice, vol_target_values},
    {"risk_target", "--risk-target", Type::Real, no_choices},
    {"risk_target_bias", "--risk-target-bias", Type::Real, no_choices},
    {"risk_target_cadence", "--risk-target-cadence", Type::Count, no_choices},
    {"gamma", "--gamma", Type::Real, no_choices},
    {"ic_book", "--ic-book", Type::Real, no_choices},
    {"w_max", "--w-max", Type::Real, no_choices},
    {"adv_cap_q", "--adv-cap-q", Type::Real, no_choices},
    {"adv_trade_p", "--adv-trade-p", Type::Real, no_choices},
    {"spo_iters", "--spo-iters", Type::Count, no_choices},
    {"spo_tol", "--spo-tol", Type::Real, no_choices},
    {"target_vol", "--target-vol", Type::Real, no_choices},
    {"spo_horizon", "--spo-horizon", Type::Real, no_choices},
    {"alpha_horizon", "--alpha-horizon", Type::Real, no_choices},
    {"specific_ceiling", "--specific-ceiling", Type::Real, no_choices},
    {"specific_ceiling_void", "--specific-ceiling-void", Type::Choice, on_off_values},
    {"spo_gross", "--spo-gross", Type::Real, no_choices},
    {"spo_alpha", "--spo-alpha", Type::Choice, implied_aim_values},
    {"spo_books", "--spo-books", Type::Choice, spo_books_values},
    // P9 AL-CLOCK (Ruling ALCLOCK-1): print-only, summary.json's scenarios gain info_clock
    {"info_clock_print", "--info-clock-print", Type::Switch, no_choices},
};

tl::unexpected<co::Error> bad(const std::string& what) {
  return co::Err(co::ErrorCode::InvalidArgument, "nav spec: " + what);
}

// The command line's numbers (book/nav_run.cpp parse_real / parse_integer): the whole
// token, else refused; a NavSpec number is finite.
co::Result<f64> real_of(const std::string& value) {
  try {
    usize used = 0;
    const f64 x = std::stod(value, &used);
    if (used == value.size() && std::isfinite(x)) return co::Ok(x);
  } catch (const std::exception&) {
    // invalid_argument / out_of_range: refused below
  }
  return bad("'" + value + "' is not a finite number");
}
co::Result<u64> count_of(const std::string& value) {
  u64 x = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
  if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    return bad("'" + value + "' is not a non-negative integer");
  return co::Ok(x);
}
// Shortest round-trip text: stod reads it back to the same bits.
std::string number_text(f64 x) {
  std::array<char, 64> buf{};
  const auto written = std::to_chars(buf.data(), buf.data() + buf.size(), x);
  return std::string(buf.data(), written.ptr);
}
bool listed(const Key& k, std::string_view value) {
  for (const auto choice : k.choices)
    if (choice == value) return true;
  return false;
}

// A command-line value as the key's JSON value.
co::Result<Json> typed(const Key& k, const std::string& value) {
  switch (k.type) {
  case Type::Switch: return co::Ok(Json(true));
  case Type::Real: {
    ATX_TRY(const f64 x, real_of(value));
    return co::Ok(Json(x));
  }
  case Type::Count: {
    ATX_TRY(const u64 x, count_of(value));
    return co::Ok(Json(x));
  }
  case Type::Text:
    if (value.empty()) return bad(std::string(k.flag) + " takes a non-empty value");
    return co::Ok(Json(value));
  case Type::Choice:
    if (!listed(k, value))
      return bad(std::string(k.flag) + " " + value + " is not one of its values");
    return co::Ok(Json(value));
  case Type::RealPair: {
    const auto comma = value.find(',');
    if (comma == std::string::npos)
      return bad(std::string(k.flag) + " takes LO,HI");
    ATX_TRY(const f64 lo, real_of(value.substr(0, comma)));
    ATX_TRY(const f64 hi, real_of(value.substr(comma + 1)));
    return co::Ok(Json::array({lo, hi}));
  }
  }
  return bad("unknown key type");
}

co::Result<f64> finite_number(const Json& v, const Key& k) {
  if (!v.is_number() || !std::isfinite(v.get<f64>()))
    return bad(std::string(k.key) + " is a finite number");
  return co::Ok(v.get<f64>());
}
// One option as its command-line tokens (appended to `out`).
co::Status render(const Key& k, const Json& v, std::vector<std::string>& out) {
  const std::string flag(k.flag);
  switch (k.type) {
  case Type::Switch:
    if (!v.is_boolean()) return bad(std::string(k.key) + " is true or false");
    if (v.get<bool>()) out.push_back(flag);
    return co::Ok();
  case Type::Real: {
    ATX_TRY(const f64 x, finite_number(v, k));
    out.push_back(flag); out.push_back(number_text(x));
    return co::Ok();
  }
  case Type::Count:
    if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<i64>() < 0))
      return bad(std::string(k.key) + " is a non-negative integer");
    out.push_back(flag); out.push_back(std::to_string(v.get<u64>()));
    return co::Ok();
  case Type::Text:
    if (!v.is_string() || v.get<std::string>().empty())
      return bad(std::string(k.key) + " is a non-empty string");
    out.push_back(flag); out.push_back(v.get<std::string>());
    return co::Ok();
  case Type::Choice:
    if (!v.is_string() || !listed(k, v.get<std::string>()))
      return bad(std::string(k.key) + " is one of its listed strings");
    out.push_back(flag); out.push_back(v.get<std::string>());
    return co::Ok();
  case Type::RealPair: {
    if (!v.is_array() || v.size() != 2)
      return bad(std::string(k.key) + " is [lo, hi]");
    ATX_TRY(const f64 lo, finite_number(v.at(0), k));
    ATX_TRY(const f64 hi, finite_number(v.at(1), k));
    out.push_back(flag); out.push_back(number_text(lo) + "," + number_text(hi));
    return co::Ok();
  }
  }
  return bad("unknown key type");
}

bool same_number(const Json& v, f64 expected) {
  return v.is_number() && v.get<f64>() == expected;
}
bool same_count(const Json& v, usize expected) {
  return v.is_number_unsigned() && v.get<u64>() == expected;
}
bool same_multiples(const Json& v) {
  if (!v.is_array() || v.size() != cost_v2::capacity_multiples.size()) return false;
  for (usize k = 0; k < v.size(); ++k)
    if (!same_number(v.at(k), cost_v2::capacity_multiples[k])) return false;
  return true;
}
// A compiled constant at today's value (header); false for any other key or value.
bool pinned_constant_holds(const std::string& key, const Json& v) {
  const NavReplayConfig nav{};
  if (key == "initial_nav") return same_number(v, nav.initial_nav);
  if (key == "liquidity_window") return same_count(v, nav.liquidity_window);
  if (key == "min_vol_pairs") return same_count(v, nav.min_vol_pairs);
  if (key == "capacity_multiples") return same_multiples(v);
  return false;
}
bool pinned_constant(const std::string& key) {
  return key == "initial_nav" || key == "liquidity_window" || key == "min_vol_pairs" ||
         key == "capacity_multiples";
}
// leverage_range (header): inside [1, 2]; narrower only around the run's aim_leverage and
// without calibrate_gross or construction_grid (either one moves the aim leverage after this
// check: PM6-6's correction, the grid variants' --aim-leverage).
co::Status check_leverage_range(const Json& range, const Json& options) {
  if (!range.is_array() || range.size() != 2 || !range.at(0).is_number() ||
      !range.at(1).is_number())
    return bad("constants.leverage_range is [lo, hi]");
  const f64 lo = range.at(0).get<f64>(), hi = range.at(1).get<f64>();
  if (!(lo >= aim_leverage_min && lo <= hi && hi <= aim_leverage_max))
    return bad("constants.leverage_range must lie inside [1, 2] (a wider range is owner "
               "decision OD-P9-3)");
  if (lo == aim_leverage_min && hi == aim_leverage_max) return co::Ok();
  const f64 aim = options.contains("aim_leverage") && options.at("aim_leverage").is_number()
                      ? options.at("aim_leverage").get<f64>()
                      : TargetReplayConfig{}.aim_leverage;
  if (!(aim >= lo && aim <= hi))
    return bad("aim_leverage " + number_text(aim) + " is outside constants.leverage_range");
  if (options.contains("calibrate_gross"))
    return bad("calibrate_gross corrects inside [1, 2] only; a narrower "
               "constants.leverage_range is refused with it");
  if (options.contains("construction_grid"))
    return bad("construction_grid variants set their own --aim-leverage, which this range does "
               "not check; a narrower constants.leverage_range is refused with it");
  return co::Ok();
}
// The constants block (header).
co::Status check_constants(const Json& spec, const Json& options) {
  if (!spec.contains("constants")) return co::Ok();
  const auto& c = spec.at("constants");
  if (!c.is_object()) return bad("constants is an object");
  for (auto it = c.begin(); it != c.end(); ++it) {
    const std::string& key = it.key();
    if (key == "leverage_range") {
      ATX_TRY_VOID(check_leverage_range(it.value(), options));
    } else if (!pinned_constant(key)) {
      return bad("constants." + key + " is not a NavSpec constant");
    } else if (!pinned_constant_holds(key, it.value())) {
      return bad("constants." + key + " is a compiled constant: today's value only (moving it "
                 "is a new registered rule; nav --print-spec prints it)");
    }
  }
  return co::Ok();
}

// The spec file's bytes (at most max_spec_bytes).
co::Result<std::string> read_text(const std::string& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > max_spec_bytes)
    return bad("the spec file is missing or above 1 MiB: " + path);
  std::string text(static_cast<usize>(size), '\0');
  std::ifstream file(path, std::ios::binary);
  if (!file || !file.read(text.data(), static_cast<std::streamsize>(size)))
    return co::Err(co::ErrorCode::IoError, "nav spec: read " + path);
  return co::Ok(std::move(text));
}
// The spec text as JSON. A key repeated inside one object is refused: the parser keeps the last
// value while a reader of the pinned file sees the first (review C3 R-1). The duplicate-key
// callback of atx-engine/src/eval/trial_epoch.cpp: one key set per open object.
co::Result<Json> parse_spec(const std::string& text, const std::string& path) {
  std::vector<std::set<std::string>> objects;
  std::string duplicate;
  const auto unique_keys = [&objects, &duplicate](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) {
      objects.emplace_back();
    } else if (event == Json::parse_event_t::object_end) {
      if (!objects.empty()) objects.pop_back();
    } else if (event == Json::parse_event_t::key && duplicate.empty() && !objects.empty() &&
               !objects.back().insert(value.get<std::string>()).second) {
      duplicate = value.get<std::string>();
    }
    return true;
  };
  auto doc = Json::parse(text, unique_keys, false);
  if (doc.is_discarded()) return bad("the spec file is not JSON: " + path);
  if (!duplicate.empty())
    return bad("duplicate key \"" + duplicate + "\" in " + path +
               " (a key appears once per object: JSON keeps the last value, a reader the first)");
  return co::Ok(std::move(doc));
}
// A spec file read once: its document (checked by to_args) and the SHA-256 of the very bytes
// parsed (review C3 S-4: the printed digest describes what the run read).
struct SpecFile {
  Json doc;
  std::string sha256;
};
co::Result<SpecFile> load(const std::string& path) {
  ATX_TRY(const auto text, read_text(path));
  SpecFile out;
  ATX_TRY(out.doc, parse_spec(text, path));
  ATX_TRY_VOID(to_args(out.doc));
  ATX_TRY(out.sha256, co::sha256_hex(text));
  return co::Ok(std::move(out));
}
} // namespace

std::span<const Key> keys() noexcept { return table; }
const Key* by_key(std::string_view key) noexcept {
  for (const auto& k : table)
    if (k.key == key) return &k;
  return nullptr;
}
const Key* by_flag(std::string_view flag) noexcept {
  for (const auto& k : table)
    if (k.flag == flag) return &k;
  return nullptr;
}

co::Result<Json> from_args(std::span<const std::string> args) {
  Json options = Json::object();
  for (usize i = 0; i < args.size(); ++i) {
    const auto* k = by_flag(args[i]);
    if (!k) return bad(args[i] + " is not a run flag of the nav verb");
    const std::string key(k->key);
    if (options.contains(key)) return bad("repeated flag " + args[i]);
    if (k->type == Type::Switch) {
      options[key] = true;
      continue;
    }
    if (i + 1 >= args.size()) return bad(args[i] + " takes a value");
    ATX_TRY(options[key], typed(*k, args[++i]));
  }
  return co::Ok(Json{{"schema", schema}, {"options", std::move(options)}});
}

co::Result<std::vector<std::string>> to_args(const Json& spec) {
  if (!spec.is_object() || !spec.contains("schema") || spec.at("schema") != schema ||
      !spec.contains("options") || !spec.at("options").is_object())
    return bad("a NavSpec is {\"schema\": \"atx.nav-spec/v1\", \"options\": {...}, "
                              "\"constants\": {...} (optional)}");
  for (auto it = spec.begin(); it != spec.end(); ++it)
    if (it.key() != "schema" && it.key() != "options" && it.key() != "constants")
      return bad("unknown top-level key " + it.key());
  const auto& options = spec.at("options");
  for (auto it = options.begin(); it != options.end(); ++it)
    if (!by_key(it.key())) return bad("unknown option " + it.key());
  ATX_TRY_VOID(check_constants(spec, options));
  std::vector<std::string> out;
  for (const auto& k : table) {
    const std::string key(k.key);
    if (options.contains(key)) ATX_TRY_VOID(render(k, options.at(key), out));
  }
  return co::Ok(std::move(out));
}

co::Result<Json> read(const std::string& path) {
  ATX_TRY(auto spec, load(path));
  return co::Ok(std::move(spec.doc));
}

Json constants_json() {
  const NavReplayConfig nav{};
  return Json{{"initial_nav", nav.initial_nav},
              {"liquidity_window", nav.liquidity_window},
              {"min_vol_pairs", nav.min_vol_pairs},
              {"capacity_multiples", Json(std::vector<f64>(cost_v2::capacity_multiples.begin(),
                                                           cost_v2::capacity_multiples.end()))},
              {"leverage_range", Json::array({aim_leverage_min, aim_leverage_max})}};
}

int dispatch(int argc, char** argv, std::ostream& out, std::ostream& err) {
  bool spec = false, print = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view token = argv[i];
    spec = spec || token == "--spec";
    print = print || token == "--print-spec";
  }
  if (!spec && !print) return not_spec;
  if (spec && print) {
    err << "nav spec: --spec and --print-spec are exclusive\n";
    return 2;
  }
  if (print) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
      if (std::string_view(argv[i]) != "--print-spec") args.emplace_back(argv[i]);
    auto doc = from_args(args);
    if (!doc) {
      err << doc.error().to_string() << '\n';
      return 2;
    }
    (*doc)["constants"] = constants_json();
    out << doc->dump(2) << '\n';
    return 0;
  }
  std::string path, output;
  for (int i = 1; i < argc; ++i) {
    const std::string_view key = argv[i];
    if ((key == "--spec" || key == "--output") && i + 1 < argc) {
      auto& slot = key == "--spec" ? path : output;
      if (!slot.empty()) {
        err << "nav spec: repeated " << key << '\n';
        return 2;
      }
      slot = argv[++i];
      continue;
    }
    err << "nav spec: nav --spec NAVSPEC.json [--output NEWDIR]; the spec holds every other flag ("
        << key << ")\n";
    return 2;
  }
  auto spec_file = load(path);
  if (!spec_file) {
    err << spec_file.error().to_string() << '\n';
    return 2;
  }
  auto& doc = spec_file->doc;
  if (!output.empty()) doc["options"]["output"] = output;
  auto args = to_args(doc);
  if (!args) {
    err << args.error().to_string() << '\n';
    return 2;
  }
  out << "nav spec: " << schema << ' ' << path << " sha256 " << spec_file->sha256 << '\n';
  std::vector<std::string> tokens{argv[0]};
  tokens.insert(tokens.end(), args->begin(), args->end());
  std::vector<char*> pointers;
  pointers.reserve(tokens.size());
  for (auto& token : tokens) pointers.push_back(token.data());
  return dispatch_nav_replay(static_cast<int>(pointers.size()), pointers.data(), out, err);
}

} // namespace atx::impl::strategy::nav_spec
