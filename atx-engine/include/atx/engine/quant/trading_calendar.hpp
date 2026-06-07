#pragma once

// atx::engine::quant — NYSE trading-day calendar for ACT/252 time-to-expiry.
// Holidays are computed by rule (fixed-date with weekend shift, floating
// nth-weekday, Good Friday via Computus) for years 2025-2035. A day is a
// trading day if it is Mon-Fri and not an NYSE holiday.
//
// SUPPORTED RANGE: holidays are populated for 2025-2035 only. Dates outside this
// window are treated as holiday-free (weekends still excluded), so trading-day
// counts and ACT/252 fractions spanning out-of-range dates are SILENTLY
// under-counted. Extend add_year_holidays' loop in the .cpp before using dates
// beyond 2035.

namespace atx::engine::quant {

[[nodiscard]] bool is_trading_day(int y, int m, int d);

// Count trading days strictly after (y0,m0,d0) up to and including (y1,m1,d1).
// Returns 0 when the second date is not strictly after the first.
[[nodiscard]] int trading_days_between(int y0, int m0, int d0, int y1, int m1, int d1);

// trading_days_between / 252.0. NOTE: accuracy degrades silently for dates
// outside the supported range 2025-2035 (see file-level comment above).
[[nodiscard]] double act252_years(int y0, int m0, int d0, int y1, int m1, int d1);

} // namespace atx::engine::quant
