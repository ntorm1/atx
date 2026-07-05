#include "atx/vol/opra_panel.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"       // time::Timestamp
#include "atx/core/error.hpp"          // Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/io/parquet.hpp"     // read_parquet, ParquetTable, DType
#include "atx/vol/data.hpp"            // QuoteFrame/Row, build_uid_list, year_fraction
#include "atx/vol/dividend.hpp"        // imply_forward_atm_pcp, CoTermQuote

namespace atx::vol {

using atx::core::Ok;
using atx::core::Err;
using atx::core::ErrorCode;

namespace {

namespace io = atx::core::io;

// Unset-price sentinel written by the OPRA pull (bid_px/ask_px).
constexpr std::int64_t kUnsetPx = std::numeric_limits<std::int64_t>::min();

// Fixed-point price scale: raw fields are 1e-9 dollars.
constexpr double kPxScale = 1e-9;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && s[b] == ' ') {
    ++b;
  }
  while (e > b && s[e - 1] == ' ') {
    --e;
  }
  return s.substr(b, e - b);
}

// Parse a full-width unsigned decimal field into `out`. Returns false unless the
// entire span was consumed as digits.
[[nodiscard]] bool parse_field(std::string_view s, int& out) noexcept {
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const std::from_chars_result res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

// Record the first row's `ts` as epoch nanoseconds, tolerating either the real
// Timestamp column or an Int64(ns) column. 0 when absent/empty.
[[nodiscard]] std::int64_t first_ts_ns(const io::ParquetTable& table) {
  const io::ColumnInfo* col = table.schema().find("ts");
  if (col == nullptr || table.num_rows() <= 0) {
    return 0;
  }
  if (col->dtype == io::DType::Timestamp) {
    const auto v = table.column_view<atx::core::time::Timestamp>("ts");
    if (v.has_value() && !v->empty()) {
      return (*v)[0].unix_nanos();
    }
  } else if (col->dtype == io::DType::Int64) {
    const auto v = table.column_view<std::int64_t>("ts");
    if (v.has_value() && !v->empty()) {
      return (*v)[0];
    }
  }
  return 0;
}

// Imply the underlying spot from put-call parity on the earliest expiry that
// carries at least one co-terminal call/put pair with both mids > 0.
[[nodiscard]] Result<double> imply_spot_from_pcp(const QuoteFrame& frame, double r) {
  struct MidPair {
    double call_mid = -1.0;
    double put_mid = -1.0;
  };
  // Keyed on the ISO expiry string, which sorts chronologically, so ascending
  // map iteration visits the front expiry first.
  std::map<std::string, std::map<double, MidPair>> by_expiry;
  for (const QuoteRow& row : frame.rows) {
    const double mid = 0.5 * (row.bid + row.ask);
    if (!(mid > 0.0)) {
      continue;
    }
    MidPair& pair = by_expiry[row.expiry_iso][row.strike];
    if (row.side == Side::Call) {
      pair.call_mid = mid;
    } else {
      pair.put_mid = mid;
    }
  }

  // Real OPRA chains include 0DTE / same-day expiries whose year-fraction is
  // ~0 (or negative against an intraday snapshot) — a PCP forward back-out there
  // is wildly ill-conditioned. Require a few days to expiry so the earliest
  // WELL-CONDITIONED expiry sets the spot; a per-expiry PCP failure is non-fatal
  // (we try the next), and only a fully empty search errors.
  constexpr double kMinSpotT = 3.0 / 365.0;
  for (const auto& [expiry, strikes] : by_expiry) {
    std::vector<CoTermQuote> quotes;
    for (const auto& [strike, pair] : strikes) {
      if (pair.call_mid > 0.0 && pair.put_mid > 0.0) {
        quotes.push_back(CoTermQuote{strike, pair.call_mid, pair.put_mid});
      }
    }
    if (quotes.empty()) {
      continue;
    }
    const double t_front = year_fraction(frame.snapshot_iso, expiry);
    if (!(t_front > kMinSpotT)) {
      continue;  // 0DTE / same-week: too ill-conditioned for a PCP spot back-out
    }
    // ATM reference: the strike whose call/put mids are closest (C == P at F).
    double s_ref = quotes.front().strike;
    double best = std::numeric_limits<double>::infinity();
    for (const CoTermQuote& q : quotes) {
      const double gap = std::fabs(q.call_mid - q.put_mid);
      if (gap < best) {
        best = gap;
        s_ref = q.strike;
      }
    }
    const Result<double> forward = imply_forward_atm_pcp(
        std::span<const CoTermQuote>(quotes), s_ref, t_front, r);
    if (!forward.has_value() || !(*forward > 0.0) || !std::isfinite(*forward)) {
      continue;  // this expiry yielded no usable forward; try the next
    }
    return Ok(*forward * std::exp(-r * t_front));
  }
  return Err(ErrorCode::Unavailable,
             "no well-conditioned co-terminal expiry to imply spot; pass spot_override");
}

} // namespace

Result<OsiSymbol> parse_osi_symbol(std::string_view sym) {
  constexpr std::size_t kFixedLen = 15;
  if (sym.size() < kFixedLen) {
    return Err(ErrorCode::InvalidArgument, "OSI symbol shorter than 15 chars");
  }
  const std::size_t n = sym.size();
  const std::string_view fixed = sym.substr(n - kFixedLen);

  int yy = 0;
  int mm = 0;
  int dd = 0;
  if (!parse_field(fixed.substr(0, 2), yy) || !parse_field(fixed.substr(2, 2), mm) ||
      !parse_field(fixed.substr(4, 2), dd)) {
    return Err(ErrorCode::ParseError, "OSI date field not numeric");
  }
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31) {
    return Err(ErrorCode::ParseError, "OSI date field out of range");
  }

  const char cp = fixed[6];
  const Side side = (cp == 'P' || cp == 'p') ? Side::Put : Side::Call;

  std::int64_t strike_milli = 0;
  const char* sfirst = fixed.data() + 7;
  const char* slast = fixed.data() + kFixedLen;
  const std::from_chars_result sres = std::from_chars(sfirst, slast, strike_milli);
  if (sres.ec != std::errc{} || sres.ptr != slast) {
    return Err(ErrorCode::ParseError, "OSI strike field not numeric");
  }
  const double strike = static_cast<double>(strike_milli) / 1000.0;
  if (!(strike > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "OSI strike non-positive");
  }

  OsiSymbol out;
  out.root = std::string(trim(sym.substr(0, n - kFixedLen)));
  out.expiry_iso.reserve(10);
  out.expiry_iso.append("20").append(fixed.substr(0, 2)).append("-").append(fixed.substr(2, 2)).append("-").append(fixed.substr(4, 2));
  out.side = side;
  out.strike = strike;
  return Ok(std::move(out));
}

Result<OpraPanel> load_opra_cbbo_parquet(const OpraLoadSpec& spec) {
  auto table_res = io::read_parquet(spec.path);
  if (!table_res.has_value()) {
    return Err(ErrorCode::InvalidArgument, table_res.error().to_string());
  }
  const io::ParquetTable table = std::move(table_res.value());
  const io::Schema& schema = table.schema();
  const std::size_t n_rows =
      static_cast<std::size_t>(std::max<std::int64_t>(0, table.num_rows()));

  ATX_TRY(const auto symbols, table.strings("symbol"));
  ATX_TRY(const auto bid_px, table.column_view<std::int64_t>("bid_px"));
  ATX_TRY(const auto ask_px, table.column_view<std::int64_t>("ask_px"));
  ATX_TRY(const auto bid_sz, table.column_view<std::int64_t>("bid_sz"));
  ATX_TRY(const auto ask_sz, table.column_view<std::int64_t>("ask_sz"));
  ATX_TRY(const auto bid_null, table.null_mask("bid_px"));
  ATX_TRY(const auto ask_null, table.null_mask("ask_px"));

  std::vector<std::string_view> underlyings;
  const bool has_underlying = schema.find("underlying") != nullptr;
  if (has_underlying) {
    ATX_TRY(auto u, table.strings("underlying"));
    underlyings = std::move(u);
  }

  const std::string_view filter = spec.underlying;

  std::vector<QuoteRow> rows;
  rows.reserve(n_rows);
  std::size_t n_dropped = 0;
  std::string first_underlying;
  std::string first_root;

  for (std::size_t i = 0; i < n_rows; ++i) {
    const std::string_view und = has_underlying ? underlyings[i] : std::string_view{};
    if (!filter.empty() && und != filter) {
      continue; // filtered out (not a drop)
    }
    if (bid_px[i] == kUnsetPx || ask_px[i] == kUnsetPx || bid_null[i] != 0 ||
        ask_null[i] != 0) {
      ++n_dropped;
      continue;
    }
    auto osi = parse_osi_symbol(symbols[i]);
    if (!osi.has_value()) {
      ++n_dropped;
      continue;
    }
    const double bid = static_cast<double>(bid_px[i]) * kPxScale;
    const double ask = static_cast<double>(ask_px[i]) * kPxScale;
    if (bid > ask) { // crossed
      ++n_dropped;
      continue;
    }

    if (first_underlying.empty() && !und.empty()) {
      first_underlying = std::string(und);
    }
    if (first_root.empty() && !osi->root.empty()) {
      first_root = osi->root;
    }

    QuoteRow row;
    row.uid = !osi->root.empty() ? osi->root : std::string(und);
    row.expiry_iso = std::move(osi->expiry_iso);
    row.strike = osi->strike;
    row.side = osi->side;
    row.bid = bid;
    row.ask = ask;
    row.bid_size = static_cast<std::int32_t>(bid_sz[i]);
    row.ask_size = static_cast<std::int32_t>(ask_sz[i]);
    rows.push_back(std::move(row));
  }

  std::string frame_uid = spec.underlying;
  if (frame_uid.empty()) {
    frame_uid = !first_underlying.empty() ? first_underlying : first_root;
  }

  const std::int64_t snapshot_ts_ns = first_ts_ns(table);
  std::string snapshot_iso = spec.snapshot_iso;
  if (snapshot_iso.empty()) {
    snapshot_iso = ns_to_iso_date(snapshot_ts_ns);
  }

  QuoteFrame frame;
  frame.uid = std::move(frame_uid);
  frame.snapshot_iso = snapshot_iso;
  frame.snapshot_ts_ns = snapshot_ts_ns;
  frame.spot_ts_ns = snapshot_ts_ns;
  frame.yc_pillar_t = {1.0};
  frame.yc_pillar_r = {spec.r};
  frame.divs = spec.cash_divs;
  frame.rows = std::move(rows);

  ATX_TRY_VOID(build_uid_list(frame));
  build_expiry_inputs(frame);

  const std::size_t n_contracts = frame.rows.size();

  std::vector<std::string_view> distinct_expiries;
  for (const QuoteRow& row : frame.rows) {
    if (std::find(distinct_expiries.begin(), distinct_expiries.end(), row.expiry_iso) ==
        distinct_expiries.end()) {
      distinct_expiries.push_back(row.expiry_iso);
    }
  }
  const std::size_t n_expiries = distinct_expiries.size();

  double implied_spot = 0.0;
  if (spec.spot_override > 0.0) {
    implied_spot = spec.spot_override;
  } else {
    ATX_TRY(const double s, imply_spot_from_pcp(frame, spec.r));
    implied_spot = s;
  }
  frame.spot = implied_spot;

  OpraPanel panel;
  panel.frame = std::move(frame);
  panel.implied_spot = implied_spot;
  panel.snapshot_iso = std::move(snapshot_iso);
  panel.n_contracts = n_contracts;
  panel.n_expiries = n_expiries;
  panel.n_dropped = n_dropped;
  return Ok(std::move(panel));
}

} // namespace atx::vol
