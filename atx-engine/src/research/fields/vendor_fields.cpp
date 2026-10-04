#include "atx/engine/research/fields/vendor_fields.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/role_rows.hpp"
#include "atx/engine/research/fields/sources/nyse_calendar.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();

constexpr std::array<std::string_view, 6> kVendorFields{"ret_overnight", "ret_intraday",
                                                        "ceq_iss_5y",    "open_adj",
                                                        "high_adj",      "low_adj"};

// ---- spec text: verbatim research_fields_price.py / research_fields_ohlc.py (field_registry.json
// spec_text); the formula fingerprint test pins every copy against the registry's formula_sha256.

const char *const kPriceClock =
    "price-close-lag1-v1: row t reads vendor observations dated on or before session t-1 only"
    " (each known at that session's 22:00 UTC close mark, before the t-1 decision); sessions are"
    " the role's own inside the role and the NYSE rule sessions (nyse-rule-v1) before it; rows"
    " after the role's last session or on or after the seal are never read";
const char *const kLineNote =
    "one price line's values (the vendor securityID), not an issuer total";
const char *const kOhlcClock =
    "ohlc-same-session-v1: row t reads the vendor TickerHistory3 row of session t (its open, high"
    " and low, delivered in the end-of-day row that also carries the role's close: known at"
    " session t's 22:00 UTC close mark, TH_CLOCK) and the role's close.f64, raw_close.f64 and"
    " present.u8 of row t; nothing dated after session t; rows on or after the seal are never read";

[[nodiscard]] std::vector<std::string> open_caveats() {
  return {"the open is the vendor's (TickerHistory3 open), not an auction print; the research"
          " projection never carried it",
          "a factor step dated t-1 (dividend, split) is inside the overnight part, as the vendor"
          " dates it",
          kLineNote};
}

[[nodiscard]] std::vector<std::string> open_columns() {
  return {"open", "close", "cumulReturnFactor", "volume", "tradingDate", "securityID"};
}

[[nodiscard]] FieldSpec make_ret_overnight_spec() {
  FieldSpec s;
  s.name = "ret_overnight";
  s.group = "price_open";
  s.formula_id = "price-ret-overnight-lag1-v1";
  s.min_history = "2 extended sessions (row 0 reads the two sessions before the role)";
  FieldDefinition &d = s.definition;
  d.units = "simple return, decimal: adjusted open over the previous adjusted close, minus 1";
  d.clock = kPriceClock;
  d.staleness = "no fill: an absent observation, open or guard failure -> NaN";
  d.source_columns = open_columns();
  d.definition =
      "row t: a = session t-1, b = session t-2 (extended axis); O_a F_a / (C_b F_b) - 1, C ="
      " vendor close, O = vendor open, F = chained factor (SOURCE_RULE); needs observations at a"
      " and b, a finite positive open at a, no kept_gap step in (b, a], and the house guard on"
      " the adjusted log ratio (|x| <= 1.5 and |x| <= |log(O_a / C_b)| + 0.10)";
  d.point_in_time = true;
  s.caveats = open_caveats();
  return s;
}

[[nodiscard]] FieldSpec make_ret_intraday_spec() {
  FieldSpec s;
  s.name = "ret_intraday";
  s.group = "price_open";
  s.formula_id = "price-ret-intraday-lag1-v1";
  s.min_history = "1 extended session";
  FieldDefinition &d = s.definition;
  d.units = "simple return, decimal: close over open of the same session, minus 1";
  d.clock = kPriceClock;
  d.staleness = "no fill: an absent observation or open, or a guard failure -> NaN";
  d.source_columns = open_columns();
  d.definition =
      "row t: a = session t-1; C_a / O_a - 1 (the factor cancels within a session); needs an"
      " observation at a with a finite positive open, and |log(C_a / O_a)| <= 1.5";
  d.point_in_time = true;
  s.caveats = open_caveats();
  return s;
}

[[nodiscard]] FieldSpec make_ceq_spec() {
  FieldSpec s;
  s.name = "ceq_iss_5y";
  s.group = "price_ceq";
  s.formula_id = "price-ceq-iss-5y-lag1-v1";
  s.min_history = "1,261 extended sessions plus the 490-day share lookback before the role start";
  FieldDefinition &d = s.definition;
  d.units = "log ratio: 5-year composite equity issuance (Daniel-Titman 2006), ln(ME_a / ME_b) -"
            " ln(P_a / P_b)";
  d.clock = kPriceClock;
  d.staleness = "no fill: any missing term -> NaN; outside [-ln 100, ln 100] -> NaN, counted"
                " (outside_domain_member_cells)";
  d.source_columns = {"shares", "cumulReturnFactor", "close", "volume", "tradingDate",
                      "securityID"};
  d.definition =
      "row t: a = session t-1, b = a - 1260 sessions (extended axis). ME_s = 1000 x shares(o_s)"
      " x F_s / F(o_s) x C_s: the A8 house market equity, o_s = the line's last share"
      " observation dated <= date(s) - 90 days and >= date(s) - 490 days (vendor shares in"
      " thousands, 0 < shares <= the A9 ceiling). The price terms cancel exactly, so the value is"
      " ln(q(o_a) / q(o_b)), q = shares / F: share growth net of splits (they move shares and F"
      " together) and of dividends (a dividend raises F: paid-out cash counts as negative"
      " issuance, as in Daniel-Titman). Needs price observations at a and b, both share"
      " observations, no kept_gap step in (o_b, a], the line not withheld under C-81 at a (its"
      " first above-ceiling row dated <= date(a)), and a value inside the declared domain";
  d.point_in_time = true;
  d.domain = std::array<f64, 2>{-kCeqDomainHigh, kCeqDomainHigh};
  s.caveats = {"vendor share runs start at the filing cover date; the 90-day lag (A8) keeps each"
               " share count point in time",
               "one price line's share count (an ADR line counts ADS), not the issuer total"
               " across share classes",
               "genuine splits missing from the vendor factor are not restated (as shares_out)"};
  return s;
}

[[nodiscard]] FieldSpec make_bar_spec(const char *name, const std::string &column,
                                      const char *formula) {
  FieldSpec s;
  s.name = name;
  s.group = "ohlc_bar";
  s.formula_id = formula;
  s.min_history = "0 sessions (row t reads session t only)";
  FieldDefinition &d = s.definition;
  d.units = "price on the role close's adjusted basis: the session's vendor " + column +
            " x close / raw close";
  d.clock = kOhlcClock;
  d.staleness = "same-session vendor bar only; no fill: an absent, duplicated, invalid or"
                " order-violating bar, or an absent role close -> NaN";
  d.source_columns = {"tradingDate", "securityID", "open",      "high",
                      "low",         "close.f64",  "raw_close.f64", "present.u8"};
  d.definition =
      "ohlc-bar-v1: x_adj[t] = x[t] * close.f64[t] / raw_close.f64[t], x = the vendor " + column +
      " of (session t, line) held as float32 (the vendor column precision, as the role's close)"
      " and widened to f64; NaN unless present.u8[t] = 1 with finite positive close and raw"
      " close, exactly one vendor row has that (tradingDate, securityID) key (duplicates"
      " quarantined), open, high and low are finite and positive, and low <= min(open, raw"
      " close) and high >= max(open, raw close) (a violating bar is withheld whole)";
  d.point_in_time = true;
  s.caveats = {"the vendor's open, high and low (TickerHistory3), not exchange auction or"
               " consolidated prints",
               "same modeled clock as the role's close (vendor end-of-day row at the 22:00 UTC"
               " mark); the vendor's real delivery time and history vintage are the open caveats"
               " of every same-date vendor value, close included",
               "the role's adjustment (vendor cumulReturnFactor, unrepaired, as close.f64): any"
               " vendor factor artefact in close (the 2021-01-04 re-anchoring) is carried"
               " identically, so cross-session ratios equal close's",
               kLineNote};
  return s;
}

struct Specs {
  FieldSpec ret_overnight = make_ret_overnight_spec();
  FieldSpec ret_intraday = make_ret_intraday_spec();
  FieldSpec ceq = make_ceq_spec();
  FieldSpec open_adj = make_bar_spec("open_adj", "open", "ohlc-open-adj-v1");
  FieldSpec high_adj = make_bar_spec("high_adj", "high", "ohlc-high-adj-v1");
  FieldSpec low_adj = make_bar_spec("low_adj", "low", "ohlc-low-adj-v1");
};

[[nodiscard]] const Specs &specs() {
  static const Specs s;
  return s;
}

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "vendor field: " + std::move(message));
}

// The panel serves this role: its axis ends with the role's sessions, its lines are the role's.
[[nodiscard]] core::Status require_role_panel(const VendorPanel &p, const RoleAxes &role) {
  const auto days = p.days();
  const auto role_days = role.days();
  const bool same = p.lines() == role.instruments() && p.rows() == p.prefix() + role.dates() &&
                    std::equal(role_days.begin(), role_days.end(),
                               days.begin() + static_cast<isize>(p.prefix()));
  if (!same) {
    return core::Err(refused("the vendor panel was not loaded for this role"));
  }
  return core::Ok();
}

[[nodiscard]] core::Result<Json> session_calendar_json() {
  ATX_TRY(const auto pin, session_calendar_pin());
  return core::Ok(Json{{"rule", pin.rule},
                       {"first", pin.first},
                       {"last", pin.last},
                       {"sessions", pin.sessions},
                       {"sha256", pin.sha256}});
}

[[nodiscard]] Json scan_json(const VendorPanel &p) {
  const VendorScanStats &s = p.stats();
  Json out{{"rows_in_file", s.rows_in_file},
           {"row_groups", s.row_groups},
           {"row_groups_pruned_sealed", s.row_groups_pruned_sealed},
           {"rows_in_row_groups_pruned_sealed", s.rows_in_row_groups_pruned_sealed},
           {"row_groups_pruned_outside_window", s.row_groups_pruned_outside_window},
           {"rows_in_row_groups_pruned_outside_window", s.rows_in_row_groups_pruned_outside_window},
           {"row_groups_keys_decoded", s.row_groups_keys_decoded},
           {"row_groups_values_decoded", s.row_groups_values_decoded},
           {"rows_keys_decoded", s.rows_keys_decoded},
           {"rows_sealed_dropped", s.rows_sealed_dropped},
           {"rows_sealed_value_decoded", s.rows_sealed_value_decoded},
           {"rows_selected", s.rows_selected},
           {"rows_off_calendar", s.rows_off_calendar},
           {"duplicate_keys_quarantined", s.duplicate_keys_quarantined},
           {"extended_sessions", p.rows()},
           {"sessions_before_role", p.prefix()},
           {"first_session", iso_date(p.days().front())},
           {"calendar", "nyse-rule-v1 before the role, the role's sessions inside it"},
           {"source", Json{{"path", p.source().path},
                           {"bytes", p.source().bytes},
                           {"sha256", p.source().sha256}}}};
  if (p.request().shares) {
    out["shares_rows_above_a9_ceiling"] = s.shares_rows_above_a9_ceiling;
    out["shares_lines_withheld_c81"] = s.shares_lines_withheld_c81;
  }
  if (p.request().price) {
    Json mass = Json::array();
    for (const MassSession &m : p.factor_breaks().mass) {
      mass.push_back(iso_date(p.days()[m.row]));
    }
    out["factor_break"] = Json{{"rule", std::string(kFactorBreakRule)},
                               {"mass_sessions", std::move(mass)},
                               {"repaired_steps", p.repaired_steps()},
                               {"kept_gap_steps", p.kept_gap_steps()}};
  }
  return out;
}

[[nodiscard]] Json checks_json(const VendorPanel &p, const char *clock, usize lag) {
  return Json{{"lag_sessions", lag},
              {"clock", clock},
              {"seal", std::string(data::kSealBeginDate)},
              {"window", std::string(data::kResearchWindowId)},
              {"source", scan_json(p)}};
}

[[nodiscard]] core::Result<VendorField> build_open_return(OpenReturn which, const FieldSpec &spec,
                                                          const VendorPanel &p,
                                                          const RoleAxes &role,
                                                          const std::filesystem::path &dir) {
  ATX_TRY_VOID(require_role_panel(p, role));
  if (!p.request().price || !p.request().price_open || p.prefix() < kOpenReturnSessions) {
    return core::Err(refused(spec.name + " needs the panel's open and two sessions before role"));
  }
  ATX_TRY(auto writer, FieldWriter::create(dir, spec.name, role));
  std::vector<f64> row(role.instruments());
  u64 guarded = 0;
  for (usize t = 0; t < role.dates(); ++t) {
    const usize a = p.prefix() + t - kPriceLagSessions; // row t reads session t-1 (and t-2)
    guarded += open_return_row(which, p, a, role.member_row(t), row);
    ATX_TRY_VOID(writer.write(row));
  }
  ATX_TRY(auto written, writer.close());
  ATX_TRY(auto calendar, session_calendar_json());
  VendorField out;
  out.field = std::move(written);
  out.sources = {p.source()};
  out.extra = Json{{"lag_sessions", kPriceLagSessions},
                   {"guarded_member_cells", guarded},
                   {"session_calendar", std::move(calendar)}};
  out.source_checks = checks_json(p, kPriceClock, kPriceLagSessions);
  return core::Ok(std::move(out));
}

[[nodiscard]] core::Result<VendorField> build_ceq(const VendorPanel &p, const RoleAxes &role,
                                                  const std::filesystem::path &dir) {
  ATX_TRY_VOID(require_role_panel(p, role));
  const FieldSpec &spec = specs().ceq;
  if (!p.request().price || !p.request().shares || p.prefix() < kPriceLagSessions) {
    return core::Err(refused("ceq_iss_5y needs the panel's shares"));
  }
  const ShareIndex shares(p);
  ATX_TRY(auto writer, FieldWriter::create(dir, spec.name, role));
  std::vector<f64> row(role.instruments());
  u64 outside = 0;
  for (usize t = 0; t < role.dates(); ++t) {
    const usize a = p.prefix() + t - kPriceLagSessions;
    outside += ceq_issuance_row(p, shares, a, role.member_row(t), row);
    ATX_TRY_VOID(writer.write(row));
  }
  ATX_TRY(auto written, writer.close());
  ATX_TRY(auto calendar, session_calendar_json());
  VendorField out;
  out.field = std::move(written);
  out.sources = {p.source()};
  out.extra = Json{{"lag_sessions", kPriceLagSessions},
                   {"domain", Json::array({-kCeqDomainHigh, kCeqDomainHigh})},
                   {"outside_domain_member_cells", outside},
                   {"session_calendar", std::move(calendar)}};
  out.source_checks = checks_json(p, kPriceClock, kPriceLagSessions);
  return core::Ok(std::move(out));
}

[[nodiscard]] core::Result<VendorField> build_bar(Bar which, const FieldSpec &spec,
                                                  const VendorPanel &p, const RoleAxes &role,
                                                  const std::filesystem::path &dir) {
  ATX_TRY_VOID(require_role_panel(p, role));
  if (!p.request().bars) {
    return core::Err(refused(spec.name + " needs the panel's bars"));
  }
  ATX_TRY(auto close_rows, RoleRowReader::open(role, "close.f64", RowKind::F64));
  ATX_TRY(auto raw_rows, RoleRowReader::open(role, "raw_close.f64", RowKind::F64));
  ATX_TRY(auto present_rows, RoleRowReader::open(role, "present.u8", RowKind::U8));
  ATX_TRY(auto writer, FieldWriter::create(dir, spec.name, role));
  const usize n = role.instruments();
  std::vector<f64> row(n);
  std::vector<f64> close_row(n);
  std::vector<f64> raw_row(n);
  std::vector<u8> present_row(n);
  BarCounts counts;
  for (usize t = 0; t < role.dates(); ++t) {
    ATX_TRY_VOID(close_rows.next(std::span<f64>(close_row)));
    ATX_TRY_VOID(raw_rows.next(std::span<f64>(raw_row)));
    ATX_TRY_VOID(present_rows.next(std::span<u8>(present_row)));
    const usize bar_row = p.prefix() + t - kOhlcLagSessions; // the bar of session t itself
    ohlc_bar_row(which, p, bar_row, close_row, raw_row, present_row, role.member_row(t), counts,
                 row);
    ATX_TRY_VOID(writer.write(row));
  }
  ATX_TRY(auto close_source, close_rows.finish());
  ATX_TRY(auto raw_source, raw_rows.finish());
  ATX_TRY(auto present_source, present_rows.finish());
  ATX_TRY(auto written, writer.close());
  VendorField out;
  out.field = std::move(written);
  out.sources = {p.source(), std::move(close_source), std::move(raw_source),
                 std::move(present_source)};
  out.extra = Json{{"lag_sessions", kOhlcLagSessions},
                   {"order_violation_member_cells", counts.order_violation_member_cells},
                   {"missing_bar_member_cells", counts.missing_bar_member_cells}};
  out.source_checks = checks_json(p, kOhlcClock, kOhlcLagSessions);
  return core::Ok(std::move(out));
}

[[nodiscard]] f64 widened(f32 value) noexcept { return static_cast<f64>(value); }

} // namespace

std::span<const std::string_view> vendor_field_names() noexcept { return kVendorFields; }

const FieldSpec *vendor_field_spec(std::string_view name) {
  const Specs &s = specs();
  for (const FieldSpec *spec :
       {&s.ret_overnight, &s.ret_intraday, &s.ceq, &s.open_adj, &s.high_adj, &s.low_adj}) {
    if (spec->name == name) {
      return spec;
    }
  }
  return nullptr;
}

std::optional<VendorPanelRequest> vendor_request(std::string_view name) {
  VendorPanelRequest r;
  if (name == "ret_overnight" || name == "ret_intraday") {
    r.pre_sessions = kOpenReturnSessions;
    r.price = true;
    r.price_open = true;
    return r;
  }
  if (name == "ceq_iss_5y") {
    r.pre_sessions = kPriceHistorySessions;
    r.lookback_days = kSharesLagDays + kSharesMaxAgeDays;
    r.price = true;
    r.shares = true;
    return r;
  }
  if (name == "open_adj" || name == "high_adj" || name == "low_adj") {
    r.bars = true;
    return r;
  }
  return std::nullopt;
}

bool reads_role_close(std::string_view name) noexcept {
  return name == "open_adj" || name == "high_adj" || name == "low_adj";
}

core::Result<VendorField> build_vendor_field(std::string_view name, const VendorPanel &panel,
                                             const RoleAxes &role,
                                             const std::filesystem::path &output_dir) {
  const Specs &s = specs();
  if (name == s.ret_overnight.name) {
    return build_open_return(OpenReturn::Overnight, s.ret_overnight, panel, role, output_dir);
  }
  if (name == s.ret_intraday.name) {
    return build_open_return(OpenReturn::Intraday, s.ret_intraday, panel, role, output_dir);
  }
  if (name == s.ceq.name) {
    return build_ceq(panel, role, output_dir);
  }
  if (name == s.open_adj.name) {
    return build_bar(Bar::Open, s.open_adj, panel, role, output_dir);
  }
  if (name == s.high_adj.name) {
    return build_bar(Bar::High, s.high_adj, panel, role, output_dir);
  }
  if (name == s.low_adj.name) {
    return build_bar(Bar::Low, s.low_adj, panel, role, output_dir);
  }
  return core::Err(refused("no vendor-panel field " + std::string(name)));
}

u64 open_return_row(OpenReturn which, const VendorPanel &p, usize a, std::span<const u8> member,
                    std::span<f64> out) {
  assert(a >= 1 && a < p.rows() && member.size() == p.lines() && out.size() == p.lines());
  u64 guarded = 0;
  for (usize j = 0; j < p.lines(); ++j) {
    out[j] = kNan;
    const f64 o = widened(p.price_open(a, j));
    const f64 fa = p.factor(a, j);
    bool kept = false;
    f64 value = kNan;
    if (which == OpenReturn::Overnight) {
      const f64 cb = widened(p.close(a - 1, j));
      const f64 fb = p.factor(a - 1, j);
      const bool ok = std::isfinite(o) && std::isfinite(fa) && std::isfinite(cb) &&
                      std::isfinite(fb) && !p.gap_crosses(j, a - 1, a);
      if (!ok) {
        continue;
      }
      const f64 num = o * fa;
      const f64 den = cb * fb;
      const f64 adjusted = std::log(num) - std::log(den);
      const f64 raw = std::log(o) - std::log(cb);
      kept = std::fabs(adjusted) <= kGuardLog &&
             std::fabs(adjusted) <= std::fabs(raw) + kGuardExcess;
      const f64 ratio = num / den;
      value = ratio - 1.0;
    } else {
      const f64 ca = widened(p.close(a, j));
      if (!(std::isfinite(o) && std::isfinite(ca) && std::isfinite(fa))) {
        continue;
      }
      const f64 log_ratio = std::log(ca) - std::log(o);
      kept = std::fabs(log_ratio) <= kGuardLog;
      const f64 ratio = ca / o;
      value = ratio - 1.0;
    }
    if (!kept) {
      guarded += member[j] != 0 ? 1U : 0U;
      continue;
    }
    out[j] = value;
  }
  return guarded;
}

ShareIndex::ShareIndex(const VendorPanel &panel) : panel_(&panel), rows_(panel.lines()) {
  assert(panel.rows() <= std::numeric_limits<u32>::max());
  for (usize e = 0; e < panel.rows(); ++e) {
    for (usize j = 0; j < panel.lines(); ++j) {
      if (std::isfinite(panel.shares(e, j))) {
        rows_[j].push_back(static_cast<u32>(e));
      }
    }
  }
}

std::optional<usize> ShareIndex::lagged(usize line, usize s) const noexcept {
  const auto days = panel_->days();
  const i64 day = days[s];
  // c: the last axis row dated <= day(s) - 90 (Python searchsorted(side="right") - 1).
  const auto cut = std::upper_bound(days.begin(), days.end(), day - kSharesLagDays);
  if (cut == days.begin()) {
    return std::nullopt;
  }
  const auto c = static_cast<u32>(cut - days.begin() - 1);
  const auto &rows = rows_[line];
  const auto it = std::upper_bound(rows.begin(), rows.end(), c); // the last observation <= c
  if (it == rows.begin()) {
    return std::nullopt;
  }
  const usize o = *(it - 1);
  if (days[o] < day - (kSharesLagDays + kSharesMaxAgeDays)) {
    return std::nullopt; // older than the A8 maximum age
  }
  return o;
}

u64 ceq_issuance_row(const VendorPanel &p, const ShareIndex &shares, usize a,
                     std::span<const u8> member, std::span<f64> out) {
  assert(a < p.rows() && member.size() == p.lines() && out.size() == p.lines());
  std::fill(out.begin(), out.end(), kNan);
  if (a < kCeqSessions) {
    return 0;
  }
  const usize b = a - kCeqSessions;
  const i64 day_a = p.days()[a];
  u64 outside = 0;
  for (usize j = 0; j < p.lines(); ++j) {
    const auto oa = shares.lagged(j, a);
    const auto ob = shares.lagged(j, b);
    if (!oa || !ob) {
      continue;
    }
    const bool priced = std::isfinite(p.close(a, j)) && std::isfinite(p.factor(a, j)) &&
                        std::isfinite(p.close(b, j)) && std::isfinite(p.factor(b, j));
    // C-81, point in time: withheld once an above-ceiling row dated <= date(a) is known.
    if (!priced || !(p.first_above(j) > day_a) || p.gap_crosses(j, *ob, a)) {
      continue;
    }
    const f64 shares_a = widened(p.shares(*oa, j));
    const f64 shares_b = widened(p.shares(*ob, j));
    const f64 head = std::log(shares_a) - std::log(p.factor(*oa, j));
    const f64 mid = head - std::log(shares_b);
    const f64 v = mid + std::log(p.factor(*ob, j));
    if (!std::isfinite(v)) {
      continue;
    }
    if (!(v >= -kCeqDomainHigh && v <= kCeqDomainHigh)) {
      outside += member[j] != 0 ? 1U : 0U;
      continue;
    }
    out[j] = v;
  }
  return outside;
}

void ohlc_bar_row(Bar which, const VendorPanel &p, std::optional<usize> bar_row,
                  std::span<const f64> close, std::span<const f64> raw_close,
                  std::span<const u8> present, std::span<const u8> member, BarCounts &counts,
                  std::span<f64> out) {
  assert(close.size() == p.lines() && raw_close.size() == p.lines() &&
         present.size() == p.lines() && member.size() == p.lines() && out.size() == p.lines());
  for (usize j = 0; j < p.lines(); ++j) {
    out[j] = kNan;
    const f64 c = close[j];
    const f64 rc = raw_close[j];
    const bool ok = present[j] != 0 && std::isfinite(c) && c > 0.0 && std::isfinite(rc) && rc > 0.0;
    const f64 o = bar_row ? widened(p.bar_open(*bar_row, j)) : kNan;
    const f64 hi = bar_row ? widened(p.bar_high(*bar_row, j)) : kNan;
    const f64 lo = bar_row ? widened(p.bar_low(*bar_row, j)) : kNan;
    const bool bar = ok && std::isfinite(o) && std::isfinite(hi) && std::isfinite(lo) && o > 0.0 &&
                     hi > 0.0 && lo > 0.0;
    const bool order = bar && lo <= std::min(o, rc) && hi >= std::max(o, rc);
    const bool counted = member[j] != 0;
    if (bar && !order && counted) {
      ++counts.order_violation_member_cells; // a bar that contradicts the role's close
    }
    if (ok && !bar && counted) {
      ++counts.missing_bar_member_cells;
    }
    if (!order) {
      continue;
    }
    const f64 k = c / rc;
    const f64 x = which == Bar::Open ? o : (which == Bar::High ? hi : lo);
    out[j] = x * k;
  }
}

} // namespace atx::engine::research::fields
