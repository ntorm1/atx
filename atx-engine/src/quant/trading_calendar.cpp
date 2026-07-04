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
    for (int y = 2025; y <= 2035; ++y) {
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
  const auto& h = holidays();
  return h.find(z) == h.end();
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
