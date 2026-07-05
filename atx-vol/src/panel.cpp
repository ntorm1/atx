#include "atx/vol/panel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/dividend.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Default bid/ask displayed size stamped on every synthetic row (contracts).
// A fixture value; the parity harness fits mids, not sizes.
constexpr std::int32_t kSynthQuoteSize = 10;

[[nodiscard]] bool finite_pos(double v) noexcept {
  return std::isfinite(v) && v > 0.0;
}

// ── CSV field helpers (self-contained; no locale, no exceptions) ────────────

[[nodiscard]] std::string_view trim(std::string_view sv) noexcept {
  const auto is_ws = [](char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  std::size_t b = 0;
  std::size_t e = sv.size();
  while (b < e && is_ws(sv[b])) {
    ++b;
  }
  while (e > b && is_ws(sv[e - 1])) {
    --e;
  }
  return sv.substr(b, e - b);
}

[[nodiscard]] char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) {
      return false;
    }
  }
  return true;
}

// Parse a full double occupying the whole (trimmed) field. from_chars is
// exception-free and locale-independent (unlike std::stod).
[[nodiscard]] bool parse_double(std::string_view sv, double& out) noexcept {
  const std::string_view t = trim(sv);
  if (t.empty()) {
    return false;
  }
  const char* first = t.data();
  const char* last = t.data() + t.size();
  const auto res = std::from_chars(first, last, out);
  return res.ec == std::errc() && res.ptr == last;
}

[[nodiscard]] bool parse_i32(std::string_view sv, std::int32_t& out) noexcept {
  const std::string_view t = trim(sv);
  if (t.empty()) {
    return false;
  }
  const char* first = t.data();
  const char* last = t.data() + t.size();
  const auto res = std::from_chars(first, last, out);
  return res.ec == std::errc() && res.ptr == last;
}

[[nodiscard]] bool parse_side(std::string_view sv, Side& out) noexcept {
  const std::string_view t = trim(sv);
  if (iequals(t, "C") || iequals(t, "Call")) {
    out = Side::Call;
    return true;
  }
  if (iequals(t, "P") || iequals(t, "Put")) {
    out = Side::Put;
    return true;
  }
  return false;
}

// Split on ',' (no quoted-field / embedded-comma support). Views alias `line`.
void split_csv(std::string_view line, std::vector<std::string_view>& out) {
  out.clear();
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      out.push_back(line.substr(start));
      return;
    }
    out.push_back(line.substr(start, comma - start));
    start = comma + 1;
  }
}

} // namespace

// ── (a) Synthetic known-truth panel ─────────────────────────────────────────

Result<SynthPanel> make_synthetic_american_panel(const SynthPanelSpec& spec) {
  if (!finite_pos(spec.spot)) {
    return Err(ErrorCode::InvalidArgument, "spot must be positive and finite");
  }
  if (!std::isfinite(spec.r)) {
    return Err(ErrorCode::InvalidArgument, "rate must be finite");
  }
  if (spec.expiries.empty()) {
    return Err(ErrorCode::InvalidArgument, "at least one expiry required");
  }
  if (spec.strikes.empty()) {
    return Err(ErrorCode::InvalidArgument, "at least one strike required");
  }

  const std::int64_t snapshot_ns = iso_to_ns(spec.snapshot_iso);
  if (snapshot_ns == 0) {
    return Err(ErrorCode::InvalidArgument, "snapshot_iso failed to parse: " + spec.snapshot_iso);
  }

  // Validate expiries/strikes up front so bad inputs surface as InvalidArgument
  // (not an interior Internal), and track the expiry T span for the flat curve.
  double t_min = std::numeric_limits<double>::infinity();
  double t_max = -std::numeric_limits<double>::infinity();
  for (const SynthExpiry& e : spec.expiries) {
    if (!finite_pos(e.T)) {
      return Err(ErrorCode::InvalidArgument, "expiry T must be positive and finite");
    }
    if (iso_to_ns(e.expiry_iso) == 0) {
      return Err(ErrorCode::InvalidArgument, "expiry_iso failed to parse: " + e.expiry_iso);
    }
    if (!finite_pos(e.truth.sigma0)) {
      return Err(ErrorCode::InvalidArgument, "truth smile sigma0 must be positive");
    }
    t_min = std::min(t_min, e.T);
    t_max = std::max(t_max, e.T);
  }
  for (const double k : spec.strikes) {
    if (!finite_pos(k)) {
      return Err(ErrorCode::InvalidArgument, "strike must be positive and finite");
    }
  }

  SynthPanel panel;
  QuoteFrame& frame = panel.frame;
  frame.uid = spec.uid;
  frame.snapshot_iso = spec.snapshot_iso;
  frame.snapshot_ts_ns = snapshot_ns;
  frame.spot = spec.spot;
  frame.spot_ts_ns = snapshot_ns;
  frame.divs = spec.cash_divs;

  // Flat yield curve: two strictly-ascending pillars bracketing the expiry span,
  // both carrying the flat rate `r` (a flat curve is rate-invariant to pillar T,
  // but a well-formed bracket keeps a downstream YieldCurve construction valid).
  const double t_lo = std::max(1.0e-3, t_min * 0.5);
  const double t_hi = std::max(t_lo * 2.0, t_max * 1.5);
  frame.yc_pillar_t = {t_lo, t_hi};
  frame.yc_pillar_r = {spec.r, spec.r};

  const std::size_t n_rows = spec.expiries.size() * spec.strikes.size() * 2u;
  frame.rows.reserve(n_rows);
  panel.truth_iv.reserve(n_rows);
  panel.truth_forward.reserve(spec.expiries.size());

  const std::array<Side, 2> sides{Side::Call, Side::Put};

  for (const SynthExpiry& e : spec.expiries) {
    const std::int64_t expiry_ns = iso_to_ns(e.expiry_iso);
    const double F = hybrid_forward(spec.spot, spec.r, spec.borrow, e.T, spec.cash_divs,
                                    expiry_ns, snapshot_ns, spec.hyb);
    if (!finite_pos(F)) {
      return Err(ErrorCode::Internal, "hybrid forward is non-finite/non-positive");
    }
    panel.truth_forward.push_back(F);

    // q_eff bridge: choose the pricer's continuous yield so its forward
    // S*e^{(r-q_eff)T} equals the hybrid forward F.
    const double q_eff = spec.r - std::log(F / spec.spot) / e.T;

    for (const double K : spec.strikes) {
      const double k_log = std::log(K / F);
      const double iv = s3_iv(k_log, e.T, e.truth);
      if (!finite_pos(iv)) {
        return Err(ErrorCode::Internal, "truth smile IV is non-finite/non-positive");
      }

      for (const Side side : sides) {
        const auto mid_res = american_price(spec.spot, K, e.T, iv, spec.r, q_eff, side, spec.method);
        if (!mid_res) {
          return Err(mid_res.error());
        }
        const double mid = *mid_res;
        if (!finite_pos(mid)) {
          return Err(ErrorCode::Internal, "American mid is non-finite/non-positive");
        }

        const double hw = std::max(spec.min_half_spread, spec.half_spread_frac * mid);
        // bid is floored at 0 so the frame installs (data_install rejects a
        // negative bid); with mid > 0 and hw > 0 the invariant bid < ask holds.
        const double bid = std::max(0.0, mid - hw);
        const double ask = mid + hw;

        QuoteRow row;
        row.uid = spec.uid;
        row.expiry_iso = e.expiry_iso;
        row.strike = K;
        row.side = side;
        row.bid = bid;
        row.ask = ask;
        row.bid_size = kSynthQuoteSize;
        row.ask_size = kSynthQuoteSize;
        row.under_spot = spec.spot;
        row.ts_ns = snapshot_ns;
        // Source-input plane (diagnostics): the truth vol, the flat rate, the
        // year-fraction, and the ATF vol as the per-expiry source ATM vol.
        row.iv_source = iv;
        row.rate_source = spec.r;
        row.years_source = e.T;
        row.atm_vol_source = e.truth.sigma0;

        frame.rows.push_back(std::move(row));
        panel.truth_iv.push_back(iv);
      }
    }
  }

  // Collapse the per-row source-input plane to one cell per (uid, expiry) so the
  // installed chains carry source_atm_vol and the parity harness reads SR-shaped
  // term structure without re-walking rows.
  build_expiry_inputs(frame);

  return Ok(std::move(panel));
}

// ── (b) CSV chain loader ─────────────────────────────────────────────────────

Result<QuoteFrame> load_chain_csv(const CsvChainSpec& spec) {
  std::ifstream in(spec.path);
  if (!in.is_open()) {
    return Err(ErrorCode::IoError, "cannot open csv: " + spec.path);
  }

  QuoteFrame frame;
  frame.yc_pillar_t = spec.yc_pillar_t;
  frame.yc_pillar_r = spec.yc_pillar_r;

  std::string line;
  std::size_t line_no = 0;
  bool frame_fields_set = false;
  bool any_source = false;
  std::vector<std::string_view> f;

  const auto parse_err = [](std::size_t ln, std::string_view what) {
    return Err(ErrorCode::ParseError, "csv line " + std::to_string(ln) + ": " + std::string(what));
  };

  while (std::getline(in, line)) {
    ++line_no;
    const std::string_view raw = trim(line);
    if (raw.empty() || raw.front() == '#') {
      continue; // blank line or comment
    }

    split_csv(line, f);
    if (!f.empty() && iequals(trim(f[0]), "uid")) {
      continue; // header row
    }
    if (f.size() < 11 || f.size() > 13) {
      return parse_err(line_no, "expected 11-13 columns");
    }

    const std::string_view uid_sv = trim(f[0]);
    if (uid_sv.empty()) {
      return parse_err(line_no, "empty uid");
    }
    const std::string_view snap_sv = trim(f[1]);

    double spot = 0.0;
    if (!parse_double(f[2], spot)) {
      return parse_err(line_no, "bad spot");
    }
    const std::string_view exp_sv = trim(f[3]);
    double strike = 0.0;
    if (!parse_double(f[4], strike)) {
      return parse_err(line_no, "bad strike");
    }
    Side side = Side::Call;
    if (!parse_side(f[5], side)) {
      return parse_err(line_no, "bad side (want C/P/Call/Put)");
    }
    double bid = 0.0;
    if (!parse_double(f[6], bid)) {
      return parse_err(line_no, "bad bid");
    }
    double ask = 0.0;
    if (!parse_double(f[7], ask)) {
      return parse_err(line_no, "bad ask");
    }
    std::int32_t bid_size = 0;
    if (!parse_i32(f[8], bid_size)) {
      return parse_err(line_no, "bad bid_size");
    }
    std::int32_t ask_size = 0;
    if (!parse_i32(f[9], ask_size)) {
      return parse_err(line_no, "bad ask_size");
    }
    double under_spot = 0.0;
    if (!parse_double(f[10], under_spot)) {
      return parse_err(line_no, "bad under_spot");
    }

    double rate = kNaN;
    if (f.size() >= 12) {
      const std::string_view rs = trim(f[11]);
      if (!rs.empty()) {
        if (!parse_double(rs, rate)) {
          return parse_err(line_no, "bad rate");
        }
        any_source = true;
      }
    }
    double ddiv = kNaN;
    if (f.size() >= 13) {
      const std::string_view ds = trim(f[12]);
      if (!ds.empty()) {
        if (!parse_double(ds, ddiv)) {
          return parse_err(line_no, "bad ddiv");
        }
        any_source = true;
      }
    }

    if (!frame_fields_set) {
      frame.uid = std::string(uid_sv);
      frame.snapshot_iso = std::string(snap_sv);
      frame.snapshot_ts_ns = iso_to_ns(snap_sv);
      frame.spot = spot;
      frame.spot_ts_ns = frame.snapshot_ts_ns;
      frame_fields_set = true;
    }

    QuoteRow row;
    row.uid = std::string(uid_sv);
    row.expiry_iso = std::string(exp_sv);
    row.strike = strike;
    row.side = side;
    row.bid = bid;
    row.ask = ask;
    row.bid_size = bid_size;
    row.ask_size = ask_size;
    row.under_spot = under_spot;
    row.ts_ns = frame.snapshot_ts_ns;
    if (std::isfinite(rate)) {
      row.rate_source = rate;
    }
    if (std::isfinite(ddiv)) {
      row.ddiv_source = ddiv;
    }
    frame.rows.push_back(std::move(row));
  }

  if (any_source) {
    build_expiry_inputs(frame);
  }

  return Ok(std::move(frame));
}

} // namespace atx::vol
