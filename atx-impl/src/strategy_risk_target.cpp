// risk-target-v1 (platform v8 R-8): the NAV replay's scaler over the engine kernel
// (engine::book::risk_target_update), its series and its published blocks. The rule is stated in
// strategy_risk_target.hpp; the wiring into the construction rules is strategy_nav_v7.cpp.
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
  Stats leverage, multiplier, sigma_hat;
};
} // namespace

Scaler::Scaler(const Options& o, std::shared_ptr<const spo::RiskStore> risk)
    : options_(o), risk_(std::move(risk)) {}

void Scaler::begin_run() { books_.clear(); }

co::Result<f64> Scaler::leverage(const TargetReplayInput& x, usize d, bool rebalance,
                                 std::string_view book, f64 base,
                                 std::span<const f64> current, bool record) {
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
  auto found = books_.find(book);
  if (found == books_.end()) found = books_.emplace(std::string(book), Book{}).first;
  Book& b = found->second;
  bool updated = false;
  const bool forecast = d < risk_->dates() && risk_->forecast()[d] != 0;
  if (forecast && eb::risk_target_due(b.state, d, options_.params.cadence)) {
    ATX_TRY(updated, estimate(d, base, current, b));
  }
  const f64 in_force = eb::risk_target_in_force(b.state, base);
  if (record) {
    Record r;
    r.session = x.session_keys[d];
    r.book = std::string(book);
    r.rebalance = rebalance;
    r.updated = updated;
    r.base = base;
    r.gross = b.gross;
    r.priced_share = b.priced_share;
    r.sigma_hat = b.state.sigma_hat;
    r.raw = b.state.at.raw;
    r.leverage = in_force;
    r.clip = b.state.at.clip;
    records_.push_back(std::move(r));
  }
  return co::Ok(in_force);
}

// The book's priced names at d, in instrument order (their weights, industry columns 1 + slot,
// style rows, specific variances), the whole book's gross, then the engine's update.
co::Result<bool> Scaler::estimate(usize d, f64 base, std::span<const f64> current, Book& book) {
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
  ATX_TRY(const bool updated, eb::risk_target_update(options_.params, base, d, model, weights_,
                                                     gross, book.state));
  if (updated) {
    book.gross = gross;
    book.priced_share = priced_gross / gross;
  }
  return co::Ok(updated);
}

std::string records_csv(std::span<const Record> records) {
  std::string text =
      "session,book,rebalance,updated,gross,priced_share,sigma_hat,raw,L_t,multiplier,clip\n";
  for (const auto& r : records)
    text += std::to_string(r.session) + ',' + r.book + ',' + (r.rebalance ? "1" : "0") + ',' +
            (r.updated ? "1" : "0") + ',' + number(r.gross) + ',' + number(r.priced_share) + ',' +
            number(r.sigma_hat) + ',' + number(r.raw) + ',' + number(r.leverage) + ',' +
            number(r.leverage / r.base) + ',' + std::to_string(static_cast<int>(r.clip)) + '\n';
  return text;
}

std::string declaration(const Options& o) {
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

Json summary_json(std::span<const Record> records) {
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
    b.estimates_at_lo += one_if(r.clip == eb::RiskTargetClip::Low);
    b.estimates_at_hi += one_if(r.clip == eb::RiskTargetClip::High);
  }
  Json out = Json::object();
  for (const auto& [book, b] : books)
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
  return out;
}

std::string rule_suffix(const Options& o) {
  std::string id = "+risk-target-" + decimal(o.params.sigma_star);
  if (o.params.bias != eb::risk_target_default_bias) id += "-bias-" + decimal(o.params.bias);
  if (o.params.cadence != eb::risk_target_default_cadence)
    id += "-cadence-" + std::to_string(o.params.cadence);
  return id;
}
} // namespace atx::impl::strategy::risk_target
