#pragma once

// The spo digest procedures (FNV-1a over bit patterns), shared by strategy_spo_pin_test.cpp
// (SpoPin: spo-v1 as pre-registered) and strategy_spo_v3_pin_test.cpp (SpoV3: spo-v1 and
// spo-v2 under the spo-v3 code). Moved verbatim out of strategy_spo_pin_test.cpp by platform
// v8 R-6; the fold order is the pinned one:
//   * planned_weights: every planned weight (bit pattern) of two books (S1, S2) planned in
//     lockstep through the v7 seam (v7::plan: rebalance and hold decisions, carried without
//     drift), then the decision fields of each plan, then the calibrated gamma;
//   * replay: the NAV replay of the S2 book (planned gross/net/turnover, net return, traded
//     and cost dollars, long/short dollars, post-trade NAV per day), then the leading
//     `columns` columns of spo_diagnostics.csv.
// Uses ONLY the API of the pre-R6 tree (the capture protocols of both pin files): FROZEN once
// pinned -- a change moves the pinned digests without any change to a rule.

#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "atx/core/types.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "strategy_spo_fixture.hpp"

namespace atx::impl::strategy::spo::digest {

inline constexpr atx::u64 fnv_basis = 0xcbf29ce484222325ULL;
inline constexpr atx::u64 fnv_prime = 0x100000001b3ULL;
// replay(..., all_columns) folds every column of spo_diagnostics.csv.
inline constexpr atx::usize all_columns = std::numeric_limits<atx::usize>::max();

[[nodiscard]] inline atx::u64 fnv_byte(atx::u64 h, atx::u64 byte) {
  return (h ^ (byte & 0xffU)) * fnv_prime;
}
[[nodiscard]] inline atx::u64 fnv_word(atx::u64 h, atx::u64 word) {
  for (atx::u32 b = 0; b < 8; ++b) h = fnv_byte(h, word >> (8U * b));
  return h;
}
// NaN canonicalized (its payload is not part of the rule).
[[nodiscard]] inline atx::u64 fold(atx::u64 h, atx::f64 x) {
  return fnv_word(h, std::isnan(x) ? 0x7ff8000000000000ULL : fixture::bits(x));
}
[[nodiscard]] inline atx::u64 fold_text(atx::u64 h, std::string_view text) {
  for (const char c : text)
    h = fnv_byte(h, static_cast<atx::u64>(static_cast<unsigned char>(c)));
  return h;
}
// The first `columns` comma-separated fields of every line of a CSV.
[[nodiscard]] inline std::string leading_columns(const std::string& csv, atx::usize columns) {
  std::string out;
  atx::usize field = 0;
  bool keep = true;
  for (const char c : csv) {
    if (c == '\n') { out += '\n'; field = 0; keep = true; continue; }
    if (c == ',' && ++field >= columns) keep = false;
    if (keep) out += c;
  }
  return out;
}

// A clean risk model of `role` (fixture::write_risk_model, every date forecast) and its store.
struct Model {
  fixture::Directory dir;
  std::shared_ptr<const RiskStore> risk; // nullptr if the store refused
};
[[nodiscard]] inline std::unique_ptr<Model> model_of(const fixture::Role& role, atx::u64 seed) {
  auto m = std::make_unique<Model>();
  const std::vector<atx::u8> forecast(role.d, atx::u8{1});
  const auto sha =
      fixture::write_risk_model(m->dir.path, role.sessions, role.n, forecast, "role-sha", seed);
  auto store = RiskStore::open(m->dir.path.string(), sha, "role-sha");
  if (store) m->risk = std::make_shared<const RiskStore>(std::move(*store));
  return m;
}

struct Digest {
  atx::u64 value{};
  atx::usize count{}; // diagnostics rows (planned_weights) or replay days (replay)
  std::string error;  // empty on success
};

// Two books (S1, S2) planned in lockstep through v7::plan under `options` on `role`.
[[nodiscard]] inline Digest planned_weights(const v7::NavV7Options& options,
                                            const fixture::Role& role) {
  const v7::ScopedNavExtension extension(options);
  const auto s2 = fixture::nav_config();
  auto s1 = s2;
  s1.scenario = fixed_nav_scenarios()[0];
  const auto x = role.target();
  const atx::usize n = role.n;
  std::vector<atx::f64> w1(n, 0.0), w2(n, 0.0), desired(n);
  atx::u64 h = fnv_basis;
  for (atx::usize d = 0; d < role.d; ++d) {
    // Desired: the members' signal, demeaned over the members (nonmembers 0).
    atx::f64 sum = 0;
    atx::usize members = 0;
    for (atx::usize i = 0; i < n; ++i) {
      desired[i] = role.member[d * n + i] ? role.signal[d * n + i] - 0.5 : 0.0;
      if (role.member[d * n + i]) { sum += desired[i]; ++members; }
    }
    for (atx::usize i = 0; i < n; ++i)
      if (role.member[d * n + i]) desired[i] -= sum / static_cast<atx::f64>(members);
    const bool rebalance = d % 3 != 2; // every third decision holds (exits only)
    for (auto* book : {&w1, &w2}) {
      const auto& cfg = book == &w1 ? s1 : s2;
      TargetReplayDay day;
      const auto status = v7::plan(x, cfg, d, rebalance, 0.0, 1e8, desired, *book, day, {});
      if (!status) return Digest{0, 0, std::to_string(d) + ": " + status.error().to_string()};
      for (const atx::f64 w : *book) h = fold(h, w);
      for (const atx::f64 v : {day.turnover, day.gross, day.net, day.long_weight, day.short_weight})
        h = fold(h, v);
      h = fnv_word(h, day.held_names);
    }
  }
  const auto* engine = extension.spo_engine();
  if (engine == nullptr) return Digest{0, 0, "no spo engine"};
  if (engine->rows().empty()) return Digest{0, 0, "no spo diagnostics rows"};
  h = fold(h, engine->calibration().gamma);
  return Digest{h, engine->rows().size(), {}};
}

// The NAV replay of `role` (fixture::nav_config) under `options`, and the leading `columns`
// columns of its spo_diagnostics.csv.
[[nodiscard]] inline Digest replay(const v7::NavV7Options& options, const fixture::Role& role,
                                   atx::usize columns) {
  const v7::ScopedNavExtension extension(options);
  const auto result = replay_nav(role.nav(), fixture::nav_config());
  if (!result) return Digest{0, 0, result.error().to_string()};
  atx::u64 h = fnv_basis;
  for (const auto& day : result->days) {
    h = fnv_word(h, static_cast<atx::u64>(day.session));
    for (const atx::f64 v : {day.planned_gross, day.planned_net, day.planned_turnover,
                             day.net_return, day.traded_dollars, day.trade_cost_dollars,
                             day.long_dollars, day.short_dollars, day.posttrade_nav})
      h = fold(h, v);
  }
  const auto* engine = extension.spo_engine();
  if (engine == nullptr) return Digest{0, 0, "no spo engine"};
  h = fold_text(h, leading_columns(diagnostics_csv(engine->rows()), columns));
  return Digest{h, result->days.size(), {}};
}
} // namespace atx::impl::strategy::spo::digest
