// Isolated benchmark adapter. Public sources remain outside the shipping tree.
// See README.md for clock boundaries, reference limitations and source pins.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fast_put.hpp>
#include <nlohmann/json.hpp>

#include "atx/vol/api/pricing/american.hpp"
#include "bench_clocks.hpp"

namespace {
using atx::vol::AlOpts;
using atx::vol::Side;
using atx::vol::bench::BenchClock;
using atx::vol::bench::seconds_between;
using Json = nlohmann::json;
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr std::string_view kUpstreamCommit = "e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4";

struct Group {
  std::string id;
  double K{}, T{}, sigma{}, r{}, q{};
  std::vector<double> spots;
};

[[nodiscard]] double number(const Json &value) {
  if (!value.is_number() || !std::isfinite(value.get<double>()))
    throw std::invalid_argument("Expected finite numeric model value");
  return value.get<double>();
}

[[nodiscard]] std::vector<Group> read_groups(const std::filesystem::path &path) {
  std::ifstream input(path);
  Json data;
  input >> data;
  if (data.at("schema") != "american-put-comparator-corpus-v1" ||
      !data.at("groups").is_array() || data.at("groups").empty() || data.at("groups").size() > 4096)
    throw std::invalid_argument("Invalid corpus schema or group count");
  std::vector<Group> groups;
  std::set<std::string> ids;
  for (const auto &g : data.at("groups")) {
    Group group{g.at("id").get<std::string>(), number(g.at("K")), number(g.at("T")),
                number(g.at("sigma")), number(g.at("r")), number(g.at("q")), {}};
    if (group.id.empty() || !ids.insert(group.id).second || group.K <= 0 || group.T <= 0 ||
        group.sigma <= 0 || !g.at("spots").is_array() || g.at("spots").empty() ||
        g.at("spots").size() > 4096)
      throw std::invalid_argument("Invalid/duplicate model group");
    for (const auto &s : g.at("spots")) {
      const double spot = number(s);
      if (spot <= 0) throw std::invalid_argument("Spot must be positive");
      group.spots.push_back(spot);
    }
    groups.push_back(std::move(group));
  }
  return groups;
}

[[nodiscard]] Json json_number(double value) {
  return std::isfinite(value) ? Json(value) : Json(nullptr);
}

[[nodiscard]] Json json_prices(const std::vector<double> &prices) {
  Json result = Json::array();
  for (const double value : prices) result.push_back(json_number(value));
  return result;
}

// Upstream calc() boundary operations without its console/timer calls. The pure
// European price is evaluated afterwards; it has no boundary-state dependency.
// A separate untimed calc() cross-check covers every corpus contract/profile.
template<class Put>
void prepare_public(Put &put) {
  put.initalGuessB();
  put.initH();
  put.initChebyshevInterpolation();
  for (int iteration = 0; iteration < put.fixed_point_iterations; ++iteration) {
    for (std::size_t i = 0; i + 1 < Put::NC; ++i)
      put.Bv[i] = put.FixedPoint_B_StepOrdinary(put.tv[i]);
    put.initH();
    put.initChebyshevInterpolation();
  }
}

template<class Put>
[[nodiscard]] double price_public(Put &put, double spot) {
  put.S = spot; // Boundary depends only on immutable (K,T,sigma,r,q), not spot.
  const double european = compute_european_put_price(put.S, put.K, put.sigma, put.r, put.q, put.T);
  return european + put.Vee(put.T);
}

class QuietCout {
public:
  QuietCout() : previous_(std::cout.rdbuf(sink_.rdbuf())), flags_(std::cout.flags()),
                precision_(std::cout.precision()) {}
  ~QuietCout() {
    std::cout.rdbuf(previous_);
    std::cout.flags(flags_);
    std::cout.precision(precision_);
  }
  QuietCout(const QuietCout &) = delete;
  QuietCout &operator=(const QuietCout &) = delete;
private:
  std::ostringstream sink_;
  std::streambuf *previous_;
  std::ios::fmtflags flags_;
  std::streamsize precision_;
};

[[nodiscard]] bool same(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

struct Measurement {
  std::vector<double> cold_prices, prepared_prices;
  std::vector<double> cold_s, setup_s, eval_s, total_s;
  std::vector<std::uint64_t> cold_digests, prepared_digests;
};

[[nodiscard]] std::uint64_t digest(const std::vector<double> &prices) noexcept {
  std::uint64_t result = 14695981039346656037ull;
  for (const double p : prices) {
    // Canonicalize nonfinite payloads only for deterministic evidence hashing.
    const auto bits = std::isfinite(p) ? std::bit_cast<std::uint64_t>(p) : 0x7ff8000000000000ull;
    result = (result ^ bits) * 1099511628211ull;
  }
  return result;
}

[[nodiscard]] Json measurement_json(std::string_view name, const Measurement &m, const Json &options) {
  return Json{{"name", name}, {"options", options}, {"cold_prices", json_prices(m.cold_prices)},
    {"prepared_prices", json_prices(m.prepared_prices)}, {"cold_s", m.cold_s},
    {"prepared_setup_s", m.setup_s}, {"prepared_eval_s", m.eval_s},
    {"prepared_total_s", m.total_s}, {"cold_digests", m.cold_digests},
    {"prepared_digests", m.prepared_digests}};
}

template<class Put>
[[nodiscard]] Json measure_public(std::string_view name, const std::vector<Group> &groups,
    std::size_t count, int reps, int iterations, const Json &options, Json &checks) {
  Measurement m;
  m.cold_prices.resize(count);
  m.prepared_prices.resize(count);
  for (int rep = 0; rep < reps; ++rep) {
    std::size_t index = 0;
    const auto cold_start = BenchClock::now();
    for (const auto &g : groups) {
      for (const double spot : g.spots) {
        Put put(spot, g.K, g.sigma, g.r, g.q, g.T, iterations);
        prepare_public(put);
        m.cold_prices[index++] = price_public(put, spot);
      }
    }
    m.cold_s.push_back(seconds_between(cold_start, BenchClock::now()));
    m.cold_digests.push_back(digest(m.cold_prices));
    index = 0;
    double setup = 0, evaluate = 0;
    const auto total_start = BenchClock::now();
    for (const auto &g : groups) {
      const auto setup_start = BenchClock::now();
      Put put(g.spots.front(), g.K, g.sigma, g.r, g.q, g.T, iterations);
      prepare_public(put);
      const auto setup_end = BenchClock::now();
      for (const double spot : g.spots) m.prepared_prices[index++] = price_public(put, spot);
      const auto eval_end = BenchClock::now();
      setup += seconds_between(setup_start, setup_end);
      evaluate += seconds_between(setup_end, eval_end);
    }
    m.total_s.push_back(seconds_between(total_start, BenchClock::now()));
    m.setup_s.push_back(setup);
    m.eval_s.push_back(evaluate);
    m.prepared_digests.push_back(digest(m.prepared_prices));
  }
  std::size_t index = 0;
  for (const auto &g : groups) {
    for (const double spot : g.spots) {
      Put original(spot, g.K, g.sigma, g.r, g.q, g.T, iterations);
      double expected = kNan;
      { const QuietCout quiet; expected = original.calc(); }
      checks.push_back({{"engine", name}, {"index", index},
          {"bit_identical", same(expected, m.cold_prices[index])},
          {"both_nonfinite", !std::isfinite(expected) && !std::isfinite(m.cold_prices[index])}});
      ++index;
    }
  }
  Json result = measurement_json(name, m, options);
  result["cold_status"] = Json::array();
  for (const double value : m.cold_prices)
    result["cold_status"].push_back(std::isfinite(value) ? "finite" : "upstream_nonfinite");
  return result;
}

[[nodiscard]] double atx_price(const Group &g, double spot, const std::optional<AlOpts> &opts) {
  const auto result = atx::vol::andersen_lake(spot, g.K, g.T, g.sigma, g.r, g.q, Side::Put, opts);
  return result ? *result : kNan;
}

[[nodiscard]] Json opts_json(const AlOpts &o) {
  return Json{{"n_collocation", o.n_collocation}, {"n_quadrature", o.n_quadrature},
    {"n_quad_price", o.n_quad_price}, {"max_newton_iter", o.max_newton_iter},
    {"tol", o.tol}, {"profile", static_cast<unsigned>(o.profile)}};
}

[[nodiscard]] Json measure_atx(std::string_view name, const std::vector<Group> &groups,
    std::size_t count, int reps, const std::optional<AlOpts> &opts) {
  Measurement m;
  m.cold_prices.resize(count);
  m.prepared_prices.resize(count);
  for (int rep = 0; rep < reps; ++rep) {
    std::size_t index = 0;
    const auto cold_start = BenchClock::now();
    for (const auto &g : groups)
      for (const double spot : g.spots) m.cold_prices[index++] = atx_price(g, spot, opts);
    m.cold_s.push_back(seconds_between(cold_start, BenchClock::now()));
    m.cold_digests.push_back(digest(m.cold_prices));
    index = 0;
    double setup = 0, evaluate = 0;
    const auto total_start = BenchClock::now();
    for (const auto &g : groups) {
      const auto setup_start = BenchClock::now();
      atx::vol::AlExpiryGeometry geometry;
      geometry.prepare(g.T, g.r, g.q, opts);
      atx::vol::AlBoundaryHandle boundary(geometry, Side::Put, g.K);
      const auto status = boundary.solve(g.sigma);
      const auto setup_end = BenchClock::now();
      for (const double spot : g.spots)
        m.prepared_prices[index++] = status ? boundary.price_from_boundary(spot, g.K) : kNan;
      const auto eval_end = BenchClock::now();
      setup += seconds_between(setup_start, setup_end);
      evaluate += seconds_between(setup_end, eval_end);
    }
    m.total_s.push_back(seconds_between(total_start, BenchClock::now()));
    m.setup_s.push_back(setup);
    m.eval_s.push_back(evaluate);
    m.prepared_digests.push_back(digest(m.prepared_prices));
  }
  AlOpts resolved = opts.value_or(atx::vol::al_fast_profile(std::nullopt));
  if (!opts) resolved.profile = atx::vol::AlProfile::Reference;
  Json options = opts_json(resolved);
  options["explicit"] = opts.has_value();
  Json result = measurement_json(name, m, options);
  result["cold_status"] = Json::array();
  for (const auto &g : groups) {
    for (const double spot : g.spots) {
      const auto value = atx::vol::andersen_lake(spot, g.K, g.T, g.sigma, g.r, g.q, Side::Put, opts);
      if (value) result["cold_status"].push_back(std::isfinite(*value) ? "finite" : "nonfinite");
      else result["cold_status"].push_back({{"error_code", static_cast<unsigned>(value.error().code())},
                                           {"message", value.error().message()}});
    }
  }
  return result;
}

// Independent Black-Scholes formula. For r<=0 and q>=0 a put has no early
// exercise advantage. General American prices still require an independent oracle.
[[nodiscard]] double european_put(const Group &g, double spot) {
  const double st = g.sigma * std::sqrt(g.T);
  const double d1 = (std::log(spot / g.K) + (g.r - g.q + .5 * g.sigma * g.sigma) * g.T) / st;
  const double d2 = d1 - st;
  return .5 * g.K * std::exp(-g.r * g.T) * std::erfc(d2 / std::sqrt(2.)) -
         .5 * spot * std::exp(-g.q * g.T) * std::erfc(d1 / std::sqrt(2.));
}
} // namespace

int main(int argc, char **argv) {
  try {
    std::filesystem::path input, output;
    int reps = 10;
    std::optional<unsigned> affinity;
    bool require_pcore = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--require-pcore") { require_pcore = true; continue; }
      if (i + 1 >= argc) throw std::invalid_argument("Missing option value");
      const std::string value(argv[++i]);
      if (arg == "--input") input = value;
      else if (arg == "--output") output = value;
      else if (arg == "--reps") reps = std::stoi(value);
      else if (arg == "--affinity") affinity = static_cast<unsigned>(std::stoul(value));
      else throw std::invalid_argument("Unknown option");
    }
    if (input.empty() || output.empty() || reps < 1 || reps > 1000 ||
        std::filesystem::exists(output) || (require_pcore && !affinity))
      throw std::invalid_argument("Require input, new output, reps 1..1000, and explicit affinity for P-core");
    if (affinity) {
      atx::vol::bench::pin_to_logical_cpu(*affinity);
      if (require_pcore && atx::vol::bench::is_performance_cpu(*affinity) != true)
        throw std::invalid_argument("Requested CPU is not a proven performance core");
    }
    const auto groups = read_groups(input);
    Json cases = Json::array();
    for (const auto &g : groups)
      for (const double spot : g.spots)
        cases.push_back({{"group", g.id}, {"S", spot}, {"K", g.K}, {"T", g.T},
                         {"sigma", g.sigma}, {"r", g.r}, {"q", g.q}});
    const auto before = atx::vol::bench::sample_cpus();
    Json checks = Json::array(), engines = Json::array();
    engines.push_back(measure_public<FastPut<double, 7, 15, 41>>("simu_fast", groups,
        cases.size(), reps, 3, {{"chebyshev_order", 7}, {"boundary_points", 8},
        {"fixed_point_quadrature", 15}, {"price_quadrature", 41}, {"iterations", 3}}, checks));
    engines.push_back(measure_public<FastPut<double, 16, 31, 61>>("simu_finer", groups,
        cases.size(), reps, 16, {{"chebyshev_order", 16}, {"boundary_points", 17},
        {"fixed_point_quadrature", 31}, {"price_quadrature", 61}, {"iterations", 16}}, checks));
    engines.push_back(measure_atx("atx_default", groups, cases.size(), reps, std::nullopt));
    engines.push_back(measure_atx("atx_bulk_fast", groups, cases.size(), reps,
                                 atx::vol::al_fast_profile(atx::vol::al_bulk_opts())));
    const auto after = atx::vol::bench::sample_cpus();
    const AlOpts ref1{.n_collocation = 24, .n_quadrature = 48, .n_quad_price = 64,
                     .max_newton_iter = 16, .tol = 1e-12};
    const AlOpts ref2{.n_collocation = 32, .n_quadrature = 64, .n_quad_price = 64,
                     .max_newton_iter = 32, .tol = 1e-14};
    Json references = Json::array();
    for (const auto &g : groups) {
      for (const double spot : g.spots) {
        const double p1 = atx_price(g, spot, ref1), p2 = atx_price(g, spot, ref2);
        references.push_back({{"price", json_number(p2)},
          {"refinement_difference", json_number(std::abs(p2 - p1))},
          {"closed_form", json_number(g.r <= 0 && g.q >= 0 ? european_put(g, spot) : kNan)}});
      }
    }
    Json build = atx::vol::bench::build_info_json();
    build["cmake_interprocedural_optimization"] = ATX_PUBLIC_COMPARATOR_IPO != 0;
    const Json report{{"schema", "american-put-public-comparator-v1"},
      {"upstream_commit", kUpstreamCommit}, {"case_count", cases.size()},
      {"group_count", groups.size()}, {"cases", cases}, {"engines", engines},
      {"upstream_adapter_checks", checks}, {"references", references},
      {"reference_profiles", {opts_json(ref1), opts_json(ref2)}},
      {"reference_independent_general_american", false},
      {"build", build},
      {"process_identity", atx::vol::bench::process_identity_json()},
      {"affinity", atx::vol::bench::affinity_json(affinity, require_pcore, before, after)}};
    std::ofstream stream(output);
    stream << report.dump(2) << '\n';
    if (!stream) throw std::runtime_error("Cannot write output");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
