#pragma once

// NavSpec (P9 C3; schema atx.nav-spec/v1): one typed JSON document holding a whole `nav` run.
//
//   {"schema": "atx.nav-spec/v1",
//    "options": {"combined": "PATH", "combined_sha256": "SHA", "role": ..., "output": "NEWDIR",
//                "rule": "aim-partial-v6", "cadence": 1, "trade_fraction": 0.25,
//                "cost_v2": true, "rate_clip": [0.5, 1.5], ...},
//    "constants": {"initial_nav": 1e9, "liquidity_window": 63, "min_vol_pairs": 20,
//                  "capacity_multiples": [0.5, 1, 2, 4, 8], "leverage_range": [1, 2]}}
//
// options: one key per command-line flag of the nav verb (both parsers: the replay's and the v7
// seam's), typed by the key table below (keys()): a switch is true (false = absent), a number is a
// finite JSON number, a count a non-negative JSON integer, a text a non-empty string, a choice one
// of its listed strings, a pair [lo, hi] of finite numbers (--rate-clip LO,HI). Unknown keys are
// refused. The flags are the one parser: a spec runs as the command line to_args renders (keys in
// table order; numbers in their shortest round-trip form, so every value parses to the same bits),
// through dispatch_nav_replay, so a spec and the flags it renders configure the same run and
// publish the same bytes. from_args is the flag adapter: the NavSpec of a command line (what
// `nav ... --print-spec` prints), so flags -> spec -> flags is the identity up to flag order.
//
// constants (optional; every key optional): the NAV's compiled constants, as fields with today's
// values. initial_nav (NavReplayConfig, also the "1bn" of the scenario ids), liquidity_window and
// min_vol_pairs (the execution liquidity window), capacity_multiples (cost_v2::capacity_multiples,
// the capacity file names) are accepted at today's values only: moving one is a new registered
// rule, not a spec edit. leverage_range [lo, hi] is the aim-partial-v5 leverage range
// (aim_leverage_min / aim_leverage_max, [1, 2]): a wider range is refused (owner decision
// OD-P9-3); a narrower one is checked against the run's aim_leverage (default 1) and refused with
// calibrate_gross (PM6-6's correction is bounded by [1, 2]) and with construction_grid (its
// variants set their own --aim-leverage). The range bounds the aim-leverage PARAMETER L, not the
// book's realised gross leverage L_t: under a leverage rule L_t moves around L (vol-target-v1
// runs L_t in [1, L], risk-target-v1 in [.8 L, 1.25 L], i.e. up to 2.5 at L = 2; Ruling C3-4: a
// cell whose rule can realise more than L states its cap on realised leverage in its
// registration).
//
// Print-only options (no weight, trade, cost or NAV value depends on them): stage_timers, and
// ann_mean_gross (Rulings PRE-7 / PRE-7a; --ann-mean-gross): each scenario of summary.json gains
// ann_mean_gross = 252 x the mean of the book's daily gross_return over the return rows that
// ann_mean averages (the same rows and the same running mean; summarize_nav). Absent, every
// output byte is the run without the option. P9 AL-CLOCK (Ruling ALCLOCK-1): info_clock_print
// (--info-clock-print) adds each scenario's info_clock block (NavEmitOptions::info_clock_print).
//
// The file is JSON with each key once per object: a repeated key is refused (the parser would
// keep the last value while a reader of the pinned file sees the first).
//
// CLI (dispatch, from dispatch_nav_replay before any other flag but --list-rules):
//   nav --spec NAVSPEC.json [--output NEWDIR]   the spec's run (--output replaces options.output);
//                                                the console's first line names the schema, the
//                                                path and the SHA-256 of the bytes parsed
//   nav <flags> --print-spec                     prints the NavSpec of <flags>; nothing runs
//
// Appending a key (AL-CLOCK, COV-MV; Ruling W3-9): one Key row in nav_spec.cpp's table, beside
// the flag's parser entry.

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy::nav_spec {

inline constexpr const char* schema = "atx.nav-spec/v1";
inline constexpr atx::u64 max_spec_bytes = 1ULL << 20;

enum class Type : atx::u8 { Switch = 0, Real = 1, Count = 2, Text = 3, Choice = 4, RealPair = 5 };
struct Key {
  std::string_view key;  // options key
  std::string_view flag; // its nav flag
  Type type{Type::Text};
  std::span<const std::string_view> choices; // Choice only
};
[[nodiscard]] std::span<const Key> keys() noexcept;
[[nodiscard]] const Key* by_key(std::string_view key) noexcept;
[[nodiscard]] const Key* by_flag(std::string_view flag) noexcept;

// The flag adapter: the NavSpec {schema, options} of a nav command line (`args` = the tokens after
// the verb). InvalidArgument for a flag outside the table, a repeated flag, a missing value, or a
// value of the wrong type (a number that does not parse whole or is not finite, a choice outside
// its list).
[[nodiscard]] atx::core::Result<nlohmann::json> from_args(std::span<const std::string> args);
// The command line of a NavSpec (no verb; keys in table order). InvalidArgument for any shape,
// key, type or constant the header refuses.
[[nodiscard]] atx::core::Result<std::vector<std::string>> to_args(const nlohmann::json& spec);
// A spec file: at most max_spec_bytes, JSON with no key repeated inside an object, then
// to_args's checks.
[[nodiscard]] atx::core::Result<nlohmann::json> read(const std::string& path);
// The constants block at today's values.
[[nodiscard]] nlohmann::json constants_json();

// nav --spec / --print-spec (above): not_spec when argv holds neither (the caller goes on), else
// the exit code (0 ok, 2 a usage error or a refused spec, else the run's own code).
inline constexpr int not_spec = -1;
[[nodiscard]] int dispatch(int argc, char** argv, std::ostream& out, std::ostream& err);

} // namespace atx::impl::strategy::nav_spec
