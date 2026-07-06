#include "atx/vol/opra_batch.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "atx/core/error.hpp"       // Ok, Err, ErrorCode
#include "atx/vol/curve.hpp"        // YieldCurve
#include "atx/vol/data.hpp"         // iso_to_ns

namespace atx::vol {

using atx::core::Ok;
using atx::core::Err;
using atx::core::ErrorCode;

namespace {

namespace fs = std::filesystem;

// ── Civil-date kernel (Howard-Hinnant days-from-civil) ──────────────────────
//
// A serial day count keyed at 1970-01-01 = 0, so an inclusive date range becomes
// a contiguous integer interval we walk one day at a time. Self-contained on
// purpose: no external date library (chrono's year_month_day round-trip would do,
// but the raw algorithm is a dozen lines and keeps the dependency surface flat).

struct Civil {
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
};

// Serial day number for a civil date (Howard Hinnant, "chrono-Compatible Low-Level
// Date Algorithms"). Valid for the Gregorian calendar; m in [1,12], d in [1,31].
[[nodiscard]] std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= (m <= 2);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);                  // [0, 399]
  const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;      // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Inverse of days_from_civil.
[[nodiscard]] Civil civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                    // [0, 11]
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
  return Civil{y + static_cast<int>(m <= 2), m, d};
}

// Parse a full-width unsigned decimal field, requiring the whole span be digits.
[[nodiscard]] bool parse_uint(std::string_view s, int& out) noexcept {
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const std::from_chars_result res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

// Parse exactly "YYYY-MM-DD". Rejects wrong length, missing dashes, non-numeric
// fields, or an out-of-range month/day.
[[nodiscard]] bool parse_civil(std::string_view s, Civil& out) noexcept {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  int y = 0;
  int m = 0;
  int d = 0;
  if (!parse_uint(s.substr(0, 4), y) || !parse_uint(s.substr(5, 2), m) ||
      !parse_uint(s.substr(8, 2), d)) {
    return false;
  }
  if (m < 1 || m > 12 || d < 1 || d > 31) {
    return false;
  }
  out = Civil{y, static_cast<unsigned>(m), static_cast<unsigned>(d)};
  return true;
}

// Format a civil date as "YYYY-MM-DD".
[[nodiscard]] std::string format_civil(const Civil& c) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", c.y, c.m, c.d);
  return std::string(buf);
}

// Substitute "{symbol}" and "{date}" tokens in a path template. Unknown text is
// copied verbatim; a lone '{' that starts neither token is copied as-is.
[[nodiscard]] std::string apply_template(std::string_view tmpl, std::string_view symbol,
                                         std::string_view date) {
  std::string out;
  out.reserve(tmpl.size() + symbol.size() + date.size());
  std::size_t i = 0;
  while (i < tmpl.size()) {
    if (tmpl.compare(i, 8, "{symbol}") == 0) {
      out.append(symbol);
      i += 8;
    } else if (tmpl.compare(i, 6, "{date}") == 0) {
      out.append(date);
      i += 6;
    } else {
      out.push_back(tmpl[i]);
      ++i;
    }
  }
  return out;
}

} // namespace

Result<OpraBatchResult> load_opra_daterange(const OpraBatchSpec& spec,
                                            const OpraBatchProgress& progress) {
  // ── Malformed-spec gate (the ONLY top-level errors) ──────────────────────
  if (spec.symbols.empty()) {
    return Err(ErrorCode::InvalidArgument, "OpraBatchSpec.symbols is empty");
  }
  Civil lo;
  Civil hi;
  if (!parse_civil(spec.date_lo, lo)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_lo '" + spec.date_lo + "'");
  }
  if (!parse_civil(spec.date_hi, hi)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_hi '" + spec.date_hi + "'");
  }
  const std::int64_t serial_lo = days_from_civil(lo.y, lo.m, lo.d);
  const std::int64_t serial_hi = days_from_civil(hi.y, hi.m, hi.d);
  if (serial_hi < serial_lo) {
    return Err(ErrorCode::InvalidArgument,
               "date_hi '" + spec.date_hi + "' precedes date_lo '" + spec.date_lo + "'");
  }
  if (spec.yc_pillar_t.size() != spec.yc_pillar_r.size()) {
    return Err(ErrorCode::InvalidArgument, "OpraBatchSpec.yc_pillar_t/_r length mismatch");
  }

  OpraBatchResult result;
  const std::size_t n_dates = static_cast<std::size_t>(serial_hi - serial_lo + 1);
  result.n_total = spec.symbols.size() * n_dates;
  result.entries.reserve(result.n_total);

  // Memoize date+suffix -> ts_ns: the symbols of one date share a single snapshot
  // stamp, so the M distinct stamps are parsed once (iso_to_ns) instead of N*M
  // times. Per-contract OSI parsing remains inside load_opra_cbbo_parquet — its
  // signature is left untouched.
  std::unordered_map<std::string, std::int64_t> snap_ts_cache;

  std::size_t done = 0;
  for (std::int64_t serial = serial_lo; serial <= serial_hi; ++serial) {
    const std::string date = format_civil(civil_from_days(serial));
    const std::string snapshot_iso = date + spec.snapshot_suffix;

    std::int64_t snap_ts = 0;
    if (const auto it = snap_ts_cache.find(snapshot_iso); it != snap_ts_cache.end()) {
      snap_ts = it->second;
    } else {
      snap_ts = iso_to_ns(snapshot_iso);
      snap_ts_cache.emplace(snapshot_iso, snap_ts);
    }

    for (const std::string& symbol : spec.symbols) {
      OpraBatchEntry entry;
      entry.symbol = symbol;
      entry.date = date;
      entry.snapshot_ts_ns = snap_ts;
      fs::path path = fs::path(spec.root_dir) / apply_template(spec.path_template, symbol, date);
      path.make_preferred();
      entry.path = path.string();

      std::error_code ec;
      const bool present = fs::exists(path, ec) && !ec;
      if (!present) {
        entry.panel = Err(ErrorCode::NotFound, "no parquet at '" + entry.path + "'");
        ++result.n_missing;
      } else {
        OpraLoadSpec load;
        load.path = entry.path;
        load.underlying = symbol;
        load.snapshot_iso = snapshot_iso;
        load.r = spec.r;
        load.yc_pillar_t = spec.yc_pillar_t;
        load.yc_pillar_r = spec.yc_pillar_r;
        Result<OpraPanel> loaded = load_opra_cbbo_parquet(load);
        if (loaded.has_value()) {
          ++result.n_loaded;
        } else {
          ++result.n_error;
        }
        entry.panel = std::move(loaded);
      }

      ++done;
      result.entries.push_back(std::move(entry));
      if (progress) {
        progress(done, result.n_total, result.entries.back());
      }
    }
  }

  return Ok(std::move(result));
}

MarketEnv market_env_from_frame(const QuoteFrame& frame) {
  // Flat rate the no/one-pillar path uses (the sole pillar's rate, else 0).
  const double flat_r = frame.yc_pillar_r.empty() ? 0.0 : frame.yc_pillar_r.front();

  // >= 2 equal-length pillars => a real term curve; each date's chain then sees
  // its own rate_at(T). A single pillar interpolates as a CONSTANT discount
  // factor (not a flat rate), so it is treated as flat — matching the loader.
  if (frame.yc_pillar_t.size() >= 2 &&
      frame.yc_pillar_t.size() == frame.yc_pillar_r.size()) {
    Result<YieldCurve> yc = YieldCurve::create(frame.yc_pillar_t, frame.yc_pillar_r);
    if (yc.has_value()) {
      MarketEnv env =
          MarketEnv::flat(frame.spot, flat_r, frame.snapshot_ts_ns, frame.divs);
      env.yield = std::move(yc.value());  // rate_at(T>0) now interpolates
      return env;
    }
    // create() failed (e.g. non-ascending pillars): fall back to flat.
  }
  return MarketEnv::flat(frame.spot, flat_r, frame.snapshot_ts_ns, frame.divs);
}

} // namespace atx::vol
