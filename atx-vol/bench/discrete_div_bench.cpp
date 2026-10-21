// Discrete-cash-dividend lattice kernel rows (lane L3f, vol-sota 2026-10-20).
//
// ── Why this bench exists ─────────────────────────────────────────────────────
// Until this file no bench row named the Vellekoop-Nieuwenhuis lattice: the
// release cost of ONE 301-step rollback (the dated-cash route's price, one third
// of all listed chains, 56 % of SPY's rows) was estimated, never measured, and the
// fitting layer's `dated_cash_iv` inversion was known only as "about 15 lattices
// per root" at /Od (LEDGER 2026-08-23 L406). These rows are the numbers the W3
// dividend plan is sized on. Every row is ONE run (user directive 2026-10-18:
// order of magnitude first, no repetitions): `Iterations(1)`, manual time, per-op
// p50 / p99 as counters; the host load line is in the lane report.
//
// ── Sets ─────────────────────────────────────────────────────────────────────
//   spy     the cohort5 SPY dividend ladder, built in: spot 772.98, the 34-expiry
//           (years, rate, q, ddiv, ddiv_pv) ladder of
//           fit-timing-20261017-cohort5/input/quotes-SPY.csv (sha256 b3fafd80...)
//           on valuation day 20740 (2026-10-14), its 17 dividend expiries' events
//           inferred with `infer_dividend_schedule` and mapped onto each expiry's
//           clock by `dividend_schedule_expiry_events` — exactly as the quote bench
//           does — 20 strikes per expiry in log-moneyness [-0.30, 0.20] around the
//           dated-cash forward, both sides, sigma in {0.12, 0.20, 0.35}: 2,040
//           contracts.
//   dev200  ATX_VOL_DDIV_DEV200=<csv> (S,K,T,sigma,r,q,side,events with events =
//           tau:amount|...): 40 lattice-routed dev200 payer chains x <= 20 strikes
//           x 2 sides (L3f-cash-dividend-kernel-20261020-inputs/dev200-payers.csv,
//           built by build_dev200_payers.py there). Rows register only when set.
//
// ── Rows ─────────────────────────────────────────────────────────────────────
//   american/discrete_div/price/<scheme>/<steps>        allocating entry, SPY set:
//           scheme in {spliced, spliced-bbs, spliced-bbsr, escrowed-lr}, steps in
//           {101, 151, 301, 601, 1201, 2401}; counters p50_us / p99_us / mean_us per
//           price, n. Steps >= 601 time every 4th contract, >= 1201 every 8th.
//   american/discrete_div/price_ws/<scheme>/301         the DiscreteDivWorkspace
//           overload beside it (the allocation's share of a price).
//   american/discrete_div/price_dev200/<scheme>/<steps> the same on the dev200 set.
//   american/discrete_div/screen_price/<set>            the Screen tier per price.
//   american/discrete_div/screen_scan/<set>             {101, 151, 201} x {Richardson,
//           plain BBS} vs the CONVERGED lattice (2,401 Bbs + Richardson vs 1,201,
//           intrinsic-floored): max / p99 abs, max / p99 rel (P >= 0.01), IV bp max /
//           p99 (vega-bearing), us per price — the Screen tier's choice.
//   american/discrete_div/error/<set>                   Spliced301 / Bbs301 / Bbsr301 /
//           EscrowedLR301 / Screen vs the converged lattice (column 1) and vs the
//           301 SplicedCrr default (column 2), the worst contract's inputs on stderr.
//   american/discrete_div/iv/<set>/{seeded,unseeded}    american_discrete_div_implied_vol
//           per root: us p50 / p99, screen / accurate lattice counts p50 / p99 / max,
//           accurate-lattice equivalents, fallbacks by reason. The quote is the 301
//           SplicedCrr price at the contract's sigma; the seed is 1.05 x sigma.
//   american/discrete_div/secant_twin/<set>             a VERBATIM copy of the fitting
//           layer's dated_cash_iv (src/fitting/quote_slice.cpp:222-290) on the
//           allocating 301 SplicedCrr entry: lattices per root p50 / p99 / max and
//           us per root — the number the seeded inversion is measured against.
//
// Class: measurement only — no production code path changes.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/marketdata/dividend_schedule.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"

namespace {

using atx::vol::american_discrete_div_converged_price;
using atx::vol::american_discrete_div_implied_vol;
using atx::vol::american_discrete_div_price;
using atx::vol::american_discrete_div_screen_price;
using atx::vol::CashDividend;
using atx::vol::discrete_div_coarsest_steps;
using atx::vol::discrete_div_screen_steps;
using atx::vol::DiscreteDivIvFallback;
using atx::vol::DiscreteDivIvOpts;
using atx::vol::DiscreteDivIvResult;
using atx::vol::DiscreteDivScheme;
using atx::vol::DiscreteDivWorkspace;
using atx::vol::ErrorCode;
using atx::vol::ExerciseStyle;
using atx::vol::kDiscreteDivConvergedSteps;
using atx::vol::kDiscreteDivScreenScheme;
using atx::vol::kDiscreteDivScreenSteps;
using atx::vol::kIvMax;
using atx::vol::kIvMin;
using atx::vol::Result;
using atx::vol::Side;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Contract {
  double S, K, T, sigma, r, q;
  Side side;
  std::vector<CashDividend> divs;
};

[[nodiscard]] std::string env_text(const char *name) {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, name) != 0 || raw == nullptr) {
    return {};
  }
  std::string s(raw);
  std::free(raw);
  return s;
#else
  const char *raw = std::getenv(name);
  return raw == nullptr ? std::string{} : std::string(raw);
#endif
}

[[nodiscard]] double sorted_quantile(const std::vector<double> &s, double q) {
  if (s.empty()) {
    return 0.0;
  }
  const auto idx = static_cast<std::size_t>(q * static_cast<double>(s.size() - 1));
  return s[idx];
}

[[nodiscard]] double quantile(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  return sorted_quantile(v, q);
}

// ── The SPY ladder (cohort5 quotes-SPY.csv, valuation day 20740 = 2026-10-14) ──
struct SpyExpiry {
  std::int32_t expiry_day;
  double years, rate, q, ddiv, ddiv_pv;
};
// Per expiry: years / rate / ddiv / ddiv_pv as the file carries them (identical on
// every row of the expiry); q = the expiry's median per-strike `sdiv`.
constexpr SpyExpiry kSpyLadder[] = {
    {20740, 0.0024012345820665359, 0.044928997755050659, 0.0, 0.0, 0.0},
    {20741, 0.0058995364233851433, 0.044928997755050659, 0.017441380769014359, 0.0, 0.0},
    {20742, 0.0093978382647037506, 0.044896867126226425, -0.0089969253167510033, 0.0, 0.0},
    {20745, 0.014992210082709789, 0.044879622757434845, 0.0025711921043694019, 0.0, 0.0},
    {20746, 0.018490511924028397, 0.044878832995891571, 0.0040827577468007803, 0.0, 0.0},
    {20747, 0.021988812834024429, 0.044878996908664703, 0.0053138905204832554, 0.0, 0.0},
    {20748, 0.025487115606665611, 0.044879894703626633, 0.006665415596216917, 0.0, 0.0},
    {20749, 0.028985416516661644, 0.044881433248519897, 0.0015386588638648391, 0.0, 0.0},
    {20752, 0.034579787403345108, 0.044889584183692932, 0.0043955226428806782, 0.0, 0.0},
    {20753, 0.038078092038631439, 0.044893413782119751, 0.0048951390199363232, 0.0, 0.0},
    {20754, 0.041576392948627472, 0.04489777609705925, 0.0032589517068117857, 0.0, 0.0},
    {20756, 0.048572994768619537, 0.044908095151185989, 0.0020306569058448076, 0.0, 0.0},
    {20763, 0.068204246461391449, 0.044960528612136841, 0.0016782183665782213, 0.0, 0.0},
    {20770, 0.087791822850704193, 0.045037999749183655, 0.0014628417557105422, 0.0, 0.0},
    {20777, 0.10737939924001694, 0.045138232409954071, 0.0013196667423471808, 0.0, 0.0},
    {20784, 0.12320990115404129, 0.045253366231918335, 0.00038165185833349824, 0.0, 0.0},
    {20787, 0.1288042813539505, 0.045305907726287842, 0.0011248157825320959, 0.0, 0.0},
    {20805, 0.18197263777256012, 0.04564831405878067, 0.00204611592926085, 1.8310999870300293,
     1.8162750005722046},
    {20818, 0.21389241516590118, 0.045908033847808838, 0.0026866113767027855, 1.8310999870300293,
     1.8161910772323608},
    {20833, 0.25411561131477356, 0.046193607151508331, 2.5208171791746281e-05, 1.8310999870300293,
     1.816098690032959},
    {20847, 0.2908405065536499, 0.046438328921794891, -0.00034936430165544152, 1.8310999870300293,
     1.8160195350646973},
    {20868, 0.34715297818183899, 0.046780478209257126, -0.0009229730349034071, 1.8310999870300293,
     1.8159089088439941},
    {20875, 0.36674055457115173, 0.046890530735254288, -0.0010228930041193962, 1.8310999870300293,
     1.8158732652664185},
    {20896, 0.42545962333679199, 0.047214224934577942, -0.00090806931257247925, 3.6621999740600586,
     3.6102886199951172},
    {20908, 0.45518791675567627, 0.047396775335073471, -0.00035827941610477865, 3.6621999740600586,
     3.6100897789001465},
    {20986, 0.67169928550720215, 0.048549100756645203, -0.0014650697121396661, 5.4932999610900879,
     5.3809881210327148},
    {20999, 0.70492589473724365, 0.048726364970207214, -0.00089471309911459684, 5.4932999610900879,
     5.3805832862854004},
    {21078, 0.9224853515625, 0.049653135240077972, -0.0019397493451833725, 7.3243999481201172,
     7.1272792816162109},
    {21091, 0.95816218852996826, 0.049777958542108536, -0.0013714343076571822, 7.3243999481201172,
     7.1267919540405273},
    {21169, 1.1734104156494141, 0.050364717841148376, -0.0018258773488923907, 9.1555004119873047,
     8.8503551483154297},
    {21204, 1.2664477825164795, 0.050559796392917633, -0.001997585641220212, 9.1555004119873047,
     8.8491992950439453},
    {21351, 1.6703925132751465, 0.051124785095453262, -0.0023675546981394291, 12.817699432373047,
     12.229232788085938},
    {21533, 2.1672985553741455, 0.05153101310133934, -0.0031064596259966493, 14.648799896240234,
     13.883023262023926},
    {21568, 2.2578856945037842, 0.051589004695415497, -0.0021653110161423683, 14.648799896240234,
     13.88219165802002},
};
constexpr std::int32_t kSpyValuationDay = 20740;
constexpr double kSpySpot = 772.98;
constexpr double kSpySigmas[] = {0.12, 0.20, 0.35};
constexpr int kStrikesPerExpiry = 20;

[[nodiscard]] std::vector<Contract> build_spy_set() {
  using namespace atx::vol;
  std::vector<DividendLadderExpiry> ladder;
  for (const SpyExpiry &e : kSpyLadder) {
    if (e.expiry_day <= kSpyValuationDay) {
      continue; // the 0DTE row: the inference admits only expiries after valuation
    }
    ladder.push_back(DividendLadderExpiry{e.expiry_day, e.years, e.rate, e.ddiv, e.ddiv_pv});
  }
  const DividendScheduleResult schedule = infer_dividend_schedule(kSpyValuationDay, ladder);
  std::fprintf(stderr, "[ddiv-bench] SPY schedule status=%s reason=%s events=%zu unit=%.4f\n",
               to_string(schedule.status), to_string(schedule.reason), schedule.n_events,
               schedule.unit_amount);
  std::vector<Contract> out;
  for (const SpyExpiry &e : kSpyLadder) {
    if (!(e.ddiv > 0.0)) {
      continue;
    }
    std::vector<CashDividend> divs(kDividendScheduleMaxEvents);
    const DividendExpiryEvents ev =
        dividend_schedule_expiry_events(schedule, e.expiry_day, e.years, divs);
    if (ev.coverage != DividendExpiryCoverage::Covered || ev.count == 0) {
      std::fprintf(stderr, "[ddiv-bench] SPY expiry %d not covered (%s)\n", e.expiry_day,
                   to_string(ev.coverage));
      continue;
    }
    divs.resize(ev.count);
    const double forward = discrete_div_dated_forward(kSpySpot, e.years, e.rate, e.q, divs);
    for (int i = 0; i < kStrikesPerExpiry; ++i) {
      const double k = -0.30 + 0.50 * static_cast<double>(i) /
                                   static_cast<double>(kStrikesPerExpiry - 1);
      const double K = std::round(forward * std::exp(k));
      for (const Side side : {Side::Call, Side::Put}) {
        for (const double sigma : kSpySigmas) {
          out.push_back(Contract{kSpySpot, K, e.years, sigma, e.rate, e.q, side, divs});
        }
      }
    }
  }
  std::fprintf(stderr, "[ddiv-bench] SPY set: %zu contracts\n", out.size());
  return out;
}

// S,K,T,sigma,r,q,side,events — events = tau:amount|tau:amount|...
[[nodiscard]] std::vector<Contract> load_contracts(const std::string &path) {
  std::vector<Contract> v;
  if (path.empty()) {
    return v;
  }
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) {
    return v;
  }
  while (std::getline(in, line)) {
    double f[6] = {};
    const char *c = line.c_str();
    bool ok = true;
    for (int k = 0; k < 6 && ok; ++k) {
      char *end = nullptr;
      f[k] = std::strtod(c, &end);
      ok = end != c;
      c = (*end == ',') ? end + 1 : end;
    }
    if (!ok || *c == '\0') {
      continue;
    }
    const Side side = (*c == 'C' || *c == 'c') ? Side::Call : Side::Put;
    while (*c != '\0' && *c != ',') {
      ++c;
    }
    if (*c == ',') {
      ++c;
    }
    std::vector<CashDividend> divs;
    while (*c != '\0') {
      char *end = nullptr;
      const double tau = std::strtod(c, &end);
      if (end == c || *end != ':') {
        break;
      }
      c = end + 1;
      const double amount = std::strtod(c, &end);
      if (end == c) {
        break;
      }
      divs.push_back(CashDividend{tau, amount});
      c = (*end == '|') ? end + 1 : end;
    }
    v.push_back(Contract{f[0], f[1], f[2], f[3], f[4], f[5], side, std::move(divs)});
  }
  std::fprintf(stderr, "[ddiv-bench] dev200 set: %zu contracts from %s\n", v.size(),
               path.c_str());
  return v;
}

enum class Set { Spy, Dev200 };

[[nodiscard]] const std::vector<Contract> &contracts_of(Set set) {
  static const std::vector<Contract> spy = build_spy_set();
  static const std::vector<Contract> dev200 = load_contracts(env_text("ATX_VOL_DDIV_DEV200"));
  return set == Set::Spy ? spy : dev200;
}

[[nodiscard]] const char *set_name(Set set) { return set == Set::Spy ? "spy" : "dev200"; }

// ── References: converged, default 301, vega chord — once per set ─────────────
struct Reference {
  double converged = kNaN;
  double uncertainty = kNaN;
  double default301 = kNaN;
  double vega = kNaN;
};

[[nodiscard]] const std::vector<Reference> &references_of(Set set) {
  static std::vector<Reference> spy;
  static std::vector<Reference> dev200;
  std::vector<Reference> &refs = set == Set::Spy ? spy : dev200;
  const std::vector<Contract> &cs = contracts_of(set);
  if (!refs.empty() || cs.empty()) {
    return refs;
  }
  DiscreteDivWorkspace ws(kDiscreteDivConvergedSteps);
  const auto t0 = std::chrono::steady_clock::now();
  refs.reserve(cs.size());
  for (const Contract &c : cs) {
    Reference ref;
    const auto conv = american_discrete_div_converged_price(
        c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs, ExerciseStyle::American, ws);
    if (conv) {
      ref.converged = conv->price;
      ref.uncertainty = conv->fine - conv->coarse;
    }
    const auto d301 = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side,
                                                  c.divs, 301, ExerciseStyle::American,
                                                  DiscreteDivScheme::SplicedCrr, ws);
    if (d301) {
      ref.default301 = *d301;
    }
    constexpr double h = 1.0e-3;
    const auto up = american_discrete_div_price(c.S, c.K, c.T, c.sigma + h, c.r, c.q, c.side,
                                                c.divs, 301, ExerciseStyle::American,
                                                DiscreteDivScheme::SplicedBbs, ws);
    const auto dn = american_discrete_div_price(c.S, c.K, c.T, c.sigma - h, c.r, c.q, c.side,
                                                c.divs, 301, ExerciseStyle::American,
                                                DiscreteDivScheme::SplicedBbs, ws);
    if (up && dn) {
      ref.vega = (*up - *dn) / (2.0 * h);
    }
    refs.push_back(ref);
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  double max_unc = 0.0;
  for (const Reference &r : refs) {
    if (std::isfinite(r.uncertainty)) {
      max_unc = std::max(max_unc, std::abs(r.uncertainty));
    }
  }
  std::fprintf(stderr,
               "[ddiv-bench] %s references: %zu contracts in %.1f s; converged uncertainty "
               "(2401 - 1201 Bbs) max %.3e\n",
               set_name(set), refs.size(), secs, max_unc);
  return refs;
}

// ── Error accumulation ───────────────────────────────────────────────────────
struct ErrDist {
  std::vector<double> abs_err, rel_err, iv_bp;
  double worst = 0.0;
  const Contract *worst_c = nullptr;
  std::size_t status_mismatch = 0;
  void add(double err, double reference, double vega, const Contract &c) {
    const double a = std::abs(err);
    abs_err.push_back(a);
    if (reference >= 0.01) {
      rel_err.push_back(a / reference);
    }
    if (vega >= 1.0e-2 * std::max(reference, 1.0)) {
      iv_bp.push_back(a / vega * 1.0e4);
    }
    if (a > worst) {
      worst = a;
      worst_c = &c;
    }
  }
  void publish(benchmark::State &state, const std::string &prefix) const {
    state.counters[prefix + "_n"] = static_cast<double>(abs_err.size());
    state.counters[prefix + "_max_abs"] = quantile(abs_err, 1.0);
    state.counters[prefix + "_p99_abs"] = quantile(abs_err, 0.99);
    state.counters[prefix + "_max_rel"] = quantile(rel_err, 1.0);
    state.counters[prefix + "_p99_rel"] = quantile(rel_err, 0.99);
    state.counters[prefix + "_max_iv_bp"] = quantile(iv_bp, 1.0);
    state.counters[prefix + "_p99_iv_bp"] = quantile(iv_bp, 0.99);
    state.counters[prefix + "_mismatch"] = static_cast<double>(status_mismatch);
  }
  void print(const char *set, const char *label) const {
    std::fprintf(stderr,
                 "[ddiv-err] %-6s %-22s n %5zu max_abs %.3e p99_abs %.3e max_rel %.3e p99_rel "
                 "%.3e iv_bp max %.4f p99 %.4f (n %zu) mismatch %zu",
                 set, label, abs_err.size(), quantile(abs_err, 1.0), quantile(abs_err, 0.99),
                 quantile(rel_err, 1.0), quantile(rel_err, 0.99), quantile(iv_bp, 1.0),
                 quantile(iv_bp, 0.99), iv_bp.size(), status_mismatch);
    if (worst_c != nullptr) {
      std::fprintf(stderr, " | worst S %.4f K %.2f T %.6f sigma %.3f r %.5f q %.6f %s divs %zu",
                   worst_c->S, worst_c->K, worst_c->T, worst_c->sigma, worst_c->r, worst_c->q,
                   worst_c->side == Side::Call ? "call" : "put", worst_c->divs.size());
    }
    std::fprintf(stderr, "\n");
  }
};

// ── Per-price timing ─────────────────────────────────────────────────────────
template <class Price>
void time_prices(benchmark::State &state, const std::vector<Contract> &cs, std::size_t stride,
                 Price &&price) {
  if (cs.empty()) {
    state.SkipWithError("no contracts (set not available)");
    return;
  }
  std::vector<double> samples_us;
  samples_us.reserve(cs.size() / stride + 1);
  double sink = 0.0;
  std::size_t refused = 0;
  for (auto _ : state) {
    double total_us = 0.0;
    for (std::size_t i = 0; i < cs.size(); i += stride) {
      const Contract &c = cs[i];
      const auto t0 = std::chrono::steady_clock::now();
      const Result<double> p = price(c);
      const auto t1 = std::chrono::steady_clock::now();
      if (p) {
        sink += *p;
      } else {
        ++refused;
      }
      const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      total_us += us;
      samples_us.push_back(us);
    }
    benchmark::DoNotOptimize(sink);
    state.SetIterationTime(total_us * 1.0e-6);
  }
  std::sort(samples_us.begin(), samples_us.end());
  double mean = 0.0;
  for (const double v : samples_us) {
    mean += v;
  }
  mean = samples_us.empty() ? 0.0 : mean / static_cast<double>(samples_us.size());
  state.SetItemsProcessed(static_cast<std::int64_t>(samples_us.size()));
  state.counters["n"] = static_cast<double>(samples_us.size());
  state.counters["refused"] = static_cast<double>(refused);
  state.counters["p50_us"] = sorted_quantile(samples_us, 0.50);
  state.counters["p99_us"] = sorted_quantile(samples_us, 0.99);
  state.counters["mean_us"] = mean;
}

[[nodiscard]] std::size_t stride_for(int steps) {
  if (steps >= 1201) {
    return 8;
  }
  if (steps >= 601) {
    return 4;
  }
  return 1;
}

void run_price(benchmark::State &state, Set set, DiscreteDivScheme scheme, int steps) {
  const std::vector<Contract> &cs = contracts_of(set);
  time_prices(state, cs, stride_for(steps), [&](const Contract &c) {
    return american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs, steps,
                                       ExerciseStyle::American, scheme);
  });
  state.counters["steps"] = static_cast<double>(steps);
}

void run_price_ws(benchmark::State &state, Set set, DiscreteDivScheme scheme, int steps) {
  const std::vector<Contract> &cs = contracts_of(set);
  DiscreteDivWorkspace ws(steps);
  time_prices(state, cs, stride_for(steps), [&](const Contract &c) {
    return american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs, steps,
                                       ExerciseStyle::American, scheme, ws);
  });
  state.counters["steps"] = static_cast<double>(steps);
}

void run_screen_price(benchmark::State &state, Set set) {
  const std::vector<Contract> &cs = contracts_of(set);
  DiscreteDivWorkspace ws(301);
  time_prices(state, cs, 1, [&](const Contract &c) -> Result<double> {
    const Result<int> ns = discrete_div_screen_steps(c.T, c.divs, 301);
    if (!ns) {
      return atx::core::Err(ns.error());
    }
    return american_discrete_div_screen_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs,
                                              *ns, ws);
  });
  state.counters["steps"] = static_cast<double>(kDiscreteDivScreenSteps);
}

// ── The Screen scan and the error table ──────────────────────────────────────
// The scan: {101, 151, 201} x {BBS + Richardson (r), plain BBS (p), plain spliced
// CRR (c)}. The brief's expected tier was `r`; `c` is what the cost measurement
// selected (header, kDiscreteDivScreenScheme).
struct Candidate {
  int steps;
  DiscreteDivScheme scheme;
  const char *label;
};
constexpr Candidate kScreenCandidates[] = {
    {101, DiscreteDivScheme::SplicedBbsr, "101r"}, {101, DiscreteDivScheme::SplicedBbs, "101p"},
    {101, DiscreteDivScheme::SplicedCrr, "101c"},  {151, DiscreteDivScheme::SplicedBbsr, "151r"},
    {151, DiscreteDivScheme::SplicedBbs, "151p"},  {151, DiscreteDivScheme::SplicedCrr, "151c"},
    {201, DiscreteDivScheme::SplicedBbsr, "201r"}, {201, DiscreteDivScheme::SplicedBbs, "201p"},
    {201, DiscreteDivScheme::SplicedCrr, "201c"},
};

void run_screen_scan(benchmark::State &state, Set set) {
  const std::vector<Contract> &cs = contracts_of(set);
  const std::vector<Reference> &refs = references_of(set);
  if (cs.empty()) {
    state.SkipWithError("no contracts (set not available)");
    return;
  }
  DiscreteDivWorkspace ws(301);
  for (auto _ : state) {
    for (const Candidate &cand : kScreenCandidates) {
      const DiscreteDivScheme scheme = cand.scheme;
      ErrDist err;
      std::vector<double> us;
      us.reserve(cs.size());
      std::size_t refused = 0;
      for (std::size_t i = 0; i < cs.size(); ++i) {
        const Contract &c = cs[i];
        const Reference &ref = refs[i];
        if (!std::isfinite(ref.converged)) {
          continue;
        }
        // The tier's own first-step rule, on this candidate's coarsest lattice.
        const int coarsest = discrete_div_coarsest_steps(cand.steps, scheme);
        bool early = false;
        for (const CashDividend &dv : c.divs) {
          if (dv.tau > 0.0 && dv.tau <= c.T * (1.0 + atx::vol::kTauAtExpiryRelTol) &&
              std::nearbyint(dv.tau / (c.T / static_cast<double>(coarsest))) <
                  static_cast<double>(atx::vol::kDiscreteDivScreenMinExStep)) {
            early = true;
          }
        }
        if (early) {
          ++refused;
          continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const Result<double> p = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q,
                                                             c.side, c.divs, cand.steps,
                                                             ExerciseStyle::American, scheme, ws);
        const auto t1 = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        if (!p) {
          ++err.status_mismatch;
          continue;
        }
        err.add(*p - ref.converged, ref.converged, ref.vega, c);
      }
      const std::string prefix = std::string("s") + cand.label;
      err.publish(state, prefix);
      state.counters[prefix + "_p50_us"] = quantile(us, 0.5);
      state.counters[prefix + "_refused"] = static_cast<double>(refused);
      std::fprintf(stderr, "[ddiv-scan] %s %s refused %zu p50_us %.2f :: ", set_name(set),
                   cand.label, refused, quantile(us, 0.5));
      err.print(set_name(set), cand.label);
    }
  }
}

struct ErrorRow {
  const char *label;
  DiscreteDivScheme scheme;
  int steps;
};
constexpr ErrorRow kErrorRows[] = {
    {"spliced301", DiscreteDivScheme::SplicedCrr, 301},
    {"bbs301", DiscreteDivScheme::SplicedBbs, 301},
    {"bbsr301", DiscreteDivScheme::SplicedBbsr, 301},
    {"escrowed-lr301", DiscreteDivScheme::EscrowedLeisenReimer, 301},
    {"screen", kDiscreteDivScreenScheme, kDiscreteDivScreenSteps},
};

void run_error_table(benchmark::State &state, Set set) {
  const std::vector<Contract> &cs = contracts_of(set);
  const std::vector<Reference> &refs = references_of(set);
  if (cs.empty()) {
    state.SkipWithError("no contracts (set not available)");
    return;
  }
  DiscreteDivWorkspace ws(301);
  for (auto _ : state) {
    for (const ErrorRow &row : kErrorRows) {
      ErrDist vs_conv;
      ErrDist vs_301;
      for (std::size_t i = 0; i < cs.size(); ++i) {
        const Contract &c = cs[i];
        const Reference &ref = refs[i];
        if (!std::isfinite(ref.converged) || !std::isfinite(ref.default301)) {
          continue;
        }
        int steps = row.steps;
        if (row.scheme == kDiscreteDivScreenScheme && row.steps == kDiscreteDivScreenSteps) {
          const Result<int> ns = discrete_div_screen_steps(c.T, c.divs, 301, row.steps);
          if (!ns) {
            continue; // refused by the tier: the accurate lattice prices it
          }
          steps = *ns;
        }
        const Result<double> p = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q,
                                                             c.side, c.divs, steps,
                                                             ExerciseStyle::American, row.scheme,
                                                             ws);
        if (!p) {
          ++vs_conv.status_mismatch;
          continue;
        }
        vs_conv.add(*p - ref.converged, ref.converged, ref.vega, c);
        vs_301.add(*p - ref.default301, ref.default301, ref.vega, c);
      }
      vs_conv.publish(state, std::string(row.label) + "_conv");
      vs_301.publish(state, std::string(row.label) + "_d301");
      vs_conv.print(set_name(set), (std::string(row.label) + " vs converged").c_str());
      vs_301.print(set_name(set), (std::string(row.label) + " vs spliced301").c_str());
    }
  }
}

// ── The inversions ───────────────────────────────────────────────────────────
struct IvDist {
  std::vector<double> us, screen, accurate, equiv, dist;
  std::size_t fallbacks[7] = {};
  std::size_t errors = 0;
  void publish(benchmark::State &state) const {
    state.counters["roots"] = static_cast<double>(us.size());
    state.counters["errors"] = static_cast<double>(errors);
    state.counters["p50_us"] = quantile(us, 0.5);
    state.counters["p99_us"] = quantile(us, 0.99);
    state.counters["screen_p50"] = quantile(screen, 0.5);
    state.counters["screen_p99"] = quantile(screen, 0.99);
    state.counters["accurate_p50"] = quantile(accurate, 0.5);
    state.counters["accurate_p99"] = quantile(accurate, 0.99);
    state.counters["accurate_max"] = quantile(accurate, 1.0);
    state.counters["equiv_p50"] = quantile(equiv, 0.5);
    state.counters["equiv_p99"] = quantile(equiv, 0.99);
    state.counters["equiv_max"] = quantile(equiv, 1.0);
    state.counters["dist_max"] = quantile(dist, 1.0);
    state.counters["dist_p99"] = quantile(dist, 0.99);
    const char *names[7] = {"fb_none", "fb_not_applicable", "fb_range_edge", "fb_not_increasing",
                            "fb_vol_bound", "fb_pricer_failure", "fb_no_convergence"};
    for (std::size_t i = 0; i < 7; ++i) {
      state.counters[names[i]] = static_cast<double>(fallbacks[i]);
    }
  }
};

[[nodiscard]] double screen_equivalent(int accurate_steps) {
  return atx::vol::discrete_div_screen_equivalent(kDiscreteDivScreenSteps, accurate_steps);
}

void run_iv(benchmark::State &state, Set set, bool seeded) {
  const std::vector<Contract> &cs = contracts_of(set);
  const std::vector<Reference> &refs = references_of(set);
  if (cs.empty()) {
    state.SkipWithError("no contracts (set not available)");
    return;
  }
  DiscreteDivWorkspace ws(301);
  IvDist d;
  for (auto _ : state) {
    double total_us = 0.0;
    for (std::size_t i = 0; i < cs.size(); ++i) {
      const Contract &c = cs[i];
      const Reference &ref = refs[i];
      if (!std::isfinite(ref.default301) || !(ref.vega >= 1.0e-2 * std::max(ref.default301, 1.0))) {
        continue; // not vega-bearing: no identifiable IV
      }
      DiscreteDivIvOpts opts;
      opts.seed_sigma = seeded ? 1.05 * c.sigma : 0.0;
      const auto t0 = std::chrono::steady_clock::now();
      const Result<DiscreteDivIvResult> iv = american_discrete_div_implied_vol(
          ref.default301, c.S, c.K, c.T, c.r, c.q, c.side, c.divs, 301,
          DiscreteDivScheme::SplicedCrr, opts, ws);
      const auto t1 = std::chrono::steady_clock::now();
      const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      total_us += us;
      if (!iv) {
        ++d.errors;
        continue;
      }
      d.us.push_back(us);
      d.screen.push_back(static_cast<double>(iv->screen_lattices));
      d.accurate.push_back(static_cast<double>(iv->accurate_lattices));
      d.equiv.push_back(static_cast<double>(iv->accurate_lattices) +
                        screen_equivalent(301) * static_cast<double>(iv->screen_lattices));
      d.dist.push_back(std::abs(iv->sigma - c.sigma));
      ++d.fallbacks[static_cast<std::size_t>(iv->fallback)];
    }
    state.SetIterationTime(total_us * 1.0e-6);
  }
  d.publish(state);
  state.SetItemsProcessed(static_cast<std::int64_t>(d.us.size()));
  std::fprintf(stderr,
               "[ddiv-iv] %s %s roots %zu errors %zu us p50 %.1f p99 %.1f | screen p50 %.0f p99 "
               "%.0f | accurate p50 %.0f p99 %.0f max %.0f | equiv p50 %.2f p99 %.2f max %.2f | "
               "dist max %.2e | fb none %zu n/a %zu edge %zu notinc %zu volb %zu pricer %zu "
               "noconv %zu\n",
               set_name(set), seeded ? "seeded" : "unseeded", d.us.size(), d.errors,
               quantile(d.us, 0.5), quantile(d.us, 0.99), quantile(d.screen, 0.5),
               quantile(d.screen, 0.99), quantile(d.accurate, 0.5), quantile(d.accurate, 0.99),
               quantile(d.accurate, 1.0), quantile(d.equiv, 0.5), quantile(d.equiv, 0.99),
               quantile(d.equiv, 1.0), quantile(d.dist, 1.0), d.fallbacks[0], d.fallbacks[1],
               d.fallbacks[2], d.fallbacks[3], d.fallbacks[4], d.fallbacks[5], d.fallbacks[6]);
}

// ── The secant twin ──────────────────────────────────────────────────────────
//
// A VERBATIM copy of `dated_cash_iv` (src/fitting/quote_slice.cpp:222-290 at
// be379ad4; that file is the fitting layer's and is not edited by this lane) with
// `model_price(fit, quote.strike, quote.side, sigma, q_effective)` replaced by one
// allocating 301 SplicedCrr lattice and the QuoteSliceFit / QuoteSliceConfig fields
// bound to the contract's: `fit.discrete_div_steps` = 301, `fit.discrete_div_scheme`
// = SplicedCrr, `fit.market.variance_years` = T, `d.effective_rate` = r,
// `q_effective` = q, `cfg.iv_tolerance` = 1e-7, `cfg.max_iv_iterations` = 64
// (QuoteSliceConfig defaults, api/fitting/quote_slice.hpp:265-266). `lattices`
// counts every price the twin spends.
constexpr double kTwinTolerance = 1.0e-7;
constexpr std::uint16_t kTwinMaxIterations = 64;
constexpr int kTwinSteps = 301;

[[nodiscard]] Result<double> secant_twin(const Contract &c, double target, int &lattices) {
  using atx::core::Err;
  using atx::core::Ok;
  const auto model_price = [&](double sigma) {
    ++lattices;
    return american_discrete_div_price(c.S, c.K, c.T, sigma, c.r, c.q, c.side, c.divs, kTwinSteps,
                                       ExerciseStyle::American, DiscreteDivScheme::SplicedCrr);
  };
  const int bound_steps = discrete_div_coarsest_steps(kTwinSteps, DiscreteDivScheme::SplicedCrr);
  const double min_lattice_sigma =
      std::abs(c.r - c.q) * std::sqrt(c.T / static_cast<double>(bound_steps));
  double lo = std::max(kIvMin, min_lattice_sigma * (1.0 + 1.0e-6));
  if (!(lo < kIvMax)) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: lattice has no admissible volatility range");
  }
  const auto lower = model_price(lo);
  if (!lower) {
    return Err(lower.error());
  }
  double f_lo = *lower - target;
  if (!std::isfinite(f_lo) || f_lo >= 0.0) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: quote at or below the identifiable range");
  }
  double hi = std::min(kIvMax, std::max(0.5, 2.0 * lo));
  double f_hi = 0.0;
  for (unsigned expansion = 0; expansion < 16; ++expansion) {
    const auto upper = model_price(hi);
    if (!upper) {
      return Err(upper.error());
    }
    f_hi = *upper - target;
    if (!std::isfinite(f_hi)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite bracket price");
    }
    if (f_hi >= 0.0) {
      break;
    }
    if (hi == kIvMax) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: price exceeds maximum-volatility price");
    }
    hi = std::min(kIvMax, 2.0 * hi);
  }
  for (std::uint16_t iteration = 0; iteration < kTwinMaxIterations; ++iteration) {
    const double width = hi - lo;
    if (width <= 2.0 * kTwinTolerance) {
      return Ok(0.5 * (lo + hi));
    }
    // A safeguarded secant retains a shrinking sign-change bracket, including
    // exercise plateaus where Newton's vega would vanish.
    const double fraction = std::clamp(-f_lo / (f_hi - f_lo), 0.1, 0.9);
    const double sigma = lo + fraction * width;
    const auto value = model_price(sigma);
    if (!value) {
      return Err(value.error());
    }
    const double residual = *value - target;
    if (!std::isfinite(residual)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
    }
    if (residual == 0.0) {
      return Ok(sigma);
    }
    if (residual < 0.0) {
      lo = sigma;
      f_lo = residual;
    } else {
      hi = sigma;
      f_hi = residual;
    }
  }
  return Err(ErrorCode::Unavailable, "dated-cash IV: iteration budget exhausted");
}

void run_secant_twin(benchmark::State &state, Set set) {
  const std::vector<Contract> &cs = contracts_of(set);
  const std::vector<Reference> &refs = references_of(set);
  if (cs.empty()) {
    state.SkipWithError("no contracts (set not available)");
    return;
  }
  std::vector<double> us, lattices, dist;
  std::size_t errors = 0;
  for (auto _ : state) {
    double total_us = 0.0;
    for (std::size_t i = 0; i < cs.size(); ++i) {
      const Contract &c = cs[i];
      const Reference &ref = refs[i];
      if (!std::isfinite(ref.default301) || !(ref.vega >= 1.0e-2 * std::max(ref.default301, 1.0))) {
        continue;
      }
      int n = 0;
      const auto t0 = std::chrono::steady_clock::now();
      const Result<double> iv = secant_twin(c, ref.default301, n);
      const auto t1 = std::chrono::steady_clock::now();
      const double t_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      total_us += t_us;
      if (!iv) {
        ++errors;
        continue;
      }
      us.push_back(t_us);
      lattices.push_back(static_cast<double>(n));
      dist.push_back(std::abs(*iv - c.sigma));
    }
    state.SetIterationTime(total_us * 1.0e-6);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(us.size()));
  state.counters["roots"] = static_cast<double>(us.size());
  state.counters["errors"] = static_cast<double>(errors);
  state.counters["p50_us"] = quantile(us, 0.5);
  state.counters["p99_us"] = quantile(us, 0.99);
  state.counters["lattices_p50"] = quantile(lattices, 0.5);
  state.counters["lattices_p99"] = quantile(lattices, 0.99);
  state.counters["lattices_max"] = quantile(lattices, 1.0);
  double mean = 0.0;
  for (const double v : lattices) {
    mean += v;
  }
  state.counters["lattices_mean"] = lattices.empty() ? 0.0 : mean / static_cast<double>(lattices.size());
  state.counters["dist_max"] = quantile(dist, 1.0);
  std::fprintf(stderr,
               "[ddiv-twin] %s roots %zu errors %zu us p50 %.1f p99 %.1f | lattices p50 %.0f "
               "p99 %.0f max %.0f mean %.2f | dist max %.2e\n",
               set_name(set), us.size(), errors, quantile(us, 0.5), quantile(us, 0.99),
               quantile(lattices, 0.5), quantile(lattices, 0.99), quantile(lattices, 1.0),
               state.counters["lattices_mean"].value, quantile(dist, 1.0));
}

// ── Registration ─────────────────────────────────────────────────────────────
struct SchemeRow {
  const char *name;
  DiscreteDivScheme scheme;
};
constexpr SchemeRow kSchemes[] = {
    {"spliced", DiscreteDivScheme::SplicedCrr},
    {"spliced-bbs", DiscreteDivScheme::SplicedBbs},
    {"spliced-bbsr", DiscreteDivScheme::SplicedBbsr},
    {"escrowed-lr", DiscreteDivScheme::EscrowedLeisenReimer},
};
constexpr int kStepLadder[] = {101, 151, 301, 601, 1201, 2401};

benchmark::internal::Benchmark *one_run(benchmark::internal::Benchmark *b) {
  return b->Iterations(1)->UseManualTime()->Unit(benchmark::kMillisecond);
}

void register_set(Set set, const std::string &suffix) {
  const std::string tag = suffix.empty() ? std::string{} : "_" + suffix;
  for (const SchemeRow &s : kSchemes) {
    for (const int steps : kStepLadder) {
      one_run(benchmark::RegisterBenchmark(
          "american/discrete_div/price" + tag + "/" + s.name + "/" + std::to_string(steps),
          [set, s, steps](benchmark::State &state) { run_price(state, set, s.scheme, steps); }));
    }
    one_run(benchmark::RegisterBenchmark(
        "american/discrete_div/price_ws" + tag + "/" + s.name + "/301",
        [set, s](benchmark::State &state) { run_price_ws(state, set, s.scheme, 301); }));
  }
  const std::string name = set_name(set);
  one_run(benchmark::RegisterBenchmark("american/discrete_div/screen_price/" + name,
                                       [set](benchmark::State &state) {
                                         run_screen_price(state, set);
                                       }));
  one_run(benchmark::RegisterBenchmark("american/discrete_div/screen_scan/" + name,
                                       [set](benchmark::State &state) {
                                         run_screen_scan(state, set);
                                       }));
  one_run(benchmark::RegisterBenchmark("american/discrete_div/error/" + name,
                                       [set](benchmark::State &state) {
                                         run_error_table(state, set);
                                       }));
  one_run(benchmark::RegisterBenchmark("american/discrete_div/iv/" + name + "/seeded",
                                       [set](benchmark::State &state) {
                                         run_iv(state, set, true);
                                       }));
  one_run(benchmark::RegisterBenchmark("american/discrete_div/iv/" + name + "/unseeded",
                                       [set](benchmark::State &state) {
                                         run_iv(state, set, false);
                                       }));
  one_run(benchmark::RegisterBenchmark("american/discrete_div/secant_twin/" + name,
                                       [set](benchmark::State &state) {
                                         run_secant_twin(state, set);
                                       }));
}

[[maybe_unused]] const int kRegistered = [] {
  register_set(Set::Spy, "");
  if (!env_text("ATX_VOL_DDIV_DEV200").empty()) {
    register_set(Set::Dev200, "dev200");
  }
  return 0;
}();

} // namespace
