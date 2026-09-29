#pragma once

// Shared fixtures of the spo tests (strategy_spo_test.cpp, strategy_spo_pin_test.cpp): a
// seeded LCG, a unique temporary directory, the files the `risk` verb writes with
// --emit-exposures all, and a random-walk role with the NAV replay config the hook tests use.
// Everything here is part of the spo-v1 digest pin (strategy_spo_pin_test.cpp): FROZEN once
// pinned -- a change moves the pinned digests without any change to the rule.

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"

namespace atx::impl::strategy::spo::fixture {

inline constexpr atx::i64 day_ns = 86'400'000'000'000LL;
inline constexpr atx::f64 missing = std::numeric_limits<atx::f64>::quiet_NaN();
inline constexpr atx::f64 inf = std::numeric_limits<atx::f64>::infinity();

[[nodiscard]] inline atx::u64 bits(atx::f64 x) { return std::bit_cast<atx::u64>(x); }
struct Lcg {
  atx::u64 state;
  atx::f64 next() { // uniform [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(state >> 11U) * 0x1.0p-53;
  }
  atx::f64 uniform(atx::f64 lo, atx::f64 hi) { return lo + (hi - lo) * next(); }
};

struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<atx::u64> counter{0};
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
nlohmann::json write_payload(const std::filesystem::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return nlohmann::json{{"bytes", bytes.size()},
                        {"sha256", atx::core::sha256_file(file.string()).value()}};
}
// The files the `risk` verb writes with --emit-exposures all, for `sessions` x `names`;
// forecast[d] == 0 leaves date d unforecast (NaN covariance and specific variance);
// specific_overrides sets entries (d x names + i, value) of the specific variance.
// Returns the manifest's SHA-256.
inline std::string write_risk_model(
    const std::filesystem::path& dir, std::span<const atx::i64> sessions, atx::usize names,
    std::span<const atx::u8> forecast, const std::string& role, atx::u64 seed,
    std::span<const std::pair<atx::usize, atx::f64>> specific_overrides = {}) {
  using atx::f64;
  using atx::usize;
  using Json = nlohmann::json;
  Lcg rng{seed};
  const usize dates = sessions.size(), k = risk_factors, s = risk_styles;
  std::vector<f64> cov(dates * k * k, missing), spec(dates * names, missing);
  std::vector<atx::f32> styles(dates * names * s);
  std::vector<atx::u8> slots(dates * names);
  std::vector<f64> v(k);
  for (f64& x : v) x = rng.uniform(-1e-3, 1e-3);
  for (usize d = 0; d < dates; ++d) {
    for (usize i = 0; i < names; ++i) {
      slots[d * names + i] = static_cast<atx::u8>(i % 5);
      for (usize c = 0; c < s; ++c)
        styles[(d * names + i) * s + c] = static_cast<atx::f32>(rng.uniform(-1.5, 1.5));
      if (forecast[d]) {
        const f64 vol = 0.012 + 0.004 * static_cast<f64>(i % 4);
        spec[d * names + i] = vol * vol;
      }
    }
    if (!forecast[d]) continue;
    for (usize a = 0; a < k; ++a) {
      const f64 var = a == 0 ? 1e-4 : a <= risk_industry_slots ? 2.5e-5 : 1e-5;
      for (usize b = 0; b < k; ++b)
        cov[(d * k + a) * k + b] = v[a] * v[b] + (a == b ? var : 0.0);
    }
  }
  for (const auto& [cell, value] : specific_overrides) spec.at(cell) = value;
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
           {"sha256", atx::core::sha256_file((dir / "diagnostics.csv").string()).value()}};
  const Json manifest{{"schema", "atx.risk-model/v1"}, {"status", "complete"},
                      {"role", {{"path", "role/manifest.json"}, {"manifest_sha256", role}}},
                      {"geometry", {{"dates", dates}, {"instruments", names}, {"factors", k},
                                    {"styles", s}}},
                      {"files", files}};
  {
    std::ofstream out(dir / "manifest.json", std::ios::binary);
    out << manifest.dump(2) << '\n';
  }
  return atx::core::sha256_file((dir / "manifest.json").string()).value();
}
inline std::vector<atx::i64> weekdays(atx::usize count) {
  std::vector<atx::i64> out;
  auto date = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2};
  while (out.size() < count) {
    const std::chrono::weekday wd{date};
    if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
      out.push_back(static_cast<atx::i64>(date.time_since_epoch().count()) * day_ns);
    date += std::chrono::days{1};
  }
  return out;
}

// Random-walk role: every name present, ADV rising with the index, name n-1 leaves membership
// for a stretch (the exit path). Signals random per cell.
struct Role {
  atx::usize d{}, n{};
  std::vector<atx::f64> signal, close, raw, volume;
  std::vector<atx::u8> member, present;
  std::vector<atx::i64> sessions;
  std::vector<atx::u64> ids;
  Role(atx::usize dates, atx::usize names, atx::u64 seed)
      : d(dates), n(names), signal(dates * names), close(dates * names), raw(dates * names),
        volume(dates * names), member(dates * names, 1), present(dates * names, 1),
        sessions(weekdays(dates)), ids(names) {
    Lcg rng{seed};
    for (atx::usize i = 0; i < n; ++i) ids[i] = 100 + i;
    for (atx::usize t = 0; t < d; ++t)
      for (atx::usize i = 0; i < n; ++i) {
        const atx::usize k = t * n + i;
        close[k] = t == 0 ? 100.0 : close[k - n] * (1.0 + 0.04 * (rng.next() - 0.5));
        raw[k] = close[k];
        volume[k] = 1e5 * static_cast<atx::f64>(1 + 2 * i) * (0.5 + rng.next());
        signal[k] = rng.next();
      }
    for (atx::usize t = d / 3; t < d / 2; ++t) {
      member[t * n + n - 1] = 0; signal[t * n + n - 1] = missing;
    }
  }
  [[nodiscard]] TargetReplayInput target() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present, volume};
  }
  [[nodiscard]] NavReplayInput nav() const { return {target(), volume}; }
};
[[nodiscard]] inline NavReplayConfig nav_config() {
  NavReplayConfig c;
  c.target.rule = TargetReplayRule::AimPartialV5; c.target.cadence = 1;
  c.target.trade_fraction = 0.25; c.target.dust_multiple = 0.1; c.target.aim_leverage = 1.2;
  c.target.exit_rate = 0.05;
  c.scenario = fixed_nav_scenarios()[nav_primary_scenario_index];
  c.initial_nav = 1e8; c.liquidity_window = 4; c.min_vol_pairs = 2;
  return c;
}
} // namespace atx::impl::strategy::spo::fixture
