// risk-target-v1 (platform v8 R-8): the NAV replay's scaler over the engine kernel
// (engine::book::risk_target_update), its series and its published blocks. The rule is stated in
// strategy_risk_target.hpp; each replay book plans through its own BookLeverage (P9 C1:
// strategy_nav_replay.cpp), the v7 seam through Scaler (strategy_nav_v7.cpp).
#include "strategy_risk_target.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/engine/book/two_speed.hpp"
#include "strategy_vol_target.hpp"

namespace atx::impl::strategy::risk_target {
namespace {
using namespace atx;
namespace co = atx::core;
namespace eb = atx::engine::book;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();

// A name book_variance prices: an industry slot and a finite positive specific variance.
bool priced(const spo::RiskSlice& r, usize i) {
  return r.slot[i] < spo::risk_industry_slots && std::isfinite(r.specific[i]) &&
         r.specific[i] > 0;
}
std::string number(f64 x) { // CSV: 17 significant digits, classic locale, "nan"
  if (std::isnan(x)) return "nan";
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << x;
  return out.str();
}
std::string decimal(f64 x) { // shortest round-trip spelling (the rule id, the declaration)
  std::array<char, 64> text{};
  const auto written = std::to_chars(text.data(), text.data() + text.size(), x);
  return std::string(text.data(), written.ptr);
}
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
usize one_if(bool b) { return b ? 1U : 0U; }

// n, mean, min and max of the finite values added.
struct Stats {
  f64 sum{}, lo{inf}, hi{-inf};
  usize n{};
  void add(f64 v) {
    if (!std::isfinite(v)) return;
    sum += v; lo = std::min(lo, v); hi = std::max(hi, v); ++n;
  }
  [[nodiscard]] Json json() const {
    return Json{{"n", n}, {"mean", finite_or_null(n ? sum / static_cast<f64>(n) : nan)},
                {"min", finite_or_null(n ? lo : nan)}, {"max", finite_or_null(n ? hi : nan)}};
  }
};
struct BookStats {
  usize decisions{}, estimates{}, before_first{};
  usize at_lo{}, at_hi{}, estimates_at_lo{}, estimates_at_hi{};
  f64 base{nan};
  Stats leverage, multiplier, sigma_hat, sigma_ref;
};
bool vol_law(const Options& o) { return o.law == Law::vol_target_v1; }
} // namespace

NavLeverageRule leverage_rule(const Options& o, std::shared_ptr<const spo::RiskStore> risk) {
  NavLeverageRule rule;
  if (!o.on) return rule;
  rule.law = vol_law(o) ? NavLeverageLaw::VolTargetV1 : NavLeverageLaw::RiskTargetV1;
  rule.params = o.params;
  rule.risk = std::move(risk);
  return rule;
}

Options options_of(const NavLeverageRule& rule) noexcept {
  Options o;
  if (rule.law == NavLeverageLaw::Fixed) return o;
  o.on = true;
  o.params = rule.params;
  o.law = rule.law == NavLeverageLaw::VolTargetV1 ? Law::vol_target_v1 : Law::risk_target_v1;
  return o;
}

BookScaler::BookScaler(const Options& o, std::shared_ptr<const spo::RiskStore> risk)
    : options_(o), risk_(std::move(risk)) {}

co::Result<f64> BookScaler::leverage(const TargetReplayInput& x, usize d, bool rebalance,
                                     std::string_view book, f64 base,
                                     std::span<const f64> current, std::vector<Record>* records) {
  if (!risk_)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "risk-target-v1: no risk model (--risk-model)");
  if (d >= x.dates || current.size() != x.instruments || !std::isfinite(base) || !(base > 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "risk-target-v1: decision geometry or a leverage that is not finite and > 0");
  if (!axes_checked_) {
    ATX_TRY_VOID(risk_->check_axes(x.session_keys, x.instruments));
    axes_checked_ = true;
  }
  bool updated = false;
  const bool vol = vol_law(options_); // vol-target-v1 (v8 Y): its own state, the same forecast
  const bool forecast = d < risk_->dates() && risk_->forecast()[d] != 0;
  const bool due = vol ? eb::vol_target_due(vol_, d)
                       : eb::risk_target_due(state_, d, options_.params.cadence);
  if (forecast && due) {
    ATX_TRY(updated, estimate(d, base, current));
  }
  const f64 in_force =
      vol ? eb::vol_target_in_force(vol_, base) : eb::risk_target_in_force(state_, base);
  if (records) {
    const eb::RiskTargetLeverage& at = vol ? vol_.at : state_.at;
    Record r;
    r.session = x.session_keys[d];
    r.book = std::string(book);
    r.rebalance = rebalance;
    r.updated = updated;
    r.base = base;
    r.gross = gross_;
    r.priced_share = priced_share_;
    r.sigma_hat = vol ? vol_.sigma_hat : state_.sigma_hat;
    r.raw = at.raw;
    r.leverage = in_force;
    r.clip = at.clip;
    if (vol) r.sigma_ref = vol_.sigma_ref;
    records->push_back(std::move(r));
  }
  return co::Ok(in_force);
}

// The book's priced names at d, in instrument order (their weights, industry columns 1 + slot,
// style rows, specific variances), the whole book's gross, then the engine's update.
co::Result<bool> BookScaler::estimate(usize d, f64 base, std::span<const f64> current) {
  ATX_TRY_VOID(risk_->read(d, slice_));
  group_.clear(); exposures_.clear(); specific_.clear(); weights_.clear();
  f64 gross = 0, priced_gross = 0;
  for (usize i = 0; i < current.size(); ++i) {
    const f64 w = current[i];
    if (w == 0) continue;
    gross += std::abs(w);
    if (!priced(slice_, i)) continue;
    priced_gross += std::abs(w);
    group_.push_back(1U + static_cast<u32>(slice_.slot[i]));
    const auto row = slice_.styles.begin() + static_cast<std::ptrdiff_t>(i * spo::risk_styles);
    exposures_.insert(exposures_.end(), row,
                      row + static_cast<std::ptrdiff_t>(spo::risk_styles));
    specific_.push_back(slice_.specific[i]);
    weights_.push_back(w);
  }
  const eb::FactorRiskView model{spo::risk_industry_slots, spo::risk_styles, group_, exposures_,
                                 slice_.covariance, specific_};
  // vol-target-v1 (v8 Y): the same model and book, its own law and state (base = the cap L).
  auto update = vol_law(options_)
                    ? eb::vol_target_update(base, d, model, weights_, gross, vol_)
                    : eb::risk_target_update(options_.params, base, d, model, weights_, gross,
                                             state_);
  ATX_TRY(const bool updated, std::move(update));
  if (updated) {
    gross_ = gross;
    priced_share_ = priced_gross / gross;
  }
  return co::Ok(updated);
}

BookLeverage::BookLeverage(const Options& o, std::shared_ptr<const spo::RiskStore> risk)
    : scaler_(o, std::move(risk)) {}

co::Result<const std::vector<f64>*> BookLeverage::plan(
    const TargetReplayInput& x, const NavReplayConfig& cfg, usize d, bool rebalance,
    std::string_view book, std::span<const f64> current, const std::vector<f64>& desired,
    std::span<const f64> two_speed_fast, std::vector<Record>* records) {
  // v8 Y-5 two-speed-v1 (Ruling PM8-16 #10): the book's fast holding follows its scale lambda =
  // L_t / L. Its rebalance plans at L_t the netted desired target plus the carry of F from the
  // book's lambda at its previous two-speed rebalance (its first: lambda itself, no carry), so
  // the remainder moves at theta_s toward lambda L m_s d_s and the fast part becomes lambda F_next.
  // Checked before the scaler moves.
  const bool carry = two_speed_on(cfg.target) && rebalance;
  const usize n = x.instruments;
  if (carry && (two_speed_fast.size() != n || desired.size() != n || d >= x.dates))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav v7: two-speed-v1 under --risk-target / --vol-target needs the fast "
                   "sleeve F entering the rebalance");
  const f64 base = cfg.target.aim_leverage;
  ATX_TRY(const f64 leverage,
          scaler_.leverage(x, d, rebalance, book, base, current, records));
  scaled_ = cfg;
  scaled_.target.aim_leverage = leverage;
  if (!carry) return co::Ok(&desired);
  const f64 lambda = leverage / base;
  if (std::isnan(lambda_)) lambda_ = lambda; // the book's first two-speed rebalance: no carry
  carried_.assign(desired.begin(), desired.end());
  ATX_TRY_VOID(eb::two_speed_carry(x.member.subspan(d * n, n), two_speed_fast, lambda_, lambda,
                                   base, cfg.target.trade_fraction, carried_));
  lambda_ = lambda;
  return co::Ok(static_cast<const std::vector<f64>*>(&carried_));
}

Scaler::Scaler(const Options& o, std::shared_ptr<const spo::RiskStore> risk)
    : options_(o), risk_(std::move(risk)) {}

void Scaler::begin_run() { books_.clear(); }

BookLeverage& Scaler::book(std::string_view label) {
  auto found = books_.find(label);
  if (found == books_.end())
    found = books_.try_emplace(std::string(label), options_, risk_).first;
  return found->second;
}

co::Result<f64> Scaler::leverage(const TargetReplayInput& x, usize d, bool rebalance,
                                 std::string_view book_label, f64 base,
                                 std::span<const f64> current, bool record) {
  return book(book_label).scaler().leverage(x, d, rebalance, book_label, base, current,
                                            record ? &records_ : nullptr);
}

void Scaler::append(std::span<const Record> records) {
  records_.insert(records_.end(), records.begin(), records.end());
}

std::string records_csv(std::span<const Record> records, Law law) {
  // vol-target-v1 (v8 Y) adds sigma_ref after sigma_hat; risk-target-v1's bytes are unchanged.
  const bool vol = law == Law::vol_target_v1;
  std::string text = vol ? "session,book,rebalance,updated,gross,priced_share,sigma_hat,sigma_ref,"
                           "raw,L_t,multiplier,clip\n"
                         : "session,book,rebalance,updated,gross,priced_share,sigma_hat,raw,L_t,"
                           "multiplier,clip\n";
  for (const auto& r : records)
    text += std::to_string(r.session) + ',' + r.book + ',' + (r.rebalance ? "1" : "0") + ',' +
            (r.updated ? "1" : "0") + ',' + number(r.gross) + ',' + number(r.priced_share) + ',' +
            number(r.sigma_hat) + ',' + (vol ? number(r.sigma_ref) + ',' : std::string()) +
            number(r.raw) + ',' + number(r.leverage) + ',' + number(r.leverage / r.base) + ',' +
            std::to_string(static_cast<int>(r.clip)) + '\n';
  return text;
}

std::string declaration(const Options& o) {
  if (vol_law(o)) return vol_target::declaration();
  const auto& p = o.params;
  return "risk-target-v1 (platform v8 R-8, Ruling E-40): at every decision d each book's aim "
         "leverage is L_t = clip(S / (b sigma_hat_t), .8 L, 1.25 L) with S = " +
         decimal(p.sigma_star) + " (--risk-target), b = " + decimal(p.bias) +
         " (--risk-target-bias, registered 1.15) and L = --aim-leverage; sigma_hat_t = sqrt(252 "
         "x book_variance) of the book's current weights (its DECIDE's, after the session's "
         "fills) scaled to gross 1 over the whole book, on the atx-risk-v1 row d of the pinned "
         "store (--risk-model; the names with a risk row, no specific ceiling), estimated at the "
         "book's first decision with a forecast, a book that is not flat and a positive "
         "variance, then at the first such decision at least " + std::to_string(p.cadence) +
         " sessions (--risk-target-cadence, registered 21) after the previous estimate, held "
         "between estimates, L before the first. L_t replaces --aim-leverage in the book's rule: "
         "aim-partial-v5 moves toward L_t x desired; spo-v3 tracks L_t x desired with its gross "
         "sanity bound 2 x L and its gamma calibrated on L x desired (the parent's). The shared "
         "desired target and its ADV cap (read at L) are unchanged; every book runs on its own "
         "state, the warm-up included, each pass from a clean state";
}

Json parameters_json(const Options& o) {
  if (vol_law(o)) return vol_target::parameters_json();
  return Json{{"rule", "risk-target-v1"},
              {"declaration", declaration(o)},
              {"sigma_star", o.params.sigma_star},
              {"bias", o.params.bias},
              {"cadence_sessions", o.params.cadence},
              {"clip", Json::array({eb::risk_target_clip_lo, eb::risk_target_clip_hi})},
              {"periods_per_year", eb::risk_target_periods_per_year},
              {"series", "risk_target.csv: one row per scored decision and book of the main "
                         "pass (sigma_hat and L_t in force)"}};
}

Json summary_json(std::span<const Record> records, Law law) {
  std::map<std::string, BookStats> books;
  for (const auto& r : records) {
    auto& b = books[r.book];
    ++b.decisions;
    b.base = r.base;
    b.leverage.add(r.leverage);
    b.multiplier.add(r.leverage / r.base);
    b.before_first += one_if(std::isnan(r.sigma_hat));
    b.at_lo += one_if(r.clip == eb::RiskTargetClip::Low);
    b.at_hi += one_if(r.clip == eb::RiskTargetClip::High);
    if (!r.updated) continue;
    ++b.estimates;
    b.sigma_hat.add(r.sigma_hat);
    b.sigma_ref.add(r.sigma_ref);
    b.estimates_at_lo += one_if(r.clip == eb::RiskTargetClip::Low);
    b.estimates_at_hi += one_if(r.clip == eb::RiskTargetClip::High);
  }
  Json out = Json::object();
  for (const auto& [book, b] : books) {
    out[book] = Json{{"decisions", b.decisions},
                     {"estimates", b.estimates},
                     {"base_leverage", finite_or_null(b.base)},
                     {"L_t", b.leverage.json()},
                     {"multiplier", b.multiplier.json()},
                     {"decisions_before_first_estimate", b.before_first},
                     {"decisions_at_clip_lo", b.at_lo},
                     {"decisions_at_clip_hi", b.at_hi},
                     {"estimates_at_clip_lo", b.estimates_at_lo},
                     {"estimates_at_clip_hi", b.estimates_at_hi},
                     {"sigma_hat", b.sigma_hat.json()}};
    // vol-target-v1 (v8 Y): clip lo is the floor 1, clip hi the cap L; the running mean too.
    if (law == Law::vol_target_v1) out[book]["sigma_ref"] = b.sigma_ref.json();
  }
  return out;
}

const char* block_key(const Options& o) noexcept {
  return vol_law(o) ? "vol_target" : "risk_target";
}
const char* series_file(const Options& o) noexcept {
  return vol_law(o) ? "vol_target.csv" : "risk_target.csv";
}

std::string rule_suffix(const Options& o) {
  if (vol_law(o)) return std::string("+") + vol_target::rule_id;
  std::string id = "+risk-target-" + decimal(o.params.sigma_star);
  if (o.params.bias != eb::risk_target_default_bias) id += "-bias-" + decimal(o.params.bias);
  if (o.params.cadence != eb::risk_target_default_cadence)
    id += "-cadence-" + std::to_string(o.params.cadence);
  return id;
}

namespace {
// One JSON Schema property: its type, its nav flag (x-flag) and the extra keywords.
Json property(const char* type, const char* flag, Json keywords = Json::object()) {
  keywords["type"] = type;
  keywords["x-flag"] = flag;
  return keywords;
}
Json leverage_cap() {
  return property("number", "--aim-leverage",
                  Json{{"minimum", 1}, {"maximum", 2}, {"default", 1}});
}
Json store_properties(Json properties) {
  properties["risk_model"] = property("string", "--risk-model");
  properties["risk_model_sha256"] =
      property("string", "--risk-model-sha256", Json{{"pattern", "^[0-9a-f]{64}$"}});
  return properties;
}
Json rule_row(const char* id, const char* flag, Json properties, Json required,
              Json incompatible) {
  return Json{{"id", id},
              {"kind", "leverage"},
              {"x-flag", flag},
              {"params_schema", Json{{"type", "object"},
                                     {"additionalProperties", false},
                                     {"required", std::move(required)},
                                     {"properties", std::move(properties)}}},
              {"incompatible", std::move(incompatible)}};
}
} // namespace

Json leverage_rules_json() {
  Json rows = Json::array();
  rows.push_back(rule_row("fixed-v1", "--aim-leverage", Json{{"aim_leverage", leverage_cap()}},
                          Json::array({"aim_leverage"}),
                          Json::array({"vol-target-v1", "risk-target-v1"})));
  rows.push_back(rule_row("vol-target-v1", "--vol-target vol-target-v1",
                          store_properties(Json{{"aim_leverage", leverage_cap()}}),
                          Json::array({"aim_leverage", "risk_model", "risk_model_sha256"}),
                          Json::array({"risk-target-v1", "aim-partial-v6", "spo-v1", "spo-v2",
                                       "spo-v3"})));
  Json risk = store_properties(Json{{"aim_leverage", leverage_cap()}});
  risk["sigma_star"] = property("number", "--risk-target",
                                Json{{"exclusiveMinimum", 0}, {"maximum", 1}});
  risk["bias"] = property("number", "--risk-target-bias",
                          Json{{"exclusiveMinimum", 0}, {"maximum", 10},
                               {"default", eb::risk_target_default_bias}});
  risk["cadence"] = property("integer", "--risk-target-cadence",
                             Json{{"minimum", 1}, {"maximum", 10000},
                                  {"default", eb::risk_target_default_cadence}});
  rows.push_back(rule_row("risk-target-v1", "--risk-target S", std::move(risk),
                          Json::array({"aim_leverage", "sigma_star", "risk_model",
                                       "risk_model_sha256"}),
                          Json::array({"vol-target-v1", "aim-partial-v6", "spo-v1", "spo-v2"})));
  return rows;
}
} // namespace atx::impl::strategy::risk_target
