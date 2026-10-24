// synthetic_spiderrock_store — a deterministic, data-free, SpiderRock-SHAPED intraday
// store (api/marketdata/spiderrock_store.hpp, design 2026-10-24 §5.7). The bench's
// `--synthetic` mode and the lane tests run the IDENTICAL loader and engine path on
// it; only the directory differs from the licensed store.
//
// ## The market (decided in the lane brief; parameters left to the builder)
//
// Per underlier: S0 ~ U[20, 400], r ~ U[0.03, 0.05], q ~ U[0, 0.03], an eSSVI
// smile per expiry (ATM vol in [0.15, 0.6] with a sqrt-T term tilt; power-law
// phi = eta / sqrt(theta), eta ~ U[0.4, 1.0], which fixes the smile's shape in
// standardized moneyness z = k / (sigma_atm sqrt T); rho ~ U[-0.6, -0.1] per uid
// plus a per-expiry jitter), and for 30% of uids a quarterly cash dividend whose PV
// to each expiry is the `ddiv` column. Expiries sit 7 ... 365 calendar days after
// the date at 16:00 ET; strikes are LISTED contracts fixed at the first bucket
// (|z| <= 2.5 around F, snapped to a decimal grid fine enough to stay unique).
//
// Per bucket: a GBM spot step at annualized `spot_vol` over the bucket gap on a
// TRADING clock (390 min x 252 days), an additive ATM level shock N(0,
// level_shock_vol) and an additive rho shock N(0, skew_shock_vol), both random
// walks across buckets, and T decaying with the clock (Calendar365, the library
// default, which is also what `years` holds).
//
// ## Vendor columns
//
// srPrc / de / ga / th / ve are `american_greeks_al(S, K, T, srVol, r, q_eff, side,
// al_fast_opts())` with q_eff = spiderrock_q_eff(S, ddiv, r, q, T) — the D4 forward
// basis the loader rebuilds — so a cold kernel on the loaded state reprices the vendor
// columns to its own accuracy (vendor parity is an identity check here). al_fast_opts
// is the preset the risk kernel resolves for a marks-only state. ve / th are written
// in VENDOR units (kSpiderRockVegaScale / kSpiderRockThetaScale), like the real store.
//
// ## Determinism
//
// Every random draw happens serially in a fixed order from std::mt19937_64(seed)
// through the portable transforms below (std::*_distribution output is
// library-specific, so it is not used). The pricing fan-out writes disjoint row
// slots, so the bytes written are independent of the thread count.

#include "atx/vol/api/marketdata/spiderrock_store.hpp"

// Third-party Arrow/Parquet headers: SYSTEM includes via the vcpkg imported
// targets (the oracle_bench_test.cpp writer arrangement).
#include <arrow/array.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/io/file.h>
#include <arrow/memory_pool.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Error;
using atx::core::Ok;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::array<int, 8> kTenorDays{7, 14, 30, 60, 91, 182, 273, 365};
constexpr double kTradingMinutesPerYear = 390.0 * 252.0;
constexpr double kZSpan = 2.5;               // strikes span |z| <= kZSpan around F
constexpr double kDividendPayerShare = 0.30; // fraction of uids with cash dividends
constexpr std::size_t kMaxRows = std::size_t{1} << 31;
constexpr std::size_t kRowGroupRows = 65536;
constexpr std::size_t kPricingChunk = 256; // rows per dynamic claim

// ── Portable RNG transforms over mt19937_64 ─────────────────────────────────

class Rng {
public:
  explicit Rng(std::uint64_t seed) : eng_{seed} {}
  // [0, 1) with 53 random bits.
  [[nodiscard]] double uniform() { return static_cast<double>(eng_() >> 11) * 0x1.0p-53; }
  [[nodiscard]] double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  // Box-Muller (one deviate per call); u1 in (0, 1] so the log is finite.
  [[nodiscard]] double normal() {
    const double u1 = static_cast<double>((eng_() >> 11) + 1) * 0x1.0p-53;
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
  }

private:
  std::mt19937_64 eng_;
};

// ── Civil-day arithmetic (Hinnant; proleptic Gregorian, days since 1970-01-01) ──

struct Civil {
  std::int64_t y{0};
  std::int64_t m{0};
  std::int64_t d{0};
};

[[nodiscard]] std::int64_t days_from_civil(Civil c) noexcept {
  const std::int64_t y = c.y - (c.m <= 2 ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = y - era * 400;
  const std::int64_t mp = (c.m + 9) % 12;
  const std::int64_t doy = (153 * mp + 2) / 5 + c.d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

[[nodiscard]] Civil civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  return Civil{yoe + era * 400 + (m <= 2 ? 1 : 0), m, d};
}

// "YYYY-MM-DD" -> civil day number; nullopt on a malformed / non-round-tripping date.
[[nodiscard]] std::optional<std::int64_t> parse_day(std::string_view s) noexcept {
  auto digits = [&](std::size_t pos, std::size_t n) -> std::optional<std::int64_t> {
    std::int64_t v = 0;
    for (std::size_t i = pos; i < pos + n; ++i) {
      if (s[i] < '0' || s[i] > '9') {
        return std::nullopt;
      }
      v = v * 10 + (s[i] - '0');
    }
    return v;
  };
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return std::nullopt;
  }
  const auto y = digits(0, 4);
  const auto m = digits(5, 2);
  const auto d = digits(8, 2);
  if (!y || !m || !d || *m < 1 || *m > 12 || *d < 1 || *d > 31) {
    return std::nullopt;
  }
  const std::int64_t day = days_from_civil(Civil{*y, *m, *d});
  const Civil back = civil_from_days(day);
  if (back.y != *y || back.m != *m || back.d != *d) {
    return std::nullopt;
  }
  return day;
}

[[nodiscard]] double years_between(std::int64_t from_ns, std::int64_t to_ns) noexcept {
  // Calendar365 is infallible (vol_time.hpp); a default TimeSpec is that branch.
  const Result<double> t = time_to_expiry_years(from_ns, to_ns, TimeSpec{});
  return t.has_value() ? *t : std::numeric_limits<double>::quiet_NaN();
}

// ── The generated market ────────────────────────────────────────────────────

struct Geometry {
  std::int64_t day0{0};
  std::vector<std::int64_t> bucket_ns;
  std::vector<double> bucket_minutes; // ET minutes after midnight
  std::vector<std::int64_t> expiry_ns;
  std::vector<Civil> expiry_civil;
};

struct UidModel {
  std::string ticker;
  double spot0{0.0};
  double r{0.0};
  double q{0.0};
  double eta{0.0};
  double div_amount{0.0};
  std::vector<std::int64_t> ex_ns;          // cash-dividend ex instants (empty: no divs)
  std::vector<double> atm0;                 // per expiry
  std::vector<double> rho0;                 // per expiry
  std::vector<std::vector<double>> strikes; // per expiry, ascending, unique
};

// The market state of one uid at one bucket (random walks accumulated).
struct PathPoint {
  double spot{0.0};
  double level{0.0}; // additive ATM vol shift
  double skew{0.0};  // additive rho shift
};

// Everything a row's pricing needs that is shared by its (bucket, uid, expiry).
struct SliceInputs {
  double spot{0.0};
  double T{0.0};
  double r{0.0};
  double q{0.0};
  double ddiv{0.0};
  double q_eff{0.0};
  double forward{0.0};
  double atm{0.0};
  double theta{0.0}; // ATM total variance
  double phi{0.0};
  double rho{0.0};
};

[[nodiscard]] Status validate(const SyntheticSpiderRockSpec &spec, std::string_view out_dir) {
  auto bad = [](const std::string &why) {
    return Err(ErrorCode::InvalidArgument, "synthetic spiderrock store: " + why);
  };
  if (out_dir.empty()) {
    return bad("empty out_dir");
  }
  if (spec.buckets_et.empty()) {
    return bad("no buckets");
  }
  if (spec.n_underliers == 0 || spec.n_underliers > 100000) {
    return bad("n_underliers must be in [1, 100000]");
  }
  if (spec.n_expiries == 0 || spec.n_expiries > kTenorDays.size()) {
    return bad("n_expiries must be in [1, 8]");
  }
  if (spec.n_strikes == 0 || spec.n_strikes > 1001) {
    return bad("n_strikes must be in [1, 1001]");
  }
  const bool vols_ok = std::isfinite(spec.spot_vol) && spec.spot_vol >= 0.0 &&
                       std::isfinite(spec.level_shock_vol) && spec.level_shock_vol >= 0.0 &&
                       std::isfinite(spec.skew_shock_vol) && spec.skew_shock_vol >= 0.0;
  if (!vols_ok) {
    return bad("shock vols must be finite and >= 0");
  }
  const std::size_t rows = spec.buckets_et.size() * spec.n_underliers * spec.n_expiries *
                           spec.n_strikes * 2;
  if (rows >= kMaxRows) {
    return bad("store too large (rows >= 2^31)");
  }
  return Ok();
}

[[nodiscard]] Result<Geometry> make_geometry(const SyntheticSpiderRockSpec &spec) {
  const std::optional<std::int64_t> day0 = parse_day(spec.date);
  if (!day0.has_value()) {
    return Err(ErrorCode::InvalidArgument, "synthetic spiderrock store: bad date " + spec.date);
  }
  Geometry g;
  g.day0 = *day0;
  for (const std::string &b : spec.buckets_et) {
    ATX_TRY(const std::int64_t ts, spiderrock_bucket_instant_ns(spec.date, b));
    if (!g.bucket_ns.empty() && ts <= g.bucket_ns.back()) {
      return Err(ErrorCode::InvalidArgument,
                 "synthetic spiderrock store: buckets must be strictly ascending");
    }
    g.bucket_ns.push_back(ts);
    g.bucket_minutes.push_back(static_cast<double>((b[0] - '0') * 600 + (b[1] - '0') * 60 +
                                                   (b[2] - '0') * 10 + (b[3] - '0')));
  }
  for (std::size_t e = 0; e < spec.n_expiries; ++e) {
    const std::int64_t day = g.day0 + kTenorDays[e];
    g.expiry_ns.push_back(
        settlement_instant_ns(static_cast<std::int32_t>(day), SettlementSession::Pm));
    g.expiry_civil.push_back(civil_from_days(day));
  }
  return Ok(std::move(g));
}

[[nodiscard]] double ddiv_at(const UidModel &m, double r, std::int64_t ts_ns,
                             std::int64_t expiry_ns) noexcept {
  double pv = 0.0;
  for (const std::int64_t ex : m.ex_ns) {
    if (ex > ts_ns && ex < expiry_ns) {
      pv += m.div_amount * std::exp(-r * years_between(ts_ns, ex));
    }
  }
  return pv;
}

// eSSVI total variance at log-moneyness k.
[[nodiscard]] double essvi_w(double k, double theta, double phi, double rho) noexcept {
  const double pk = phi * k;
  return 0.5 * theta * (1.0 + rho * pk + std::sqrt((pk + rho) * (pk + rho) + 1.0 - rho * rho));
}

// Largest "nice" decimal step <= spacing / 2, so snapping cannot merge strikes.
[[nodiscard]] std::int64_t strike_step_cents(double min_spacing) noexcept {
  constexpr std::array<std::int64_t, 10> kStepsCents{1,   5,   10,  25,   50,
                                                     100, 250, 500, 1000, 2500};
  std::int64_t best = kStepsCents.front();
  for (const std::int64_t s : kStepsCents) {
    if (static_cast<double>(s) / 100.0 <= 0.5 * min_spacing) {
      best = s;
    }
  }
  return best;
}

[[nodiscard]] std::vector<double> listed_strikes(double forward, double atm, double T,
                                                 std::size_t n) {
  std::vector<double> raw(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double z =
        n == 1 ? 0.0 : -kZSpan + 2.0 * kZSpan * static_cast<double>(i) / static_cast<double>(n - 1);
    raw[i] = forward * std::exp(z * atm * std::sqrt(T));
  }
  const std::int64_t step = strike_step_cents(n > 1 ? raw[1] - raw[0] : 0.0);
  std::vector<double> out(n);
  std::int64_t prev = 0;
  for (std::size_t i = 0; i < n; ++i) {
    std::int64_t cents = std::llround(raw[i] * 100.0 / static_cast<double>(step)) * step;
    cents = std::max({cents, prev + step, step}); // strictly ascending, > 0
    out[i] = static_cast<double>(cents) / 100.0;
    prev = cents;
  }
  return out;
}

void draw_dividends(const Geometry &g, Rng &rng, UidModel &m) {
  const bool pays = rng.uniform() < kDividendPayerShare;
  const double cash_yield = rng.uniform(0.005, 0.03);
  const auto first = static_cast<std::int64_t>(std::floor(rng.uniform(3.0, 91.0)));
  if (!pays) {
    return;
  }
  m.div_amount = m.spot0 * cash_yield / 4.0;
  for (std::int64_t day = g.day0 + first; day <= g.day0 + 400; day += 91) {
    m.ex_ns.push_back(settlement_instant_ns(static_cast<std::int32_t>(day), SettlementSession::Am));
  }
}

[[nodiscard]] UidModel draw_uid(const SyntheticSpiderRockSpec &spec, const Geometry &g,
                                std::size_t u, Rng &rng) {
  UidModel m;
  std::array<char, 16> name{};
  const int len = std::snprintf(name.data(), name.size(), "SYN%04zu", u + 1);
  m.ticker.assign(name.data(), static_cast<std::size_t>(std::max(len, 0)));
  m.spot0 = rng.uniform(20.0, 400.0);
  m.r = rng.uniform(0.03, 0.05);
  m.q = rng.uniform(0.0, 0.03);
  m.eta = rng.uniform(0.4, 1.0);
  const double atm_base = rng.uniform(0.15, 0.6);
  const double tilt = rng.uniform(-0.3, 0.3);
  const double rho_base = rng.uniform(-0.6, -0.1);
  draw_dividends(g, rng, m);
  for (std::size_t e = 0; e < spec.n_expiries; ++e) {
    const double T0 = years_between(g.bucket_ns.front(), g.expiry_ns[e]);
    const double atm = std::clamp(atm_base * (1.0 + tilt * (std::sqrt(T0) - 0.5)), 0.15, 0.6);
    const double rho = std::clamp(rho_base + rng.uniform(-0.05, 0.05), -0.6, -0.1);
    const double ddiv0 = ddiv_at(m, m.r, g.bucket_ns.front(), g.expiry_ns[e]);
    const double f0 = (m.spot0 - ddiv0) * std::exp((m.r - m.q) * T0);
    m.atm0.push_back(atm);
    m.rho0.push_back(rho);
    m.strikes.push_back(listed_strikes(f0, atm, T0, spec.n_strikes));
  }
  return m;
}

// paths[b * n_uids + u]: draws in the fixed order (bucket, uid, {spot, level, skew}).
[[nodiscard]] std::vector<PathPoint> draw_paths(const SyntheticSpiderRockSpec &spec,
                                                const Geometry &g,
                                                const std::vector<UidModel> &models, Rng &rng) {
  const std::size_t nu = models.size();
  const std::size_t nb = g.bucket_ns.size();
  std::vector<PathPoint> paths(nb * nu);
  for (std::size_t u = 0; u < nu; ++u) {
    paths[u] = PathPoint{models[u].spot0, 0.0, 0.0};
  }
  for (std::size_t b = 1; b < nb; ++b) {
    const double dt = (g.bucket_minutes[b] - g.bucket_minutes[b - 1]) / kTradingMinutesPerYear;
    const double sv = spec.spot_vol;
    for (std::size_t u = 0; u < nu; ++u) {
      const PathPoint &p = paths[(b - 1) * nu + u];
      const double z_spot = rng.normal();
      const double z_level = rng.normal();
      const double z_skew = rng.normal();
      PathPoint &c = paths[b * nu + u];
      c.spot = p.spot * std::exp(-0.5 * sv * sv * dt + sv * std::sqrt(dt) * z_spot);
      c.level = p.level + spec.level_shock_vol * z_level;
      c.skew = p.skew + spec.skew_shock_vol * z_skew;
    }
  }
  return paths;
}

[[nodiscard]] SliceInputs slice_inputs(const UidModel &m, const PathPoint &p, std::size_t e,
                                       std::int64_t ts_ns, std::int64_t expiry_ns) noexcept {
  SliceInputs s;
  s.spot = p.spot;
  s.T = years_between(ts_ns, expiry_ns);
  s.r = m.r;
  s.q = m.q;
  s.ddiv = ddiv_at(m, m.r, ts_ns, expiry_ns);
  s.q_eff = spiderrock_q_eff(s.spot, s.ddiv, s.r, s.q, s.T);
  s.forward = (s.spot - s.ddiv) * std::exp((s.r - s.q) * s.T);
  s.atm = std::clamp(m.atm0[e] + p.level, 0.05, 2.0);
  s.theta = s.atm * s.atm * s.T;
  s.phi = m.eta / std::sqrt(s.theta);
  s.rho = std::clamp(m.rho0[e] + p.skew, -0.95, 0.95);
  return s;
}

// ── Pricing (fanned out, disjoint row slots) ────────────────────────────────

struct RowOut {
  double sigma{0.0};
  double price{0.0};
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0};  // vendor units
  double theta{0.0}; // vendor units
  bool ok{false};
};

struct Layout {
  std::size_t nb{0};
  std::size_t nu{0};
  std::size_t ne{0};
  std::size_t nk{0};
  [[nodiscard]] std::size_t rows_per_bucket() const noexcept { return nu * ne * nk * 2; }
  [[nodiscard]] std::size_t slice(std::size_t b, std::size_t u, std::size_t e) const noexcept {
    return (b * nu + u) * ne + e;
  }
};

void price_row(std::size_t g, const Layout &lay, const std::vector<UidModel> &models,
               const std::vector<SliceInputs> &slices, const AlOpts &opts, RowOut &out) {
  const std::size_t side_i = g % 2;
  const std::size_t k = (g / 2) % lay.nk;
  const std::size_t e = (g / (2 * lay.nk)) % lay.ne;
  const std::size_t u = (g / (2 * lay.nk * lay.ne)) % lay.nu;
  const std::size_t b = g / lay.rows_per_bucket();
  const SliceInputs &s = slices[lay.slice(b, u, e)];
  const double K = models[u].strikes[e][k];
  out.sigma = std::sqrt(essvi_w(std::log(K / s.forward), s.theta, s.phi, s.rho) / s.T);
  const Side side = side_i == 0 ? Side::Call : Side::Put;
  const Result<AmericanGreeks> gr = american_greeks_al(s.spot, K, s.T, out.sigma, s.r, s.q_eff,
                                                       side, opts, /*need_vega=*/true,
                                                       /*need_rho=*/false, /*need_charm=*/false);
  if (!gr.has_value() || !std::isfinite(gr->price)) {
    out.ok = false;
    return;
  }
  out.price = gr->price;
  out.delta = gr->delta;
  out.gamma = gr->gamma;
  out.vega = gr->vega * kSpiderRockVegaScale;
  out.theta = gr->theta * kSpiderRockThetaScale;
  out.ok = true;
}

[[nodiscard]] std::vector<RowOut> price_all(const Layout &lay, const std::vector<UidModel> &models,
                                            const std::vector<SliceInputs> &slices,
                                            unsigned n_threads) {
  const std::size_t n = lay.nb * lay.rows_per_bucket();
  std::vector<RowOut> out(n);
  const AlOpts opts = al_fast_opts();
  const std::size_t n_chunks = (n + kPricingChunk - 1) / kPricingChunk;
  // Each claimed chunk writes only its own [lo, hi) slots of `out`; every input is
  // read-only, so the result is independent of the worker count and claim order.
  pricing_executor().run_dynamic(n_chunks, n_threads, [&](std::size_t c, std::size_t) {
    const std::size_t lo = c * kPricingChunk;
    const std::size_t hi = std::min(n, lo + kPricingChunk);
    for (std::size_t g = lo; g < hi; ++g) {
      price_row(g, lay, models, slices, opts, out[g]);
    }
  });
  return out;
}

// ── Parquet writing ─────────────────────────────────────────────────────────

[[nodiscard]] Error from_arrow(const arrow::Status &s, const std::string &ctx) {
  return Error{ErrorCode::IoError, ctx + ": " + s.ToString()};
}

// Column-major staging of one bucket's rows (valid flags for nullable columns).
struct BucketColumns {
  std::vector<std::string> tk;
  std::vector<std::string> cp;
  std::vector<std::int64_t> yr, mn, dy;
  std::vector<double> strike, uprc, rate, sdiv, ddiv, years, srvol, srprc, de, ga, th, ve,
      bid_iv, ask_iv, error, bid, ask, atm;
  std::vector<std::uint8_t> vendor_valid;
  std::vector<std::uint8_t> bid_valid;
};

void quote_columns(const RowOut &o, BucketColumns &c) {
  const std::uint8_t valid = o.ok ? 1 : 0;
  const double half = std::max(0.01, 0.02 * o.price);
  double bid = std::floor((o.price - half) * 100.0) / 100.0;
  bid = bid < 0.01 ? 0.0 : bid;
  c.srprc.push_back(o.ok ? o.price : 0.0);
  c.de.push_back(o.ok ? o.delta : 0.0);
  c.ga.push_back(o.ok ? o.gamma : 0.0);
  c.th.push_back(o.ok ? o.theta : 0.0);
  c.ve.push_back(o.ok ? o.vega : 0.0);
  c.bid.push_back(o.ok ? bid : 0.0);
  c.ask.push_back(o.ok ? std::ceil((o.price + half) * 100.0) / 100.0 : 0.0);
  c.vendor_valid.push_back(valid);
  c.bid_valid.push_back(valid);
  c.bid_iv.push_back(std::max(o.sigma - 0.005, 0.001));
  c.ask_iv.push_back(o.sigma + 0.005);
  c.error.push_back(0.0);
}

[[nodiscard]] BucketColumns bucket_columns(std::size_t b, const Layout &lay, const Geometry &g,
                                           const std::vector<UidModel> &models,
                                           const std::vector<SliceInputs> &slices,
                                           const std::vector<RowOut> &rows) {
  BucketColumns c;
  const std::size_t base = b * lay.rows_per_bucket();
  for (std::size_t i = 0; i < lay.rows_per_bucket(); ++i) {
    const std::size_t gi = base + i;
    const std::size_t k = (i / 2) % lay.nk;
    const std::size_t e = (i / (2 * lay.nk)) % lay.ne;
    const std::size_t u = i / (2 * lay.nk * lay.ne);
    const SliceInputs &s = slices[lay.slice(b, u, e)];
    c.tk.push_back(models[u].ticker);
    c.cp.push_back(i % 2 == 0 ? "Call" : "Put");
    c.yr.push_back(g.expiry_civil[e].y);
    c.mn.push_back(g.expiry_civil[e].m);
    c.dy.push_back(g.expiry_civil[e].d);
    c.strike.push_back(models[u].strikes[e][k]);
    c.uprc.push_back(s.spot);
    c.rate.push_back(s.r);
    c.sdiv.push_back(s.q);
    c.ddiv.push_back(s.ddiv);
    c.years.push_back(s.T);
    c.srvol.push_back(rows[gi].sigma);
    c.atm.push_back(s.atm);
    quote_columns(rows[gi], c);
  }
  return c;
}

struct ArrowColumns {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
};

[[nodiscard]] Status add_strings(ArrowColumns &out, const char *name,
                                 const std::vector<std::string> &v) {
  arrow::LargeStringBuilder b;
  for (const std::string &s : v) {
    const arrow::Status st = b.Append(s);
    if (!st.ok()) {
      return Err(from_arrow(st, std::string("synthetic store: column ") + name));
    }
  }
  auto arr = b.Finish();
  if (!arr.ok()) {
    return Err(from_arrow(arr.status(), std::string("synthetic store: column ") + name));
  }
  out.fields.push_back(arrow::field(name, arrow::large_utf8()));
  out.arrays.push_back(*std::move(arr));
  return Ok();
}

template <class Builder, class T>
[[nodiscard]] Status add_numbers(ArrowColumns &out, const char *name,
                                 const std::shared_ptr<arrow::DataType> &type,
                                 const std::vector<T> &v,
                                 const std::vector<std::uint8_t> *valid = nullptr) {
  Builder b;
  const arrow::Status st = b.AppendValues(v.data(), static_cast<std::int64_t>(v.size()),
                                          valid != nullptr ? valid->data() : nullptr);
  auto arr = st.ok() ? b.Finish() : arrow::Result<std::shared_ptr<arrow::Array>>(st);
  if (!arr.ok()) {
    return Err(from_arrow(arr.status(), std::string("synthetic store: column ") + name));
  }
  out.fields.push_back(arrow::field(name, type));
  out.arrays.push_back(*std::move(arr));
  return Ok();
}

[[nodiscard]] Result<ArrowColumns> to_arrow(const BucketColumns &c) {
  ArrowColumns a;
  const auto f64 = arrow::float64();
  const auto i64 = arrow::int64();
  using DB = arrow::DoubleBuilder;
  using IB = arrow::Int64Builder;
  ATX_TRY_VOID(add_strings(a, "undSecKey_tk", c.tk));
  ATX_TRY_VOID(add_strings(a, "okey_cp", c.cp));
  ATX_TRY_VOID((add_numbers<IB>(a, "okey_yr", i64, c.yr)));
  ATX_TRY_VOID((add_numbers<IB>(a, "okey_mn", i64, c.mn)));
  ATX_TRY_VOID((add_numbers<IB>(a, "okey_dy", i64, c.dy)));
  ATX_TRY_VOID((add_numbers<DB>(a, "okey_xx", f64, c.strike)));
  ATX_TRY_VOID((add_numbers<DB>(a, "uPrc", f64, c.uprc)));
  ATX_TRY_VOID((add_numbers<DB>(a, "rate", f64, c.rate)));
  ATX_TRY_VOID((add_numbers<DB>(a, "sdiv", f64, c.sdiv)));
  ATX_TRY_VOID((add_numbers<DB>(a, "ddiv", f64, c.ddiv)));
  ATX_TRY_VOID((add_numbers<DB>(a, "years", f64, c.years)));
  ATX_TRY_VOID((add_numbers<DB>(a, "srVol", f64, c.srvol)));
  ATX_TRY_VOID((add_numbers<DB>(a, "srPrc", f64, c.srprc, &c.vendor_valid)));
  ATX_TRY_VOID((add_numbers<DB>(a, "de", f64, c.de, &c.vendor_valid)));
  ATX_TRY_VOID((add_numbers<DB>(a, "ga", f64, c.ga, &c.vendor_valid)));
  ATX_TRY_VOID((add_numbers<DB>(a, "th", f64, c.th, &c.vendor_valid)));
  ATX_TRY_VOID((add_numbers<DB>(a, "ve", f64, c.ve, &c.vendor_valid)));
  ATX_TRY_VOID((add_numbers<DB>(a, "bidIV", f64, c.bid_iv)));
  ATX_TRY_VOID((add_numbers<DB>(a, "askIV", f64, c.ask_iv)));
  ATX_TRY_VOID((add_numbers<DB>(a, "error", f64, c.error)));
  ATX_TRY_VOID((add_numbers<DB>(a, "bidPrc", f64, c.bid, &c.bid_valid)));
  ATX_TRY_VOID((add_numbers<DB>(a, "askPrc", f64, c.ask, &c.bid_valid)));
  ATX_TRY_VOID((add_numbers<DB>(a, "atmVol", f64, c.atm)));
  return Ok(std::move(a));
}

[[nodiscard]] Status write_bucket_file(const fs::path &dir, const ArrowColumns &cols,
                                       std::int64_t n_rows) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "synthetic store: mkdir " + dir.string() + ": " + ec.message());
  }
  const auto table = arrow::Table::Make(arrow::schema(cols.fields), cols.arrays, n_rows);
  const std::string path = (dir / "data.parquet").string();
  auto sink = arrow::io::FileOutputStream::Open(path);
  if (!sink.ok()) {
    return Err(from_arrow(sink.status(), "synthetic store: open " + path));
  }
  const std::shared_ptr<parquet::WriterProperties> props =
      parquet::WriterProperties::Builder().compression(parquet::Compression::ZSTD)->build();
  const arrow::Status st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *sink,
                                                      kRowGroupRows, props);
  if (!st.ok()) {
    return Err(from_arrow(st, "synthetic store: write " + path));
  }
  const arrow::Status closed = (*sink)->Close();
  if (!closed.ok()) {
    return Err(from_arrow(closed, "synthetic store: close " + path));
  }
  return Ok();
}

[[nodiscard]] double seconds_since(Clock::time_point t0) noexcept {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

} // namespace

Status write_synthetic_spiderrock_store(const SyntheticSpiderRockSpec &spec,
                                        std::string_view out_dir, SyntheticStoreReport &report) {
  const Clock::time_point t0 = Clock::now();
  report = SyntheticStoreReport{};
  ATX_TRY_VOID(validate(spec, out_dir));
  ATX_TRY(const Geometry geo, make_geometry(spec));

  Rng rng{spec.seed};
  std::vector<UidModel> models;
  models.reserve(spec.n_underliers);
  for (std::size_t u = 0; u < spec.n_underliers; ++u) {
    models.push_back(draw_uid(spec, geo, u, rng));
  }
  const std::vector<PathPoint> paths = draw_paths(spec, geo, models, rng);

  const Layout lay{geo.bucket_ns.size(), models.size(), spec.n_expiries, spec.n_strikes};
  std::vector<SliceInputs> slices(lay.nb * lay.nu * lay.ne);
  for (std::size_t b = 0; b < lay.nb; ++b) {
    for (std::size_t u = 0; u < lay.nu; ++u) {
      for (std::size_t e = 0; e < lay.ne; ++e) {
        slices[lay.slice(b, u, e)] = slice_inputs(models[u], paths[b * lay.nu + u], e,
                                                  geo.bucket_ns[b], geo.expiry_ns[e]);
      }
    }
  }

  const Clock::time_point tp = Clock::now();
  const std::vector<RowOut> rows = price_all(lay, models, slices, spec.n_threads);
  report.pricing_seconds = seconds_since(tp);
  report.pricing_failures = static_cast<std::size_t>(
      std::count_if(rows.begin(), rows.end(), [](const RowOut &r) { return !r.ok; }));

  const fs::path root = fs::path{std::string{out_dir}} / ("date=" + spec.date);
  for (std::size_t b = 0; b < lay.nb; ++b) {
    const BucketColumns cols = bucket_columns(b, lay, geo, models, slices, rows);
    ATX_TRY(const ArrowColumns arrow_cols, to_arrow(cols));
    const auto n_rows = static_cast<std::int64_t>(lay.rows_per_bucket());
    ATX_TRY_VOID(
        write_bucket_file(root / ("bucket_et=" + spec.buckets_et[b]), arrow_cols, n_rows));
    report.rows_written += lay.rows_per_bucket();
    ++report.files_written;
  }
  report.seconds = seconds_since(t0);
  return Ok();
}

Status write_synthetic_spiderrock_store(const SyntheticSpiderRockSpec &spec,
                                        std::string_view out_dir) {
  SyntheticStoreReport report;
  return write_synthetic_spiderrock_store(spec, out_dir, report);
}

} // namespace atx::vol
