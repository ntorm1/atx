#include "strategy_reconcile.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr i64 open_start = std::numeric_limits<i64>::min();
constexpr i64 open_end = std::numeric_limits<i64>::max();
constexpr usize max_rows = 5'000'000;
constexpr u64 max_json_bytes = 16ULL << 20;
constexpr usize max_report_lines = 200;
constexpr const char* book_deploy = "atx.book-deploy/v1";
constexpr const char* book_decision = "atx.book-decision/v1";
constexpr const char* expected_file = "expected_holdings.csv";

// Converts to any Result (a refusal of bad input).
tl::unexpected<co::Error> refuse(const std::string& why) {
  return co::Err(co::ErrorCode::InvalidArgument, "reconcile: " + why);
}
template<class T> bool parse_cell(std::string_view s, T& out) {
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size();
}
co::Result<i64> date_ns(std::string_view text) {
  int year = 0; unsigned month = 0, day = 0;
  const auto part = [&](usize at, usize size, auto& out) {
    return parse_cell(text.substr(at, size), out);
  };
  if (text.size() != 10 || text[4] != '-' || text[7] != '-' || !part(0, 4, year) ||
      !part(5, 2, month) || !part(8, 2, day))
    return refuse("date must be YYYY-MM-DD: " + std::string(text));
  const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month},
                                         std::chrono::day{day}};
  if (!date.ok()) return refuse("not a calendar date: " + std::string(text));
  return co::Ok(static_cast<i64>(std::chrono::sys_days{date}.time_since_epoch().count()) * day_ns);
}
std::string upper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return s;
}

// ---- plain CSV (comma-separated, no quoting) ----
struct Table {
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  [[nodiscard]] usize column(std::string_view name) const {
    const auto it = std::find(columns.begin(), columns.end(), name);
    return it == columns.end() ? npos : static_cast<usize>(it - columns.begin());
  }
  // The first of `names` present, else npos.
  [[nodiscard]] usize either(std::initializer_list<std::string_view> names) const {
    for (const auto name : names)
      if (const usize k = column(name); k != npos) return k;
    return npos;
  }
  static constexpr usize npos = std::numeric_limits<usize>::max();
};
std::vector<std::string> split(std::string_view line) {
  std::vector<std::string> out;
  for (usize at = 0;;) {
    const usize comma = line.find(',', at);
    out.emplace_back(line.substr(at, comma == std::string_view::npos ? line.npos : comma - at));
    if (comma == std::string_view::npos) return out;
    at = comma + 1;
  }
}
co::Result<Table> read_table(const std::string& path, const char* what) {
  std::ifstream file(path, std::ios::binary);
  std::string line;
  if (!file || !std::getline(file, line))
    return refuse(std::string(what) + " unreadable: " + path);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  Table t;
  t.columns = split(line);
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto cells = split(line);
    if (cells.size() != t.columns.size())
      return refuse(std::string(what) + " row malformed: " + line.substr(0, 120));
    if (t.rows.size() >= max_rows) return refuse(std::string(what) + " has too many rows");
    t.rows.push_back(std::move(cells));
  }
  return co::Ok(std::move(t));
}
co::Result<std::string> read_json_text(const std::filesystem::path& path, const char* what) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > max_json_bytes)
    return refuse(std::string(what) + " missing or oversized: " + path.string());
  std::string text(static_cast<usize>(file.tellg()), '\0');
  file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) return co::Err(co::ErrorCode::IoError, std::string("reconcile: ") + what + " read");
  return co::Ok(std::move(text));
}

// ---- identity bridge ----
enum class KeyKind : u8 { InstrumentId, Ticker, Cik };
const char* key_label(KeyKind k) {
  switch (k) {
  case KeyKind::InstrumentId: return "instrument_id";
  case KeyKind::Ticker: return "ticker";
  case KeyKind::Cik: return "cik";
  }
  return "instrument_id";
}
struct Link {
  u64 id{};
  std::string ticker; // upper case; empty: none
  std::optional<u64> cik;
  i64 start{open_start}, end{open_end};
  bool primary{}; // P line (primary column absent: false for every link)
};
struct Mapping {
  std::optional<u64> id;
  const char* problem{}; // "no link" | "ambiguous"
};
class Identity {
public:
  static co::Result<Identity> load(const std::string& path);
  [[nodiscard]] bool empty() const noexcept { return links_.empty(); }
  [[nodiscard]] Mapping resolve(KeyKind kind, const std::string& key, i64 at) const;

private:
  std::vector<Link> links_;
};
co::Result<Identity> Identity::load(const std::string& path) {
  ATX_TRY(const auto t, read_table(path, "identity"));
  const usize id = t.either({"instrument_id", "sr_id"}), ticker = t.column("ticker");
  const usize cik = t.column("cik"), start = t.column("start");
  const usize end = t.either({"end", "end_incl"}), primary = t.column("primary");
  if (id == Table::npos || (ticker == Table::npos && cik == Table::npos))
    return refuse("identity needs instrument_id (or sr_id) and ticker and/or cik");
  Identity out;
  out.links_.reserve(t.rows.size());
  for (const auto& r : t.rows) {
    Link link;
    if (!parse_cell(r[id], link.id)) return refuse("identity instrument id: " + r[id]);
    if (ticker != Table::npos) link.ticker = upper(r[ticker]);
    if (cik != Table::npos && !r[cik].empty()) {
      u64 value = 0;
      if (!parse_cell(r[cik], value)) return refuse("identity cik: " + r[cik]);
      link.cik = value;
    }
    if (start != Table::npos && !r[start].empty()) { ATX_TRY(link.start, date_ns(r[start])); }
    if (end != Table::npos && !r[end].empty()) { ATX_TRY(link.end, date_ns(r[end])); }
    if (link.end < link.start) return refuse("identity link ends before it starts");
    link.primary = primary != Table::npos && r[primary] == "P";
    out.links_.push_back(std::move(link));
  }
  return co::Ok(std::move(out));
}
Mapping Identity::resolve(KeyKind kind, const std::string& key, i64 at) const {
  std::optional<u64> cik;
  if (kind == KeyKind::Cik) {
    u64 value = 0;
    if (!parse_cell(key, value)) return {std::nullopt, "no link"};
    cik = value;
  }
  const std::string ticker = kind == KeyKind::Ticker ? upper(key) : std::string{};
  std::set<u64> all, primary;
  for (const auto& l : links_) {
    if (at < l.start || at > l.end) continue;
    const bool match = kind == KeyKind::Ticker ? !ticker.empty() && l.ticker == ticker
                                               : l.cik && *l.cik == *cik;
    if (!match) continue;
    all.insert(l.id);
    if (l.primary) primary.insert(l.id);
  }
  if (all.size() == 1) return {*all.begin(), nullptr};
  if (all.empty()) return {std::nullopt, "no link"};
  if (primary.size() == 1) return {*primary.begin(), nullptr};
  return {std::nullopt, "ambiguous"};
}
co::Result<KeyKind> key_kind(const Table& t, usize& column, const char* what) {
  const std::array<std::pair<const char*, KeyKind>, 3> kinds{{
      {"instrument_id", KeyKind::InstrumentId}, {"ticker", KeyKind::Ticker},
      {"cik", KeyKind::Cik}}};
  for (const auto& [name, kind] : kinds)
    if ((column = t.column(name)) != Table::npos) return co::Ok(kind);
  return refuse(std::string(what) + " needs an instrument_id, ticker or cik column");
}
// An instrument id from a key cell: direct, or through the identity at `at`.
co::Result<Mapping> map_key(KeyKind kind, const std::string& cell, const Identity* identity,
                            i64 at, const char* what) {
  if (kind == KeyKind::InstrumentId) {
    u64 id = 0;
    if (!parse_cell(cell, id)) return refuse(std::string(what) + " instrument id: " + cell);
    return co::Ok(Mapping{id, nullptr});
  }
  if (!identity)
    return refuse(std::string(what) + " keys by " + key_label(kind) +
                  ": --identity maps it to instruments");
  return co::Ok(identity->resolve(kind, cell, at));
}

// ---- the three books ----
struct Deploy {
  std::string sha256, book;
};
co::Result<Deploy> read_deploy(const std::string& path) {
  ATX_TRY(const auto text, read_json_text(path, "deploy manifest"));
  ATX_TRY(auto sha, co::sha256_hex(std::string_view{text}));
  const Json m = Json::parse(text);
  if (!m.is_object() || m.value("schema", std::string{}) != book_deploy ||
      !m.contains("book") || !m.at("book").is_string())
    return refuse("deploy manifest must be atx.book-deploy/v1 with a book");
  return co::Ok(Deploy{std::move(sha), m.at("book").get<std::string>()});
}
struct ExpectedRow {
  f64 shares{}, price{nan};
};
struct Expected {
  std::map<u64, ExpectedRow> rows;
  i64 asof{};
  std::string asof_text, file, file_sha, decision_sha;
};
co::Result<Expected> read_expected(const std::string& path, const Deploy& deploy) {
  std::error_code ec;
  const bool dir = std::filesystem::is_directory(path, ec);
  const std::filesystem::path file = dir ? std::filesystem::path(path) / expected_file
                                         : std::filesystem::path(path);
  const auto decision_path = file.parent_path() / "decision.json";
  ATX_TRY(const auto text, read_json_text(decision_path, "decision.json"));
  const Json j = Json::parse(text);
  Expected out;
  out.file = file.string();
  ATX_TRY(out.decision_sha, co::sha256_hex(std::string_view{text}));
  ATX_TRY(out.file_sha, co::sha256_file(out.file));
  if (!j.is_object() || j.value("schema", std::string{}) != book_decision ||
      j.value("status", std::string{}) != "complete")
    return refuse("the expected book's decision.json must be a complete atx.book-decision/v1");
  if (j.value("book", std::string{}) != deploy.book)
    return refuse("the decision's book differs from the deploy manifest's");
  const auto& pins = j.at("pins_verified");
  if (pins.value("deploy_manifest_sha256", std::string{}) != deploy.sha256)
    return refuse("the decision was not made under this deploy manifest (SHA-256 differs)");
  if (j.at("files").value(expected_file, std::string{}) != out.file_sha)
    return refuse("expected_holdings.csv differs from the decision's SHA-256");
  out.asof_text = j.at("asof").get<std::string>();
  ATX_TRY(out.asof, date_ns(out.asof_text));
  ATX_TRY(const auto t, read_table(out.file, "expected holdings"));
  const usize id = t.column("instrument_id"), shares = t.column("shares");
  const usize price = t.column("reference_price");
  if (id == Table::npos || shares == Table::npos)
    return refuse("expected holdings need instrument_id and shares");
  for (const auto& r : t.rows) {
    u64 key = 0; ExpectedRow row;
    if (!parse_cell(r[id], key) || !parse_cell(r[shares], row.shares))
      return refuse("expected holdings row malformed");
    if (price != Table::npos && !parse_cell(r[price], row.price)) row.price = nan;
    if (!out.rows.emplace(key, row).second) return refuse("expected holdings list a name twice");
  }
  return co::Ok(std::move(out));
}
struct BrokerRow {
  f64 shares{};
  std::string key;
};
struct Unmapped {
  std::string key;
  const char* problem{};
  f64 shares{};
};
struct Broker {
  std::map<u64, BrokerRow> rows;
  std::vector<Unmapped> unmapped;
  KeyKind kind{KeyKind::InstrumentId};
  std::string sha;
};
co::Result<Broker> read_broker(const std::string& path, const Identity* identity, i64 asof) {
  ATX_TRY(const auto t, read_table(path, "broker positions"));
  Broker out;
  ATX_TRY(out.sha, co::sha256_file(path));
  usize key = 0;
  ATX_TRY(out.kind, key_kind(t, key, "broker positions"));
  const usize shares = t.column("shares");
  if (shares == Table::npos) return refuse("broker positions need a shares column");
  for (const auto& r : t.rows) {
    f64 quantity = 0;
    if (!parse_cell(r[shares], quantity) || !std::isfinite(quantity))
      return refuse("broker shares malformed: " + r[shares]);
    ATX_TRY(const auto mapped, map_key(out.kind, r[key], identity, asof, "broker positions"));
    if (!mapped.id) { out.unmapped.push_back({r[key], mapped.problem, quantity}); continue; }
    if (!out.rows.emplace(*mapped.id, BrokerRow{quantity, r[key]}).second)
      return refuse("broker positions list instrument " + std::to_string(*mapped.id) + " twice");
  }
  return co::Ok(std::move(out));
}
struct Actions {
  std::map<u64, f64> ratio; // product of the applicable ratios, in date order
  usize rows{}, applied{};
  std::string sha;
};
co::Result<Actions> read_actions(const std::string& path, const Identity* identity, i64 after,
                                 i64 through) {
  ATX_TRY(const auto t, read_table(path, "corporate actions"));
  Actions out;
  ATX_TRY(out.sha, co::sha256_file(path));
  usize key = 0;
  ATX_TRY(const auto kind, key_kind(t, key, "corporate actions"));
  const usize date = t.either({"date", "ex_date"}), ratio = t.column("ratio");
  if (date == Table::npos || ratio == Table::npos)
    return refuse("corporate actions need date (or ex_date) and ratio");
  struct Action { i64 date; usize row; u64 id; f64 ratio; };
  std::vector<Action> applicable;
  for (usize k = 0; k < t.rows.size(); ++k) {
    const auto& r = t.rows[k];
    ATX_TRY(const i64 when, date_ns(r[date]));
    f64 value = 0;
    if (!parse_cell(r[ratio], value) || !std::isfinite(value) || !(value > 0))
      return refuse("corporate action ratio must be finite > 0: " + r[ratio]);
    ATX_TRY(const auto mapped, map_key(kind, r[key], identity, when, "corporate actions"));
    if (!mapped.id)
      return refuse("corporate action for " + r[key] + " maps to no instrument (" +
                    mapped.problem + ")");
    ++out.rows;
    if (when > after && when <= through) applicable.push_back({when, k, *mapped.id, value});
  }
  std::sort(applicable.begin(), applicable.end(), [](const Action& a, const Action& b) {
    return a.date != b.date ? a.date < b.date : a.row < b.row;
  });
  for (const auto& a : applicable) {
    auto [it, inserted] = out.ratio.emplace(a.id, a.ratio);
    if (!inserted) it->second *= a.ratio;
    ++out.applied;
  }
  return co::Ok(std::move(out));
}

// ---- comparison ----
enum class Status : u8 { Ok, Explained, Missing, Extra, Quantity, Unmapped };
const char* status_label(Status s) {
  switch (s) {
  case Status::Ok: return "ok";
  case Status::Explained: return "explained";
  case Status::Missing: return "missing";
  case Status::Extra: return "extra";
  case Status::Quantity: return "quantity";
  case Status::Unmapped: return "unmapped";
  }
  return "unmapped";
}
struct Line {
  u64 id{};
  std::string key;
  Status status{Status::Ok};
  f64 expected{}, ratio{1.0}, adjusted{}, broker{}, difference{}, price{nan};
};
Status classify(const Line& l, f64 tol_shares, f64 tol_fraction) {
  if (!std::isfinite(l.adjusted)) return Status::Quantity;
  const auto within = [&](f64 reference) {
    const f64 tolerance = std::max(tol_shares, tol_fraction * std::abs(reference));
    return std::abs(l.broker - reference) <= tolerance;
  };
  if (within(l.adjusted))
    return l.ratio != 1.0 && !within(l.expected) ? Status::Explained : Status::Ok;
  if (l.broker == 0) return Status::Missing;
  if (l.adjusted == 0) return Status::Extra;
  return Status::Quantity;
}
std::vector<Line> compare(const Expected& e, const Broker& b, const Actions& a,
                          const ReconcileConfig& cfg) {
  std::set<u64> ids;
  for (const auto& [id, row] : e.rows) ids.insert(id);
  for (const auto& [id, row] : b.rows) ids.insert(id);
  std::vector<Line> out;
  out.reserve(ids.size() + b.unmapped.size());
  for (const u64 id : ids) {
    Line l;
    l.id = id;
    if (const auto it = e.rows.find(id); it != e.rows.end()) {
      l.expected = it->second.shares; l.price = it->second.price;
    }
    if (const auto it = a.ratio.find(id); it != a.ratio.end()) l.ratio = it->second;
    l.adjusted = l.expected * l.ratio;
    l.price = l.price / l.ratio;
    if (const auto it = b.rows.find(id); it != b.rows.end()) {
      l.broker = it->second.shares; l.key = it->second.key;
    }
    l.difference = l.broker - l.adjusted;
    l.status = classify(l, cfg.tolerance_shares, cfg.tolerance_fraction);
    out.push_back(std::move(l));
  }
  for (const auto& u : b.unmapped) {
    Line l;
    l.key = u.key + " (" + u.problem + ")"; l.status = Status::Unmapped;
    l.broker = u.shares; l.difference = u.shares; l.adjusted = nan; l.expected = nan;
    out.push_back(std::move(l));
  }
  return out;
}
ReconcileOutcome tally(const std::vector<Line>& lines) {
  ReconcileOutcome o;
  for (const auto& l : lines) {
    ++o.compared;
    switch (l.status) {
    case Status::Ok: ++o.ok; break;
    case Status::Explained: ++o.explained; break;
    case Status::Missing: ++o.missing; break;
    case Status::Extra: ++o.extra; break;
    case Status::Quantity: ++o.quantity; break;
    case Status::Unmapped: ++o.unmapped; break;
    }
  }
  return o;
}
void write_number(std::ostream& out, f64 v) {
  if (std::isnan(v)) out << "nan"; else out << v;
}
co::Status write_lines(const std::filesystem::path& path, const std::vector<Line>& lines) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "reconcile: output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "instrument_id,broker_key,status,expected_shares,ratio,expected_adjusted,"
          "broker_shares,difference,reference_price,difference_notional\n";
  for (const auto& l : lines) {
    if (l.status == Status::Unmapped) file << ','; else file << l.id << ',';
    file << l.key << ',' << status_label(l.status) << ',';
    write_number(file, l.expected); file << ',' << l.ratio << ',';
    write_number(file, l.adjusted); file << ',' << l.broker << ',' << l.difference << ',';
    write_number(file, l.price); file << ',';
    write_number(file, l.difference * l.price); file << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "reconcile: close"));
}
struct Loaded {
  Deploy deploy;
  Expected expected;
  Broker broker;
  Actions actions;
  i64 asof{};
};
Json summary_json(const ReconcileConfig& cfg, const Loaded& in, const ReconcileOutcome& o,
                  const std::string& lines_sha) {
  const auto optional_file = [](const std::string& path, const std::string& sha) {
    return path.empty() ? Json(nullptr) : Json{{"path", path}, {"sha256", sha}};
  };
  return Json{{"schema", book_reconcile_schema}, {"status", "complete"},
      {"book", in.deploy.book}, {"decision_asof", in.expected.asof_text},
      {"broker_asof", cfg.asof},
      {"inputs", {{"deploy", {{"path", cfg.deploy_path}, {"sha256", in.deploy.sha256}}},
                  {"expected", {{"path", in.expected.file}, {"sha256", in.expected.file_sha},
                                {"decision_sha256", in.expected.decision_sha}}},
                  {"broker", {{"path", cfg.broker_path}, {"sha256", in.broker.sha},
                              {"key", key_label(in.broker.kind)}}},
                  {"corporate_actions", optional_file(cfg.corporate_actions_path,
                                                      in.actions.sha)},
                  {"identity", cfg.identity_path.empty() ? Json(nullptr)
                                                         : Json(cfg.identity_path)}}},
      {"tolerance", {{"shares", cfg.tolerance_shares}, {"fraction", cfg.tolerance_fraction},
                     {"rule", "|broker - expected x ratio| <= max(shares, fraction x "
                              "|expected x ratio|)"}}},
      {"corporate_actions", {{"rows", in.actions.rows}, {"applied", in.actions.applied},
                             {"names", in.actions.ratio.size()}}},
      {"counts", {{"compared", o.compared}, {"ok", o.ok}, {"explained", o.explained},
                  {"missing", o.missing}, {"extra", o.extra}, {"quantity", o.quantity},
                  {"unmapped", o.unmapped}, {"unexplained_breaks", o.breaks()}}},
      {"files", {{"reconciliation.csv", lines_sha}}}};
}
co::Status validate(const ReconcileConfig& cfg) {
  if (cfg.deploy_path.empty() || cfg.expected_path.empty() || cfg.broker_path.empty() ||
      cfg.asof.empty())
    return refuse("--deploy, --expected, --broker and --asof are required");
  if (!std::isfinite(cfg.tolerance_shares) || cfg.tolerance_shares < 0 ||
      !std::isfinite(cfg.tolerance_fraction) || cfg.tolerance_fraction < 0)
    return refuse("tolerances must be finite and >= 0");
  if (!cfg.output_directory.empty() && std::filesystem::exists(cfg.output_directory))
    return co::Err(co::ErrorCode::AlreadyExists, "reconcile: output must not exist");
  return co::Ok();
}
co::Result<Loaded> load(const ReconcileConfig& cfg) {
  Loaded in;
  ATX_TRY(in.asof, date_ns(cfg.asof));
  ATX_TRY(in.deploy, read_deploy(cfg.deploy_path));
  ATX_TRY(in.expected, read_expected(cfg.expected_path, in.deploy));
  if (in.asof < in.expected.asof) return refuse("the broker as-of precedes the decision's");
  std::optional<Identity> identity;
  if (!cfg.identity_path.empty()) { ATX_TRY(identity, Identity::load(cfg.identity_path)); }
  const Identity* bridge = identity ? &*identity : nullptr;
  ATX_TRY(in.broker, read_broker(cfg.broker_path, bridge, in.asof));
  if (!cfg.corporate_actions_path.empty()) {
    ATX_TRY(in.actions, read_actions(cfg.corporate_actions_path, bridge, in.expected.asof,
                                     in.asof));
  }
  return co::Ok(std::move(in));
}
void print(std::ostream& report, const ReconcileOutcome& o, const std::vector<Line>& lines) {
  report << "reconcile: " << o.compared << " names, " << o.breaks() << " unexplained breaks"
         << " (missing " << o.missing << ", extra " << o.extra << ", quantity " << o.quantity
         << ", unmapped " << o.unmapped << "), " << o.explained
         << " explained by corporate actions\n";
  usize shown = 0;
  for (const auto& l : lines) {
    if (l.status == Status::Ok || l.status == Status::Explained) continue;
    if (++shown > max_report_lines) {
      report << "reconcile: ... " << o.breaks() - max_report_lines << " more breaks\n";
      return;
    }
    report << "break " << status_label(l.status) << ' '
           << (l.status == Status::Unmapped ? l.key : std::to_string(l.id)) << ": expected "
           << l.adjusted << " (x" << l.ratio << "), broker " << l.broker << '\n';
  }
}
co::Result<ReconcileOutcome> reconcile(const ReconcileConfig& cfg, std::ostream& report) {
  ATX_TRY_VOID(validate(cfg));
  ATX_TRY(const auto in, load(cfg));
  const auto lines = compare(in.expected, in.broker, in.actions, cfg);
  const auto outcome = tally(lines);
  if (!cfg.output_directory.empty()) {
    const std::filesystem::path dir(cfg.output_directory);
    if (!std::filesystem::create_directory(dir))
      return co::Err(co::ErrorCode::AlreadyExists, "reconcile: output must not exist");
    ATX_TRY_VOID(write_lines(dir / "reconciliation.csv", lines));
    ATX_TRY(const auto sha, co::sha256_file((dir / "reconciliation.csv").string()));
    std::ofstream file(dir / "reconcile.json", std::ios::binary);
    file << summary_json(cfg, in, outcome, sha).dump(2) << '\n';
    file.close();
    if (!file) return co::Err(co::ErrorCode::IoError, "reconcile: reconcile.json");
  }
  print(report, outcome, lines);
  return co::Ok(outcome);
}
} // namespace

co::Result<ReconcileOutcome> run_reconcile(const ReconcileConfig& cfg, std::ostream& report) {
  try {
    return reconcile(cfg, report);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "reconcile: allocation failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("reconcile: ") + e.what());
  }
}

int dispatch_reconcile(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    ReconcileConfig cfg;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "reconcile --deploy MANIFEST.json --expected DECIDE_DIR|expected_holdings.csv "
               "--broker POSITIONS.csv --asof YYYY-MM-DD [--identity BRIDGE.csv] "
               "[--corporate-actions ACTIONS.csv] [--tolerance-shares .5] "
               "[--tolerance-fraction 0] [--output NEWDIR]\n"
               "Breaks between the book a decide expected and the broker's positions, after "
               "splits and stock dividends. Exit 0 reconciled, 1 refused, 2 usage, 5 "
               "unexplained breaks.\n";
        return 0;
      }
      if (!seen.insert(key).second || i + 1 >= argc)
        throw std::invalid_argument("duplicate/missing flag: " + key);
      const std::string value = argv[++i];
      const auto real = [&]() {
        f64 x = 0;
        if (!parse_cell(std::string_view{value}, x)) throw std::invalid_argument("invalid " + key);
        return x;
      };
      if (key == "--deploy") cfg.deploy_path = value;
      else if (key == "--expected") cfg.expected_path = value;
      else if (key == "--broker") cfg.broker_path = value;
      else if (key == "--asof") cfg.asof = value;
      else if (key == "--identity") cfg.identity_path = value;
      else if (key == "--corporate-actions") cfg.corporate_actions_path = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--tolerance-shares") cfg.tolerance_shares = real();
      else if (key == "--tolerance-fraction") cfg.tolerance_fraction = real();
      else throw std::invalid_argument("unknown flag: " + key);
    }
    const auto outcome = run_reconcile(cfg, out);
    if (!outcome) { err << outcome.error().to_string() << '\n'; return 1; }
    if (outcome->breaks()) {
      err << "reconcile: " << outcome->breaks() << " unexplained breaks\n";
      return 5;
    }
    return 0;
  } catch (const std::exception& e) { err << "reconcile: " << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
