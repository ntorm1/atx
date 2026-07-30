#pragma once

// Opt-in phase timers for backtest diagnostics. ATX_VOL_PROFILE is OFF by
// default. In an OFF build ATX_VOL_PROFILE_SCOPE expands to ((void)0), timing
// arguments are not evaluated, and no clock/atomic storage is compiled.

#include <cstdint>

#if defined(ATX_VOL_PROFILE)
#include <array>
#include <atomic>
#include <chrono>
#endif

namespace atx::vol::phase_profile {

enum class Region : unsigned {
  BacktestTotal = 0,
  SnapshotLoad,
  ArchiveOpen,
  ArchiveMap,
  StrategyStep,
  StrategyBuildBook,
  StrategyEntryMarks,
  Execution,
  EntryRisk,
  HedgeRisk,
  StepPnl,
  BookGreeks,
  Signals,
  Count_,
};

inline constexpr unsigned kCount = static_cast<unsigned>(Region::Count_);
inline constexpr const char *kNames[kCount] = {
    "backtest_total",
    "snapshot_load",
    "archive_open",
    "archive_map",
    "strategy_step",
    "strategy_build_book",
    "strategy_entry_marks",
    "execution",
    "entry_risk",
    "hedge_risk",
    "step_pnl",
    "book_greeks",
    "signals",
};

struct Snapshot {
  bool enabled{false};
  std::uint64_t calls[kCount]{};
  std::uint64_t nanoseconds[kCount]{};
};

[[nodiscard]] constexpr bool profile_enabled() noexcept {
#if defined(ATX_VOL_PROFILE)
  return true;
#else
  return false;
#endif
}

#if defined(ATX_VOL_PROFILE)

namespace detail {
inline std::array<std::atomic<std::uint64_t>, kCount> g_calls{};
inline std::array<std::atomic<std::uint64_t>, kCount> g_nanoseconds{};
} // namespace detail

class Scope {
public:
  explicit Scope(Region region) noexcept
      : region_{region}, started_{std::chrono::steady_clock::now()} {}
  ~Scope() noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const unsigned index = static_cast<unsigned>(region_);
    detail::g_calls[index].fetch_add(1u, std::memory_order_relaxed);
    detail::g_nanoseconds[index].fetch_add(static_cast<std::uint64_t>(ns),
                                           std::memory_order_relaxed);
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

private:
  Region region_;
  std::chrono::steady_clock::time_point started_;
};

inline void reset() noexcept {
  for (unsigned i = 0; i < kCount; ++i) {
    detail::g_calls[i].store(0u, std::memory_order_relaxed);
    detail::g_nanoseconds[i].store(0u, std::memory_order_relaxed);
  }
}

[[nodiscard]] inline Snapshot snapshot() noexcept {
  Snapshot result;
  result.enabled = true;
  for (unsigned i = 0; i < kCount; ++i) {
    result.calls[i] = detail::g_calls[i].load(std::memory_order_relaxed);
    result.nanoseconds[i] = detail::g_nanoseconds[i].load(std::memory_order_relaxed);
  }
  return result;
}

#define ATX_VOL_PROFILE_CONCAT_INNER(a, b) a##b
#define ATX_VOL_PROFILE_CONCAT(a, b) ATX_VOL_PROFILE_CONCAT_INNER(a, b)
#define ATX_VOL_PROFILE_SCOPE(region)                                                              \
  ::atx::vol::phase_profile::Scope ATX_VOL_PROFILE_CONCAT(atx_vol_profile_scope_, __LINE__)(       \
      ::atx::vol::phase_profile::Region::region)

#else

inline void reset() noexcept {}
[[nodiscard]] inline Snapshot snapshot() noexcept { return Snapshot{}; }
#define ATX_VOL_PROFILE_SCOPE(region) ((void)0)

#endif

} // namespace atx::vol::phase_profile
