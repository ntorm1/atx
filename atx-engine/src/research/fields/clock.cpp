#include "atx/engine/research/fields/clock.hpp"

#include <array>
#include <charconv>
#include <string>
#include <system_error>

#include "atx/engine/data/research_window.hpp"

namespace atx::engine::research::fields {
namespace {

static_assert(data::kSealBeginNs % kDayNs == 0, "the research seal is a midnight UTC label");

[[nodiscard]] bool is_leap(i64 year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] u32 days_in_month(i64 year, u32 month) noexcept {
  constexpr std::array<u32, 12> kDays{31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  if (month == 2U && is_leap(year)) {
    return 29U;
  }
  return kDays[month - 1U];
}

// The value of `count` decimal digits at text[begin..), or -1 when one is not a digit.
[[nodiscard]] i64 digits(std::string_view text, usize begin, usize count) noexcept {
  i64 out = 0;
  for (usize i = begin; i < begin + count; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return -1;
    }
    out = out * 10 + static_cast<i64>(c - '0');
  }
  return out;
}

void append_padded(std::string &out, i64 value, usize width) {
  std::array<char, 32> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  const auto len = static_cast<usize>(res.ptr - buf.data());
  if (value >= 0 && len < width) {
    out.append(width - len, '0');
  }
  out.append(buf.data(), len);
}

} // namespace

i64 days_from_civil(i64 year, u32 month, u32 day) noexcept {
  const i64 y = month <= 2U ? year - 1 : year;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const i64 yoe = y - era * 400;                                                         // [0, 399]
  const i64 mp = month > 2U ? static_cast<i64>(month) - 3 : static_cast<i64>(month) + 9; // [0, 11]
  const i64 doy = (153 * mp + 2) / 5 + static_cast<i64>(day) - 1;                        // [0, 365]
  const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
  return era * 146097 + doe - 719468;
}

CivilDate civil_from_days(i64 day) noexcept {
  const i64 z = day + 719468;
  const i64 era = (z >= 0 ? z : z - 146096) / 146097;
  const i64 doe = z - era * 146097;                                      // [0, 146096]
  const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
  const i64 mp = (5 * doy + 2) / 153;                                    // [0, 11]
  const i64 d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
  const i64 m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
  const i64 y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  return CivilDate{y, static_cast<u32>(m), static_cast<u32>(d)};
}

i64 year_of(i64 day) noexcept { return civil_from_days(day).year; }

std::optional<i64> parse_iso_day(std::string_view text) noexcept {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return std::nullopt;
  }
  const i64 year = digits(text, 0, 4);
  const i64 month = digits(text, 5, 2);
  const i64 day = digits(text, 8, 2);
  if (year < 1 || month < 1 || month > 12 || day < 1) {
    return std::nullopt;
  }
  const auto m = static_cast<u32>(month);
  if (static_cast<u32>(day) > days_in_month(year, m)) {
    return std::nullopt;
  }
  return days_from_civil(year, m, static_cast<u32>(day));
}

std::string iso_date(i64 day) {
  const CivilDate c = civil_from_days(day);
  std::string out;
  out.reserve(10);
  append_padded(out, c.year, 4);
  out.push_back('-');
  append_padded(out, static_cast<i64>(c.month), 2);
  out.push_back('-');
  append_padded(out, static_cast<i64>(c.day), 2);
  return out;
}

i64 seal_day() noexcept { return data::kSealBeginNs / kDayNs; }

bool is_sealed_day(i64 day) noexcept { return day >= seal_day(); }

std::string seal_refusal(std::string_view what) {
  return "research fields: " + std::string(what) + " at or after the research seal " +
         std::string(data::kSealBeginDate) + " (" + std::string(data::kResearchWindowId) + ")";
}

} // namespace atx::engine::research::fields
