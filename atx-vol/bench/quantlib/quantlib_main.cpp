// Official QuantLib API adapter; no upstream numerical source is modified.
#include <cmath>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <ql/exercise.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/oneassetoption.hpp>
#include <ql/pricingengines/vanilla/qdfpamericanengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounter.hpp>

#include "bench_clocks.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace {
namespace ql = QuantLib;
using Json = nlohmann::json;
constexpr std::string_view kCommit = "e96579fd471b2e9f6bc974c96674d1ccfff248fa";
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
using atx::vol::bench::BenchClock;
using atx::vol::bench::seconds_between;

struct Contract { double S, K, T, sigma, r, q; };

// A model clock, not a market day-count recommendation. The fixed synthetic
// maturity is 365 days after the fixed reference date and maps EXACTLY to T.
class ModelClock final : public ql::DayCounter {
  class ModelImpl final : public Impl {
  public:
    explicit ModelImpl(double maturity) : maturity_(maturity) {}
    std::string name() const override { return "Frozen comparator T=" + std::to_string(maturity_); }
    ql::Time yearFraction(const ql::Date &a, const ql::Date &b,
                         const ql::Date &, const ql::Date &) const override {
      return (static_cast<double>(b - a) / 365.) * maturity_;
    }
  private:
    double maturity_;
  };
public:
  explicit ModelClock(double maturity) : DayCounter(ql::ext::make_shared<ModelImpl>(maturity)) {}
};

[[nodiscard]] ql::Date reference_date() { return {1, ql::January, 2020}; }

enum class Scheme { Fast, Accurate, HighPrecision };

[[nodiscard]] ql::ext::shared_ptr<ql::QdFpIterationScheme> fresh_scheme(Scheme scheme) {
  switch (scheme) {
  case Scheme::Fast: return ql::ext::make_shared<ql::QdFpLegendreScheme>(7, 2, 7, 27);
  case Scheme::Accurate: return ql::ext::make_shared<ql::QdFpLegendreTanhSinhScheme>(25, 5, 13, 1e-8);
  case Scheme::HighPrecision: return ql::ext::make_shared<ql::QdFpTanhSinhIterationScheme>(10, 30, 1e-10);
  }
  throw std::invalid_argument("Unknown QdFp scheme");
}

[[nodiscard]] ql::ext::shared_ptr<ql::QdFpIterationScheme> factory_scheme(Scheme scheme) {
  switch (scheme) {
  case Scheme::Fast: return ql::QdFpAmericanEngine::fastScheme();
  case Scheme::Accurate: return ql::QdFpAmericanEngine::accurateScheme();
  case Scheme::HighPrecision: return ql::QdFpAmericanEngine::highPrecisionScheme();
  }
  throw std::invalid_argument("Unknown QdFp scheme");
}

struct Market {
  const ql::Date maturity = reference_date() + 365;
  ql::ext::shared_ptr<ql::SimpleQuote> spot;
  ql::ext::shared_ptr<ql::FlatForward> dividend, funding;
  ql::ext::shared_ptr<ql::BlackConstantVol> volatility;
  ql::ext::shared_ptr<ql::BlackScholesMertonProcess> process;
  // VanillaOption's constructor only delegates to this public base and adds no
  // pricing override. Use its identical NPV/arguments path without linking the
  // unrelated implied-volatility/finite-difference helper translation unit.
  ql::OneAssetOption option;

  Market(const Contract &c, ql::ext::shared_ptr<ql::QdFpIterationScheme> scheme,
         ql::QdFpAmericanEngine::FixedPointEquation equation = ql::QdFpAmericanEngine::Auto)
      : spot(ql::ext::make_shared<ql::SimpleQuote>(c.S)),
        dividend(ql::ext::make_shared<ql::FlatForward>(reference_date(), c.q, ModelClock(c.T))),
        funding(ql::ext::make_shared<ql::FlatForward>(reference_date(), c.r, ModelClock(c.T))),
        volatility(ql::ext::make_shared<ql::BlackConstantVol>(reference_date(), ql::NullCalendar(),
                                                            c.sigma, ModelClock(c.T))),
        process(ql::ext::make_shared<ql::BlackScholesMertonProcess>(ql::Handle<ql::Quote>(spot),
          ql::Handle<ql::YieldTermStructure>(dividend), ql::Handle<ql::YieldTermStructure>(funding),
          ql::Handle<ql::BlackVolTermStructure>(volatility))),
        option(ql::ext::make_shared<ql::PlainVanillaPayoff>(ql::Option::Put, c.K),
               ql::ext::make_shared<ql::AmericanExercise>(reference_date(), maturity)) {
    option.setPricingEngine(ql::ext::make_shared<ql::QdFpAmericanEngine>(process, std::move(scheme), equation));
  }

  [[nodiscard]] Json effective(double strike) const {
    const double t = process->time(maturity);
    const double dr = funding->discount(maturity), dq = dividend->discount(maturity);
    return {{"T", t}, {"r", -std::log(dr) / t}, {"q", -std::log(dq) / t},
            {"variance", volatility->blackVariance(t, strike)},
            {"discount", dr}, {"dividend_discount", dq}};
  }

  [[nodiscard]] double price(double value) {
    spot->setValue(value);
    return option.NPV();
  }
};

[[nodiscard]] Json contract_json(const Contract &c) {
  return {{"S", c.S}, {"K", c.K}, {"T", c.T}, {"sigma", c.sigma}, {"r", c.r}, {"q", c.q}};
}

[[nodiscard]] double european_put(const Contract &c) {
  const double v = c.sigma * std::sqrt(c.T);
  const double d1 = (std::log(c.S / c.K) + (c.r - c.q + .5 * c.sigma * c.sigma) * c.T) / v;
  return .5 * c.K * std::exp(-c.r * c.T) * std::erfc((d1 - v) / std::sqrt(2.)) -
         .5 * c.S * std::exp(-c.q * c.T) * std::erfc(d1 / std::sqrt(2.));
}

[[nodiscard]] Json self_test() {
  Json report{{"schema", "atx-quantlib-api-self-test-v2"}, {"quantlib_commit", kCommit},
              {"official_checks", Json::array()}, {"clock_checks", Json::array()},
              {"factory_checks", Json::array()}, {"european_checks", Json::array()}};
  // Selected published regressions from pinned test-suite/americanoption.cpp,
  // testAndersenLakeHighPrecisionExample. Expected figures are American premium.
  struct Example { unsigned l, m, n; double r, a, b, tol; };
  const Example examples[] = {{24, 3, 9, .05, .1069528125898476, .1069524359360852, 1e-6},
                              {35, 8, 16, .075, .3671116758420414, .3671111055677869, 1e-9}};
  for (const auto &e : examples) {
    const Contract c{100, 100, 1, .25, e.r, .05};
    for (const auto equation : {ql::QdFpAmericanEngine::FP_A, ql::QdFpAmericanEngine::FP_B}) {
      Market market(c, ql::ext::make_shared<ql::QdFpLegendreTanhSinhScheme>(e.l, e.m, e.n, e.tol), equation);
      report["official_checks"].push_back({{"input", contract_json(c)}, {"equation", equation},
          {"l", e.l}, {"m", e.m}, {"n", e.n}, {"tolerance", e.tol},
          {"premium", market.option.NPV() - european_put(c)},
          {"expected", equation == ql::QdFpAmericanEngine::FP_A ? e.a : e.b}});
    }
  }
  for (const double t : {1. / 365., .25, 1., 3.}) {
    const Contract c{100, 100, t, .2, .03, .01};
    Market market(c, fresh_scheme(Scheme::Accurate));
    report["clock_checks"].push_back({{"input", contract_json(c)}, {"effective", market.effective(c.K)}});
  }
  for (const auto scheme : {Scheme::Fast, Scheme::Accurate, Scheme::HighPrecision}) {
    const Contract c{100, 100, .25, .2, .03, .01};
    Market fresh(c, fresh_scheme(scheme)), factory(c, factory_scheme(scheme));
    const double fresh_value = fresh.option.NPV(), factory_value = factory.option.NPV();
    report["factory_checks"].push_back({{"scheme", static_cast<unsigned>(scheme)},
        {"fresh", fresh_value}, {"factory", factory_value}, {"same_prices", fresh_value == factory_value}});
    for (const double spot : {70., 100., 140.}) {
      const Contract european{spot, 100, 1, .2, 0, 0};
      Market m(european, fresh_scheme(scheme));
      report["european_checks"].push_back({{"input", contract_json(european)},
          {"scheme", static_cast<unsigned>(scheme)},
          {"price", m.option.NPV()}, {"closed_form", european_put(european)}});
    }
  }
  report["build"] = atx::vol::bench::build_info_json();
  report["boost_version"] = BOOST_VERSION;
  return report;
}

struct Group { std::string id; Contract model; std::vector<double> spots; };

[[nodiscard]] double number(const Json &j) {
  if (!j.is_number() || !std::isfinite(j.get<double>()))
    throw std::invalid_argument("Finite model number required");
  return j.get<double>();
}

[[nodiscard]] std::vector<Group> read_groups(const std::filesystem::path &path) {
  std::ifstream stream(path);
  Json input;
  stream >> input;
  if (input.at("schema") != "american-put-comparator-corpus-v1" ||
      !input.at("groups").is_array() || input.at("groups").empty())
    throw std::invalid_argument("Invalid comparator corpus");
  std::vector<Group> groups;
  std::set<std::string> ids;
  std::size_t contracts = 0;
  for (const auto &j : input.at("groups")) {
    Group g{j.at("id").get<std::string>(),
      {0, number(j.at("K")), number(j.at("T")), number(j.at("sigma")),
          number(j.at("r")), number(j.at("q"))}, {}};
    if (g.id.empty() || !ids.insert(g.id).second || g.model.K <= 0 ||
        g.model.T <= 0 || g.model.sigma <= 0 || !j.at("spots").is_array() || j.at("spots").empty())
      throw std::invalid_argument("Invalid/duplicate contract group");
    for (const auto &jspot : j.at("spots")) {
      const double spot = number(jspot);
      if (spot <= 0 || ++contracts > 100000) throw std::invalid_argument("Invalid spot/corpus size");
      g.spots.push_back(spot);
    }
    g.model.S = g.spots.front();
    groups.push_back(std::move(g));
  }
  return groups;
}

[[nodiscard]] Json jnumber(double d) { return std::isfinite(d) ? Json(d) : Json(nullptr); }
[[nodiscard]] Json jprices(const std::vector<double> &values) {
  Json result = Json::array();
  for (const double v : values) result.push_back(jnumber(v));
  return result;
}
[[nodiscard]] std::uint64_t digest(const std::vector<double> &values) {
  std::uint64_t d = 14695981039346656037ull;
  for (const double v : values)
    d = (d ^ (std::isfinite(v) ? std::bit_cast<std::uint64_t>(v) : 0x7ff8000000000000ull)) * 1099511628211ull;
  return d;
}

template<class Price>
[[nodiscard]] double attempted_price(Price price, std::string &status) {
  try {
    const double value = price();
    status = std::isfinite(value) ? "finite" : "nonfinite";
    return value;
  } catch (const std::exception &e) {
    status = e.what();
    return kNan;
  }
}

template<class Cold, class Factory>
[[nodiscard]] Json measure(std::string_view name, const std::vector<Group> &groups, std::size_t count,
    int reps, Cold cold, Factory factory, std::string_view reuse_kind, const Json &options) {
  std::vector<double> cold_prices(count), reused_prices(count), cold_s, setup_s, eval_s, total_s;
  std::vector<std::string> cold_status(count), reused_status(count);
  std::vector<std::uint64_t> cold_digests, reused_digests;
  for (int rep = 0; rep < reps; ++rep) {
    std::size_t index = 0;
    const auto start = BenchClock::now();
    for (const auto &group : groups) {
      for (const double spot : group.spots) {
        Contract c = group.model;
        c.S = spot;
        cold_prices[index] = attempted_price([&] { return cold(c); }, cold_status[index]);
        ++index;
      }
    }
    cold_s.push_back(seconds_between(start, BenchClock::now()));
    cold_digests.push_back(digest(cold_prices));
    index = 0;
    double setup = 0, evaluation = 0;
    const auto reuse_start = BenchClock::now();
    for (const auto &group : groups) {
      const auto begin = BenchClock::now();
      try {
        auto context = factory(group.model);
        const auto prepared = BenchClock::now();
        setup += seconds_between(begin, prepared);
        for (const double spot : group.spots) {
          reused_prices[index] = attempted_price([&] { return context.price(spot); }, reused_status[index]);
          ++index;
        }
        evaluation += seconds_between(prepared, BenchClock::now());
      } catch (const std::exception &e) {
        setup += seconds_between(begin, BenchClock::now());
        for (std::size_t i = 0; i < group.spots.size(); ++i) {
          reused_prices[index] = kNan;
          reused_status[index++] = e.what();
        }
      }
    }
    total_s.push_back(seconds_between(reuse_start, BenchClock::now()));
    setup_s.push_back(setup);
    eval_s.push_back(evaluation);
    reused_digests.push_back(digest(reused_prices));
  }
  return {{"name", name}, {"options", options}, {"reuse_kind", reuse_kind},
    {"cold_prices", jprices(cold_prices)}, {"reused_prices", jprices(reused_prices)},
    {"cold_s", cold_s}, {"reuse_setup_s", setup_s}, {"reuse_eval_s", eval_s}, {"reuse_total_s", total_s},
    {"cold_digests", cold_digests}, {"reused_digests", reused_digests},
    {"cold_status", cold_status}, {"reused_status", reused_status}};
}

struct AtxMarket {
  atx::vol::AlExpiryGeometry geometry;
  atx::vol::AlBoundaryHandle boundary;
  double strike;
  AtxMarket(const Contract &c, const std::optional<atx::vol::AlOpts> &options)
      : geometry(atx::vol::prepare_al_geometry(c.T, c.r, c.q, options)),
        boundary(geometry, atx::vol::Side::Put, c.K), strike(c.K) {
    const auto result = boundary.solve(c.sigma);
    if (!result) throw std::runtime_error(result.error().to_string());
  }
  [[nodiscard]] double price(double spot) const { return boundary.price_from_boundary(spot, strike); }
};

[[nodiscard]] double atx_price(const Contract &c, const std::optional<atx::vol::AlOpts> &options) {
  const auto value = atx::vol::andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, atx::vol::Side::Put, options);
  if (!value) throw std::runtime_error(value.error().to_string());
  return *value;
}

[[nodiscard]] Json atx_options_json(const std::optional<atx::vol::AlOpts> &options) {
  auto resolved = options.value_or(atx::vol::al_fast_profile(std::nullopt));
  if (!options) resolved.profile = atx::vol::AlProfile::Reference;
  const auto profile = resolved.profile == atx::vol::AlProfile::Reference ? "Reference" :
      resolved.profile == atx::vol::AlProfile::Fast ? "Fast" : "Screen";
  return {{"n_collocation", resolved.n_collocation}, {"n_quadrature", resolved.n_quadrature},
      {"n_quad_price", resolved.n_quad_price}, {"max_newton_iter", resolved.max_newton_iter},
      {"tol", resolved.tol}, {"profile", profile}, {"explicit", options.has_value()}};
}

[[nodiscard]] Json benchmark(const std::filesystem::path &input, int reps,
    const std::optional<unsigned> &affinity, bool require_pcore) {
  const auto groups = read_groups(input);
  Json cases = Json::array();
  for (const auto &g : groups) {
    for (const double spot : g.spots) {
      Contract c = g.model;
      c.S = spot;
      auto j = contract_json(c);
      j["group"] = g.id;
      cases.push_back(std::move(j));
    }
  }
  const auto before = atx::vol::bench::sample_cpus();
  Json engines = Json::array();
  for (const auto scheme : {Scheme::Fast, Scheme::Accurate}) {
    const auto factory = [scheme](const Contract &c) { return Market(c, fresh_scheme(scheme)); };
    const auto cold = [&](const Contract &c) { auto m = factory(c); return m.option.NPV(); };
    const bool fast = scheme == Scheme::Fast;
    const Json options = fast ? Json{{"l", 7}, {"m", 2}, {"n", 7}, {"p", 27}, {"equation", "Auto"}}
                             : Json{{"l", 25}, {"m", 5}, {"n", 13}, {"tanh_sinh_tolerance", 1e-8}, {"equation", "Auto"}};
    engines.push_back(measure(fast ? "ql_fast" : "ql_accurate", groups, cases.size(), reps, cold, factory,
        "engine-market-scheme; boundary recomputed per spot", options));
  }
  struct AtxProfile { std::string_view name; std::optional<atx::vol::AlOpts> options; };
  for (const auto &profile : {AtxProfile{"atx_default", std::nullopt},
      AtxProfile{"atx_accurate_fast", atx::vol::al_fast_profile(std::nullopt)},
      AtxProfile{"atx_screen", atx::vol::al_screen_opts()},
      AtxProfile{"atx_bulk_fast", atx::vol::al_fast_profile(atx::vol::al_bulk_opts())}}) {
    const auto &opts = profile.options;
    const auto cold = [&](const Contract &c) { return atx_price(c, opts); };
    const auto factory = [&](const Contract &c) { return AtxMarket(c, opts); };
    engines.push_back(measure(profile.name, groups, cases.size(), reps,
        cold, factory, "solved boundary across spots", atx_options_json(opts)));
  }
  const auto after = atx::vol::bench::sample_cpus();
  Json effective = Json::array(), references = Json::array();
  const atx::vol::AlOpts ref_opts{.n_collocation = 32, .n_quadrature = 64, .n_quad_price = 64,
                                 .max_newton_iter = 32, .tol = 1e-14};
  for (const auto &g : groups) {
    Market clock(g.model, fresh_scheme(Scheme::Fast));
    effective.push_back({{"group", g.id}, {"input", contract_json(g.model)}, {"effective", clock.effective(g.model.K)}});
    for (const double spot : g.spots) {
      Contract c = g.model;
      c.S = spot;
      std::string ql_status, atx_status;
      const double ql_reference = attempted_price([&] {
        Market m(c, fresh_scheme(Scheme::HighPrecision)); return m.option.NPV();
      }, ql_status);
      const double atx_reference = attempted_price([&] { return atx_price(c, ref_opts); }, atx_status);
      references.push_back({{"quantlib_high_precision", jnumber(ql_reference)}, {"atx_refined", jnumber(atx_reference)},
          {"quantlib_status", ql_status}, {"atx_status", atx_status},
          {"closed_form", jnumber(c.r <= 0 && c.q >= 0 ? european_put(c) : kNan)}});
    }
  }
  Json build = atx::vol::bench::build_info_json();
  build["cmake_interprocedural_optimization"] = ATX_QUANTLIB_COMPARATOR_IPO != 0;
  return {{"schema", "american-put-quantlib-comparator-v2"}, {"quantlib_commit", kCommit},
    {"case_count", cases.size()}, {"group_count", groups.size()}, {"cases", cases}, {"engines", engines},
    {"effective_models", effective}, {"references", references}, {"self_test", self_test()},
    {"reference_options", {{"atx_refined", atx_options_json(ref_opts)},
      {"quantlib_high_precision", {{"m", 10}, {"n", 30}, {"tanh_sinh_tolerance", 1e-10}, {"equation", "Auto"}}}}},
    {"general_american_independent_method_reference", false}, {"build", build},
    {"process_identity", atx::vol::bench::process_identity_json()},
    {"affinity", atx::vol::bench::affinity_json(affinity, require_pcore, before, after)}};
}
} // namespace

int main(int argc, char **argv) {
  try {
    bool run_self_test = false;
    std::filesystem::path input, output;
    int reps = 5;
    std::optional<unsigned> affinity;
    bool require_pcore = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--self-test") run_self_test = true;
      else if (arg == "--require-pcore") require_pcore = true;
      else if (arg == "--output" && i + 1 < argc) output = argv[++i];
      else if (arg == "--input" && i + 1 < argc) input = argv[++i];
      else if (arg == "--reps" && i + 1 < argc) reps = std::stoi(argv[++i]);
      else if (arg == "--affinity" && i + 1 < argc) affinity = static_cast<unsigned>(std::stoul(argv[++i]));
      else throw std::invalid_argument("Unknown/incomplete option");
    }
    if ((run_self_test && !input.empty()) || (!run_self_test && input.empty()) ||
        output.empty() || std::filesystem::exists(output) ||
        reps < 1 || reps > 1000 || (require_pcore && !affinity))
      throw std::invalid_argument("Require exactly one of --self-test/--input, new --output, and reps1..1000");
    if (affinity) {
      atx::vol::bench::pin_to_logical_cpu(*affinity);
      if (require_pcore && atx::vol::bench::is_performance_cpu(*affinity) != true)
        throw std::invalid_argument("Requested CPU is not a proven P-core");
    }
    ql::Settings::instance().evaluationDate() = reference_date();
    std::ofstream stream(output);
    stream << (run_self_test ? self_test() : benchmark(input, reps, affinity, require_pcore)).dump(2) << '\n';
    if (!stream) throw std::runtime_error("Cannot write output");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
