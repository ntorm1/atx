// American IV root-finding comparator (lane B0, 2026-10-22; docs/benchmark-clocks.md
// section 8).
//
// For every served row of a quote-bench prediction CSV (status ok: a price and a
// volatility), the American implied volatility of the served PRICE is bracketed with the
// reference pricer and bisected to a stated absolute tolerance: no Newton step, no
// family, no memo. The bracket starts around the served IV (served_iv * (1 -+ 1e-3),
// widened x4), so the search is seeded by the fitter's answer; on a monotone price map
// the root does not depend on the seed. The error |served_iv - bracketed_iv| is in
// ABSOLUTE volatility (0.20 = 20%; 1e-7 = 0.001 vol bp). The same rows give the
// continuous-carry price table: the reference price at the served IV against the served
// price, in USD.
//
// SCOPE - what this checks, and what it does not. The pricer is the fitter's own
// Andersen-Lake accurate preset (std::nullopt, AlProfile::Reference): the independence is
// in the ROOT FINDING only. On a row whose served price is the curve price at the served
// IV (served price == P_ref(served_iv) within 1e-10 USD; summary "served_pair_consistent")
// the check is therefore circular for pricer error: |served - bracketed| is bounded by
// the bisection tolerance by construction, and only the fine-scheme reference
// uncertainty speaks to the pricer. The genuine root checks are the rows whose served
// pair is NOT consistent (re-implied, repaired or moved nodes, excluded-side rows). The
// fitter's quote inversions (de-Am bid / ask / mid IVs) are not compared.
// Follow-up: PDE-backed pricer option (D0's american_discrete_div_reference with zero
// dividends) for true pricer independence.
//
// Populations (summary "populations", per-row column "population"; plan 3.2: an
// unresolved row is not a pass), every served row in exactly one (bench_clocks.hpp
// classify_iv_row): resolved_pass / resolved_fail (bracketed, reference uncertainty u
// finite and <= the threshold, |served - bracketed| <= / > the threshold),
// decidable_fail (u above the threshold, but the error > threshold + 3 u: a failure the
// reference decides despite its uncertainty), reference_unresolved (the rest of the
// bracketed rows, and every undefined u - a zero reference vega), iv_not_identified
// (below), and the other statuses. "non_circular" counts, per population, the rows whose
// served pair is NOT consistent: only those are genuine root checks.
//
// The contract each row is priced under is rebuilt from the INPUT CSV, independently of
// the fitter, by quote-bench's documented continuous-carry convention: chain market =
// the median of the chain's supported rows (spot, variance and calendar clocks, rate,
// yield, cash PV); r_eff = rate * T_cal / T_var; q_eff = the row's own yield converted to
// the variance clock with the --dividends aggregate PV folded in; T = T_var; spot S.
// Reference: Andersen-Lake, the accurate preset (std::nullopt, AlProfile::Reference).
// Reference uncertainty: the same price on a finer scheme (32 nodes, quadrature 64/64,
// 16 sweeps, tol 1e-14), reported per row in USD and, through the reference vega, in vol.
// A row whose reference price does not move with sigma at the served IV (both sides of
// a 1e-5 relative step within the price tolerance 1e-12 * max(P, 1), e.g. a deep
// in-the-money put priced at intrinsic) has no identified IV: it is classified
// not_identified with the interval of sigma that reprices it, never counted as a root
// error. Rows routed to the dated-cash lattice (a --report with dividend routes) are
// excluded: they are not continuous-carry contracts.

#include "bench_clocks.hpp"

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__FAST_MATH__) || defined(_M_FP_FAST) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
#error "atx-vol-iv-comparator must not be built with fast-math: it compares roots to 1e-12"
#endif
static_assert(std::numeric_limits<double>::is_iec559, "the comparator needs IEEE-754 doubles");

namespace {
using namespace atx::vol;
using Json = bench::Json;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Dividends { FundingPv, EscrowedPv, None };

struct Arguments {
  std::filesystem::path input;
  std::filesystem::path predictions;
  std::filesystem::path output;
  std::filesystem::path summary;
  std::filesystem::path report; // optional quote-bench report: dated-cash routes excluded
  Dividends dividends{Dividends::FundingPv};
  double tolerance{1.0e-12}; // bisection: final bracket width <= 2 * tolerance (abs vol)
  double threshold{1.0e-7};  // the IV requirement the counts are reported against (abs vol)
  std::size_t max_rows{0};   // 0: every served row
  bool help{false};
};

template <class T> T number(std::string_view text, std::string_view what) {
  T out{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::runtime_error("invalid numeric field: " + std::string(what));
  }
  return out;
}

double real_or_nan(std::string_view text) {
  return text.empty() ? kNaN : number<double>(text, "real");
}

Dividends dividends_of(std::string_view value) {
  if (value == "funding-pv") return Dividends::FundingPv;
  if (value == "escrowed-pv") return Dividends::EscrowedPv;
  if (value == "none") return Dividends::None;
  throw std::runtime_error("--dividends must be funding-pv, escrowed-pv or none");
}

Arguments arguments(int argc, char **argv) {
  Arguments args;
  std::set<std::string_view> seen;
  for (int i = 1; i < argc; ++i) {
    const std::string_view key{argv[i]};
    if (key == "--help") {
      std::cout << "atx-vol-iv-comparator --input quotes.csv --predictions predictions.csv "
                   "--output rows.csv --summary summary.json [--report native.json] "
                   "[--dividends funding-pv|escrowed-pv|none] [--tolerance 1e-12] "
                   "[--threshold 1e-7] [--max-rows N]\n"
                   "Root-finding check only: the pricer is the fitter's own Andersen-Lake "
                   "accurate preset, so on rows whose served price is the curve price at "
                   "the served IV the check is circular for pricer error (see "
                   "atx-vol/docs/benchmark-clocks.md section 8). Populations: resolved "
                   "pass / resolved fail / decidable fail (error > threshold + 3 x the "
                   "reference uncertainty) / reference unresolved / IV not identified.\n";
      args.help = true;
      return args;
    }
    if (!seen.insert(key).second) {
      throw std::runtime_error("duplicate argument " + std::string(key));
    }
    if (i + 1 == argc) throw std::runtime_error("missing value for " + std::string(key));
    const std::string value{argv[++i]};
    if (key == "--input") args.input = value;
    else if (key == "--predictions") args.predictions = value;
    else if (key == "--output") args.output = value;
    else if (key == "--summary") args.summary = value;
    else if (key == "--report") args.report = value;
    else if (key == "--tolerance") args.tolerance = number<double>(value, key);
    else if (key == "--threshold") args.threshold = number<double>(value, key);
    else if (key == "--max-rows") args.max_rows = number<std::size_t>(value, key);
    else if (key == "--dividends") args.dividends = dividends_of(value);
    else throw std::runtime_error("unknown argument " + std::string(key));
  }
  if (args.input.empty() || args.predictions.empty() || args.output.empty() ||
      args.summary.empty()) {
    throw std::runtime_error("--input, --predictions, --output and --summary are required");
  }
  if (!(args.tolerance > 0.0 && args.tolerance < 1.0e-6) || !(args.threshold > 0.0)) {
    throw std::runtime_error("--tolerance must be in (0, 1e-6) and --threshold positive");
  }
  for (const auto &path : {args.output, args.summary}) {
    if (std::filesystem::exists(path)) {
      throw std::runtime_error("refusing to overwrite " + path.string());
    }
  }
  return args;
}

// RFC 4180 record (no embedded newlines), the quote-bench input rule.
std::vector<std::string> csv_record(std::string_view line) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c != '"') {
        field.push_back(c);
      } else if (i + 1U < line.size() && line[i + 1U] == '"') {
        field.push_back('"');
        ++i;
      } else {
        quoted = false;
      }
    } else if (c == ',') {
      fields.push_back(std::move(field));
      field.clear();
    } else if (c == '"' && field.empty()) {
      quoted = true;
    } else {
      field.push_back(c);
    }
  }
  if (quoted) throw std::runtime_error("unterminated CSV quote");
  fields.push_back(std::move(field));
  return fields;
}

struct Table {
  std::map<std::string, std::size_t, std::less<>> column;
  std::vector<std::vector<std::string>> rows;
  [[nodiscard]] const std::string &at(std::size_t row, std::string_view name) const {
    const auto found = column.find(name);
    if (found == column.end()) throw std::runtime_error("missing column " + std::string(name));
    return rows[row][found->second];
  }
};

Table read_table(const std::filesystem::path &path) {
  std::ifstream stream{path};
  if (!stream) throw std::runtime_error("cannot open " + path.string());
  Table table;
  std::string line;
  if (!std::getline(stream, line)) throw std::runtime_error("empty CSV " + path.string());
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const auto header = csv_record(line);
  for (std::size_t i = 0; i < header.size(); ++i) table.column.emplace(header[i], i);
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto fields = csv_record(line);
    if (fields.size() != header.size()) throw std::runtime_error("CSV field count mismatch");
    table.rows.push_back(std::move(fields));
  }
  return table;
}

// quote-bench median(): middle value, or the mean of the two middle values.
double median(std::vector<double> values) {
  if (values.empty()) return kNaN;
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2U;
  return values.size() % 2U ? values[mid] : 0.5 * values[mid - 1U] + 0.5 * values[mid];
}

struct Market {
  double spot{kNaN};
  double variance_years{kNaN};
  double calendar_years{kNaN};
  double rate{kNaN};
  double yield{kNaN};
  double dividend_pv{kNaN};
};

// The contract's continuous yield on the variance clock with the aggregate cash PV folded
// in (the quote-slice convention for the three --dividends models).
double effective_yield(const Market &m, double calendar_yield, Dividends model) {
  const double value = calendar_yield * (m.calendar_years / m.variance_years);
  if (model == Dividends::None || m.dividend_pv == 0.0) return value;
  double fraction = m.dividend_pv / m.spot;
  if (model == Dividends::FundingPv) {
    fraction = std::exp(std::log(m.dividend_pv) - std::log(m.spot) +
                        calendar_yield * m.calendar_years);
  }
  if (!(fraction < 1.0)) return kNaN;
  return value - std::log1p(-fraction) / m.variance_years;
}

struct Contract {
  double spot;
  double strike;
  double years; // variance clock
  double rate;  // effective, variance clock
  double yield; // effective, variance clock
  Side side;
};

[[nodiscard]] AlOpts fine_scheme() noexcept {
  AlOpts opts;
  opts.n_collocation = 32;
  opts.n_quadrature = 64;
  opts.n_quad_price = 64;
  opts.max_newton_iter = 16;
  opts.tol = 1.0e-14;
  opts.profile = AlProfile::Reference;
  return opts;
}

// The reference price (Andersen-Lake; nullopt = the accurate preset) or NaN on a refusal.
double reference_price(const Contract &c, double sigma, const std::optional<AlOpts> &opts) {
  const auto price = american_price(c.spot, c.strike, c.years, sigma, c.rate, c.yield, c.side,
                                    AmericanMethod::AndersenLake, opts);
  return price ? *price : kNaN;
}

struct Root {
  std::string status{"unbracketed"};
  double sigma{kNaN};
  double lo{kNaN};
  double hi{kNaN};
  double residual_usd{kNaN};
  unsigned evaluations{0};
};

// Bisection of P_ref(sigma) = target. The bracket starts at served_iv * (1 -+ 1e-3) and
// widens x4 per step inside [kIvMin, kIvMax] until P(lo) <= target <= P(hi) (at most 32
// widenings); then halves until hi - lo <= 2 * tol (at most 200 halvings). The root is the
// midpoint, within tol of a sign change of the evaluated map.
Root bracket_root(const Contract &c, double target, double seed, double tol) {
  Root root;
  const auto f = [&](double sigma) {
    ++root.evaluations;
    return reference_price(c, sigma, std::nullopt) - target;
  };
  double half = std::max(1.0e-3 * seed, 1.0e-6);
  double lo = std::max(kIvMin, seed - half);
  double hi = std::min(kIvMax, seed + half);
  double f_lo = f(lo);
  double f_hi = f(hi);
  for (unsigned widen = 0; widen < 32U && !(f_lo <= 0.0 && f_hi >= 0.0); ++widen) {
    if (!std::isfinite(f_lo) || !std::isfinite(f_hi)) break;
    half *= 4.0;
    if (f_lo > 0.0 && lo > kIvMin) {
      lo = std::max(kIvMin, seed - half);
      f_lo = f(lo);
    }
    if (f_hi < 0.0 && hi < kIvMax) {
      hi = std::min(kIvMax, seed + half);
      f_hi = f(hi);
    }
    if ((f_lo > 0.0 && lo <= kIvMin) || (f_hi < 0.0 && hi >= kIvMax)) break;
  }
  if (!(std::isfinite(f_lo) && std::isfinite(f_hi))) {
    root.status = "reference_error";
    return root;
  }
  if (!(f_lo <= 0.0 && f_hi >= 0.0)) return root; // outside the identifiable range
  for (unsigned halving = 0; halving < 200U && hi - lo > 2.0 * tol; ++halving) {
    const double mid = 0.5 * lo + 0.5 * hi;
    const double f_mid = f(mid);
    if (!std::isfinite(f_mid)) {
      root.status = "reference_error";
      return root;
    }
    if (f_mid <= 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  root.status = "bracketed";
  root.lo = lo;
  root.hi = hi;
  root.sigma = 0.5 * lo + 0.5 * hi;
  root.residual_usd = reference_price(c, root.sigma, std::nullopt) - target;
  return root;
}

// The edge, from `inside` (a sigma that reprices `target` within tol_p) in `direction`
// (-1 or +1), of the sigma interval that reprices `target` within tol_p: steps out x4
// (at most 40 steps, clamped to [kIvMin, kIvMax]), then bisects the edge 60 times.
double flat_edge(const Contract &c, double target, double inside, double direction,
                 double tol_p) {
  const double bound = direction < 0.0 ? kIvMin : kIvMax;
  double step = std::max(1.0e-4 * inside, 1.0e-6);
  double in = inside;
  double out = inside;
  bool left = false;
  for (unsigned i = 0; i < 40U && !left; ++i) {
    out = std::clamp(inside + direction * step, kIvMin, kIvMax);
    const double f = reference_price(c, out, std::nullopt) - target;
    if (!std::isfinite(f)) return kNaN;
    left = std::fabs(f) > tol_p;
    if (!left) {
      in = out;
      if (out == bound) return bound; // flat to the search bound
      step *= 4.0;
    }
  }
  if (!left) return kNaN;
  for (unsigned i = 0; i < 60U; ++i) {
    const double mid = 0.5 * in + 0.5 * out;
    const double f = reference_price(c, mid, std::nullopt) - target;
    if (std::isfinite(f) && std::fabs(f) <= tol_p) {
      in = mid;
    } else {
      out = mid;
    }
  }
  return 0.5 * in + 0.5 * out;
}

class Quantiles {
public:
  void add(double v) {
    if (std::isfinite(v)) values_.push_back(v);
  }
  // n, p50 / p90 / p99 / p99.9 / max (linear order statistics), and the count above
  // `threshold` when one is given.
  [[nodiscard]] Json json(double threshold = kNaN) const {
    std::vector<double> sorted = values_;
    std::sort(sorted.begin(), sorted.end());
    const auto q = [&sorted](double p) {
      if (sorted.empty()) return Json(nullptr);
      const double at = p * static_cast<double>(sorted.size() - 1U);
      const auto lo = static_cast<std::size_t>(at);
      const std::size_t hi = std::min(lo + 1U, sorted.size() - 1U);
      return Json(sorted[lo] + (at - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]));
    };
    Json out = {{"n", sorted.size()}, {"p50", q(0.50)}, {"p90", q(0.90)}, {"p99", q(0.99)},
                {"p999", q(0.999)}, {"max", q(1.0)}};
    if (std::isfinite(threshold)) {
      const auto above = std::count_if(sorted.begin(), sorted.end(),
                                       [threshold](double v) { return v > threshold; });
      out["over_threshold"] = static_cast<std::size_t>(above);
    }
    return out;
  }

private:
  std::vector<double> values_;
};

struct Group {
  Quantiles iv_error;
  Quantiles price_error_usd;
  std::size_t rows{0};
};

std::set<std::string> lattice_chains(const std::filesystem::path &report) {
  std::set<std::string> out;
  if (report.empty()) return out;
  std::ifstream stream{report};
  if (!stream) throw std::runtime_error("cannot open --report " + report.string());
  const Json j = Json::parse(stream);
  if (!j.contains("dividend_schedule")) return out;
  for (const auto &route : j.at("dividend_schedule").at("routes")) {
    if (route.value("route", "") == "lattice") {
      out.insert(std::to_string(route.at("chain_id").get<std::uint64_t>()));
    }
  }
  return out;
}

// Chain markets from the supported input rows (quote-bench chain_market + convert_row).
std::map<std::string, Market> chain_markets(const Table &input) {
  std::map<std::string, std::vector<std::size_t>> chain_rows;
  for (std::size_t i = 0; i < input.rows.size(); ++i) {
    const std::string &root = input.at(i, "root");
    const bool adjusted = !root.empty() && root.back() >= '1' && root.back() <= '9';
    if (input.at(i, "input_status") != "ok" || adjusted ||
        input.at(i, "exercise") != "American") {
      continue;
    }
    chain_rows[input.at(i, "chain_id")].push_back(i);
  }
  std::map<std::string, Market> markets;
  for (const auto &[chain, rows] : chain_rows) {
    const auto column = [&input, &rows](std::string_view name) {
      std::vector<double> values;
      values.reserve(rows.size());
      for (const std::size_t i : rows) values.push_back(real_or_nan(input.at(i, name)));
      return median(std::move(values));
    };
    markets[chain] = {column("spot"), column("years"), column("years_calendar"),
                      column("rate"), column("sdiv"), column("ddiv_pv")};
  }
  return markets;
}

struct Totals {
  Quantiles iv_error;
  Quantiles price_error_usd;
  Quantiles price_error_rel;
  Quantiles newton_distance;
  Quantiles uncertainty_usd;
  Quantiles uncertainty_iv;
  Quantiles evaluations;
  std::map<std::string, std::size_t> status_counts;
  std::map<std::string, Group> by_expiry;
  std::map<std::string, Group> by_side_moneyness;
  Quantiles interval_width;
  std::size_t served_in_interval{0};
  std::size_t unresolved{0};            // bracketed, uncertainty > threshold or undefined
  std::size_t unresolved_over{0};       // ... whose |served - bracketed| > threshold
  std::size_t uncertainty_undefined{0}; // bracketed, no reference vega (subset of unresolved)
  std::size_t resolved_pass{0};
  std::size_t resolved_fail{0};
  std::size_t decidable_fail{0};  // bracketed, u > threshold, error > threshold + 3 u
  std::size_t pair_consistent{0}; // bracketed, served price == P_ref(served_iv) within 1e-10
  std::map<std::string, std::size_t> non_circular; // per population: pair NOT consistent
  std::size_t compared{0};
};

// A served (price, iv) pair the reference reprices within this many USD is the curve's
// own pair: the IV check on it is circular for pricer error (docs section 8).
constexpr double kPairConsistentUsd = 1.0e-10;

struct RowResult {
  std::string status;
  Root root;
  double ref_at_served{kNaN};
  double vega{kNaN};
  double fine_gap{kNaN};
  double interval_lo{kNaN}; // not_identified: the sigma interval that reprices the row
  double interval_hi{kNaN};
};

RowResult compare_row(const Contract &c, double served_price, double served_iv, double tol,
                      bool excluded, bool price_only) {
  RowResult r;
  if (excluded) {
    r.status = "dated_cash_route";
    return r;
  }
  if (price_only || !std::isfinite(served_iv)) {
    r.status = "price_only";
    return r;
  }
  r.ref_at_served = reference_price(c, served_iv, std::nullopt);
  const double h = std::max(1.0e-5 * served_iv, 1.0e-7);
  const double up = reference_price(c, served_iv + h, std::nullopt);
  const double down = reference_price(c, served_iv - h, std::nullopt);
  r.vega = (up - down) / (2.0 * h);
  const double tol_p = 1.0e-12 * std::max(served_price, 1.0);
  if (std::fabs(up - served_price) <= tol_p && std::fabs(down - served_price) <= tol_p &&
      std::fabs(r.ref_at_served - served_price) <= tol_p) {
    r.status = "not_identified";
    r.interval_lo = flat_edge(c, served_price, served_iv, -1.0, tol_p);
    r.interval_hi = flat_edge(c, served_price, served_iv, +1.0, tol_p);
    return r;
  }
  r.root = bracket_root(c, served_price, served_iv, tol);
  r.status = r.root.status;
  if (r.root.status == "bracketed") {
    r.fine_gap = std::fabs(reference_price(c, r.root.sigma, fine_scheme()) -
                           reference_price(c, r.root.sigma, std::nullopt));
  }
  return r;
}

Json group_json(const Group &g, double threshold) {
  return {{"rows", g.rows}, {"abs_iv_error", g.iv_error.json(threshold)},
          {"price_error_usd", g.price_error_usd.json()}};
}

Json summary_json(const Arguments &args, const Totals &t,
                  const std::map<std::string, Market> &markets, double runtime_s) {
  Json expiry = Json::object();
  for (const auto &[chain, g] : t.by_expiry) {
    expiry[chain] = group_json(g, args.threshold);
    expiry[chain]["variance_years"] = markets.at(chain).variance_years;
  }
  Json classes = Json::object();
  for (const auto &[name, g] : t.by_side_moneyness) classes[name] = group_json(g, args.threshold);
  const auto canonical = [](const std::filesystem::path &p) {
    return p.empty() ? Json(nullptr) : Json(std::filesystem::weakly_canonical(p).string());
  };
  const char *dividends = args.dividends == Dividends::FundingPv    ? "funding-pv"
                          : args.dividends == Dividends::EscrowedPv ? "escrowed-pv"
                                                                    : "none";
  Json contract = {
      {"units", "absolute volatility (0.20 = 20%); 1e-7 = 0.001 vol bp; prices in USD"},
      {"threshold", args.threshold}, {"bisection_tolerance", args.tolerance},
      {"method", "bracket from served_iv * (1 -+ 1e-3), widen x4 inside [kIvMin, kIvMax], "
                 "bisect to hi - lo <= 2 * tolerance; no Newton, no family, no memo"},
      {"reference", "Andersen-Lake accurate preset (std::nullopt, AlProfile::Reference): "
                    "the fitter's own pricer; independence is in the root finding only"},
      {"scope", "served (price, iv) pairs, not the fitter's quote inversions; on "
                "served_pair_consistent rows the IV check is circular for pricer error"},
      {"follow_up", "PDE-backed pricer option (D0's american_discrete_div_reference with "
                    "zero dividends) for true pricer independence"},
      {"reference_uncertainty", "|P(32 nodes, quad 64/64, 16 sweeps, tol 1e-14) - "
                                "P(accurate)| at the bracketed root; in vol via the reference "
                                "vega (central difference, h = 1e-5 sigma)"},
      {"carry", "chain market = median of supported rows; r_eff = rate * T_cal / T_var; "
                "q_eff = row yield on the variance clock with the aggregate cash PV folded in "
                "(--dividends); T = T_var"},
      {"dividends", dividends}};
  const auto count_of = [&t](const char *status) {
    const auto found = t.status_counts.find(status);
    return found == t.status_counts.end() ? std::size_t{0} : found->second;
  };
  const std::size_t not_identified = count_of("not_identified");
  const std::size_t bracketed = count_of("bracketed");
  const std::size_t other = t.compared - t.resolved_pass - t.resolved_fail - t.decidable_fail -
                            t.unresolved - not_identified;
  const auto non_circular = [&t](const char *population) {
    const auto found = t.non_circular.find(population);
    return found == t.non_circular.end() ? std::size_t{0} : found->second;
  };
  const std::string headline =
      std::to_string(t.resolved_pass) + " resolved pass (" +
      std::to_string(non_circular("resolved_pass")) + " non-circular) / " +
      std::to_string(t.resolved_fail) + " resolved fail / " + std::to_string(t.decidable_fail) +
      " decidable fail / " + std::to_string(t.unresolved) + " reference unresolved / " +
      std::to_string(not_identified) + " IV not identified / " + std::to_string(other) +
      " other, of " + std::to_string(t.compared) + " served rows";
  Json populations = {
      {"headline", headline}, {"rows", t.compared}, {"resolved_pass", t.resolved_pass},
      {"resolved_fail", t.resolved_fail}, {"decidable_fail", t.decidable_fail},
      {"reference_unresolved", t.unresolved},
      {"reference_unresolved_over_threshold", t.unresolved_over},
      {"uncertainty_undefined", t.uncertainty_undefined},
      {"iv_not_identified", not_identified}, {"other_statuses", other},
      {"non_circular", {{"resolved_pass", non_circular("resolved_pass")},
                        {"resolved_fail", non_circular("resolved_fail")},
                        {"decidable_fail", non_circular("decidable_fail")},
                        {"reference_unresolved", non_circular("reference_unresolved")},
                        {"note", "bracketed rows whose served pair is NOT consistent "
                                 "(|P_ref(served_iv) - served price| > 1e-10 USD): the only "
                                 "genuine root checks; every other bracketed row is circular "
                                 "for pricer error"}}},
      {"decidable_fail_margin", bench::kDecidableFailMargin},
      {"rule", "resolved = bracketed with a finite reference uncertainty u <= threshold; pass "
               "/ fail = |served - bracketed| <= / > threshold; decidable_fail = u > "
               "threshold and |served - bracketed| > threshold + 3 u; reference_unresolved = "
               "the other bracketed rows and every undefined u (zero reference vega) - not a "
               "pass; iv_not_identified = price flat in sigma"}};
  return {{"schema", "atx-vol-iv-comparator-v3"}, {"input", canonical(args.input)},
          {"predictions", canonical(args.predictions)}, {"report", canonical(args.report)},
          {"contract", std::move(contract)}, {"rows_compared", t.compared},
          {"status_counts", t.status_counts}, {"populations", std::move(populations)},
          {"resolved_rows", t.resolved_pass + t.resolved_fail},
          {"resolved_over_threshold", t.resolved_fail},
          {"decidable_fail_rows", t.decidable_fail},
          {"uncertainty_undefined_rows", t.uncertainty_undefined},
          {"served_pair_consistent",
           {{"rows", t.pair_consistent}, {"of_bracketed", bracketed},
            {"tolerance_usd", kPairConsistentUsd},
            {"note", "bracketed rows whose served price equals P_ref(served_iv): the IV "
                     "check on them is circular for pricer error"}}},
          {"abs_iv_error", t.iv_error.json(args.threshold)},
          {"abs_iv_error_scope", "every bracketed row, resolved or not: its over_threshold is "
                                 "NOT a failure count (see populations)"},
          {"reference_uncertainty_iv", t.uncertainty_iv.json(args.threshold)},
          {"reference_uncertainty_usd", t.uncertainty_usd.json()},
          {"reference_unresolved_rows", t.unresolved},
          {"not_identified", {{"rows", t.status_counts.contains("not_identified")
                                           ? t.status_counts.at("not_identified") : 0U},
                              {"served_iv_in_interval", t.served_in_interval},
                              {"interval_width", t.interval_width.json()},
                              {"rule", "reference price flat in sigma at the served IV "
                                       "(1e-5 relative step on both sides within 1e-12 * "
                                       "max(P, 1)): the price does not identify an IV; the "
                                       "interval is every sigma that reprices it"}}},
          {"bisection_evaluations", t.evaluations.json()},
          {"price_table", {{"scope", "served rows with a reference price at the served IV"},
                           {"abs_error_usd", t.price_error_usd.json()},
                           {"rel_error_price_ge_0p01", t.price_error_rel.json()},
                           {"newton_distance_iv", t.newton_distance.json(args.threshold)}}},
          {"per_expiry", std::move(expiry)}, {"per_side_moneyness", std::move(classes)},
          {"build_info", bench::build_info_json()}, {"runtime_s", runtime_s}};
}

int run(const Arguments &args) {
  const auto started = std::chrono::steady_clock::now();
  const Table input = read_table(args.input);
  const Table predicted = read_table(args.predictions);
  const std::set<std::string> excluded = lattice_chains(args.report);
  const std::map<std::string, Market> markets = chain_markets(input);
  std::map<std::string, std::size_t> input_row_of;
  for (std::size_t i = 0; i < input.rows.size(); ++i) {
    input_row_of.emplace(input.at(i, "row_id"), i);
  }
  std::ofstream out{args.output};
  if (!out) throw std::runtime_error("cannot open --output");
  out << "row_id,chain_id,side,strike,variance_years,calendar_years,spot,rate_eff,yield_eff,"
         "served_price,served_iv,bracketed_iv,abs_iv_error,bracket_width,evaluations,"
         "root_residual_usd,ref_price_at_served_iv,price_error_usd,price_error_rel,vega_ref,"
         "newton_distance_iv,ref_uncertainty_usd,ref_uncertainty_iv,iv_interval_lo,"
         "iv_interval_hi,status,population\n";
  out << std::setprecision(17);
  Totals t;
  for (std::size_t p = 0; p < predicted.rows.size(); ++p) {
    const std::string &status = predicted.at(p, "status");
    if (status != "ok" && status != "ok_price_only") continue;
    if (args.max_rows != 0U && t.compared >= args.max_rows) break;
    ++t.compared;
    const std::string &row_id = predicted.at(p, "row_id");
    const std::string &chain = predicted.at(p, "chain_id");
    const auto found = input_row_of.find(row_id);
    if (found == input_row_of.end()) throw std::runtime_error("prediction row_id not in input");
    const std::size_t i = found->second;
    const auto market = markets.find(chain);
    if (market == markets.end()) throw std::runtime_error("served chain has no market");
    const Market &m = market->second;
    const double served_price = real_or_nan(predicted.at(p, "predicted_price"));
    const double served_iv = real_or_nan(predicted.at(p, "predicted_vol"));
    const Side side = input.at(i, "is_call") == "1" ? Side::Call : Side::Put;
    const Contract c{m.spot, real_or_nan(input.at(i, "strike")), m.variance_years,
                     m.rate * (m.calendar_years / m.variance_years),
                     effective_yield(m, real_or_nan(input.at(i, "sdiv")), args.dividends), side};
    const RowResult r = compare_row(c, served_price, served_iv, args.tolerance,
                                    excluded.contains(chain), status == "ok_price_only");
    ++t.status_counts[r.status];
    const double err = std::fabs(served_iv - r.root.sigma);
    const double price_err = std::fabs(r.ref_at_served - served_price);
    const double price_rel = served_price >= 0.01 ? price_err / served_price : kNaN;
    const double newton = r.vega > 0.0 ? price_err / r.vega : kNaN;
    const double fine_iv = r.vega > 0.0 ? r.fine_gap / r.vega : kNaN;
    std::string population = r.status == "not_identified" ? "iv_not_identified" : r.status;
    if (r.status == "bracketed") {
      t.iv_error.add(err);
      t.evaluations.add(static_cast<double>(r.root.evaluations));
      t.uncertainty_usd.add(r.fine_gap);
      t.uncertainty_iv.add(fine_iv);
      // An undefined uncertainty (no reference vega) is unresolved, never a pass.
      const bench::IvPopulation p = bench::classify_iv_row(err, fine_iv, args.threshold);
      population = bench::iv_population_name(p);
      switch (p) {
      case bench::IvPopulation::ResolvedPass:
        ++t.resolved_pass;
        break;
      case bench::IvPopulation::ResolvedFail:
        ++t.resolved_fail;
        break;
      case bench::IvPopulation::DecidableFail:
        ++t.decidable_fail;
        break;
      case bench::IvPopulation::ReferenceUnresolved:
        ++t.unresolved;
        if (!std::isfinite(fine_iv)) ++t.uncertainty_undefined;
        if (err > args.threshold) ++t.unresolved_over;
        break;
      }
      if (price_err <= kPairConsistentUsd) {
        ++t.pair_consistent;
      } else {
        ++t.non_circular[population];
      }
      const double moneyness = std::log(c.strike / c.spot);
      const std::string bucket = std::string(side == Side::Call ? "call" : "put") +
          (moneyness < -0.1 ? ":k<-0.1" : moneyness > 0.1 ? ":k>0.1" : ":|k|<=0.1");
      for (Group *g : {&t.by_expiry[chain], &t.by_side_moneyness[bucket]}) {
        ++g->rows;
        g->iv_error.add(err);
        g->price_error_usd.add(price_err);
      }
    }
    if (r.status == "not_identified") {
      t.interval_width.add(r.interval_hi - r.interval_lo);
      if (r.interval_lo <= served_iv && served_iv <= r.interval_hi) ++t.served_in_interval;
    }
    if (std::isfinite(r.ref_at_served)) {
      t.price_error_usd.add(price_err);
      t.price_error_rel.add(price_rel);
      t.newton_distance.add(newton);
    }
    out << row_id << ',' << chain << ',' << (side == Side::Call ? "call" : "put") << ',';
    for (const double v : {c.strike, m.variance_years, m.calendar_years, c.spot, c.rate,
                           c.yield, served_price, served_iv, r.root.sigma, err,
                           r.root.hi - r.root.lo, static_cast<double>(r.root.evaluations),
                           r.root.residual_usd, r.ref_at_served, price_err, price_rel, r.vega,
                           newton, r.fine_gap, fine_iv, r.interval_lo, r.interval_hi}) {
      if (std::isfinite(v)) out << v;
      out << ',';
    }
    out << r.status << ',' << population << '\n';
  }
  out.close();
  if (!out) throw std::runtime_error("--output write failed");
  const Json summary = summary_json(args, t, markets,
      bench::seconds_between(started, std::chrono::steady_clock::now()));
  std::ofstream summary_stream{args.summary};
  if (!summary_stream) throw std::runtime_error("cannot open --summary");
  summary_stream << summary.dump(2) << '\n';
  summary_stream.close();
  if (!summary_stream) throw std::runtime_error("--summary write failed");
  std::cout << summary.dump(2) << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Arguments args = arguments(argc, argv);
    return args.help ? 0 : run(args);
  } catch (const std::exception &error) {
    std::cerr << "iv-comparator: " << error.what() << '\n';
    return 1;
  }
}
