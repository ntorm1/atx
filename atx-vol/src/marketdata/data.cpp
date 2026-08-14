#include "atx/vol/api/marketdata/data.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/marketdata/universe.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ── ISO-8601 parsing (ports ats_vol_data.c parse_* helpers) ─────────────────

// Parse exactly `n` decimal digits starting at `pos`. Faithful to
// `parse_ndigits`: on success writes the value and returns true.
[[nodiscard]] bool parse_ndigits(std::string_view s, std::size_t pos, std::size_t n,
                                 int &out) noexcept {
  if (pos + n > s.size()) {
    return false;
  }
  int v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[pos + i]);
    if (c < '0' || c > '9') {
      return false;
    }
    v = v * 10 + static_cast<int>(c - '0');
  }
  out = v;
  return true;
}

// Validate + split "YYYY-MM-DD" (ports `parse_iso_date_len`).
[[nodiscard]] bool parse_iso_date(std::string_view s, int &yy, int &mm, int &dd) noexcept {
  if (s.size() < 10u || s[4] != '-' || s[7] != '-') {
    return false;
  }
  if (!parse_ndigits(s, 0, 4, yy) || !parse_ndigits(s, 5, 2, mm) || !parse_ndigits(s, 8, 2, dd)) {
    return false;
  }
  if (yy < 1970 || yy > 2100 || mm < 1 || mm > 12) {
    return false;
  }
  static constexpr int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int max_day = mdays[mm - 1];
  if (mm == 2) {
    const bool leap = ((yy % 4 == 0) && (yy % 100 != 0)) || (yy % 400 == 0);
    max_day += leap ? 1 : 0;
  }
  return dd >= 1 && dd <= max_day;
}

// Days since the Unix epoch for a civil date (ports `days_since_epoch`; Howard
// Hinnant's algorithm).
[[nodiscard]] std::int64_t days_since_epoch(int yy, int mm, int dd) noexcept {
  yy -= (mm <= 2) ? 1 : 0;
  const int era = (yy >= 0 ? yy : yy - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(yy - era * 400);
  const int mp_int = ((mm > 2 ? mm - 3 : mm + 9) * 153 + 2) / 5;
  const unsigned doy = static_cast<unsigned>(mp_int) + static_cast<unsigned>(dd) - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Parse a time-of-day starting at `pos` into nanoseconds since midnight and the
// consumed end position (ports `parse_iso_time_len`, incl. fractional seconds
// truncated/padded to 9 digits).
[[nodiscard]] bool parse_iso_time(std::string_view s, std::size_t pos, std::int64_t &out_ns,
                                  std::size_t &out_pos) noexcept {
  int hh = 0;
  int mi = 0;
  int ss = 0;
  if (!parse_ndigits(s, pos, 2, hh)) {
    return false;
  }
  pos += 2;
  if (pos >= s.size() || s[pos++] != ':') {
    return false;
  }
  if (!parse_ndigits(s, pos, 2, mi)) {
    return false;
  }
  pos += 2;
  if (pos < s.size() && s[pos] == ':') {
    pos++;
    if (!parse_ndigits(s, pos, 2, ss)) {
      return false;
    }
    pos += 2;
  }
  if (hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) {
    return false;
  }

  std::int64_t frac = 0;
  if (pos < s.size() && s[pos] == '.') {
    pos++;
    std::size_t digits = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      if (digits < 9u) {
        frac = frac * 10 + static_cast<std::int64_t>(s[pos] - '0');
      }
      digits++;
      pos++;
    }
    if (digits == 0) {
      return false;
    }
    while (digits < 9u) {
      frac *= 10;
      digits++;
    }
  }

  out_ns = (static_cast<std::int64_t>(hh) * 3600 + static_cast<std::int64_t>(mi) * 60 +
            static_cast<std::int64_t>(ss)) *
               1000000000LL +
           frac;
  out_pos = pos;
  return true;
}

// Full ISO datetime -> epoch nanoseconds (ports `parse_iso_datetime_ns`).
[[nodiscard]] bool parse_iso_ns(std::string_view iso, std::int64_t &out) noexcept {
  int yy = 0;
  int mm = 0;
  int dd = 0;
  if (!parse_iso_date(iso, yy, mm, dd)) {
    return false;
  }

  std::int64_t tod_ns = 0;
  std::size_t pos = 10;
  int offset_seconds = 0;
  const std::size_t len = iso.size();
  if (pos < len) {
    if (iso[pos] != 'T' && iso[pos] != ' ') {
      return false;
    }
    pos++;
    if (!parse_iso_time(iso, pos, tod_ns, pos)) {
      return false;
    }
    if (pos < len) {
      if (iso[pos] == 'Z' || iso[pos] == 'z') {
        pos++;
      } else if (iso[pos] == '+' || iso[pos] == '-') {
        const int sign = (iso[pos] == '+') ? 1 : -1;
        int oh = 0;
        int om = 0;
        pos++;
        if (!parse_ndigits(iso, pos, 2, oh)) {
          return false;
        }
        pos += 2;
        if (pos >= len || iso[pos++] != ':') {
          return false;
        }
        if (!parse_ndigits(iso, pos, 2, om)) {
          return false;
        }
        pos += 2;
        if (oh > 23 || om > 59) {
          return false;
        }
        offset_seconds = sign * (oh * 3600 + om * 60);
      } else {
        return false;
      }
    }
    if (pos != len) {
      return false;
    }
  }

  const std::int64_t days = days_since_epoch(yy, mm, dd);
  const std::int64_t sec =
      days * 86400LL + tod_ns / 1000000000LL - static_cast<std::int64_t>(offset_seconds);
  out = sec * 1000000000LL + (tod_ns % 1000000000LL);
  return true;
}

// Left-pad a non-negative integer's decimal text to `width`.
[[nodiscard]] std::string pad(int value, std::size_t width) {
  std::string s = std::to_string(value);
  while (s.size() < width) {
    s.insert(s.begin(), '0');
  }
  return s;
}

// The default uid for a row: its own uid, else the frame's default.
[[nodiscard]] const std::string &row_uid(const QuoteFrame &frame, const QuoteRow &row) noexcept {
  return !row.uid.empty() ? row.uid : frame.uid;
}

// Sort an underlier's chains ascending in `T`, re-issuing `expiry_id` to the
// new positions (ports `install_sort_chains_by_T`). Insertion sort — n is tiny.
void sort_chains_by_T(Underlying &under) {
  std::vector<Chain> &chains = under.chains;
  for (std::size_t i = 1; i < chains.size(); ++i) {
    Chain pivot = std::move(chains[i]);
    std::size_t j = i;
    while (j > 0 && chains[j - 1].T > pivot.T) {
      chains[j] = std::move(chains[j - 1]);
      --j;
    }
    chains[j] = std::move(pivot);
  }
  // Re-issue `expiry_id` to the new positions and rebuild the underlier's
  // expiry index so `add_expiry`'s O(1) idempotency lookup stays consistent with
  // the reordered chains (a later install into the same universe/uid relies on
  // the index resolving expiry_ns -> the *current* post-sort id).
  under.expiry_index.clear();
  for (std::size_t i = 0; i < chains.size(); ++i) {
    chains[i].expiry_id = static_cast<ExpiryId>(i);
    under.expiry_index.emplace(chains[i].expiry_ns, static_cast<ExpiryId>(i));
  }
}

} // namespace

// ── ISO-8601 / civil date kernels ───────────────────────────────────────────

std::int64_t iso_to_ns(std::string_view iso) noexcept {
  std::int64_t ns = 0;
  return parse_iso_ns(iso, ns) ? ns : 0;
}

double year_fraction(std::string_view from_iso, std::string_view to_iso) noexcept {
  std::int64_t from_ns = 0;
  std::int64_t to_ns = 0;
  if (!parse_iso_ns(from_iso, from_ns) || !parse_iso_ns(to_iso, to_ns)) {
    return kNaN;
  }
  // Delegates the actual arithmetic to time_to_expiry_years' default (Calendar365)
  // path so there is exactly one copy of the calendar-year constant/expression in
  // the codebase (vol_time.hpp kCalendarYearNs) -- see that header's doc.
  //
  // `TimeSpec{}` is Calendar365 STATICALLY, and that branch reads no calendar,
  // so the vol-time coverage error (vol_time.hpp) is unreachable from here:
  // `value_or` makes this a total function without discarding any reachable
  // error. It cannot become reachable silently either -- routing this legacy
  // ISO entry point through VolTime would mean passing a non-default spec,
  // which no caller can do (the parameter list is two ISO strings).
  return time_to_expiry_years(from_ns, to_ns, TimeSpec{}).value_or(kNaN);
}

std::int64_t expiry_instant_ns(std::string_view expiry_iso, SettlementSession settle) noexcept {
  int yy = 0;
  int mm = 0;
  int dd = 0;
  if (!parse_iso_date(expiry_iso, yy, mm, dd)) {
    return 0; // unparseable date -> 0, matching iso_to_ns's parse-failure sentinel
  }
  // `days_since_epoch` yields the plain civil-day index (timezone-agnostic); the
  // OSI expiry date IS the ET calendar expiration day, so hand it straight to the
  // ET->UTC settlement-instant conversion (16:00/09:30 ET per `settle`).
  const std::int64_t et_day = days_since_epoch(yy, mm, dd);
  return settlement_instant_ns(static_cast<std::int32_t>(et_day), settle);
}

std::string ns_to_iso_date(std::int64_t ns) {
  // Civil-from-days (Howard Hinnant), matching the inline block in the C
  // install's source-vol stamping.
  const std::int64_t day_ns = 86400LL * 1000000000LL;
  const std::int64_t z = ns / day_ns + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  int yy = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
  const int mm = static_cast<int>(mp) + (mp < 10u ? 3 : -9);
  if (mm <= 2) {
    yy += 1;
  }
  return pad(yy, 4) + "-" + pad(mm, 2) + "-" + pad(static_cast<int>(dd), 2);
}

// ── Frame helpers ───────────────────────────────────────────────────────────

Status build_uid_list(QuoteFrame &frame) {
  std::vector<std::string> uids;
  // Membership set for O(1) dedupe; `uids` still carries the first-seen order.
  // Owns its own key copies (not string_views into `uids`) because `uids` grows
  // via push_back and its SSO'd std::string elements relocate on reallocation,
  // which would dangle any view aliasing that storage.
  std::unordered_set<std::string> seen;
  if (!frame.uid.empty()) {
    seen.insert(frame.uid);
    uids.push_back(frame.uid);
  }
  for (const QuoteRow &row : frame.rows) {
    const std::string &uid = row_uid(frame, row);
    if (uid.empty() || uid.size() >= kDataUidStrCap) {
      return Err(ErrorCode::InvalidArgument, "build_uid_list: empty or over-long row uid");
    }
    if (seen.insert(uid).second) { // newly inserted -> first sight
      uids.push_back(uid);
    }
  }
  frame.uid_strs = std::move(uids);
  if (frame.uid.empty() && !frame.uid_strs.empty()) {
    frame.uid = frame.uid_strs.front();
  }
  return Ok();
}

// Join a (uid, expiry_iso) pair into a single unordered_map key. The 0x1F
// (ASCII Unit Separator) delimiter cannot occur in either field — a uid is an
// interned ticker and expiry_iso is an ISO date — so the concatenation is an
// injective key: distinct pairs never collide onto the same string.
[[nodiscard]] std::string join_uid_expiry(std::string_view uid, std::string_view expiry_iso) {
  std::string key;
  key.reserve(uid.size() + 1u + expiry_iso.size());
  key.append(uid);
  key.push_back('\x1f');
  key.append(expiry_iso);
  return key;
}

void build_expiry_inputs(QuoteFrame &frame) {
  std::vector<ExpiryInputs> table;
  table.reserve(frame.rows.size());
  // (uid, expiry_iso) -> index into `table`, replacing the O(n) inner scan with
  // an O(1) probe. Indices (not pointers) stay valid across `table` growth.
  std::unordered_map<std::string, std::size_t> index;
  for (const QuoteRow &row : frame.rows) {
    const std::string &uid = row_uid(frame, row);
    std::string key = join_uid_expiry(uid, row.expiry_iso);

    ExpiryInputs *cell = nullptr;
    if (const auto it = index.find(key); it != index.end()) {
      cell = &table[it->second];
    } else {
      ExpiryInputs fresh;
      fresh.uid = uid;
      fresh.expiry_iso = row.expiry_iso;
      const std::size_t new_idx = table.size();
      table.push_back(std::move(fresh));
      index.emplace(std::move(key), new_idx);
      cell = &table[new_idx];
    }

    // First finite row value wins per field (matches the C dedupe).
    if (std::isfinite(row.rate_source) && !has_flag(cell->completeness, ExpiryInputField::Rate)) {
      cell->rate = row.rate_source;
      cell->completeness |= ExpiryInputField::Rate;
    }
    if (std::isfinite(row.sdiv_source) && !has_flag(cell->completeness, ExpiryInputField::Sdiv)) {
      cell->sdiv = row.sdiv_source;
      cell->completeness |= ExpiryInputField::Sdiv;
    }
    if (std::isfinite(row.ddiv_source) && !has_flag(cell->completeness, ExpiryInputField::Ddiv)) {
      cell->ddiv = row.ddiv_source;
      cell->completeness |= ExpiryInputField::Ddiv;
    }
    if (std::isfinite(row.years_source) && !has_flag(cell->completeness, ExpiryInputField::T)) {
      cell->T_vol = row.years_source;
      cell->completeness |= ExpiryInputField::T;
    }
    if (std::isfinite(row.atm_vol_source) &&
        !has_flag(cell->completeness, ExpiryInputField::AtmVol)) {
      cell->atm_vol = row.atm_vol_source;
      cell->completeness |= ExpiryInputField::AtmVol;
    }
  }
  frame.expiry_inputs = std::move(table);
}

const ExpiryInputs *find_expiry_inputs(const QuoteFrame &frame, std::string_view uid,
                                       std::string_view expiry_iso) noexcept {
  for (const ExpiryInputs &cell : frame.expiry_inputs) {
    if (std::string_view{cell.uid} == uid && std::string_view{cell.expiry_iso} == expiry_iso) {
      return &cell;
    }
  }
  return nullptr;
}

// ── Install ─────────────────────────────────────────────────────────────────

Result<Uid> data_install(Universe &u, const QuoteFrame &frame) {
  // Fail loud when the data plane delivered no yield curve; the calibrator
  // silently degenerates without one (the C's ATS_VOL_ERR_NO_YIELD_CURVE gate).
  if (frame.yc_pillar_t.empty()) {
    return Err(ErrorCode::InvalidArgument, "data_install: snapshot has no yield curve");
  }

  std::string default_uid = frame.uid;
  if (default_uid.empty() && !frame.rows.empty()) {
    default_uid = row_uid(frame, frame.rows.front());
  }
  if (default_uid.empty()) {
    return Err(ErrorCode::InvalidArgument, "data_install: no default uid");
  }

  ATX_TRY(const Uid first_uid, u.intern_ticker(default_uid));
  ATX_TRY(Underlying * first_under, u.get_underlying(first_uid));
  first_under->spot = frame.spot;
  first_under->spot_ts_ns = frame.spot_ts_ns != 0 ? frame.spot_ts_ns : frame.snapshot_ts_ns;

  // Track every distinct uid touched so we can sort each underlier's chains by
  // T after all rows are in. A std::vector replaces the C's fixed 256 cap.
  std::vector<Uid> touched;
  touched.push_back(first_uid);

  for (const QuoteRow &row : frame.rows) {
    const bool side_ok = (row.side == Side::Call || row.side == Side::Put);
    if (!side_ok || !(std::isfinite(row.strike) && row.strike > 0.0) ||
        !(std::isfinite(row.bid) && row.bid >= 0.0) ||
        !(std::isfinite(row.ask) && row.ask >= 0.0) || row.bid_size < 0 || row.ask_size < 0) {
      return Err(ErrorCode::InvalidArgument, "data_install: invalid row");
    }
    const std::string &ru = row_uid(frame, row);
    if (ru.empty() || ru.size() >= kDataUidStrCap) {
      return Err(ErrorCode::InvalidArgument, "data_install: empty or over-long row uid");
    }

    ATX_TRY(const Uid uid, u.intern_ticker(ru));
    if (std::find(touched.begin(), touched.end(), uid) == touched.end()) {
      touched.push_back(uid);
    }

    ATX_TRY(Underlying * under, u.get_underlying(uid));
    if (row.under_spot > 0.0 && std::isfinite(row.under_spot)) {
      under->spot = row.under_spot;
    } else if (under->spot <= 0.0 && frame.spot > 0.0) {
      under->spot = frame.spot;
    }
    const std::int64_t quote_ts =
        row.ts_ns != 0 ? row.ts_ns
                       : (frame.snapshot_ts_ns != 0 ? frame.snapshot_ts_ns : frame.spot_ts_ns);
    if (under->spot_ts_ns == 0 || row.under_spot > 0.0) {
      under->spot_ts_ns = quote_ts;
    }

    // The expiry_iso string is validated (and keyed on) as before; the INSTANT
    // used for Chain::expiry_ns and Chain::T is the loader-stamped TRUE
    // settlement instant (`row.expiry_ns`, 16:00/09:30 ET) when present, else the
    // legacy midnight-UTC parse — so hand-built / synthetic frames that never set
    // `row.expiry_ns` stay BIT-IDENTICAL to their historical T (see
    // QuoteRow::expiry_ns).
    std::int64_t expiry_ns = 0;
    if (!parse_iso_ns(row.expiry_iso, expiry_ns)) {
      return Err(ErrorCode::InvalidArgument, "data_install: bad expiry");
    }
    if (row.expiry_ns != 0) {
      expiry_ns = row.expiry_ns;
    }

    ATX_TRY(const ExpiryId expiry_id, u.add_expiry(uid, expiry_ns));

    // `under` is stable across add_expiry (deque element); index the chain now.
    // The T convention is the frame's own (`frame.time`) — see QuoteFrame::time.
    Chain &chain = under->chains[expiry_id];
    if (chain.strikes.empty()) {
      chain.exercise_style = row.exercise_style;
    } else if (chain.exercise_style != row.exercise_style) {
      return Err(ErrorCode::InvalidArgument,
                 "data_install: mixed exercise styles in one expiry chain");
    }
    std::int64_t snapshot_ns = 0;
    if (!parse_iso_ns(frame.snapshot_iso, snapshot_ns)) {
      return Err(ErrorCode::InvalidArgument, "data_install: bad snapshot timestamp");
    }
    // Propagates the VolTime coverage error (vol_time.hpp): a frame whose
    // snapshot/expiry pair falls outside the calendar's covered window fails
    // the install rather than installing a silently-wrong Chain::T.
    ATX_TRY(const double T, time_to_expiry_years(snapshot_ns, expiry_ns, frame.time));
    if (!std::isfinite(T)) {
      return Err(ErrorCode::InvalidArgument, "data_install: bad year-fraction");
    }
    chain.T = T;

    std::uint16_t strike_idx = std::numeric_limits<std::uint16_t>::max();
    for (std::size_t s = 0; s < chain.strikes.size(); ++s) {
      if (chain.strikes[s] == row.strike) {
        strike_idx = static_cast<std::uint16_t>(s);
        break;
      }
    }
    if (strike_idx == std::numeric_limits<std::uint16_t>::max()) {
      ATX_TRY(const std::uint16_t new_idx, u.add_strike(uid, expiry_id, row.strike));
      strike_idx = new_idx;
    }

    // add_strike grows the chain's SoA in place (no reallocation of the chains
    // vector), so `chain` still refers to the same object.
    const std::size_t idx = chain_index(strike_idx, row.side);
    // Rank rule: an install may never DECREASE a slot's informational rank. A
    // two-sided quote (bid > 0) outranks a one-sided bound (bid = 0, ask > 0 --
    // the shape T6's loader admits); within equal rank last-wins keeps the
    // freshest row, unchanged. Skipping the WHOLE write matters: a slot half
    // overwritten by a bound would be worse than either row alone. A virgin
    // slot has bids[idx] == 0 (add_strike value-initializes), so a bound always
    // lands in an empty slot.
    if (!(row.bid > 0.0) && chain.bids[idx] > 0.0) {
      continue;
    }
    chain.bids[idx] = row.bid;
    chain.asks[idx] = row.ask;
    chain.bid_sizes[idx] = row.bid_size;
    chain.ask_sizes[idx] = row.ask_size;
    chain.mids[idx] = 0.5 * (row.bid + row.ask);
    chain.ts_ns[idx] = quote_ts;
    std::uint8_t flag = 0u;
    if (row.bid > row.ask) {
      flag = kQFlagCrossed;
    } else if (row.bid >= row.ask) {
      flag = kQFlagLocked;
    }
    chain.flags[idx] = flag;
  }

  // (uid, expiry_iso) -> source-input cell, built once so the per-chain ATM-IV
  // stamp below is O(1) instead of `find_expiry_inputs`'s O(n) scan (that scan
  // ran once per chain, i.e. O(chains * inputs) overall). `frame.expiry_inputs`
  // is not mutated here, so the borrowed pointers stay valid.
  std::unordered_map<std::string, const ExpiryInputs *> input_index;
  input_index.reserve(frame.expiry_inputs.size());
  for (const ExpiryInputs &ei : frame.expiry_inputs) {
    input_index.emplace(join_uid_expiry(ei.uid, ei.expiry_iso), &ei);
  }

  // Sort each touched underlier's chains by T (surface evaluators assume slices
  // ascending in T), then stamp the source-side ATM IV onto each chain from the
  // frame's expiry-input table (NaN/absent when the frame carries no inputs).
  for (const Uid uid : touched) {
    const auto under_res = u.get_underlying(uid);
    if (!under_res) {
      continue;
    }
    Underlying *under = *under_res;
    sort_chains_by_T(*under);

    const auto ticker_res = u.ticker_for(uid);
    if (!ticker_res) {
      continue;
    }
    const std::string_view uid_str = *ticker_res;
    for (Chain &chain : under->chains) {
      chain.source_atm_vol = kNaN;
      chain.source_atm_vol_present = false;
      if (chain.expiry_ns <= 0) {
        continue;
      }
      const std::string expiry_iso = ns_to_iso_date(chain.expiry_ns);
      const auto it = input_index.find(join_uid_expiry(uid_str, expiry_iso));
      const ExpiryInputs *cell = (it != input_index.end()) ? it->second : nullptr;
      if (cell != nullptr && has_flag(cell->completeness, ExpiryInputField::AtmVol) &&
          std::isfinite(cell->atm_vol) && cell->atm_vol > 0.0) {
        chain.source_atm_vol = cell->atm_vol;
        chain.source_atm_vol_present = true;
      }
    }
  }

  return Ok(first_uid);
}

// ── SpiderRock Parquet loader (DEFERRED — see data.hpp PORT NOTE) ────────────

Result<QuoteFrame> load_spiderrock_parquet(const SpiderRockLoadSpec &spec) {
  (void)spec;
  return Err(ErrorCode::NotImplemented,
             "load_spiderrock_parquet: deferred; build a QuoteFrame in memory or "
             "re-port on atx::core::io::LazyParquet (see data.hpp PORT NOTE)");
}

} // namespace atx::vol
