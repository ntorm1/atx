#include "atx/vol/opra_panel.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include "atx/core/hash.hpp"           // hash_bytes
#include "atx/core/io/parquet.hpp"     // read_parquet, ParquetTable, DType
#include "atx/vol/data.hpp"            // QuoteFrame/Row, build_uid_list, iso_to_ns
#include "atx/vol/dividend.hpp"        // imply_forward_atm_pcp, CoTermQuote
#include "atx/vol/vol_time.hpp"        // TimeSpec, time_to_expiry_years

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

[[nodiscard]] bool numeric_symbol_fallback(std::string_view symbol,
                                           std::int64_t instrument_id) noexcept {
  std::int64_t parsed = 0;
  const std::from_chars_result result =
      std::from_chars(symbol.data(), symbol.data() + symbol.size(), parsed);
  return result.ec == std::errc{} && result.ptr == symbol.data() + symbol.size() &&
         parsed == instrument_id;
}

void append_source_u64(std::string &out, std::uint64_t value) {
  char text[32];
  const auto [ptr, ec] = std::to_chars(text, text + sizeof text, value);
  (void)ec;
  out.append(text, static_cast<std::size_t>(ptr - text));
  out.push_back('|');
}

void append_source_text(std::string &out, std::string_view value) {
  append_source_u64(out, value.size());
  out.append(value);
  out.push_back('|');
}

// Content hash over the frame's SOURCE rows/identities only (schema version,
// snapshot stamp, uid, every row's quote fields, and the instrument-identity
// map) -- it intentionally does NOT fold in `frame.time`/`spec.time`
// (TimeSpec), so two panels loaded from byte-identical source rows under
// DIFFERENT T conventions (e.g. Calendar365 vs VolTime) share the same
// fingerprint; it identifies the market-data rows, not the fitting
// convention applied to them.
[[nodiscard]] std::uint64_t
source_fingerprint(const QuoteFrame &frame, std::span<const std::uint32_t> instrument_ids,
                   const std::map<std::uint32_t, std::string> &identities,
                   std::uint32_t schema_version) {
  std::string bytes;
  append_source_u64(bytes, schema_version);
  append_source_text(bytes, frame.snapshot_iso);
  append_source_u64(bytes, static_cast<std::uint64_t>(frame.snapshot_ts_ns));
  append_source_text(bytes, frame.uid);
  append_source_u64(bytes, frame.rows.size());
  for (std::size_t i = 0; i < frame.rows.size(); ++i) {
    const QuoteRow &row = frame.rows[i];
    append_source_text(bytes, row.uid);
    append_source_text(bytes, row.expiry_iso);
    append_source_u64(bytes, std::bit_cast<std::uint64_t>(row.strike));
    append_source_u64(bytes, static_cast<std::uint64_t>(row.side));
    append_source_u64(bytes, std::bit_cast<std::uint64_t>(row.bid));
    append_source_u64(bytes, std::bit_cast<std::uint64_t>(row.ask));
    append_source_u64(bytes, static_cast<std::uint64_t>(row.bid_size));
    append_source_u64(bytes, static_cast<std::uint64_t>(row.ask_size));
    append_source_u64(bytes, i < instrument_ids.size() ? instrument_ids[i] : 0u);
  }
  append_source_u64(bytes, identities.size());
  for (const auto &[instrument_id, raw_symbol] : identities) {
    append_source_u64(bytes, instrument_id);
    append_source_text(bytes, raw_symbol);
  }
  const std::uint64_t hash = atx::core::hash_bytes(bytes.data(), bytes.size());
  return hash == 0u ? 1u : hash;
}

// Record the first row's `ts` as epoch nanoseconds, tolerating either the real
// Timestamp column or an Int64(ns) column. 0 when absent/empty.
[[nodiscard]] std::int64_t first_ts_ns(const io::ParquetTable& table) {
  const io::ColumnInfo* col = table.schema().find("ts");
  if (col == nullptr || table.num_rows() <= 0) {
    return 0;
  }
  if (col->dtype == io::DType::Timestamp) {
    // column_view<Timestamp> is a rejected specialization (Arrow stores raw i64 in
    // the array's own unit, so it cannot alias) — it ALWAYS errors. Must go through
    // to_column<Timestamp>, which copies + normalises each cell to i64 nanoseconds.
    const auto v = table.to_column<atx::core::time::Timestamp>("ts");
    if (v.has_value() && v->size() > 0) {
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
// carries at least one co-terminal call/put pair with both mids > 0. `rate_at`
// yields the continuously-compounded rate at a maturity T (a flat scalar, or a
// term YieldCurve query); the front expiry is discounted at its OWN r(T_front).
// `time` governs every year-fraction below (default Calendar365 is
// BIT-IDENTICAL to the historical `year_fraction`-derived T).
template <typename RateFn>
[[nodiscard]] Result<double> imply_spot_from_pcp(const QuoteFrame& frame, RateFn rate_at,
                                                 const TimeSpec& time) {
  const std::int64_t snapshot_ns = iso_to_ns(frame.snapshot_iso);
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
  // Small local: back out one expiry's PCP forward from its co-terminal pairs.
  // `out_gap` receives the nearest-ATM |call_mid - put_mid| of the chosen ref
  // strike — the conditioning proxy the fallback below minimizes across expiries.
  const auto forward_for_expiry = [&](const std::string& expiry, double t_front,
                                      double& out_gap) -> Result<double> {
    std::vector<CoTermQuote> quotes;
    for (const auto& [strike, pair] : by_expiry.at(expiry)) {
      if (pair.call_mid > 0.0 && pair.put_mid > 0.0) {
        quotes.push_back(CoTermQuote{strike, pair.call_mid, pair.put_mid});
      }
    }
    if (quotes.empty()) {
      return Err(ErrorCode::Unavailable, "no co-terminal pair");
    }
    // This expiry's own rate (flat scalar, or the term curve at T_front). On the
    // flat path `rate_at` returns the scalar verbatim, so every float op below is
    // bit-identical to the historical single-`r` code.
    const double r = rate_at(t_front);
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
    out_gap = best;
    const Result<double> forward =
        imply_forward_atm_pcp(std::span<const CoTermQuote>(quotes), s_ref, t_front, r);
    if (!forward.has_value() || !(*forward > 0.0) || !std::isfinite(*forward)) {
      return Err(ErrorCode::Unavailable, "no usable forward");
    }
    const double spot = *forward * std::exp(-r * t_front);
    if (!(spot > 0.0) || !std::isfinite(spot)) {
      return Err(ErrorCode::Unavailable, "degenerate implied spot");
    }
    return Ok(spot);
  };

  // First pass — historical behavior, BIT-IDENTICAL for any name that carries at
  // least one well-conditioned (T > 3/365) co-terminal pair: the earliest such
  // expiry sets the spot and we return immediately. Healthy names (AAPL/SPY) all
  // qualify here, so their implied spot is unchanged.
  for (const auto& [expiry, strikes] : by_expiry) {
    (void)strikes;
    // TRUE 16:00 ET PM-settled expiry instant (OPRA equity/ETF universe), so the
    // front-expiry PCP back-out discounts at the same intraday T data_install uses.
    const double t_front =
        time_to_expiry_years(snapshot_ns, expiry_instant_ns(expiry, SettlementSession::Pm), time);
    if (!(t_front > kMinSpotT)) {
      continue;  // 0DTE / same-week: too ill-conditioned for a PCP spot back-out
    }
    double gap = 0.0;
    const Result<double> spot = forward_for_expiry(expiry, t_front, gap);
    if (spot.has_value()) {
      return spot;  // this expiry yielded a usable forward; done
    }
  }

  // FIX D fallback — only reached when NO T > 3/365 expiry produced a usable
  // spot (the ~thinnest names carry only very-short-dated two-sided pairs). Widen
  // the horizon toward ~1 day and pick the BEST-CONDITIONED co-terminal pair
  // across ALL qualifying expiries (nearest-ATM: smallest |call_mid - put_mid|),
  // rather than the first. Still rejects any non-positive / non-finite forward,
  // so a degenerate pair never yields a garbage spot. Because this runs only
  // after the historical pass fully failed, no healthy-name spot is affected.
  constexpr double kMinSpotTFallback = 1.0 / 365.0;
  double best_spot = std::numeric_limits<double>::quiet_NaN();
  double best_gap = std::numeric_limits<double>::infinity();
  for (const auto& [expiry, strikes] : by_expiry) {
    (void)strikes;
    // TRUE 16:00 ET PM-settled expiry instant (OPRA equity/ETF universe), so the
    // front-expiry PCP back-out discounts at the same intraday T data_install uses.
    const double t_front =
        time_to_expiry_years(snapshot_ns, expiry_instant_ns(expiry, SettlementSession::Pm), time);
    if (!(t_front > kMinSpotTFallback)) {
      continue;  // sub-1-day: PCP forward back-out too ill-conditioned even here
    }
    double gap = 0.0;
    const Result<double> spot = forward_for_expiry(expiry, t_front, gap);
    if (spot.has_value() && gap < best_gap) {
      best_gap = gap;
      best_spot = *spot;
    }
  }
  if (std::isfinite(best_spot) && best_spot > 0.0) {
    return Ok(best_spot);
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
  // ── Projected read (W4.3) ─────────────────────────────────────────────────
  // Decode ONLY the columns panel construction consumes (verified against every
  // table access in this file): ts, symbol, bid_px/ask_px/bid_sz/ask_sz, plus
  // the optional instrument_id / underlying. We project the intersection of that
  // set with the file schema, so a file MISSING an optional column still loads,
  // and a missing REQUIRED column still fails at the identical downstream
  // column_view/strings call (same code + text) the read-all path hit. Kept
  // values and row count are byte-identical to a full read, so fitted surfaces
  // are unchanged — only the decoded-byte count drops.
  static constexpr std::array<std::string_view, 8> kOpraWanted = {
      "ts",     "symbol", "bid_px",        "ask_px",
      "bid_sz", "ask_sz", "instrument_id", "underlying"};
  auto scan_res = io::LazyParquet::scan(spec.path);
  if (!scan_res.has_value()) {
    return Err(ErrorCode::InvalidArgument, scan_res.error().to_string());
  }
  const io::Schema& file_schema = scan_res.value().schema();
  std::vector<std::string_view> projection;
  projection.reserve(kOpraWanted.size());
  for (const std::string_view name : kOpraWanted) {
    if (file_schema.find(name) != nullptr) {
      projection.push_back(name);
    }
  }
  auto table_res =
      io::read_parquet(spec.path, std::span<const std::string_view>{projection});
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

  std::span<const std::int64_t> instrument_ids;
  const bool has_instrument_id = schema.find("instrument_id") != nullptr;
  if (has_instrument_id) {
    ATX_TRY(auto ids, table.column_view<std::int64_t>("instrument_id"));
    instrument_ids = ids;
  } else if (spec.provenance_mode == OpraProvenanceMode::Strict) {
    return Err(ErrorCode::InvalidArgument, "strict OPRA provenance requires 'instrument_id'");
  }

  std::vector<std::string_view> underlyings;
  const bool has_underlying = schema.find("underlying") != nullptr;
  if (has_underlying) {
    ATX_TRY(auto u, table.strings("underlying"));
    underlyings = std::move(u);
  }

  const std::string_view filter = spec.underlying;

  // ── P2-2 multi-symbol validation ────────────────────────────────────────
  // This loader implies a SINGLE underlier's spot, so ambiguous multi-symbol
  // input must be rejected rather than silently mixing books.
  if (!filter.empty() && !has_underlying) {
    return Err(ErrorCode::InvalidArgument,
               "underlying filter '" + std::string(filter) +
                   "' requested but parquet has no 'underlying' column");
  }
  if (has_underlying) {
    std::vector<std::string_view> distinct;
    bool filter_present = false;
    for (std::size_t i = 0; i < n_rows; ++i) {
      const std::string_view und = underlyings[i];
      if (und.empty()) {
        continue;
      }
      if (!filter.empty() && und == filter) {
        filter_present = true;
      }
      if (std::find(distinct.begin(), distinct.end(), und) == distinct.end()) {
        distinct.push_back(und);
      }
    }
    if (filter.empty() && distinct.size() > 1) {
      std::string list;
      for (std::size_t j = 0; j < distinct.size(); ++j) {
        if (j != 0) {
          list.push_back(',');
        }
        list.append(distinct[j]);
      }
      return Err(ErrorCode::InvalidArgument,
                 "mixed-symbol parquet: found {" + list +
                     "}; set OpraLoadSpec.underlying to select one");
    }
    if (!filter.empty() && !filter_present) {
      return Err(ErrorCode::InvalidArgument,
                 "underlying '" + std::string(filter) + "' not found in parquet");
    }
  }

  bool provenance_complete = has_instrument_id;
  std::map<std::uint32_t, std::string> source_mappings;
  if (has_instrument_id) {
    for (std::size_t i = 0; i < n_rows; ++i) {
      const std::string_view und = has_underlying ? underlyings[i] : std::string_view{};
      if (!filter.empty() && und != filter) {
        continue;
      }
      const std::int64_t raw_id = instrument_ids[i];
      const bool valid_id = raw_id > 0 && raw_id <= static_cast<std::int64_t>(
                                                        std::numeric_limits<std::uint32_t>::max());
      const bool fallback = numeric_symbol_fallback(symbols[i], raw_id);
      if (!valid_id || fallback) {
        provenance_complete = false;
        if (spec.provenance_mode == OpraProvenanceMode::Strict) {
          return Err(ErrorCode::InvalidArgument,
                     fallback ? "strict OPRA provenance rejects numeric-symbol fallback"
                              : "strict OPRA provenance rejects invalid instrument_id");
        }
        continue;
      }
      const std::uint32_t id = static_cast<std::uint32_t>(raw_id);
      const auto [it, inserted] = source_mappings.try_emplace(id, std::string(symbols[i]));
      if (!inserted && it->second != symbols[i]) {
        return Err(ErrorCode::InvalidArgument,
                   "ambiguous instrument_id maps to multiple raw symbols");
      }
    }
  }

  // Snapshot stamp is hoisted above the row loop so the loop can drop expired /
  // same-day contracts by year-fraction. (snapshot_ts_ns / snapshot_iso depend only
  // on the table + spec, never on the kept rows.)
  const std::int64_t snapshot_ts_ns = first_ts_ns(table);
  std::string snapshot_iso = spec.snapshot_iso;
  if (snapshot_iso.empty()) {
    snapshot_iso = ns_to_iso_date(snapshot_ts_ns);
  }
  const std::int64_t snapshot_iso_ns = iso_to_ns(snapshot_iso);

  std::vector<QuoteRow> rows;
  rows.reserve(n_rows);
  std::vector<std::uint32_t> kept_instrument_ids;
  kept_instrument_ids.reserve(n_rows);
  std::map<std::uint32_t, std::string> kept_mappings;
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
    // TRUE PM-settled expiry instant (16:00 ET). The entire OPRA equity/ETF
    // universe is PM-settled, so every row is stamped Pm (an AM-settled index
    // loader would pass SettlementSession::Am). This instant — not the legacy
    // midnight-UTC `iso_to_ns` — is what T, the drop filter, and every
    // downstream consumer see (G1, gaps finding 3).
    const std::int64_t expiry_instant =
        expiry_instant_ns(osi->expiry_iso, SettlementSession::Pm);
    // Drop only genuinely EXPIRED contracts: T <= 0 against the TRUE expiry
    // instant. Same-session (0DTE) contracts are now KEPT — before 16:00 ET they
    // carry a small positive intraday T (e.g. a 15:55 ET snapshot leaves ~5 min),
    // which is a tradeable forward node; only after the 16:00 ET settle does T go
    // non-positive and the contract drop. (Previously the midnight-UTC parse made
    // every same-day expiry T <= 0 and hard-dropped the highest-volume segment.)
    if (!(time_to_expiry_years(snapshot_iso_ns, expiry_instant, spec.time) > 0.0)) {
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
    row.settle = SettlementSession::Pm;   // OPRA equity/ETF universe: PM-settled
    row.expiry_ns = expiry_instant;       // TRUE 16:00 ET instant -> data_install T
    row.bid = bid;
    row.ask = ask;
    row.bid_size = static_cast<std::int32_t>(bid_sz[i]);
    row.ask_size = static_cast<std::int32_t>(ask_sz[i]);
    rows.push_back(std::move(row));
    if (has_instrument_id && instrument_ids[i] > 0 &&
        instrument_ids[i] <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) &&
        !numeric_symbol_fallback(symbols[i], instrument_ids[i])) {
      const std::uint32_t id = static_cast<std::uint32_t>(instrument_ids[i]);
      kept_instrument_ids.push_back(id);
      kept_mappings.try_emplace(id, std::string(symbols[i]));
    } else {
      kept_instrument_ids.push_back(0u);
    }
  }

  std::string frame_uid = spec.underlying;
  if (frame_uid.empty()) {
    frame_uid = !first_underlying.empty() ? first_underlying : first_root;
  }

  // (snapshot_ts_ns / snapshot_iso computed above the row loop.)

  // ── P2-3 term-structure yield curve ─────────────────────────────────────
  // Caller-supplied pillars define a real term curve queried at each maturity;
  // absent them we keep the historical flat {T=1, r=spec.r} pillar. A SINGLE
  // pillar is treated as flat too: a 1-pillar YieldCurve interpolates as a
  // CONSTANT discount factor (zero(T) = r*T0/T, NOT a flat rate), so the flat /
  // single-pillar path uses the scalar rate directly and never routes through
  // the curve — keeping it BIT-IDENTICAL to the pre-P2-3 behavior (this is the
  // SPY/XOM held-quality invariant).
  if (spec.yc_pillar_t.size() != spec.yc_pillar_r.size()) {
    return Err(ErrorCode::InvalidArgument,
               "OpraLoadSpec.yc_pillar_t/_r length mismatch");
  }
  const bool has_term_curve = spec.yc_pillar_t.size() >= 2;
  YieldCurve yield;
  if (has_term_curve) {
    ATX_TRY(auto yc, YieldCurve::create(spec.yc_pillar_t, spec.yc_pillar_r));
    yield = std::move(yc);
  }
  // The scalar rate the flat / single-pillar path uses: the sole supplied
  // pillar's rate, else spec.r (the historical scalar, verbatim).
  const double flat_r = spec.yc_pillar_r.empty() ? spec.r : spec.yc_pillar_r.front();
  const auto rate_at = [&](double T) -> double {
    return has_term_curve ? yield.zero(T) : flat_r;
  };

  QuoteFrame frame;
  frame.uid = std::move(frame_uid);
  frame.snapshot_iso = snapshot_iso;
  frame.snapshot_ts_ns = snapshot_ts_ns;
  frame.spot_ts_ns = snapshot_ts_ns;
  // Stamp the governing T convention onto the frame itself (QuoteFrame::time):
  // `data_install` reads it for Chain::T, so the installed chains automatically
  // share the convention every T computed below (PCP spot, 0DTE filter, term
  // rates) was derived under — no caller threading required.
  frame.time = spec.time;
  if (spec.yc_pillar_t.empty()) {
    frame.yc_pillar_t = {1.0};
    frame.yc_pillar_r = {spec.r};
  } else {
    frame.yc_pillar_t = spec.yc_pillar_t;
    frame.yc_pillar_r = spec.yc_pillar_r;
  }
  frame.divs = spec.cash_divs;
  frame.rows = std::move(rows);

  // Per-expiry rate wiring (P2-3): when the caller supplied pillars, stamp each
  // kept row's source rate with the curve rate at its expiry's year-fraction so
  // build_expiry_inputs surfaces the per-(uid, expiry) term rate. Left UNTOUCHED
  // on the no-pillars flat path (rate_source stays NaN, ExpiryInputs.rate
  // absent) so that path's frame is byte-for-byte the historical one.
  if (!spec.yc_pillar_t.empty()) {
    for (QuoteRow& row : frame.rows) {
      // Use the row's TRUE stamped expiry instant (16:00 ET), so the term-curve
      // rate is queried at the same T `data_install` will assign to Chain::T.
      const double T = time_to_expiry_years(snapshot_iso_ns, row.expiry_ns, spec.time);
      if (std::isfinite(T) && T > 0.0) {
        row.rate_source = rate_at(T);
      }
    }
  }

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
    ATX_TRY(const double s, imply_spot_from_pcp(frame, rate_at, spec.time));
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
  panel.source_schema_version = has_instrument_id ? 2u : 1u;
  panel.source_fingerprint = source_fingerprint(panel.frame, kept_instrument_ids, kept_mappings,
                                                panel.source_schema_version);
  panel.provenance_complete = provenance_complete;
  panel.source_instrument_ids = std::move(kept_instrument_ids);
  panel.source_identities.reserve(kept_mappings.size());
  for (auto &[instrument_id, raw_symbol] : kept_mappings) {
    panel.source_identities.push_back(OpraInstrumentIdentity{instrument_id, std::move(raw_symbol)});
  }
  panel.fit_context = spec.fit_context;
  panel.market_input_provenance = spec.market_input_provenance;
  panel.time = spec.time;
  return Ok(std::move(panel));
}

} // namespace atx::vol
