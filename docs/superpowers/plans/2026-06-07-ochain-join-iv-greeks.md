# OCHAIN Join + IV + Greeks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Join OPRA_BBO option quotes onto OHLC1M underlying quotes, enrich each row with ACT/252 time-to-expiry, mid-price implied vol, and Black-Scholes greeks, and write the result to the OCHAIN store — computed in parallel across all rows.

**Architecture:** Three pure, header/`.cpp` quant units (`osi`, `trading_calendar`, `black_scholes`) under `atx/engine/quant/`, each unit-tested in isolation, plus one `build_ochain` example driver that reads the two source partitions, joins them, fans the per-row compute out over `std::thread` chunks (each row writes a disjoint output slot → deterministic, lock-free), and writes the OCHAIN partition.

**Tech Stack:** C++20, GoogleTest (auto-globbed `*_test.cpp` in `atx-engine/tests/`), Arrow-backed Parquet via `atx::core::io`, `DiskStore` hive layout, clang-cl `/W4 /permissive- /WX` warnings gate.

**Spec:** `docs/superpowers/specs/2026-06-07-ochain-join-iv-greeks-design.md`

**Worktree/branch:** `feat/disk-data-layer` at `C:/Users/natha/atx-wt/disk-data-layer`.

**Build/test commands** (run from worktree root, MSVC env via vcvars):
```
cmd.exe /c "C:\Users\natha\atx-wt\disk-data-layer\_build_test.bat"
```
where `_build_test.bat` (already-known pattern) runs vcvars64 + `cmake --build build` + `ctest --test-dir build -C Debug --output-on-failure`. Subagents may instead invoke the existing `_regress.bat` for full build+ctest, or run the single test binary `build\atx-engine\tests\atx-engine-tests.exe --gtest_filter=...`.

## File Structure

| File | Responsibility |
|------|----------------|
| `atx-engine/include/atx/engine/quant/osi.hpp` | Parse 21-char OSI option symbol → root/expiry/type/strike (header-only, pure) |
| `atx-engine/include/atx/engine/quant/trading_calendar.hpp` | NYSE trading-day predicate + day count + ACT/252 (decls) |
| `atx-engine/src/quant/trading_calendar.cpp` | Calendar impl: Hinnant civil-date, rule-based holidays, holiday set |
| `atx-engine/include/atx/engine/quant/black_scholes.hpp` | norm_cdf/pdf, price, vega, greeks, implied_vol (header-only, pure) |
| `atx-engine/examples/build_ochain.cpp` | Join driver + parallel compute + OCHAIN write |
| `atx-engine/tests/osi_test.cpp` | OSI parser tests |
| `atx-engine/tests/trading_calendar_test.cpp` | Calendar tests |
| `atx-engine/tests/black_scholes_test.cpp` | BS / IV / greeks tests |
| `atx-engine/CMakeLists.txt` | Add `src/quant/trading_calendar.cpp` to library sources |
| `atx-engine/examples/CMakeLists.txt` | Add `build_ochain` executable |

---

## Task O1: OSI Symbol Parser

**Files:**
- Create: `atx-engine/include/atx/engine/quant/osi.hpp`
- Test: `atx-engine/tests/osi_test.cpp`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/osi_test.cpp`:
```cpp
#include "atx/engine/quant/osi.hpp"

#include <gtest/gtest.h>

namespace {
using atx::engine::quant::parse_osi;

TEST(Osi, ParsesStandardCall) {
  auto o = parse_osi("AAPL  260615C00322500");
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->root, "AAPL");
  EXPECT_EQ(o->year, 2026);
  EXPECT_EQ(o->month, 6);
  EXPECT_EQ(o->day, 15);
  EXPECT_TRUE(o->is_call);
  EXPECT_DOUBLE_EQ(o->strike, 322.5);
}

TEST(Osi, ParsesPutAndShortRoot) {
  auto o = parse_osi("XLF   261016P00047000");
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->root, "XLF");
  EXPECT_FALSE(o->is_call);
  EXPECT_DOUBLE_EQ(o->strike, 47.0);
  EXPECT_EQ(o->year, 2026);
  EXPECT_EQ(o->month, 10);
  EXPECT_EQ(o->day, 16);
}

TEST(Osi, ParsesDotStrippedRootAndFractionalStrike) {
  auto o = parse_osi("BRKB  271217C00058500");
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->root, "BRKB");
  EXPECT_DOUBLE_EQ(o->strike, 58.5);
  EXPECT_EQ(o->year, 2027);
}

TEST(Osi, RejectsWrongLength) {
  EXPECT_FALSE(parse_osi("AAPL 260615C0032250").has_value());   // 20 chars
  EXPECT_FALSE(parse_osi("").has_value());
}

TEST(Osi, RejectsBadTypeAndNonDigits) {
  EXPECT_FALSE(parse_osi("AAPL  260615X00322500").has_value());  // type X
  EXPECT_FALSE(parse_osi("AAPL  2606X5C00322500").has_value());  // non-digit date
  EXPECT_FALSE(parse_osi("AAPL  260615C003225X0").has_value());  // non-digit strike
}
} // namespace
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build` then run `atx-engine-tests --gtest_filter=Osi.*`
Expected: FAIL to compile (`osi.hpp` not found).

- [ ] **Step 3: Write the implementation**

Create `atx-engine/include/atx/engine/quant/osi.hpp`:
```cpp
#pragma once

// atx::engine::quant — parse an OSI (Options Symbology Initiative) 21-char
// option symbol "RRRRRRYYMMDDTSSSSSSSS": 6-char space-padded root, 6-digit
// YYMMDD expiry, 1-char type (C/P), 8-digit strike in thousandths of a dollar.
// Pure and header-only.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace atx::engine::quant {

struct OsiOption {
  std::string root;   // e.g. "AAPL", "BRKB"
  int year{};         // full year, e.g. 2027
  int month{};        // 1-12
  int day{};          // 1-31
  bool is_call{};
  double strike{};    // dollars, e.g. 250.0
};

[[nodiscard]] inline std::optional<OsiOption> parse_osi(std::string_view sym) {
  if (sym.size() != 21) {
    return std::nullopt;
  }
  const auto all_digits = [](std::string_view s) {
    for (const char c : s) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    return true;
  };
  if (!all_digits(sym.substr(6, 6)) || !all_digits(sym.substr(13, 8))) {
    return std::nullopt;
  }
  const char type = sym[12];
  if (type != 'C' && type != 'P') {
    return std::nullopt;
  }
  std::size_t root_end = 6;
  while (root_end > 0 && sym[root_end - 1] == ' ') {
    --root_end;
  }
  if (root_end == 0) {
    return std::nullopt;
  }
  const auto to_int = [](std::string_view s) {
    int v = 0;
    for (const char c : s) {
      v = v * 10 + (c - '0');
    }
    return v;
  };
  OsiOption o;
  o.root = std::string{sym.substr(0, root_end)};
  o.year = 2000 + to_int(sym.substr(6, 2));
  o.month = to_int(sym.substr(8, 2));
  o.day = to_int(sym.substr(10, 2));
  o.is_call = (type == 'C');
  o.strike = static_cast<double>(to_int(sym.substr(13, 8))) / 1000.0;
  return o;
}

} // namespace atx::engine::quant
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build` then `atx-engine-tests --gtest_filter=Osi.*`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/quant/osi.hpp atx-engine/tests/osi_test.cpp
git commit -m "feat(quant): OSI option-symbol parser"
```

---

## Task O2: NYSE Trading Calendar (ACT/252)

**Files:**
- Create: `atx-engine/include/atx/engine/quant/trading_calendar.hpp`
- Create: `atx-engine/src/quant/trading_calendar.cpp`
- Modify: `atx-engine/CMakeLists.txt` (add source to `atx-engine` library)
- Test: `atx-engine/tests/trading_calendar_test.cpp`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/trading_calendar_test.cpp`:
```cpp
#include "atx/engine/quant/trading_calendar.hpp"

#include <gtest/gtest.h>

namespace {
namespace cal = atx::engine::quant;

TEST(Calendar, WeekdaysAndWeekends) {
  EXPECT_TRUE(cal::is_trading_day(2026, 6, 5));   // Fri
  EXPECT_FALSE(cal::is_trading_day(2026, 6, 6));  // Sat
  EXPECT_FALSE(cal::is_trading_day(2026, 6, 7));  // Sun
}

TEST(Calendar, FixedAndFloatingHolidays) {
  EXPECT_FALSE(cal::is_trading_day(2026, 1, 1));    // New Year (Thu)
  EXPECT_FALSE(cal::is_trading_day(2026, 1, 19));   // MLK (3rd Mon)
  EXPECT_FALSE(cal::is_trading_day(2026, 4, 3));    // Good Friday
  EXPECT_FALSE(cal::is_trading_day(2026, 5, 25));   // Memorial (last Mon)
  EXPECT_FALSE(cal::is_trading_day(2026, 11, 26));  // Thanksgiving (4th Thu)
  EXPECT_FALSE(cal::is_trading_day(2026, 12, 25));  // Christmas (Fri)
}

TEST(Calendar, WeekendObservedHolidays) {
  // Jul 4 2026 is Saturday -> observed Friday Jul 3.
  EXPECT_FALSE(cal::is_trading_day(2026, 7, 3));
  EXPECT_TRUE(cal::is_trading_day(2026, 7, 4));   // the Saturday itself: not a trading day anyway
  // New Year exception: Jan 1 2028 is Saturday -> NOT observed; Dec 31 2027 stays open.
  EXPECT_TRUE(cal::is_trading_day(2027, 12, 31)); // Fri, open
}

TEST(Calendar, TradingDaysBetween) {
  EXPECT_EQ(cal::trading_days_between(2026, 6, 5, 2026, 6, 12), 5);   // one clear week
  EXPECT_EQ(cal::trading_days_between(2026, 12, 24, 2026, 12, 28), 1); // Christmas inside
  EXPECT_EQ(cal::trading_days_between(2026, 6, 5, 2026, 6, 5), 0);     // same day
  EXPECT_EQ(cal::trading_days_between(2026, 6, 12, 2026, 6, 5), 0);    // backwards
}

TEST(Calendar, Act252Years) {
  EXPECT_DOUBLE_EQ(cal::act252_years(2026, 6, 5, 2026, 6, 5), 0.0);
  EXPECT_DOUBLE_EQ(cal::act252_years(2026, 6, 5, 2026, 6, 12), 5.0 / 252.0);
}
} // namespace
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build`
Expected: FAIL to compile (`trading_calendar.hpp` not found).

- [ ] **Step 3a: Write the header**

Create `atx-engine/include/atx/engine/quant/trading_calendar.hpp`:
```cpp
#pragma once

// atx::engine::quant — NYSE trading-day calendar for ACT/252 time-to-expiry.
// Holidays are computed by rule (fixed-date with weekend shift, floating
// nth-weekday, Good Friday via Computus) for years 2025-2030. A day is a
// trading day if it is Mon-Fri and not an NYSE holiday.

namespace atx::engine::quant {

[[nodiscard]] bool is_trading_day(int y, int m, int d);

// Count trading days strictly after (y0,m0,d0) up to and including (y1,m1,d1).
// Returns 0 when the second date is not strictly after the first.
[[nodiscard]] int trading_days_between(int y0, int m0, int d0, int y1, int m1, int d1);

// trading_days_between / 252.0
[[nodiscard]] double act252_years(int y0, int m0, int d0, int y1, int m1, int d1);

} // namespace atx::engine::quant
```

- [ ] **Step 3b: Write the implementation**

Create `atx-engine/src/quant/trading_calendar.cpp`:
```cpp
#include "atx/engine/quant/trading_calendar.hpp"

#include <unordered_set>

namespace atx::engine::quant {
namespace {

// Howard Hinnant: days since 1970-01-01 (may be negative). m in [1,12].
constexpr long days_from_civil(int y, int m, int d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<long>(era) * 146097L + doe - 719468L;
}

// 0=Sunday .. 6=Saturday
constexpr int weekday(long z) {
  return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

// day-number of the n-th (1-based) `target` weekday (0=Sun..6=Sat) of month m.
long nth_weekday(int y, int m, int target, int n) {
  const long first = days_from_civil(y, m, 1);
  const int offset = (target - weekday(first) + 7) % 7;
  return first + offset + static_cast<long>(n - 1) * 7;
}

// day-number of the last `target` weekday of month m.
long last_weekday(int y, int m, int target) {
  const int ny = (m == 12) ? y + 1 : y;
  const int nm = (m == 12) ? 1 : m + 1;
  const long last = days_from_civil(ny, nm, 1) - 1;
  const int back = (weekday(last) - target + 7) % 7;
  return last - back;
}

// Easter Sunday day-number (Anonymous Gregorian Computus).
long easter(int y) {
  const int a = y % 19;
  const int b = y / 100;
  const int c = y % 100;
  const int dd = b / 4;
  const int e = b % 4;
  const int f = (b + 8) / 25;
  const int g = (b - f + 1) / 3;
  const int h = (19 * a + b - dd - g + 15) % 30;
  const int i = c / 4;
  const int k = c % 4;
  const int l = (32 + 2 * e + 2 * i - h - k) % 7;
  const int mth = (a + 11 * h + 22 * l) / 451;
  const int month = (h + l - 7 * mth + 114) / 31;
  const int day = ((h + l - 7 * mth + 114) % 31) + 1;
  return days_from_civil(y, month, day);
}

// Sat -> preceding Fri, Sun -> following Mon, else unchanged.
long observed_fixed(long dnum) {
  const int w = weekday(dnum);
  if (w == 6) {
    return dnum - 1;
  }
  if (w == 0) {
    return dnum + 1;
  }
  return dnum;
}

void add_year_holidays(std::unordered_set<long>& h, int y) {
  // New Year's Day with NYSE Saturday exception (when Jan 1 is Sat, the prior
  // Friday is NOT a holiday; only Sun shifts forward to Mon).
  const long nyd = days_from_civil(y, 1, 1);
  const int nw = weekday(nyd);
  if (nw == 0) {
    h.insert(nyd + 1);
  } else if (nw != 6) {
    h.insert(nyd);
  }
  h.insert(nth_weekday(y, 1, 1, 3));    // MLK Day (3rd Mon, Mon=1)
  h.insert(nth_weekday(y, 2, 1, 3));    // Washington's Birthday (3rd Mon)
  h.insert(easter(y) - 2);              // Good Friday
  h.insert(last_weekday(y, 5, 1));      // Memorial Day (last Mon)
  h.insert(observed_fixed(days_from_civil(y, 6, 19)));  // Juneteenth
  h.insert(observed_fixed(days_from_civil(y, 7, 4)));   // Independence Day
  h.insert(nth_weekday(y, 9, 1, 1));    // Labor Day (1st Mon)
  h.insert(nth_weekday(y, 11, 4, 4));   // Thanksgiving (4th Thu, Thu=4)
  h.insert(observed_fixed(days_from_civil(y, 12, 25))); // Christmas
}

const std::unordered_set<long>& holidays() {
  static const std::unordered_set<long> set = [] {
    std::unordered_set<long> h;
    for (int y = 2025; y <= 2030; ++y) {
      add_year_holidays(h, y);
    }
    return h;
  }();
  return set;
}

bool is_trading_day_z(long z) {
  const int w = weekday(z);
  if (w == 0 || w == 6) {
    return false;
  }
  return holidays().find(z) == holidays().end();
}

} // namespace

bool is_trading_day(int y, int m, int d) {
  return is_trading_day_z(days_from_civil(y, m, d));
}

int trading_days_between(int y0, int m0, int d0, int y1, int m1, int d1) {
  const long a = days_from_civil(y0, m0, d0);
  const long b = days_from_civil(y1, m1, d1);
  if (b <= a) {
    return 0;
  }
  int count = 0;
  for (long z = a + 1; z <= b; ++z) {
    if (is_trading_day_z(z)) {
      ++count;
    }
  }
  return count;
}

double act252_years(int y0, int m0, int d0, int y1, int m1, int d1) {
  return trading_days_between(y0, m0, d0, y1, m1, d1) / 252.0;
}

} // namespace atx::engine::quant
```

- [ ] **Step 3c: Register the source in the engine library**

Modify `atx-engine/CMakeLists.txt` — change the `add_library` source list from:
```cmake
add_library(atx-engine STATIC
    src/engine.cpp
    src/data/disk.cpp
)
```
to:
```cmake
add_library(atx-engine STATIC
    src/engine.cpp
    src/data/disk.cpp
    src/quant/trading_calendar.cpp
)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build` then `atx-engine-tests --gtest_filter=Calendar.*`
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/quant/trading_calendar.hpp \
        atx-engine/src/quant/trading_calendar.cpp \
        atx-engine/tests/trading_calendar_test.cpp \
        atx-engine/CMakeLists.txt
git commit -m "feat(quant): NYSE trading calendar + ACT/252 years"
```

---

## Task O3: Black-Scholes — Price, Vega, Greeks, Implied Vol

**Files:**
- Create: `atx-engine/include/atx/engine/quant/black_scholes.hpp`
- Test: `atx-engine/tests/black_scholes_test.cpp`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/black_scholes_test.cpp`:
```cpp
#include "atx/engine/quant/black_scholes.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace {
namespace bs = atx::engine::quant;

TEST(BlackScholes, AtmCallReference) {
  // S=K=100, T=1, r=q=0, sigma=0.2 -> 7.9655674...
  const double c = bs::bs_price(100, 100, 1.0, 0.0, 0.0, 0.2, true);
  EXPECT_NEAR(c, 7.9655674, 1e-4);
}

TEST(BlackScholes, PutCallParity) {
  const double S = 105, K = 100, T = 0.75, r = 0.043, q = 0.0, sig = 0.25;
  const double c = bs::bs_price(S, K, T, r, q, sig, true);
  const double p = bs::bs_price(S, K, T, r, q, sig, false);
  const double lhs = c - p;
  const double rhs = S * std::exp(-q * T) - K * std::exp(-r * T);
  EXPECT_NEAR(lhs, rhs, 1e-9);
}

TEST(BlackScholes, ImpliedVolRecoversSigma) {
  const double S = 98, K = 100, T = 0.5, r = 0.043, q = 0.0;
  for (const bool call : {true, false}) {
    for (const double sig : {0.1, 0.2, 0.45, 0.9}) {
      const double price = bs::bs_price(S, K, T, r, q, sig, call);
      const double iv = bs::implied_vol(price, S, K, T, r, q, call);
      EXPECT_NEAR(iv, sig, 1e-4) << "call=" << call << " sig=" << sig;
    }
  }
}

TEST(BlackScholes, GreeksMatchFiniteDifference) {
  const double S = 100, K = 95, T = 0.6, r = 0.043, q = 0.0, sig = 0.3;
  const bool call = true;
  const bs::Greeks g = bs::bs_greeks(S, K, T, r, q, sig, call);
  const double h = 1e-4;
  const double dS =
      (bs::bs_price(S + h, K, T, r, q, sig, call) - bs::bs_price(S - h, K, T, r, q, sig, call)) /
      (2 * h);
  const double d2S = (bs::bs_price(S + h, K, T, r, q, sig, call) -
                      2 * bs::bs_price(S, K, T, r, q, sig, call) +
                      bs::bs_price(S - h, K, T, r, q, sig, call)) /
                     (h * h);
  const double dSig =
      (bs::bs_price(S, K, T, r, q, sig + h, call) - bs::bs_price(S, K, T, r, q, sig - h, call)) /
      (2 * h);
  // theta per calendar day == -dPrice/dT / 365
  const double dT =
      (bs::bs_price(S, K, T + h, r, q, sig, call) - bs::bs_price(S, K, T - h, r, q, sig, call)) /
      (2 * h);
  EXPECT_NEAR(g.delta, dS, 1e-4);
  EXPECT_NEAR(g.gamma, d2S, 1e-3);
  EXPECT_NEAR(g.vega, dSig, 1e-3);
  EXPECT_NEAR(g.theta, -dT / 365.0, 1e-5);
}

TEST(BlackScholes, ImpliedVolGuards) {
  EXPECT_TRUE(std::isnan(bs::implied_vol(5.0, 100, 100, 0.0, 0.043, 0.0, true)));   // T=0
  EXPECT_TRUE(std::isnan(bs::implied_vol(-1.0, 100, 100, 0.5, 0.043, 0.0, true)));  // price<=0
  // price below intrinsic (deep ITM call worth < S-K*df) -> no solution
  const double df = std::exp(-0.043 * 0.5);
  const double intrinsic = 100 - 50 * df;
  EXPECT_TRUE(std::isnan(bs::implied_vol(intrinsic - 1.0, 100, 50, 0.5, 0.043, 0.0, true)));
}
} // namespace
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build`
Expected: FAIL to compile (`black_scholes.hpp` not found).

- [ ] **Step 3: Write the implementation**

Create `atx-engine/include/atx/engine/quant/black_scholes.hpp`:
```cpp
#pragma once

// atx::engine::quant — European Black-Scholes-Merton pricing, vega, greeks, and
// an implied-vol solver (Newton-Raphson seeded at 0.5 with a bisection fallback
// on [1e-6, 5.0]). All functions pure and header-only. `theta` is per calendar
// day; `vega` is per 1.00 absolute change in volatility (per year). Unsolvable
// or out-of-domain inputs yield NaN rather than throwing.

#include <cmath>

namespace atx::engine::quant {

[[nodiscard]] inline double norm_pdf(double x) {
  return 0.3989422804014327 * std::exp(-0.5 * x * x);
}

[[nodiscard]] inline double norm_cdf(double x) {
  return 0.5 * std::erfc(-x * 0.7071067811865476);
}

[[nodiscard]] inline double bs_d1(double S, double K, double T, double r, double q,
                                  double sigma) {
  return (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
}

[[nodiscard]] inline double bs_price(double S, double K, double T, double r, double q,
                                     double sigma, bool is_call) {
  if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0) {
    const double intrinsic = is_call ? (S - K) : (K - S);
    return intrinsic > 0.0 ? intrinsic : 0.0;
  }
  const double d1 = bs_d1(S, K, T, r, q, sigma);
  const double d2 = d1 - sigma * std::sqrt(T);
  const double dfr = std::exp(-r * T);
  const double dfq = std::exp(-q * T);
  if (is_call) {
    return S * dfq * norm_cdf(d1) - K * dfr * norm_cdf(d2);
  }
  return K * dfr * norm_cdf(-d2) - S * dfq * norm_cdf(-d1);
}

[[nodiscard]] inline double bs_vega(double S, double K, double T, double r, double q,
                                    double sigma) {
  if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0) {
    return 0.0;
  }
  const double d1 = bs_d1(S, K, T, r, q, sigma);
  return S * std::exp(-q * T) * norm_pdf(d1) * std::sqrt(T);
}

struct Greeks {
  double delta;
  double gamma;
  double vega;   // per 1.00 vol, per year
  double theta;  // per calendar day
};

[[nodiscard]] inline Greeks bs_greeks(double S, double K, double T, double r, double q,
                                      double sigma, bool is_call) {
  const double nan = std::nan("");
  if (T <= 0.0 || sigma <= 0.0 || S <= 0.0 || K <= 0.0) {
    return Greeks{nan, nan, nan, nan};
  }
  const double sqrtT = std::sqrt(T);
  const double d1 = bs_d1(S, K, T, r, q, sigma);
  const double d2 = d1 - sigma * sqrtT;
  const double dfr = std::exp(-r * T);
  const double dfq = std::exp(-q * T);
  const double pdf = norm_pdf(d1);
  Greeks g{};
  g.gamma = dfq * pdf / (S * sigma * sqrtT);
  g.vega = S * dfq * pdf * sqrtT;
  if (is_call) {
    g.delta = dfq * norm_cdf(d1);
    const double theta = -(S * dfq * pdf * sigma) / (2.0 * sqrtT) -
                         r * K * dfr * norm_cdf(d2) + q * S * dfq * norm_cdf(d1);
    g.theta = theta / 365.0;
  } else {
    g.delta = dfq * (norm_cdf(d1) - 1.0);
    const double theta = -(S * dfq * pdf * sigma) / (2.0 * sqrtT) +
                         r * K * dfr * norm_cdf(-d2) - q * S * dfq * norm_cdf(-d1);
    g.theta = theta / 365.0;
  }
  return g;
}

[[nodiscard]] inline double implied_vol(double price, double S, double K, double T, double r,
                                        double q, bool is_call) {
  const double nan = std::nan("");
  if (!std::isfinite(price) || !std::isfinite(S) || !std::isfinite(K) || !std::isfinite(T) ||
      T <= 0.0 || S <= 0.0 || K <= 0.0 || price <= 0.0) {
    return nan;
  }
  const double dfr = std::exp(-r * T);
  const double dfq = std::exp(-q * T);
  const double fwd = is_call ? (S * dfq - K * dfr) : (K * dfr - S * dfq);
  const double intrinsic = fwd > 0.0 ? fwd : 0.0;
  const double upper = is_call ? S * dfq : K * dfr;
  if (price < intrinsic - 1e-9 || price > upper + 1e-9) {
    return nan;
  }
  // Newton-Raphson.
  double sigma = 0.5;
  for (int i = 0; i < 100; ++i) {
    const double diff = bs_price(S, K, T, r, q, sigma, is_call) - price;
    if (std::abs(diff) < 1e-7) {
      return sigma;
    }
    const double v = bs_vega(S, K, T, r, q, sigma);
    if (v < 1e-12) {
      break;
    }
    sigma -= diff / v;
    if (!(sigma > 1e-6 && sigma < 5.0)) {
      break;
    }
  }
  // Bisection fallback on [1e-6, 5.0].
  double lo = 1e-6;
  double hi = 5.0;
  double flo = bs_price(S, K, T, r, q, lo, is_call) - price;
  double fhi = bs_price(S, K, T, r, q, hi, is_call) - price;
  if (flo * fhi > 0.0) {
    return nan;
  }
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    const double fm = bs_price(S, K, T, r, q, mid, is_call) - price;
    if (std::abs(fm) < 1e-7) {
      return mid;
    }
    if (flo * fm <= 0.0) {
      hi = mid;
      fhi = fm;
    } else {
      lo = mid;
      flo = fm;
    }
  }
  return 0.5 * (lo + hi);
}

} // namespace atx::engine::quant
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build` then `atx-engine-tests --gtest_filter=BlackScholes.*`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/quant/black_scholes.hpp \
        atx-engine/tests/black_scholes_test.cpp
git commit -m "feat(quant): Black-Scholes price/vega/greeks + implied-vol solver"
```

---

## Task O4: build_ochain Join Driver (parallel)

**Files:**
- Create: `atx-engine/examples/build_ochain.cpp`
- Modify: `atx-engine/examples/CMakeLists.txt`

This task has no unit test (it is an I/O example, like `build_universe`); correctness of its math is covered by O1–O3 tests. Verification is the live run in the final step.

- [ ] **Step 1: Write the driver**

Create `atx-engine/examples/build_ochain.cpp`:
```cpp
// build_ochain — join OPRA_BBO option quotes onto OHLC1M underlying quotes for
// one date partition, compute ACT/252 time-to-expiry, mid-price implied vol and
// Black-Scholes greeks for every row in parallel, and write the OCHAIN partition.
//
// Usage: build_ochain [DATA_ROOT] [DATE]   (defaults: data/universe 2026-06-05)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/disk.hpp"
#include "atx/engine/quant/black_scholes.hpp"
#include "atx/engine/quant/osi.hpp"
#include "atx/engine/quant/trading_calendar.hpp"

using namespace atx::engine::data;
namespace io = atx::core::io;
namespace q = atx::engine::quant;
namespace time = atx::core::time;
using atx::i64;
using atx::f64;

namespace {

constexpr double kRate = 0.043;
constexpr double kDiv = 0.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] double px_f64(i64 v) {  // 1e-9 fixed-point; non-positive/unset -> NaN
  return v > 0 ? static_cast<double>(v) * 1e-9 : kNaN;
}

[[nodiscard]] std::string strip_dot(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c != '.') {
      out.push_back(c);
    }
  }
  return out;
}

[[nodiscard]] std::string fmt_date(int y, int m, int d) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return std::string{buf};
}

} // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "data/universe";
  const std::string date = argc > 2 ? argv[2] : "2026-06-05";

  auto store_r = DiskStore::open(root);
  if (!store_r.has_value()) {
    std::fprintf(stderr, "open: %s\n", store_r.error().message().c_str());
    return 1;
  }
  const DiskStore store = std::move(*store_r);

  // --- underlying map: strip-dot(symbol) -> (bid_px,ask_px) ------------------
  auto equ_lazy = store.scan_partition(Store::Ohlc1M, date);
  if (!equ_lazy.has_value()) {
    std::fprintf(stderr, "OHLC1M: %s\n", equ_lazy.error().message().c_str());
    return 1;
  }
  auto equ_tbl_r = equ_lazy->collect();
  if (!equ_tbl_r.has_value()) {
    std::fprintf(stderr, "OHLC1M collect: %s\n", equ_tbl_r.error().message().c_str());
    return 1;
  }
  const io::ParquetTable equ = std::move(*equ_tbl_r);
  auto equ_sym = equ.strings("symbol");
  auto equ_bid = equ.column_view<i64>("bid_px");
  auto equ_ask = equ.column_view<i64>("ask_px");
  if (!equ_sym.has_value() || !equ_bid.has_value() || !equ_ask.has_value()) {
    std::fprintf(stderr, "OHLC1M columns missing\n");
    return 1;
  }
  std::unordered_map<std::string, std::pair<i64, i64>> umap;
  umap.reserve(equ_sym->size());
  for (std::size_t i = 0; i < equ_sym->size(); ++i) {
    umap.emplace(strip_dot((*equ_sym)[i]), std::make_pair((*equ_bid)[i], (*equ_ask)[i]));
  }

  // --- OPRA chain ------------------------------------------------------------
  auto opt_lazy = store.scan_partition(Store::OpraBbo, date);
  if (!opt_lazy.has_value()) {
    std::fprintf(stderr, "OPRA_BBO: %s\n", opt_lazy.error().message().c_str());
    return 1;
  }
  auto opt_tbl_r = opt_lazy->collect();
  if (!opt_tbl_r.has_value()) {
    std::fprintf(stderr, "OPRA_BBO collect: %s\n", opt_tbl_r.error().message().c_str());
    return 1;
  }
  const io::ParquetTable opt = std::move(*opt_tbl_r);
  auto ts = opt.column_view<time::Timestamp>("ts");
  auto under = opt.strings("underlying");
  auto osi = opt.strings("symbol");
  auto obid_i = opt.column_view<i64>("bid_px");
  auto oask_i = opt.column_view<i64>("ask_px");
  if (!ts.has_value() || !under.has_value() || !osi.has_value() || !obid_i.has_value() ||
      !oask_i.has_value()) {
    std::fprintf(stderr, "OPRA_BBO columns missing\n");
    return 1;
  }
  const std::size_t n = osi->size();

  // observation date (partition) -> (y,m,d)
  const int oy = std::stoi(date.substr(0, 4));
  const int om = std::stoi(date.substr(5, 2));
  const int od = std::stoi(date.substr(8, 2));

  // --- output buffers --------------------------------------------------------
  std::vector<std::string> c_underlying(n), c_expiry(n), c_callput(n);
  std::vector<f64> c_strike(n), c_obid(n), c_oask(n), c_ubid(n), c_uask(n), c_years(n),
      c_iv(n), c_de(n), c_ga(n), c_ve(n), c_th(n);

  // --- parallel per-row compute ---------------------------------------------
  const unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
  const std::size_t chunk = (n + nthreads - 1) / nthreads;
  const auto work = [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      const std::string u{(*under)[i]};
      c_underlying[i] = u;
      const double obid = px_f64((*obid_i)[i]);
      const double oask = px_f64((*oask_i)[i]);
      c_obid[i] = obid;
      c_oask[i] = oask;

      const auto parsed = q::parse_osi((*osi)[i]);
      double ubid = kNaN, uask = kNaN, strike = kNaN, years = kNaN, iv = kNaN;
      q::Greeks g{kNaN, kNaN, kNaN, kNaN};
      if (parsed.has_value()) {
        c_expiry[i] = fmt_date(parsed->year, parsed->month, parsed->day);
        c_callput[i] = parsed->is_call ? "C" : "P";
        strike = parsed->strike;
        years = q::act252_years(oy, om, od, parsed->year, parsed->month, parsed->day);
        const auto it = umap.find(u);
        if (it != umap.end()) {
          ubid = px_f64(it->second.first);
          uask = px_f64(it->second.second);
        }
        const double S = 0.5 * (ubid + uask);
        const double mid = 0.5 * (obid + oask);
        if (std::isfinite(S) && std::isfinite(mid) && years > 0.0) {
          iv = q::implied_vol(mid, S, strike, years, kRate, kDiv, parsed->is_call);
          if (std::isfinite(iv)) {
            g = q::bs_greeks(S, strike, years, kRate, kDiv, iv, parsed->is_call);
          }
        }
      } else {
        c_expiry[i].clear();
        c_callput[i].clear();
      }
      c_strike[i] = strike;
      c_ubid[i] = ubid;
      c_uask[i] = uask;
      c_years[i] = years;
      c_iv[i] = iv;
      c_de[i] = g.delta;
      c_ga[i] = g.gamma;
      c_ve[i] = g.vega;
      c_th[i] = g.theta;
    }
  };
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < nthreads; ++t) {
    const std::size_t lo = static_cast<std::size_t>(t) * chunk;
    const std::size_t hi = std::min(n, lo + chunk);
    if (lo >= hi) {
      break;
    }
    pool.emplace_back(work, lo, hi);
  }
  for (auto& th : pool) {
    th.join();
  }

  // --- write OCHAIN ----------------------------------------------------------
  const std::vector<std::string> c_date(n, date);
  const std::vector<io::WriteColumn> cols{
      {"timestamp", std::span<const time::Timestamp>(*ts)},
      {"date", std::span<const std::string>(c_date)},
      {"underlying", std::span<const std::string>(c_underlying)},
      {"expiry", std::span<const std::string>(c_expiry)},
      {"call_put", std::span<const std::string>(c_callput)},
      {"strike", std::span<const f64>(c_strike)},
      {"obid", std::span<const f64>(c_obid)},
      {"oask", std::span<const f64>(c_oask)},
      {"ubid", std::span<const f64>(c_ubid)},
      {"uask", std::span<const f64>(c_uask)},
      {"years", std::span<const f64>(c_years)},
      {"mid_iv", std::span<const f64>(c_iv)},
      {"de", std::span<const f64>(c_de)},
      {"ga", std::span<const f64>(c_ga)},
      {"ve", std::span<const f64>(c_ve)},
      {"th", std::span<const f64>(c_th)},
  };
  const std::string out = store.partition_path(Store::OChain, date).string();
  auto w = io::write_parquet(cols, out);
  if (!w.has_value()) {
    std::fprintf(stderr, "write OCHAIN: %s\n", w.error().message().c_str());
    return 1;
  }

  std::size_t solved = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::isfinite(c_iv[i])) {
      ++solved;
    }
  }
  std::printf("OCHAIN %s: %zu rows, %zu IV solved, %zu NaN, threads=%u -> %s\n", date.c_str(), n,
              solved, n - solved, nthreads, out.c_str());
  return 0;
}
```

Notes for the implementer:
- Includes already cover `<limits>` (NaN), `<algorithm>` (`std::min`/`std::max`), `<span>`, `<string_view>`.
- `ParquetTable::strings` returns `std::vector<std::string_view>` borrowed from the table; `equ`/`opt` stay alive for the whole function, so the views and the `*ts` span remain valid through the `write_parquet` call. Do not move/destroy them earlier.
- The threads only read shared state (`umap`, input spans) and write disjoint output indices, so no synchronization is required. Confirm chunk math covers `[0,n)` exactly (last chunk clamped by `std::min`).

- [ ] **Step 2: Wire the executable**

Modify `atx-engine/examples/CMakeLists.txt` — append:
```cmake
add_executable(build_ochain build_ochain.cpp)
target_link_libraries(build_ochain PRIVATE atx::engine atx::core)
```

- [ ] **Step 3: Build**

Run: `cmake --build build`
Expected: `build_ochain` links clean (no `/WX` warnings).

- [ ] **Step 4: Commit**

```bash
git add atx-engine/examples/build_ochain.cpp atx-engine/examples/CMakeLists.txt
git commit -m "feat(engine): build_ochain parallel join -> OCHAIN (IV + greeks)"
```

---

## Final Step: Full Regression + Live Run

- [ ] **Full regression** — run `_regress.bat` (build all + ctest). Expect `BUILD_EXIT=0`, all tests pass including the three new suites (`Osi.*`, `Calendar.*`, `BlackScholes.*`).

- [ ] **Live run** — execute the driver against the existing data root:
```
build\atx-engine\examples\build_ochain.exe data/universe 2026-06-05
```
Expected: `OCHAIN 2026-06-05: 324770 rows, <NNN> IV solved, <MMM> NaN, threads=<K> -> .../OCHAIN/date=2026-06-05/data.parquet`.

- [ ] **Sanity-check output** with pyarrow: confirm 16 columns in the documented order and dtypes; spot-check that a known liquid near-ATM call (e.g. an `AAPL` 2026-07-x strike near spot) has a plausible `mid_iv` in roughly 0.1–0.6 and finite greeks; confirm same-day-expiry rows (expiry `2026-06-05`) carry `years==0` and NaN `mid_iv`.

- [ ] After verification, invoke **superpowers:finishing-a-development-branch** to present merge/PR options (this also covers the prior disk-layer work on the same branch).

---

## Self-Review

**Spec coverage:**
- Output schema (16 cols, order, dtypes, physical `date`) → Task O4 `cols` vector. ✓
- OSI parse → O1. ✓ Join key strip-dot → O4 `strip_dot` + `umap`. ✓
- ACT/252 with NYSE holidays 2026–2028 → O2. ✓
- r=0.043, q=0, mid prices, IV solver, greeks (theta/day) → O3 + O4 constants. ✓
- Keep-all-rows / NaN on unsolvable → O4 (every row emitted; NaN on bad price/T≤0/no match). ✓
- Parallel over all rows → O4 `std::thread` chunks. ✓
- Write to OCHAIN via DiskStore path → O4. ✓

**Placeholder scan:** No TBD/TODO; every code step is complete. ✓

**Type consistency:** `OsiOption{root,year,month,day,is_call,strike}` used identically in O1 and O4. `Greeks{delta,gamma,vega,theta}` from O3 used in O4. `is_trading_day/trading_days_between/act252_years` signatures match O2 header, impl, tests, and O4 call. `px_f64`/`strip_dot`/`fmt_date` defined and used in O4 only. ✓
