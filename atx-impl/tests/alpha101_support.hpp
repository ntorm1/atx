#pragma once

// Support utilities for the Alpha101 ORATS verification harness
// (alpha101_orats_test.cpp). Header-only; every symbol is `inline` so several
// test TUs may include it without ODR violations.
//
// The "101 Formulaic Alphas" (Kakushadze 2016, arXiv:1601.00991) reference a
// fixed input vocabulary: open high low close volume vwap returns cap adv{d}
// and the GICS group classifiers IndClass.{sector,industry,subindustry}. The
// ATX ORATS history panel materializes only a subset (open high low close
// volume market_cap sector ...). `augment_for_alpha101` takes such a base panel
// and returns one that carries EVERY field the 101 reference, so each canonical
// formula can be parsed/compiled/evaluated verbatim:
//
//   * returns        = close[t]/close[t-1] - 1   (causal; NaN on day 0 / gaps)
//   * cap            = market_cap (copied) — synthetic close*1e8 only if absent
//   * IndClass.sector/.industry/.subindustry = the GICS `sector` code (widened
//                      f64 group label). industry/subindustry are DOCUMENTED
//                      stand-ins: the ORATS panel materializes only the GICS
//                      SECTOR granularity, so the finer classifiers reuse it.
//                      This verifies the DSL/typecheck/VM path for industry-
//                      neutralization; it is NOT a claim of GICS-industry
//                      fidelity.
//   * dollar_volume / vwap / adv{d} = appended by alpha::datafields (the same
//                      derivation the engine uses; adv{d} == ts_mean(dvol,d)).
//
// `make_synth_orats_panel` builds a deterministic, multi-sector, long-history
// panel (>= 260 dates) so the long-lookback alphas (e.g. ts_sum(returns,250))
// yield finite cells without requiring the real ORATS zip — this is the CI /
// default panel. The real-data run sets ATX_ALPHA101_PANEL to a serialized APNL
// panel built from the ORATS zip via the `load`+`panel` stages.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx_impl_test {

using atx::engine::alpha::Panel;

inline constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// One alpha read from the fixture: its paper id (1..101) and DSL expression.
struct FixtureAlpha {
  int id{};
  std::string dsl;
};

// Read `<id>: <dsl>` lines from a fixture file. Lines whose first non-space
// character is '#' are comments; blank lines are skipped. Order is preserved.
[[nodiscard]] inline std::vector<FixtureAlpha> read_alpha_fixture(const std::string &path) {
  std::vector<FixtureAlpha> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos || line[b] == '#') {
      continue;
    }
    const std::size_t colon = line.find(':', b);
    if (colon == std::string::npos) {
      continue;
    }
    FixtureAlpha fa;
    fa.id = std::atoi(line.substr(b, colon - b).c_str());
    std::size_t e = line.size();
    while (e > colon + 1 && (line[e - 1] == '\r' || line[e - 1] == '\n' || line[e - 1] == ' ' ||
                             line[e - 1] == '\t')) {
      --e;
    }
    std::size_t s = colon + 1;
    while (s < e && (line[s] == ' ' || line[s] == '\t')) {
      ++s;
    }
    fa.dsl = line.substr(s, e - s);
    if (!fa.dsl.empty()) {
      out.push_back(std::move(fa));
    }
  }
  return out;
}

// Scan the fixture DSL strings for every `adv<digits>` token and return the
// sorted, de-duplicated set of windows so the panel materializes exactly the
// adv columns the alphas reference (whatever the transcription chose).
[[nodiscard]] inline std::vector<atx::u16>
collect_adv_windows(const std::vector<FixtureAlpha> &fx) {
  std::set<unsigned> windows;
  for (const FixtureAlpha &f : fx) {
    const std::string &s = f.dsl;
    for (std::size_t i = 0; i + 3 < s.size(); ++i) {
      const bool boundary = (i == 0) || (std::isalnum(static_cast<unsigned char>(s[i - 1])) == 0 &&
                                         s[i - 1] != '_' && s[i - 1] != '.');
      if (!boundary || s.compare(i, 3, "adv") != 0) {
        continue;
      }
      std::size_t j = i + 3;
      unsigned v = 0;
      bool any = false;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) {
        v = v * 10U + static_cast<unsigned>(s[j] - '0');
        ++j;
        any = true;
      }
      if (any && v >= 1 && v <= 0xFFFFU) {
        windows.insert(v);
      }
    }
  }
  std::vector<atx::u16> out;
  out.reserve(windows.size());
  for (const unsigned w : windows) {
    out.push_back(static_cast<atx::u16>(w));
  }
  return out;
}

namespace detail {

[[nodiscard]] inline atx::usize index_of(const std::vector<std::string> &names,
                                         std::string_view name) noexcept {
  for (atx::usize i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      return i;
    }
  }
  return static_cast<atx::usize>(-1);
}

[[nodiscard]] inline bool has(const std::vector<std::string> &names,
                              std::string_view name) noexcept {
  return index_of(names, name) != static_cast<atx::usize>(-1);
}

} // namespace detail

// Return a panel carrying every Alpha101 input field, derived from `base` (which
// must provide at least open/high/low/close/volume). `adv_windows` is the set of
// adv{d} columns to materialize (see collect_adv_windows).
[[nodiscard]] inline atx::core::Result<Panel>
augment_for_alpha101(const Panel &base, std::span<const atx::u16> adv_windows) {
  const atx::usize D = base.dates();
  const atx::usize I = base.instruments();
  const atx::usize cells = D * I;

  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  names.reserve(base.num_fields() + 8);
  data.reserve(base.num_fields() + 8);
  for (atx::usize f = 0; f < base.num_fields(); ++f) {
    names.emplace_back(base.field_name(f));
    const std::span<const atx::f64> col =
        base.field_all(static_cast<atx::engine::alpha::FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }

  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < I; ++n) {
      universe[d * I + n] = base.in_universe(d, n) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }

  const atx::usize close_i = detail::index_of(names, "close");
  if (close_i == static_cast<atx::usize>(-1)) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "augment_for_alpha101: base panel has no 'close' field");
  }

  // returns = close[t]/close[t-1] - 1, per instrument (date-major stride I).
  if (!detail::has(names, "returns")) {
    const std::vector<atx::f64> &close = data[close_i];
    std::vector<atx::f64> returns(cells, kNaN);
    for (atx::usize n = 0; n < I; ++n) {
      for (atx::usize d = 1; d < D; ++d) {
        const atx::usize i = d * I + n;
        const atx::usize p = (d - 1) * I + n;
        if (universe[i] != 0 && universe[p] != 0) {
          const atx::f64 c = close[i];
          const atx::f64 pc = close[p];
          returns[i] = (pc != 0.0) ? (c / pc - 1.0) : kNaN; // NaN inputs propagate
        }
      }
    }
    names.emplace_back("returns");
    data.push_back(std::move(returns));
  }

  // cap = market_cap (copied). Fall back to close*1e8 only if market_cap absent.
  if (!detail::has(names, "cap")) {
    const atx::usize mc_i = detail::index_of(names, "market_cap");
    std::vector<atx::f64> cap(cells, kNaN);
    if (mc_i != static_cast<atx::usize>(-1)) {
      cap = data[mc_i];
    } else {
      const std::vector<atx::f64> &close = data[close_i];
      for (atx::usize i = 0; i < cells; ++i) {
        if (universe[i] != 0) {
          cap[i] = close[i] * 1.0e8;
        }
      }
    }
    names.emplace_back("cap");
    data.push_back(std::move(cap));
  }

  // Group classifiers from the GICS sector code (widened f64 label). Absent ->
  // a single constant group (label 0) so the group-aware ops still run.
  {
    const atx::usize sec_i = detail::index_of(names, "sector");
    std::vector<atx::f64> sec(cells, 0.0);
    if (sec_i != static_cast<atx::usize>(-1)) {
      sec = data[sec_i];
    }
    for (std::string_view g : {"IndClass.sector", "IndClass.industry", "IndClass.subindustry"}) {
      if (!detail::has(names, g)) {
        names.emplace_back(g);
        data.push_back(sec);
      }
    }
  }

  // Append dollar_volume / vwap / adv{d} via the engine's own derivation.
  return atx::engine::alpha::datafields::with_datafields(
      D, I, std::move(names), std::move(data), std::move(universe), adv_windows);
}

// Deterministic, multi-sector, long-history ORATS-shaped panel. Random-walk
// closes (so returns/correlations are non-degenerate), >= `dates` rows so the
// 250-day-lookback alphas produce finite cells, >= 6 sectors for group ops.
// Fields: open high low close volume sector market_cap (all in-universe).
[[nodiscard]] inline Panel make_synth_orats_panel(atx::usize dates = 300, atx::usize instruments = 24) {
  const atx::usize D = dates;
  const atx::usize I = instruments;
  const atx::usize cells = D * I;

  std::vector<std::string> names = {"open", "high",   "low",       "close",
                                    "volume", "sector", "market_cap"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(cells, 0.0));

  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  auto next = [&state]() noexcept {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(state >> 11) / static_cast<atx::f64>(1ULL << 53);
  };

  std::vector<atx::f64> px(I);
  for (atx::usize n = 0; n < I; ++n) {
    px[n] = 20.0 + 80.0 * next();
  }
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < I; ++n) {
      const atx::usize i = d * I + n;
      px[n] *= (1.0 + (next() - 0.5) * 0.04); // random-walk close, +/-2%/day
      const atx::f64 close = px[n];
      const atx::f64 open = close * (1.0 + (next() - 0.5) * 0.01);
      const atx::f64 spread = close * (0.005 + 0.02 * next());
      cols[0][i] = open;
      cols[1][i] = std::max(open, close) + spread;       // high
      cols[2][i] = std::min(open, close) - spread;       // low
      cols[3][i] = close;
      cols[4][i] = 1.0e5 + 9.0e6 * next();               // volume
      cols[5][i] = static_cast<atx::f64>(n % 6);          // sector: 6 groups
      cols[6][i] = close * (1.0e7 + 5.0e7 * next());      // market_cap
    }
  }

  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  auto p = Panel::create(D, I, std::move(names), std::move(cols), std::move(universe));
  // create() only fails on ragged input, which we never produce here.
  return std::move(p).value();
}

} // namespace atx_impl_test
