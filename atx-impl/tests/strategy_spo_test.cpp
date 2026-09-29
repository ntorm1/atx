// spo-v1 (platform v7 W1): the solver on synthetic factor-model problems (KKT certificate,
// closed-form Markowitz, constraint feasibility, warm/cold determinism, a 1-D brute-force
// check of the 3/2 cost), the pinned atx-risk-v1 store (refusals), and the NAV hook
// (flag-off identity, CLI routing, an end-to-end replay on a synthetic role + risk model).

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"

namespace {
using namespace atx;
namespace co = atx::core;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace v7 = atx::impl::strategy::v7;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();

u64 bits(f64 x) { return std::bit_cast<u64>(x); }
struct Lcg {
  u64 state;
  f64 next() { // uniform [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11U) * 0x1.0p-53;
  }
  f64 uniform(f64 lo, f64 hi) { return lo + (hi - lo) * next(); }
};

// ---- synthetic problems ----------------------------------------------------------------------
// n names on market + 4 industries (every fifth name has none) + 3 styles; F = diag + v v'
// (PSD); D_i = (1..3%)^2; GK alpha; a gross budget that binds; boxes around w0.
sp::Problem random_problem(usize n, u64 seed) {
  Lcg rng{seed};
  sp::Problem p;
  p.layout = sp::FactorLayout{4, 3};
  p.n = n;
  const usize k = p.layout.factors();
  p.industry.resize(n); p.styles.resize(n * 3); p.specific.resize(n); p.alpha.resize(n);
  p.beta.resize(n); p.w0.resize(n); p.lower.resize(n); p.upper.resize(n);
  p.linear_cost.assign(n, 5e-5); p.impact_cost.resize(n); p.long_rate.assign(n, 1e-5);
  p.short_rate.resize(n);
  std::vector<f64> v(k);
  for (f64& x : v) x = rng.uniform(-2e-3, 2e-3);
  p.covariance.assign(k * k, 0.0);
  for (usize a = 0; a < k; ++a) {
    const f64 var = a == 0 ? 1e-4 : a <= 4 ? 2.5e-5 : 1e-5;
    for (usize b = 0; b < k; ++b) p.covariance[a * k + b] = v[a] * v[b] + (a == b ? var : 0.0);
  }
  p.fixed_exposure.assign(k, 0.0);
  for (usize i = 0; i < n; ++i) {
    p.industry[i] = static_cast<u32>(i % 5);
    for (usize c = 0; c < 3; ++c) p.styles[i * 3 + c] = rng.uniform(-1.5, 1.5);
    const f64 vol = rng.uniform(0.01, 0.03);
    p.specific[i] = vol * vol;
    p.alpha[i] = 0.02 * vol * rng.uniform(-1.7, 1.7);
    p.beta[i] = rng.uniform(0.5, 1.5);
    p.w0[i] = rng.uniform(-0.01, 0.01);
    p.lower[i] = std::max(-0.02, p.w0[i] - 0.006);
    p.upper[i] = std::min(0.02, p.w0[i] + 0.006);
    p.impact_cost[i] = rng.uniform(0.0, 2e-3);
    p.short_rate[i] = rng.uniform(2e-5, 6e-5);
  }
  p.gamma = 40; p.net = 0; p.gross = 0.25; p.beta_lo = -0.03; p.beta_hi = 0.03;
  return p;
}
sp::SolverOptions options(usize iterations, f64 tolerance) {
  sp::SolverOptions o;
  o.max_iterations = iterations; o.tolerance = tolerance;
  return o;
}
f64 max_abs_diff(std::span<const f64> a, std::span<const f64> b) {
  f64 m = 0;
  for (usize i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
  return m;
}
// Dense Sigma = X F X' + D of a problem (tests only).
std::vector<f64> dense_sigma(const sp::Problem& p) {
  const usize n = p.n, k = p.layout.factors(), first = 1 + p.layout.industries;
  std::vector<f64> x(n * k, 0.0), sigma(n * n, 0.0);
  for (usize i = 0; i < n; ++i) {
    x[i * k] = 1.0;
    if (p.industry[i] != 0) x[i * k + p.industry[i]] = 1.0;
    for (usize c = 0; c < p.layout.styles; ++c)
      x[i * k + first + c] = p.styles[i * p.layout.styles + c];
  }
  for (usize i = 0; i < n; ++i)
    for (usize j = 0; j < n; ++j) {
      f64 s = i == j ? p.specific[i] : 0.0;
      for (usize a = 0; a < k; ++a)
        for (usize b = 0; b < k; ++b) s += x[i * k + a] * p.covariance[a * k + b] * x[j * k + b];
      sigma[i * n + j] = s;
    }
  return sigma;
}
// Solves A x = b for symmetric positive definite A (Cholesky).
std::vector<f64> spd_solve(std::vector<f64> a, std::vector<f64> b) {
  const usize n = b.size();
  for (usize j = 0; j < n; ++j) {
    for (usize k = 0; k < j; ++k) a[j * n + j] -= a[j * n + k] * a[j * n + k];
    if (!(a[j * n + j] > 0)) throw std::runtime_error("not positive definite");
    a[j * n + j] = std::sqrt(a[j * n + j]);
    for (usize i = j + 1; i < n; ++i) {
      for (usize k = 0; k < j; ++k) a[i * n + j] -= a[i * n + k] * a[j * n + k];
      a[i * n + j] /= a[j * n + j];
    }
  }
  for (usize i = 0; i < n; ++i) {
    for (usize k = 0; k < i; ++k) b[i] -= a[i * n + k] * b[k];
    b[i] /= a[i * n + i];
  }
  for (usize ii = n; ii-- > 0;) {
    for (usize k = ii + 1; k < n; ++k) b[ii] -= a[k * n + ii] * b[k];
    b[ii] /= a[ii * n + ii];
  }
  return b;
}

// ---- solver --------------------------------------------------------------------------------
TEST(SpoSolver, KktResidualBelowToleranceOnFiftyNames) {
  const auto p = random_problem(50, 11);
  const auto o = options(20000, 1e-9);
  const auto sol = sp::solve(p, p.w0, o);
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  EXPECT_TRUE(sol->coupling_met);
  EXPECT_LE(sol->residual, o.tolerance);
  EXPECT_TRUE(sol->multipliers.gross_binding); // the budget binds: the coupled prox is exercised
  const auto r = sp::kkt_residual(p, sol->w, sol->metric_scale);
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_LT(*r, 1.01 * o.tolerance);
  // The certificate is not vacuous: w0 itself is far from optimal.
  const auto r0 = sp::kkt_residual(p, p.w0, sol->metric_scale);
  ASSERT_TRUE(r0);
  EXPECT_GT(*r0, 1e3 * o.tolerance);
}
TEST(SpoSolver, CostFreeUnboundedSolutionIsTheClosedFormMarkowitz) {
  auto p = random_problem(50, 23);
  const usize n = p.n;
  p.linear_cost.assign(n, 0.0); p.impact_cost.assign(n, 0.0);
  p.long_rate.assign(n, 0.0); p.short_rate.assign(n, 0.0);
  p.w0.assign(n, 0.0); p.lower.assign(n, -inf); p.upper.assign(n, inf);
  p.gross = inf; p.beta_lo = -inf; p.beta_hi = inf; p.net = 0; p.gamma = 100;
  const auto sol = sp::solve(p, std::vector<f64>(n, 0.0), options(50000, 1e-13));
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  // max a'w - gamma/2 w'Sigma w s.t. 1'w = 0: w = Sigma^-1 (a - nu 1) / gamma,
  // nu = 1'Sigma^-1 a / 1'Sigma^-1 1.
  const auto sigma = dense_sigma(p);
  const auto x = spd_solve(sigma, p.alpha);
  const auto y = spd_solve(sigma, std::vector<f64>(n, 1.0));
  f64 sx = 0, sy = 0;
  for (usize i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; }
  std::vector<f64> w(n);
  f64 scale = 0;
  for (usize i = 0; i < n; ++i) {
    w[i] = (x[i] - sx / sy * y[i]) / p.gamma;
    scale = std::max(scale, std::abs(w[i]));
  }
  ASSERT_GT(scale, 1e-4);
  EXPECT_LT(max_abs_diff(sol->w, w), 1e-7 * scale) << "scale " << scale;
  f64 net = 0;
  for (const f64 v : sol->w) net += v;
  EXPECT_LT(std::abs(net), 1e-12);
}
TEST(SpoSolver, NetGrossBetaAndBoxesHoldTo1e10) {
  auto p = random_problem(60, 5);
  // Fixed positions outside the problem: net -0.004 held there, so the problem's net is +0.004;
  // a tight beta band and a beta-tilted alpha make the beta constraint bind.
  p.net = 0.004; p.gross = 0.2; p.beta_lo = -0.002; p.beta_hi = 0.002;
  for (usize i = 0; i < p.n; ++i) p.alpha[i] += 3e-4 * (p.beta[i] - 1.0);
  const auto sol = sp::solve(p, p.w0, options(20000, 1e-9));
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  EXPECT_TRUE(sol->coupling_met);
  f64 net = 0, gross = 0, beta = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 w = sol->w[i];
    EXPECT_GE(w, p.lower[i]) << i;
    EXPECT_LE(w, p.upper[i]) << i;
    net += w; gross += std::abs(w); beta += p.beta[i] * w;
  }
  EXPECT_LT(std::abs(net - p.net), 1e-10);
  EXPECT_LE(gross, p.gross + 1e-10);
  EXPECT_GE(beta, p.beta_lo - 1e-10);
  EXPECT_LE(beta, p.beta_hi + 1e-10);
  EXPECT_TRUE(sol->multipliers.gross_binding);
  EXPECT_NE(sol->multipliers.rho, 0.0); // the beta band binds
}
TEST(SpoSolver, WarmAndColdStartsAgreeAndRepeatBitForBit) {
  const auto p = random_problem(50, 17);
  const auto o = options(50000, 1e-11);
  const auto warm = sp::solve(p, p.w0, o);
  const auto cold = sp::solve(p, std::vector<f64>(p.n, 0.0), o);
  ASSERT_TRUE(warm && cold);
  EXPECT_TRUE(warm->converged && cold->converged);
  EXPECT_LT(max_abs_diff(warm->w, cold->w), 1e-8);
  const auto again = sp::solve(p, p.w0, o);
  ASSERT_TRUE(again);
  ASSERT_EQ(again->w.size(), warm->w.size());
  for (usize i = 0; i < p.n; ++i) EXPECT_EQ(bits(again->w[i]), bits(warm->w[i])) << i;
  EXPECT_EQ(again->iterations, warm->iterations);
}
TEST(SpoSolver, TwoNamesMatchAOneDimensionalSearchWithTheThreeHalvesCost) {
  for (const u64 seed : {3ULL, 8ULL, 21ULL}) {
    auto p = random_problem(2, seed);
    p.gross = inf; p.beta_lo = -inf; p.beta_hi = inf; p.net = 0;
    p.lower = {-0.05, -0.05}; p.upper = {0.05, 0.05};
    p.w0 = {0.004, -0.001};
    p.linear_cost = {2e-5, 4e-5}; p.impact_cost = {3e-2, 1e-2};
    p.alpha[0] = 4e-4; p.alpha[1] = -2e-4;
    const auto sol = sp::solve(p, p.w0, options(50000, 1e-13));
    ASSERT_TRUE(sol) << sol.error().to_string();
    // Net 0: w = (u, -u); golden section on the strictly convex -objective(u).
    const auto cost = [&](f64 u) {
      const std::vector<f64> w{u, -u};
      return -sp::objective_terms(p, w).objective;
    };
    f64 lo = -0.05, hi = 0.05;
    const f64 g = 0.5 * (std::sqrt(5.0) - 1.0);
    f64 a = hi - g * (hi - lo), b = lo + g * (hi - lo), fa = cost(a), fb = cost(b);
    for (int k = 0; k < 400; ++k) {
      if (fa < fb) { hi = b; b = a; fb = fa; a = hi - g * (hi - lo); fa = cost(a); }
      else { lo = a; a = b; fa = fb; b = lo + g * (hi - lo); fb = cost(b); }
    }
    const f64 u = 0.5 * (lo + hi);
    EXPECT_NEAR(sol->w[0], u, 1e-7) << seed;
    EXPECT_NEAR(sol->w[1], -sol->w[0], 1e-12) << seed;
    EXPECT_LE(cost(sol->w[0]), cost(u) + 1e-15) << seed;
  }
}

// ---- synthetic atx-risk-v1 output ------------------------------------------------------------
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_spo_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("fixture directory");
  }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};
template <class T>
Json write_payload(const std::filesystem::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
// The files the `risk` verb writes with --emit-exposures all, for `sessions` x `names`;
// forecast[d] == 0 leaves date d unforecast (NaN covariance and specific variance).
// Returns the manifest's SHA-256.
std::string write_risk_model(const std::filesystem::path& dir, std::span<const i64> sessions,
                             usize names, std::span<const u8> forecast, const std::string& role,
                             u64 seed) {
  Lcg rng{seed};
  const usize dates = sessions.size(), k = sp::risk_factors, s = sp::risk_styles;
  std::vector<f64> cov(dates * k * k, missing), spec(dates * names, missing);
  std::vector<f32> styles(dates * names * s);
  std::vector<u8> slots(dates * names);
  std::vector<f64> v(k);
  for (f64& x : v) x = rng.uniform(-1e-3, 1e-3);
  for (usize d = 0; d < dates; ++d) {
    for (usize i = 0; i < names; ++i) {
      slots[d * names + i] = static_cast<u8>(i % 5);
      for (usize c = 0; c < s; ++c)
        styles[(d * names + i) * s + c] = static_cast<f32>(rng.uniform(-1.5, 1.5));
      if (forecast[d]) {
        const f64 vol = 0.012 + 0.004 * static_cast<f64>(i % 4);
        spec[d * names + i] = vol * vol;
      }
    }
    if (!forecast[d]) continue;
    for (usize a = 0; a < k; ++a) {
      const f64 var = a == 0 ? 1e-4 : a <= sp::risk_industry_slots ? 2.5e-5 : 1e-5;
      for (usize b = 0; b < k; ++b)
        cov[(d * k + a) * k + b] = v[a] * v[b] + (a == b ? var : 0.0);
    }
  }
  Json files = Json::object();
  files["factor_covariance.f64"] = write_payload(dir / "factor_covariance.f64", cov);
  files["specific_variance.f64"] = write_payload(dir / "specific_variance.f64", spec);
  files["style_exposures.f32"] = write_payload(dir / "style_exposures.f32", styles);
  files["industry_slot.u8"] = write_payload(dir / "industry_slot.u8", slots);
  {
    std::ofstream csv(dir / "diagnostics.csv", std::ios::binary);
    csv << "session,forecast,lambda2_factor,lambda2_specific,bias_factor,bias_specific,"
           "structural_names,specific_names\n";
    for (usize d = 0; d < dates; ++d)
      csv << sessions[d] << ',' << (forecast[d] ? 1 : 0) << ",1,1,nan,nan,0," << names << '\n';
  }
  files["diagnostics.csv"] =
      Json{{"bytes", std::filesystem::file_size(dir / "diagnostics.csv")},
           {"sha256", co::sha256_file((dir / "diagnostics.csv").string()).value()}};
  const Json manifest{{"schema", "atx.risk-model/v1"}, {"status", "complete"},
                      {"role", {{"path", "role/manifest.json"}, {"manifest_sha256", role}}},
                      {"geometry", {{"dates", dates}, {"instruments", names}, {"factors", k},
                                    {"styles", s}}},
                      {"files", files}};
  {
    std::ofstream out(dir / "manifest.json", std::ios::binary);
    out << manifest.dump(2) << '\n';
  }
  return co::sha256_file((dir / "manifest.json").string()).value();
}
std::vector<i64> weekdays(usize count) {
  std::vector<i64> out;
  auto date = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2};
  while (out.size() < count) {
    const std::chrono::weekday wd{date};
    if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
      out.push_back(static_cast<i64>(date.time_since_epoch().count()) * day_ns);
    date += std::chrono::days{1};
  }
  return out;
}

TEST(SpoRisk, RefusesADateWithoutForecastAnotherRoleAndAnotherPin) {
  const Directory dir;
  const auto sessions = weekdays(3);
  const std::vector<u8> forecast{0, 1, 1};
  const auto sha = write_risk_model(dir.path, sessions, 4, forecast, "role-sha", 7);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  EXPECT_EQ(store->dates(), 3U);
  EXPECT_EQ(store->instruments(), 4U);
  sp::RiskSlice slice;
  const auto lacking = store->read(0, slice);
  ASSERT_FALSE(lacking);
  EXPECT_EQ(lacking.error().code(), co::ErrorCode::Unavailable);
  EXPECT_NE(lacking.error().message().find("no forecast"), std::string::npos);
  ASSERT_TRUE(store->read(1, slice));
  EXPECT_EQ(slice.session, sessions[1]);
  EXPECT_EQ(slice.covariance.size(), sp::risk_factors * sp::risk_factors);
  EXPECT_EQ(slice.nan_covariance_entries, 0U);
  EXPECT_EQ(slice.slot[3], 3U);
  EXPECT_FALSE(store->read(3, slice)); // beyond the model
  EXPECT_TRUE(store->check_axes(sessions, 4));
  EXPECT_FALSE(store->check_axes(sessions, 5));
  EXPECT_FALSE(sp::RiskStore::open(dir.path.string(), sha, "another-role"));
  EXPECT_FALSE(sp::RiskStore::open(dir.path.string(), std::string(64, '0'), "role-sha"));
  ASSERT_TRUE(std::filesystem::remove(dir.path / "style_exposures.f32")); // not "all"
  EXPECT_FALSE(sp::RiskStore::open(dir.path.string(), sha, "role-sha"));
}

// ---- the NAV hook ------------------------------------------------------------------------------
// Random-walk role: every name present, ADV rising with the index, name n-1 leaves membership
// for a stretch (the exit path). Signals random per cell.
struct Role {
  usize d{}, n{};
  std::vector<f64> signal, close, raw, volume;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Role(usize dates, usize names, u64 seed)
      : d(dates), n(names), signal(dates * names), close(dates * names), raw(dates * names),
        volume(dates * names), member(dates * names, 1), present(dates * names, 1),
        sessions(weekdays(dates)), ids(names) {
    Lcg rng{seed};
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
    for (usize t = 0; t < d; ++t)
      for (usize i = 0; i < n; ++i) {
        const usize k = t * n + i;
        close[k] = t == 0 ? 100.0 : close[k - n] * (1.0 + 0.04 * (rng.next() - 0.5));
        raw[k] = close[k];
        volume[k] = 1e5 * static_cast<f64>(1 + 2 * i) * (0.5 + rng.next());
        signal[k] = rng.next();
      }
    for (usize t = d / 3; t < d / 2; ++t) {
      member[t * n + n - 1] = 0; signal[t * n + n - 1] = missing;
    }
  }
  [[nodiscard]] st::TargetReplayInput target() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present, volume};
  }
  [[nodiscard]] st::NavReplayInput nav() const { return {target(), volume}; }
};
st::NavReplayConfig nav_config() {
  st::NavReplayConfig c;
  c.target.rule = st::TargetReplayRule::AimPartialV5; c.target.cadence = 1;
  c.target.trade_fraction = 0.25; c.target.dust_multiple = 0.1; c.target.aim_leverage = 1.2;
  c.target.exit_rate = 0.05;
  c.scenario = st::fixed_nav_scenarios()[st::nav_primary_scenario_index];
  c.initial_nav = 1e8; c.liquidity_window = 4; c.min_vol_pairs = 2;
  return c;
}
atx::core::Result<v7::NavV7Command> parse_v7(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
}
bool claims(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::claims_nav_args(static_cast<int>(argv.size()), argv.data());
}

TEST(SpoHook, FlagOffKeepsAimPartialV5BitForBit) {
  const Role role(40, 12, 31);
  const auto cfg = nav_config();
  // The seam with the new tier / locate spans and no extension is update_weights bit for bit.
  const auto x = role.target();
  Lcg rng{9};
  const std::vector<u8> tier(role.n, u8{3}), no_locate(role.n, u8{1});
  for (const usize d : {5ULL, 17ULL, 30ULL}) {
    std::vector<f64> desired(role.n, 0.0), current(role.n);
    f64 mean = 0;
    for (usize i = 0; i < role.n; ++i) {
      desired[i] = role.member[d * role.n + i] ? rng.next() : 0.0;
      mean += desired[i];
      current[i] = 0.02 * (rng.next() - 0.5);
    }
    for (f64& v : desired) v -= mean / static_cast<f64>(role.n);
    std::vector<f64> plain = current, hooked = current;
    st::TargetReplayDay a, b;
    ASSERT_TRUE(st::detail::update_weights(x, cfg.target, d, true, 0.0, desired, plain, a));
    ASSERT_TRUE(v7::plan(x, cfg, d, true, 0.0, 1e8, desired, hooked, b, {}, tier, no_locate));
    for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(plain[i]), bits(hooked[i])) << d << ' ' << i;
    EXPECT_EQ(bits(a.turnover), bits(b.turnover));
    EXPECT_EQ(bits(a.gross), bits(b.gross));
    EXPECT_EQ(a.construction.banded_names, b.construction.banded_names);
  }
  // A whole replay with an extension that only observes (no rule) keeps every planned weight.
  const auto plain = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(plain) << plain.error().to_string();
  const v7::ScopedNavExtension extension(v7::NavV7Options{});
  EXPECT_EQ(extension.spo_engine(), nullptr);
  const auto hooked = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(hooked) << hooked.error().to_string();
  ASSERT_EQ(plain->days.size(), hooked->days.size());
  for (usize t = 0; t < plain->days.size(); ++t) {
    const auto& p = plain->days[t]; const auto& h = hooked->days[t];
    EXPECT_EQ(bits(p.planned_gross), bits(h.planned_gross)) << t;
    EXPECT_EQ(bits(p.planned_net), bits(h.planned_net)) << t;
    EXPECT_EQ(bits(p.planned_turnover), bits(h.planned_turnover)) << t;
    EXPECT_EQ(bits(p.net_return), bits(h.net_return)) << t;
    EXPECT_EQ(p.construction.banded_names, h.construction.banded_names) << t;
  }
  // Without --rule spo-v1 the parse leaves the rule and every token alone.
  const auto parsed = parse_v7({"nav", "--rule", "aim-partial-v5", "--output", "x"});
  ASSERT_TRUE(parsed);
  EXPECT_FALSE(parsed->options.spo_v1);
  EXPECT_EQ(parsed->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5", "--output",
                                                    "x"}));
}
TEST(SpoHook, ParseRoutesTheRuleAndRefusesBadCombinations) {
  const std::vector<std::string> base{"nav", "--rule", "spo-v1", "--risk-model", "risk",
                                      "--risk-model-sha256", "abc", "--trade-fraction", ".05",
                                      "--output", "x"};
  const auto with = [&](std::initializer_list<std::string> extra) {
    auto args = base;
    args.insert(args.end(), extra.begin(), extra.end());
    return args;
  };
  const auto parsed =
      parse_v7(with({"--gamma", "5", "--spo-books", "primary", "--spo-iters", "300"}));
  ASSERT_TRUE(parsed) << parsed.error().to_string();
  const auto& o = parsed->options;
  EXPECT_TRUE(o.spo_v1); EXPECT_FALSE(o.aim_v6);
  EXPECT_EQ(o.spo_params.gamma, 5.0);
  EXPECT_FALSE(o.spo_params.all_books);
  EXPECT_EQ(o.spo_params.max_iterations, 300U);
  EXPECT_EQ(o.spo_params.ic_book, 0.02); EXPECT_EQ(o.spo_params.w_max, 0.01);
  EXPECT_EQ(o.spo_params.adv_cap_q, 0.05); EXPECT_EQ(o.spo_params.adv_trade_p, 0.01);
  EXPECT_EQ(o.spo_params.tolerance, 1e-8); EXPECT_EQ(o.spo_params.target_vol, 0.05);
  EXPECT_TRUE(std::isnan(o.spo_params.horizon)); // 1 / theta at the first decision
  EXPECT_EQ(parsed->risk_model, "risk");
  EXPECT_EQ(parsed->risk_model_sha256, "abc");
  EXPECT_EQ(parsed->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5",
                                                    "--trade-fraction", ".05", "--output", "x"}));
  EXPECT_TRUE(claims({"nav", "--rule", "spo-v1"}));
  EXPECT_TRUE(claims({"nav", "--gamma", "1"}));
  EXPECT_FALSE(parse_v7({"nav", "--rule", "spo-v1", "--output", "x"})); // no risk model
  EXPECT_FALSE(parse_v7({"nav", "--gamma", "5", "--output", "x"}));     // no rule
  EXPECT_FALSE(parse_v7(with({"--capacity-curve"})));
  EXPECT_FALSE(parse_v7(with({"--rate", "per-name-v1"})));
  EXPECT_FALSE(parse_v7(with({"--spo-books", "some"})));
  EXPECT_FALSE(parse_v7(with({"--spo-iters", "-1"})));
  EXPECT_FALSE(parse_v7(with({"--ic-book", "0"})));
  EXPECT_FALSE(parse_v7(with({"--gamma", "1", "--gamma", "2"})));
  std::vector<std::string> both = base;
  both.insert(both.end(), {"--rule", "aim-partial-v6"});
  EXPECT_FALSE(parse_v7(both));
}
TEST(SpoHook, ReplayPlansNeutralBudgetedBooksAndRelabelsTheRule) {
  const Directory dir;
  const Role role(40, 12, 53);
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 3);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const auto cfg = nav_config();
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(result) << result.error().to_string();
  const auto* engine = extension.spo_engine();
  ASSERT_NE(engine, nullptr);
  EXPECT_TRUE(engine->calibration().done);
  EXPECT_GT(engine->calibration().gamma, 0.0);
  EXPECT_EQ(engine->horizon(), 4.0); // 1 / theta
  const auto rows = engine->rows();
  ASSERT_FALSE(rows.empty());
  f64 traded = 0;
  for (const auto& r : rows) {
    EXPECT_TRUE(r.converged) << r.session;
    EXPECT_TRUE(r.coupling_met) << r.session;
    EXPECT_LE(r.gross, cfg.target.aim_leverage + 1e-9) << r.session;
    EXPECT_LT(std::abs(r.net), 1e-9) << r.session;
    EXPECT_LE(r.abs_beta, 0.02 + 1e-9) << r.session;
    EXPECT_TRUE(std::isfinite(r.exante_vol)) << r.session;
    traded += r.turnover;
  }
  EXPECT_GT(traded, 0.0);
  for (const auto& day : result->days)
    if (day.decision && day.rebalance) EXPECT_LT(std::abs(day.planned_net), 1e-9) << day.session;
  EXPECT_FALSE(extension.tc_records().empty()); // the L4 transfer coefficient is still recorded
  const auto csv = sp::diagnostics_csv(rows);
  EXPECT_EQ(static_cast<usize>(std::count(csv.begin(), csv.end(), '\n')), rows.size() + 1);
  Json recipe{{"rule", "aim-partial-v5+neutral-price-risk-v1"}};
  v7::extend_recipe(recipe);
  EXPECT_EQ(recipe["rule"], "spo-v1+neutral-price-risk-v1");
  ASSERT_TRUE(recipe.contains("v7"));
  EXPECT_TRUE(recipe["v7"].contains("spo_v1"));
  // Deterministic: a second replay under a fresh extension plans the same weights.
  const v7::ScopedNavExtension again(o);
  const auto repeat = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(repeat);
  ASSERT_EQ(repeat->days.size(), result->days.size());
  for (usize t = 0; t < result->days.size(); ++t)
    EXPECT_EQ(bits(repeat->days[t].planned_gross), bits(result->days[t].planned_gross)) << t;
}
TEST(SpoHook, ReplayRefusesWhenTheRiskModelLacksADecisionDate) {
  const Directory dir;
  const Role role(30, 10, 61);
  std::vector<u8> forecast(role.d, u8{1});
  forecast[12] = 0;
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 5);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), nav_config());
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message().find("no forecast"), std::string::npos)
      << result.error().to_string();
}
} // namespace
