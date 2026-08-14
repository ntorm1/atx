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

// ── Build-configuration tag: the ODR guard for this header (plan 5.2) ────────
//
// Same contract, and same reasoning, as atx/vol/detail/counters.hpp — read the
// long note there. In short: `profile_enabled()`, `snapshot()` and `reset()` are
// inline entities whose DEFINITION depends on `ATX_VOL_PROFILE`, so a TU and a
// library that disagree used to put two definitions of one entity into a single
// program. Naming the configuration in an INLINE namespace makes them different
// entities (no ODR violation to form) while leaving every existing unqualified
// spelling resolving exactly as before, and the guard symbol called from
// `snapshot()`/`reset()` turns a mismatched pair into a link error naming the
// configuration instead of a silent wrong answer.
//
// `Region`, `kCount`, `kNames` and `Snapshot` above are identical in both
// configurations and stay untagged, so a `Snapshot` still crosses freely.
#if defined(ATX_VOL_PROFILE)
#define ATX_VOL_PROFILE_ABI_TAG profile_on
#else
#define ATX_VOL_PROFILE_ABI_TAG profile_off
#endif

inline namespace ATX_VOL_PROFILE_ABI_TAG {

namespace detail {
// Defined once, in src/instrumentation_abi.cpp, under the tag the LIBRARY was
// compiled with. Empty body: the symbol's existence is the assertion.
void assert_build_configuration_matches() noexcept;
} // namespace detail

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
  detail::assert_build_configuration_matches();
  for (unsigned i = 0; i < kCount; ++i) {
    detail::g_calls[i].store(0u, std::memory_order_relaxed);
    detail::g_nanoseconds[i].store(0u, std::memory_order_relaxed);
  }
}

[[nodiscard]] inline Snapshot snapshot() noexcept {
  detail::assert_build_configuration_matches();
  Snapshot result;
  result.enabled = true;
  for (unsigned i = 0; i < kCount; ++i) {
    result.calls[i] = detail::g_calls[i].load(std::memory_order_relaxed);
    result.nanoseconds[i] = detail::g_nanoseconds[i].load(std::memory_order_relaxed);
  }
  return result;
}

#else

inline void reset() noexcept { detail::assert_build_configuration_matches(); }
[[nodiscard]] inline Snapshot snapshot() noexcept {
  detail::assert_build_configuration_matches();
  return Snapshot{};
}

#endif

} // inline namespace ATX_VOL_PROFILE_ABI_TAG (profile_on / profile_off)

#undef ATX_VOL_PROFILE_ABI_TAG

// ── The macros this header leaves defined, in EVERY configuration ────────────
//
//   ATX_VOL_PROFILE_SCOPE(region)             the RAII timer (public)
//   ATX_VOL_PROFILE_DETAIL_CONCAT(a, b)       internal, ##-through-expansion
//   ATX_VOL_PROFILE_DETAIL_CONCAT_INNER(a, b) internal, the ## itself
//
// That is the complete set (verified by preprocessing this header with and
// without ATX_VOL_PROFILE and diffing `-dM` against a baseline TU including the
// same standard headers). Two hygiene properties are deliberate:
//
//   * the two CONCAT helpers carry _DETAIL_ in the name. They cannot be
//     `#undef`ed -- macro expansion is lazy, so they must still be defined at
//     the SCOPE macro's expansion site, not merely at its definition site -- so
//     naming them as internals is the available fix. They were plain
//     ATX_VOL_PROFILE_CONCAT / _CONCAT_INNER, which read like API.
//   * all three are defined in BOTH configurations. Previously the two helpers
//     existed only under ATX_VOL_PROFILE, so the header handed a consumer a
//     DIFFERENT preprocessor environment depending on a build option -- the
//     same class of defect as the ODR trap above, one level down: a consumer
//     that defined its own ATX_VOL_PROFILE_CONCAT compiled fine until somebody
//     flipped the option. Uniform costs the OFF build two unused macro names and
//     makes that collision fail immediately instead of eventually.
//
// The SCOPE macro needs the concatenation because the RAII object must have a
// unique name: two timers in one scope, or a timer in a scope that already has
// one, would otherwise redeclare or (-Wshadow, which is -Werror here) shadow it.
#define ATX_VOL_PROFILE_DETAIL_CONCAT_INNER(a, b) a##b
#define ATX_VOL_PROFILE_DETAIL_CONCAT(a, b) ATX_VOL_PROFILE_DETAIL_CONCAT_INNER(a, b)

#if defined(ATX_VOL_PROFILE)
#define ATX_VOL_PROFILE_SCOPE(region)                                                              \
  ::atx::vol::phase_profile::Scope ATX_VOL_PROFILE_DETAIL_CONCAT(atx_vol_profile_scope_,           \
                                                                 __LINE__)(                        \
      ::atx::vol::phase_profile::Region::region)
#else
#define ATX_VOL_PROFILE_SCOPE(region) ((void)0)
#endif

} // namespace atx::vol::phase_profile
