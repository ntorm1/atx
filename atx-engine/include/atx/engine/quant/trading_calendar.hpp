#pragma once

// atx::engine::quant — NYSE trading-day calendar for ACT/252 time-to-expiry.
// Holidays are computed by rule (fixed-date with weekend shift, floating
// nth-weekday, Good Friday via Computus) for years 2025-2035. A day is a
// trading day if it is Mon-Fri and not an NYSE holiday.

namespace atx::engine::quant {

[[nodiscard]] bool is_trading_day(int y, int m, int d);

// Count trading days strictly after (y0,m0,d0) up to and including (y1,m1,d1).
// Returns 0 when the second date is not strictly after the first.
[[nodiscard]] int trading_days_between(int y0, int m0, int d0, int y1, int m1, int d1);

// trading_days_between / 252.0
[[nodiscard]] double act252_years(int y0, int m0, int d0, int y1, int m1, int d1);

} // namespace atx::engine::quant
