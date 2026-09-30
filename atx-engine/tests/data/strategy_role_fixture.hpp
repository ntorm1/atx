#pragma once

// strategy_role_fixture.hpp -- a synthetic atx.recent-research-role/v1 directory for the
// read_strategy_role tests (strategy_data_test.cpp, data_research_window_test.cpp).
// Header-only; no real data. kD sessions on consecutive calendar days from `first_day` (days
// since the epoch), two instruments, score window [384, kD), score_end_ns one day after the
// last session. Instrument 0 is absent at session 385 (NaN prices, or finite ones when
// `finite_absent` plants a contract violation).

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"

namespace atx_test_strategy_role {
inline constexpr atx::usize kD = 390, kN = 2;
inline constexpr atx::i64 kDay = 86'400'000'000'000LL;
inline constexpr atx::i64 kFirstDay = 17683; // 2018-06-01

struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<unsigned> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      auto candidate = std::filesystem::temp_directory_path() /
          ("atx-strategy-role-" + std::to_string(stamp) + "-" +
           std::to_string(sequence.fetch_add(1)));
      if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); break; }
    }
  }
  ~Directory() { if (!path.empty()) { std::error_code e; std::filesystem::remove_all(path, e); } }
};

template<class T> bool payload(const Directory& dir, nlohmann::json& files, const char* name,
                               const std::vector<T>& values) {
  const auto path = dir.path / name;
  std::ofstream file(path, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  file.close();
  const auto sha = atx::core::sha256_file(path.string());
  if (!sha) return false;
  files[name] = {{"bytes", bytes.size()}, {"sha256", *sha}};
  return true;
}

inline bool fixture(const Directory& dir, bool finite_absent = false,
                    atx::i64 first_day = kFirstDay) {
  using namespace atx;
  using Json = nlohmann::json;
  if (dir.path.empty()) return false;
  std::vector<i64> sessions(kD);
  for (usize d = 0; d < kD; ++d) sessions[d] = (first_day + static_cast<i64>(d)) * kDay;
  std::vector<u8> present(kD * kN, 1), member(kD * kN, 0);
  for (usize d = 63; d < kD; ++d) member[d * kN] = 1;
  present[385 * kN] = 0;
  std::vector<f64> close(kD * kN, 20), raw(kD * kN, 10), volume(kD * kN, 1000000);
  close[385 * kN] = raw[385 * kN] = volume[385 * kN] =
      finite_absent ? 777 : std::numeric_limits<f64>::quiet_NaN();
  Json files;
  if (!payload(dir, files, "sessions.i64", sessions) ||
      !payload(dir, files, "ids.u64", std::vector<u64>{7, 81}) ||
      !payload(dir, files, "present.u8", present) || !payload(dir, files, "member.u8", member) ||
      !payload(dir, files, "close.f64", close) || !payload(dir, files, "raw_close.f64", raw) ||
      !payload(dir, files, "volume.f64", volume)) return false;
  const Json membership{{"rule", "research-prior63-usd-adv-topn-v1"}, {"top_n", 3000},
    {"lookback_sessions", 63}, {"lag_sessions", 1}, {"min_raw_price_exclusive", 5},
    {"min_adv_exclusive", 5000000}, {"ties", "securityID-ascending"},
    {"missing", "complete-prior-calendar-window-required"}, {"common_stock_verified", false}};
  const Json manifest{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
    {"instrument_namespace", "spiderrock.securityID"}, {"dates", kD}, {"instruments", kN},
    {"score_begin", 384}, {"score_end", kD}, {"score_start_ns", sessions[384]},
    {"score_end_ns", sessions.back() + kDay}, {"source_sha256", std::string(64, 'a')},
    {"membership_recipe", membership.dump()},
    {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
    {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
    {"volume_basis", "raw-share-volume"},
    {"common_stock_verified", false}, {"historical_vintage_verified", false},
    {"declared_output_bytes", kD * kN * 26 + kD * 8 + kN * 8}, {"files", files}};
  std::ofstream out(dir.path / "manifest.json"); out << manifest.dump(2) << '\n'; return bool(out);
}
} // namespace atx_test_strategy_role
