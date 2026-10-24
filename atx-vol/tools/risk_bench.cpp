// risk_bench — the ENGINE driver of atx-vol-risk-bench (see risk_bench.hpp):
// `run_risk_bench` loads SpiderRock buckets, builds one MarketState per bucket,
// selects the book on the first bucket and drives RiskStream (plus the threads=1
// shadow stream for --determinism and the cold-kernel stream for --kernel both),
// filling the report through the engine-free core, and writes the --dump-legs CSV.
// Links only where the risk engine is compiled in (the tool target; the
// ATX_VOL_RISK_E2E test).

#include "risk_bench.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol::riskbench {

using atx::core::Err;
using atx::core::Ok;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double seconds_since(Clock::time_point t0) noexcept {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

// One loaded bucket, its owning state storage and the MarketState view over it.
// Heap-allocated and never moved once `state` is built: the state's spans point
// into `storage`. Kept alive until every stream has stepped past it (RiskStream
// reads the PREVIOUS state during step()).
struct Snapshot {
  SpiderRockBucket bucket;
  MarketStateStorage storage;
  std::optional<MarketState> state;
  BucketStateReport report;
  std::vector<SpiderRockExpiryForward> forwards; // per ExpiryCarry row, (uid, expiry) order
  double load_seconds{0.0};
  double state_seconds{0.0};
};

[[nodiscard]] Result<std::unique_ptr<Snapshot>> load_snapshot(const SpiderRockStoreSpec &spec,
                                                               const std::string &bucket_et,
                                                               UnderlierTable &table) {
  auto snap = std::make_unique<Snapshot>();
  const Clock::time_point t0 = Clock::now();
  ATX_TRY(snap->bucket, load_spiderrock_bucket(spec, bucket_et, table));
  snap->load_seconds = seconds_since(t0);
  const Clock::time_point t1 = Clock::now();
  ATX_TRY(snap->storage, market_state_from_bucket(snap->bucket, spec.time, &snap->report,
                                                  &snap->forwards));
  ATX_TRY(MarketState state, make_market_state(snap->storage));
  snap->state.emplace(std::move(state));
  snap->state_seconds = seconds_since(t1);
  return Ok(std::move(snap));
}

[[nodiscard]] BucketLoad bucket_load(const Snapshot &s) {
  BucketLoad b;
  b.bucket = s.bucket.bucket_et;
  b.rows_read = s.bucket.rows_read;
  b.rows_kept = static_cast<std::int64_t>(s.bucket.size());
  b.rows_dropped_null = s.bucket.rows_dropped_null;
  b.rows_dropped_bad_input = s.bucket.rows_dropped_bad_input;
  b.rows_not_selected = s.bucket.rows_not_selected;
  b.dropped_non_american = s.bucket.dropped_non_american;
  b.dropped_nonstandard_root = s.bucket.dropped_nonstandard_root;
  b.dropped_forward_framework = s.bucket.dropped_forward_framework;
  b.pv_divs_from_ddiv_pv = s.report.pv_divs_from_ddiv_pv;
  b.n_expiries_implied = s.report.n_expiries_implied;
  b.n_expiries_published = s.report.n_expiries_published;
  b.max_forward_gap_rel = s.report.max_implied_vs_published_forward_rel;
  b.implied_carry_seconds = s.report.implied_carry_seconds;
  b.rows_null_vendor = s.bucket.rows_null_vendor;
  b.n_uids = s.report.n_uids;
  b.n_marks = s.report.n_marks;
  b.n_uids_with_divs = s.report.n_uids_with_divs;
  b.max_ddiv_spread = s.report.max_ddiv_spread;
  return b;
}

[[nodiscard]] RiskKernel make_kernel(bool cold, unsigned threads) {
  if (cold) {
    ColdKernelPolicy p;
    p.n_threads = threads;
    return RiskKernel::cold(p);
  }
  FamilyKernelPolicy p;
  p.n_threads = threads;
  return RiskKernel::family(p);
}

// The main stream plus the optional shadows, all over one book.
struct Streams {
  std::optional<RiskStream> main;
  std::optional<RiskStream> shadow; // --determinism: same kernel at threads = 1
  std::optional<RiskStream> cold;   // --kernel both: the cold oracle
};

[[nodiscard]] Streams make_streams(const RiskBook &book, const RiskBenchArgs &args) {
  RiskNeeds needs;
  needs.rho = args.needs_rho;
  ExplainOptions opts;
  opts.trapezoid_second_order = args.trapezoid;
  opts.n_threads = args.threads;
  const bool main_cold = args.kernel == BenchKernel::Cold;
  Streams s;
  s.main.emplace(book, make_kernel(main_cold, args.threads), needs, opts);
  if (args.determinism) {
    ExplainOptions serial = opts;
    serial.n_threads = 1;
    s.shadow.emplace(book, make_kernel(main_cold, 1), needs, serial);
  }
  if (args.kernel == BenchKernel::Both) {
    s.cold.emplace(book, make_kernel(true, args.threads), needs, opts);
  }
  return s;
}

// ── Step statistics ─────────────────────────────────────────────────────────

void fill_frame(StepReport &t, const RiskFrame &f) {
  t.evaluate_seconds = f.counters.evaluate_seconds;
  t.family_build_seconds = f.counters.family_build_seconds;
  t.families_built = f.counters.families_built;
  t.carry_sets_built = f.counters.carry_sets_built;
  t.family_rows = f.counters.family_rows;
  t.cold_rows = f.counters.cold_rows;
  t.exercised_rows = f.counters.exercised_rows;
  t.refused_rows = f.counters.refused_rows;
  for (const RiskRoute r : f.route) {
    const auto i = static_cast<std::size_t>(r);
    t.routes[std::min(i, kRouteCount - 1)] += 1;
  }
  for (const RiskFlags bits : f.flags) {
    for (std::size_t b = 0; b < kFlagCount; ++b) {
      t.flags[b] += (bits >> b) & 1u;
    }
  }
}

[[nodiscard]] AxisTotals axes_of(const ExplainTotals &x) noexcept {
  AxisTotals a;
  a.delta = x.pnl_delta;
  a.gamma = x.pnl_gamma;
  a.vega = x.pnl_vega;
  a.volga = x.pnl_volga;
  a.vanna = x.pnl_vanna;
  a.theta = x.pnl_theta;
  a.rho = x.pnl_rho;
  a.charm = x.pnl_charm;
  a.unexplained = x.pnl_unexplained;
  a.vol_level = x.vol_level;
  a.vol_skew = x.vol_skew;
  a.vol_curv = x.vol_curv;
  a.vol_resid = x.vol_resid;
  a.shares = x.shares_pnl;
  return a;
}

// Each Greek axis (and the remainder) as a fraction of the sum of their magnitudes;
// the vega split pieces as fractions of the same denominator.
[[nodiscard]] AxisTotals axis_shares(const AxisTotals &a) noexcept {
  const double denom = std::abs(a.delta) + std::abs(a.gamma) + std::abs(a.vega) +
                       std::abs(a.volga) + std::abs(a.vanna) + std::abs(a.theta) +
                       std::abs(a.rho) + std::abs(a.charm) + std::abs(a.unexplained);
  if (!(denom > 0.0)) {
    return AxisTotals{};
  }
  AxisTotals s;
  s.delta = a.delta / denom;
  s.gamma = a.gamma / denom;
  s.vega = a.vega / denom;
  s.volga = a.volga / denom;
  s.vanna = a.vanna / denom;
  s.theta = a.theta / denom;
  s.rho = a.rho / denom;
  s.charm = a.charm / denom;
  s.unexplained = a.unexplained / denom;
  s.vol_level = a.vol_level / denom;
  s.vol_skew = a.vol_skew / denom;
  s.vol_curv = a.vol_curv / denom;
  s.vol_resid = a.vol_resid / denom;
  s.shares = a.shares / denom;
  return s;
}

void fill_explain(StepReport &t, const ExplainResult &ex) {
  t.ts_from = ex.base_ts_ns;
  t.ts_to = ex.shifted_ts_ns;
  t.dt_years = ex.dt_years;
  t.explain_seconds = ex.explain_seconds;
  t.n_legs = ex.size();
  t.n_ok = ex.total.n_ok;
  t.n_excluded = ex.total.n_excluded;
  t.excluded_premium = ex.total.excluded_premium;
  t.excluded_by_reason = ex.excluded_count;
  t.pnl_total = ex.total.pnl_total;
  t.explained = ex.total.pnl_total - ex.total.pnl_unexplained;
  t.axes = axes_of(ex.total);
  t.axis_share = axis_shares(t.axes);
  std::vector<double> explained;
  std::vector<double> actual;
  explained.reserve(ex.size());
  actual.reserve(ex.size());
  for (std::size_t i = 0; i < ex.size(); ++i) {
    if (ex.status[i] != RiskLaneStatus::Ok) {
      continue;
    }
    actual.push_back(ex.pnl_total[i]);
    explained.push_back(ex.pnl_total[i] - ex.pnl_unexplained[i]);
    t.sum_abs_unexplained += std::abs(ex.pnl_unexplained[i]);
    t.sum_abs_pnl_total += std::abs(ex.pnl_total[i]);
  }
  t.explained_ratio = t.sum_abs_pnl_total > 0.0 ? t.sum_abs_unexplained / t.sum_abs_pnl_total
                                                : kNaN;
  t.spearman = spearman_rho(explained, actual);
  t.ks = ks_statistic(explained, actual);
  t.zone = pla_zone(t.spearman, t.ks);
}

// Ours vs the vendor's price / delta / vega on the shifted frame's Ok rows that carry
// VendorParityAvailable; vega comes from the bucket (vendor units -> per unit vol).
void fill_parity(StepReport &t, const RiskFrame &f, const PreparedRiskBook &pb,
                 const SpiderRockBucket &b) {
  std::vector<double> op;
  std::vector<double> vp;
  std::vector<double> od;
  std::vector<double> vd;
  std::vector<double> ov;
  std::vector<double> vv;
  for (std::size_t r = 0; r < f.size(); ++r) {
    if (f.status[r] != RiskLaneStatus::Ok ||
        !has_flag(f.flags[r], RiskFlag::VendorParityAvailable)) {
      continue;
    }
    op.push_back(f.price[r]);
    vp.push_back(f.vendor_price[r]);
    od.push_back(f.delta[r]);
    vd.push_back(f.vendor_delta[r]);
    const std::optional<std::size_t> row =
        find_bucket_row(b, pb.uid()[r], pb.expiry_ns()[r], pb.side()[r], pb.strike()[r]);
    if (row.has_value()) {
      ov.push_back(f.vega[r]);
      vv.push_back(b.vendor_vega[*row] / kSpiderRockVegaScale);
    }
  }
  t.parity_price = parity(op, vp, kParityPriceFloor);
  t.parity_delta = parity(od, vd, kParityDeltaFloor);
  t.parity_vega = parity(ov, vv, kParityVegaFloor);
}

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] bool columns_equal(std::span<const double> a, std::span<const double> b) noexcept {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](double x, double y) {
           return bits_equal(x, y);
         });
}

[[nodiscard]] bool totals_equal(const ExplainTotals &a, const ExplainTotals &b) noexcept {
  const AxisTotals x = axes_of(a);
  const AxisTotals y = axes_of(b);
  return bits_equal(a.pv_base, b.pv_base) && bits_equal(a.pv_target, b.pv_target) &&
         bits_equal(a.pnl_total, b.pnl_total) && bits_equal(x.delta, y.delta) &&
         bits_equal(x.gamma, y.gamma) && bits_equal(x.vega, y.vega) &&
         bits_equal(x.volga, y.volga) && bits_equal(x.vanna, y.vanna) &&
         bits_equal(x.theta, y.theta) && bits_equal(x.rho, y.rho) &&
         bits_equal(x.charm, y.charm) && bits_equal(x.unexplained, y.unexplained) &&
         bits_equal(x.vol_level, y.vol_level) && bits_equal(x.vol_skew, y.vol_skew) &&
         bits_equal(x.vol_curv, y.vol_curv) && bits_equal(x.vol_resid, y.vol_resid) &&
         bits_equal(x.shares, y.shares) && a.n_ok == b.n_ok && a.n_excluded == b.n_excluded &&
         bits_equal(a.excluded_premium, b.excluded_premium);
}

// --determinism: totals, per-leg P&L and the frame's price / Greek columns must be
// bit-identical between the N-thread and the 1-thread stream.
[[nodiscard]] bool deterministic(const ExplainResult &a, const RiskFrame &fa,
                                 const ExplainResult &b, const RiskFrame &fb) noexcept {
  return totals_equal(a.total, b.total) && columns_equal(a.pnl_total, b.pnl_total) &&
         columns_equal(a.pnl_unexplained, b.pnl_unexplained) &&
         columns_equal(fa.price, fb.price) && columns_equal(fa.delta, fb.delta) &&
         columns_equal(fa.gamma, fb.gamma) && columns_equal(fa.vega, fb.vega) &&
         columns_equal(fa.theta, fb.theta);
}

// NaN (JSON null) when no row is Ok in both: "nothing compared" is not "no difference".
[[nodiscard]] double max_abs_diff(std::span<const double> a, std::span<const double> b,
                                  std::span<const std::uint8_t> both_ok) noexcept {
  double m = 0.0;
  bool any = false;
  for (std::size_t i = 0; i < both_ok.size(); ++i) {
    if (both_ok[i] != 0) {
      m = std::max(m, std::abs(a[i] - b[i]));
      any = true;
    }
  }
  return any ? m : kNaN;
}

// --kernel both: the cold frame / explain against the main ones.
void fill_cold(ColdComparison &c, const RiskFrame &m, const RiskFrame &k, const ExplainResult &mx,
               const ExplainResult &kx, double main_evaluate_seconds) {
  c.ran = true;
  c.evaluate_seconds = k.counters.evaluate_seconds;
  c.speedup = main_evaluate_seconds > 0.0 ? c.evaluate_seconds / main_evaluate_seconds : kNaN;
  std::vector<std::uint8_t> ok(std::min(m.size(), k.size()));
  for (std::size_t r = 0; r < ok.size(); ++r) {
    ok[r] = (m.status[r] == RiskLaneStatus::Ok && k.status[r] == RiskLaneStatus::Ok) ? 1 : 0;
    c.n_compared += ok[r];
  }
  c.max_abs_price = max_abs_diff(m.price, k.price, ok);
  c.max_abs_delta = max_abs_diff(m.delta, k.delta, ok);
  c.max_abs_gamma = max_abs_diff(m.gamma, k.gamma, ok);
  c.max_abs_vega = max_abs_diff(m.vega, k.vega, ok);
  c.max_abs_theta = max_abs_diff(m.theta, k.theta, ok);
  c.max_abs_vanna = max_abs_diff(m.vanna, k.vanna, ok);
  c.max_abs_volga = max_abs_diff(m.volga, k.volga, ok);
  c.max_abs_charm = max_abs_diff(m.charm, k.charm, ok);
  std::vector<std::uint8_t> legs_ok(std::min(mx.size(), kx.size()));
  for (std::size_t i = 0; i < legs_ok.size(); ++i) {
    legs_ok[i] = (mx.status[i] == RiskLaneStatus::Ok && kx.status[i] == RiskLaneStatus::Ok);
  }
  c.max_abs_pnl_total = max_abs_diff(mx.pnl_total, kx.pnl_total, legs_ok);
}

// ── --dump-legs ─────────────────────────────────────────────────────────────

// The per-option-leg CSV: opened (header written) before any bucket is read, so a bad
// path fails fast; one block per selected step (dump_step_selected).
struct LegDump {
  std::ofstream file;
  std::string path;
  std::size_t limit{1};
  std::size_t steps_seen{0};
  std::size_t steps_written{0};
  std::size_t rows{0};
  const UnderlierTable *table{nullptr};
};

[[nodiscard]] Status write_dump_text(LegDump &d, std::string_view text) {
  d.file.write(text.data(), static_cast<std::streamsize>(text.size()));
  d.file.flush();
  if (!d.file) {
    return Err(ErrorCode::IoError, "risk-bench: --dump-legs write failed: " + d.path);
  }
  return Ok();
}

// nullptr without --dump-legs.
[[nodiscard]] Result<std::unique_ptr<LegDump>> open_leg_dump(const RiskBenchArgs &args,
                                                             const UnderlierTable &table) {
  if (args.dump_legs.empty()) {
    return Ok(std::unique_ptr<LegDump>{});
  }
  auto d = std::make_unique<LegDump>();
  d->path = args.dump_legs;
  d->limit = args.dump_steps;
  d->table = &table;
  d->file.open(d->path, std::ios::binary | std::ios::trunc); // binary: LF line endings
  if (!d->file) {
    return Err(ErrorCode::IoError, "risk-bench: cannot open --dump-legs file: " + d->path);
  }
  ATX_TRY_VOID(write_dump_text(*d, leg_dump_csv_header()));
  return Ok(std::move(d));
}

struct CarryUsed {
  CarrySource source{CarrySource::Absent};
  double rate{kNaN};
  double q{kNaN};
  double pv_divs{kNaN};
};

// The carry the state hands a contract under CarryFrom::State (market_state.hpp's
// precedence): the exact-expiry ExpiryCarry row, else the flat fallbacks.
[[nodiscard]] CarryUsed carry_used(const MarketState &state, std::uint32_t uid,
                                   std::int64_t expiry_ns) noexcept {
  CarryUsed c;
  const UnderlierState *u = state.underlier(uid);
  if (u == nullptr) {
    return c;
  }
  if (u->carry == CarryFrom::Surface) {
    c.source = CarrySource::Surface;
    return c;
  }
  const auto it = std::lower_bound(
      u->term_carry.begin(), u->term_carry.end(), expiry_ns,
      [](const ExpiryCarry &e, std::int64_t x) { return e.expiry_ns < x; });
  if (it != u->term_carry.end() && it->expiry_ns == expiry_ns) {
    return CarryUsed{CarrySource::TermCarry, it->rate, it->q, it->pv_divs};
  }
  if (u->yield != nullptr || !u->cash_divs.empty()) {
    c.source = CarrySource::FlatCurveOrDivs;
    c.rate = u->yield == nullptr ? u->flat_rate : kNaN;
    c.q = u->flat_q;
    return c;
  }
  return CarryUsed{CarrySource::Flat, u->flat_rate, u->flat_q, 0.0};
}

// The base state's basis for (uid, expiry); nullptr when it wrote no such row.
[[nodiscard]] const SpiderRockExpiryForward *expiry_forward(const Snapshot &s, std::uint32_t uid,
                                                            std::int64_t expiry_ns) noexcept {
  const auto it = std::lower_bound(
      s.forwards.begin(), s.forwards.end(), std::pair{uid, expiry_ns},
      [](const SpiderRockExpiryForward &f, const std::pair<std::uint32_t, std::int64_t> &key) {
        return std::pair{f.uid, f.expiry_ns} < key;
      });
  return it != s.forwards.end() && it->uid == uid && it->expiry_ns == expiry_ns ? &*it
                                                                                : nullptr;
}

// The to_string(RiskFlag) names of the set bits, lowest first, '|'-joined.
[[nodiscard]] std::string flag_names(RiskFlags bits) {
  std::string s;
  for (std::uint32_t b = 0; b < 32; ++b) {
    const std::uint32_t bit = 1u << b;
    if ((bits & bit) == 0u) {
      continue;
    }
    if (!s.empty()) {
      s.push_back('|');
    }
    s += to_string(static_cast<RiskFlag>(bit));
  }
  return s;
}

// The base frame's row `r`, the base state's spot / carry and the base bucket's row.
void fill_dump_model(LegDumpRow &row, const RiskFrame &f, std::uint32_t r, const Snapshot &base) {
  row.T = f.T[r];
  row.sigma = f.sigma[r];
  const UnderlierState *u = base.state->underlier(row.uid);
  row.spot = u != nullptr ? u->spot : kNaN;
  row.forward = f.forward[r];
  row.rate = f.rate[r];
  row.q_eff = f.q_eff[r];
  const CarryUsed c = carry_used(*base.state, row.uid, row.expiry_ns);
  row.carry_source = c.source;
  row.carry_rate = c.rate;
  row.carry_q = c.q;
  row.pv_divs = c.pv_divs;
  if (const SpiderRockExpiryForward *fw = expiry_forward(base, row.uid, row.expiry_ns)) {
    row.carry_mode = carry_mode_name(fw->mode);
    row.implied_forward = fw->implied_forward;
    row.published_forward = fw->published_forward;
  }
  row.route = to_string(f.route[r]);
  row.flags = flag_names(f.flags[r]);
  row.status = to_string(f.status[r]);
  row.price = f.price[r];
  row.delta = f.delta[r];
  row.gamma = f.gamma[r];
  row.vega = f.vega[r];
  row.theta = f.theta[r];
  row.vanna = f.vanna[r];
  row.volga = f.volga[r];
  row.charm = f.charm[r];
  row.vendor_price = f.vendor_price[r];
  row.vendor_delta = f.vendor_delta[r];
  const std::optional<std::size_t> br =
      find_bucket_row(base.bucket, row.uid, row.expiry_ns, row.side, row.strike);
  if (br.has_value()) {
    set_leg_dump_vendor(row, base.bucket, *br);
  }
}

void fill_dump_pnl(LegDumpRow &row, const ExplainResult &ex, std::size_t i) {
  row.leg_status = to_string(ex.status[i]);
  row.pnl_total = ex.pnl_total[i];
  row.pnl_delta = ex.pnl_delta[i];
  row.pnl_gamma = ex.pnl_gamma[i];
  row.pnl_vega = ex.pnl_vega[i];
  row.pnl_volga = ex.pnl_volga[i];
  row.pnl_vanna = ex.pnl_vanna[i];
  row.pnl_theta = ex.pnl_theta[i];
  row.pnl_rho = ex.pnl_rho[i];
  row.pnl_charm = ex.pnl_charm[i];
  row.pnl_unexplained = ex.pnl_unexplained[i];
}

// One block of option-leg rows for a step, in prepared row order. The model columns
// are the step's BASE side: `stream.previous()` after the step and the `base` snapshot
// (still alive: run_risk_bench releases it only after every stream stepped).
[[nodiscard]] Status dump_step(LegDump &d, const std::string &label, const RiskStream &stream,
                               const ExplainResult &ex, const Snapshot &base) {
  d.steps_seen += 1;
  if (!dump_step_selected(d.limit, d.steps_seen)) {
    return Ok();
  }
  const PreparedRiskBook &pb = stream.prepared();
  const RiskFrame &f = stream.previous();
  const std::span<const OptionLeg> legs = pb.book().option_legs();
  const std::span<const std::uint32_t> prow = pb.leg_prepared_row();
  if (prow.size() != legs.size() || ex.size() < legs.size() || f.size() != pb.n_unique()) {
    return Err(ErrorCode::InvalidArgument,
               "risk-bench: --dump-legs: book, frame and explain sizes disagree");
  }
  std::string text;
  text.reserve(legs.size() * 512);
  for (const std::size_t i : leg_dump_order(prow)) {
    const std::uint32_t r = prow[i];
    const OptionLeg &leg = legs[i];
    LegDumpRow row;
    row.step = label;
    row.leg_index = i;
    row.leg_id = leg.id;
    row.prepared_row = r;
    row.uid = pb.uid()[r];
    row.ticker = std::string{d.table->ticker(row.uid)};
    const auto slot = std::lower_bound(pb.uids().begin(), pb.uids().end(), row.uid);
    row.uid_slot = static_cast<std::uint32_t>(slot - pb.uids().begin());
    row.strike = pb.strike()[r];
    row.expiry_ns = pb.expiry_ns()[r];
    row.side = pb.side()[r];
    row.qty = leg.qty;
    // The effective deliverable (risk_book.hpp: <= 0 or non-finite => 100).
    row.multiplier = std::isfinite(leg.multiplier) && leg.multiplier > 0.0 ? leg.multiplier : 100.0;
    fill_dump_model(row, f, r, base);
    fill_dump_pnl(row, ex, i);
    append_leg_dump_csv(text, row);
  }
  ATX_TRY_VOID(write_dump_text(d, text));
  d.steps_written += 1;
  d.rows += legs.size();
  return Ok();
}

// ── Steps ───────────────────────────────────────────────────────────────────

// Steps every stream to `cur` and records the step; `prev` stays alive throughout.
// With a LegDump, the main stream's step is dumped (base side = `prev`).
[[nodiscard]] Result<StepReport> run_step(Streams &s, const Snapshot &prev, const Snapshot &cur,
                                          LegDump *dump) {
  StepReport t;
  t.bucket_from = prev.bucket.bucket_et;
  t.bucket_to = cur.bucket.bucket_et;
  t.load_seconds = cur.load_seconds;
  t.state_seconds = cur.state_seconds;
  const Clock::time_point t0 = Clock::now();
  ATX_TRY(const ExplainResult *ex, s.main->step(*cur.state));
  t.step_seconds = seconds_since(t0);
  fill_frame(t, s.main->current());
  fill_explain(t, *ex);
  fill_parity(t, s.main->current(), s.main->prepared(), cur.bucket);
  if (dump != nullptr) {
    ATX_TRY_VOID(dump_step(*dump, t.bucket_from + "->" + t.bucket_to, *s.main, *ex, prev));
  }
  if (s.shadow.has_value()) {
    ATX_TRY(const ExplainResult *dx, s.shadow->step(*cur.state));
    t.determinism_checked = true;
    t.determinism_bit_equal = deterministic(*ex, s.main->current(), *dx, s.shadow->current());
  }
  if (s.cold.has_value()) {
    ATX_TRY(const ExplainResult *cx, s.cold->step(*cur.state));
    fill_cold(t.cold, s.main->current(), s.cold->current(), *ex, *cx, t.evaluate_seconds);
  }
  return Ok(std::move(t));
}

[[nodiscard]] Status start_streams(Streams &s, const MarketState &s0) {
  ATX_TRY_VOID(s.main->start(s0));
  if (s.shadow.has_value()) {
    ATX_TRY_VOID(s.shadow->start(s0));
  }
  if (s.cold.has_value()) {
    ATX_TRY_VOID(s.cold->start(s0));
  }
  return Ok();
}

void log_step(std::ostream &log, const StepReport &t) {
  log << "[risk-bench] " << t.bucket_from << "->" << t.bucket_to << ": " << t.n_ok << "/"
      << t.n_legs << " legs ok, load " << t.load_seconds * 1e3 << " ms, state "
      << t.state_seconds * 1e3 << " ms, evaluate " << t.evaluate_seconds * 1e3
      << " ms, explain " << t.explain_seconds * 1e3 << " ms, unexplained ratio "
      << t.explained_ratio;
  if (t.determinism_checked) {
    log << (t.determinism_bit_equal ? ", det bit-equal" : ", det MISMATCH");
  }
  log << "\n";
}

} // namespace

Result<RiskBenchReport> run_risk_bench(const RiskBenchArgs &args, std::ostream &log) {
  RiskBenchReport rep = report_skeleton(args);
  ATX_TRY_VOID(prepare_synthetic_store(args, rep, log));
  ATX_TRY(const std::vector<std::string> all,
          list_spiderrock_buckets(args.store_root(), args.date));
  rep.buckets = select_buckets(all, args.from, args.to);
  if (rep.buckets.size() < 2) {
    return Err(ErrorCode::InvalidArgument, "risk-bench: fewer than 2 buckets in [" + args.from +
                                               ", " + args.to + "] for " + args.date);
  }
  SpiderRockStoreSpec spec;
  spec.store_root = args.store_root();
  spec.date = args.date;
  spec.max_underliers = args.underliers;
  spec.time = TimeSpec{args.time}; // every state (and so the kernel) measures T on it
  spec.carry_mode = bench_carry_mode(args);
  UnderlierTable table;
  ATX_TRY(const std::unique_ptr<LegDump> dump, open_leg_dump(args, table));
  ATX_TRY(std::unique_ptr<Snapshot> prev, load_snapshot(spec, rep.buckets.front(), table));
  rep.bucket_loads.push_back(bucket_load(*prev));
  // The universe is fixed at the first bucket: later buckets read exactly these
  // tickers (stable uids through the shared table).
  spec.underliers.assign(table.tickers().begin(), table.tickers().end());
  spec.max_underliers = 0;

  std::mt19937_64 rng{args.seed};
  ATX_TRY(const BenchBook picked, select_bench_book(prev->bucket, args, rng));
  ATX_TRY(const RiskBook book, RiskBook::create(picked.options, picked.shares));
  rep.n_option_legs = book.n_option_legs();
  rep.n_unique_options = book.n_unique_options();
  rep.n_share_legs = book.n_share_legs();
  rep.n_underliers = book.n_underliers();
  rep.book_shortfall = picked.shortfall;

  Streams streams = make_streams(book, args);
  ATX_TRY_VOID(start_streams(streams, *prev->state));
  rep.start_state_seconds = prev->state_seconds;
  rep.start_evaluate_seconds = streams.main->current().counters.evaluate_seconds;
  log << "[risk-bench] book " << rep.n_option_legs << " option legs (" << rep.n_unique_options
      << " unique) + " << rep.n_share_legs << " shares on " << rep.n_underliers
      << " underliers; first frame " << rep.start_evaluate_seconds * 1e3 << " ms\n";

  for (std::size_t k = 1; k < rep.buckets.size(); ++k) {
    ATX_TRY(std::unique_ptr<Snapshot> cur, load_snapshot(spec, rep.buckets[k], table));
    rep.bucket_loads.push_back(bucket_load(*cur));
    ATX_TRY(StepReport step, run_step(streams, *prev, *cur, dump.get()));
    log_step(log, step);
    rep.steps.push_back(std::move(step));
    prev = std::move(cur); // the old state is released only after every stream stepped
  }
  if (dump != nullptr) {
    log << "[risk-bench] --dump-legs: " << dump->rows << " option-leg rows over "
        << dump->steps_written << " step(s) -> " << dump->path << "\n";
  }
  summarize(rep);
  return Ok(std::move(rep));
}

} // namespace atx::vol::riskbench
