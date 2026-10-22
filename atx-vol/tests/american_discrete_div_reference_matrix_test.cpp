#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "pricing/american_discrete_div_reference.hpp"

// SLOW suite (ATX_VOL_SLOW_FILTER: DiscreteDivReferenceMatrix.*) of lane D0: the
// data-driven matrix of the independent cash-dividend reference against the
// lattice KERNEL — the 301-step SplicedCrr default and its refined comparator
// (american_discrete_div_converged_price) — under matched conventions. It is the
// runner whose output populates atx-vol/docs/american-cash-dividend-reference.md:
// every number in that document's tables is a line this test prints (between the
// BEGIN/END markers), and with ATX_VOL_DDIV_REF_OUT=<dir> it also writes
// matrix.csv / tables.md there. Nothing here is a pinned price: the assertions are
// structural (every case resolves to a finite reference, the kernel answers it),
// and the findings — errors, UNRESOLVED cases — are reported, not asserted away.

namespace {

using atx::vol::american_discrete_div_converged_price;
using atx::vol::american_discrete_div_greek_bundle;
using atx::vol::american_discrete_div_implied_vol;
using atx::vol::american_discrete_div_price;
using atx::vol::CashDividend;
using atx::vol::DiscreteDivIvOpts;
using atx::vol::DiscreteDivScheme;
using atx::vol::DiscreteDivTier;
using atx::vol::DiscreteDivWorkspace;
using atx::vol::ExerciseStyle;
using atx::vol::kDiscreteDivConvergedSteps;
using atx::vol::kDiscreteDivDefaultSteps;
using atx::vol::Side;
using atx::vol::ddiv_ref::CashProcess;
using atx::vol::ddiv_ref::Estimate;
using atx::vol::ddiv_ref::pde_price;
using atx::vol::ddiv_ref::pde_reference;
using atx::vol::ddiv_ref::PdeGrid;
using atx::vol::ddiv_ref::PdeLadder;
using atx::vol::ddiv_ref::Problem;
using atx::vol::ddiv_ref::Reference;
using atx::vol::ddiv_ref::reference_price;
using atx::vol::ddiv_ref::ReferenceOpts;
using atx::vol::ddiv_ref::richardson_axis;

// The IV contract under test (master plan section 3.2): 1e-7 in volatility, the
// kernel inversion's own `DiscreteDivIvOpts::tol`. A reference whose uncertainty
// exceeds it in IV terms cannot certify a root at that contract: UNRESOLVED.
constexpr double kIvContract = 1.0e-7;

// The vega below which an IV is NOT identified (price-only, master plan 3.2): a
// price uncertainty maps to an IV uncertainty through 1 / vega, so at vega below
// 1e-4 x max(P, 1) no reference resolution certifies a root.
constexpr double kVegaIdentifiedRel = 1.0e-4;

// The matrix ladders. PDE: 4 levels, finest 9,600 log-space intervals x 8 x the
// nominal time base (300 per year, at least 150). Lattice: 5 levels, finest 16,000
// nominal steps. The first probe ran 800 / 200 per year (finest 6,400) in 248 s;
// this ladder is 1.5x on both PDE axes (~2.25x its cost) to bring the American
// puts' uncertainty under the 1e-7 IV contract, inside the lane's 10-minute cap.
[[nodiscard]] ReferenceOpts matrix_opts(double T) {
  ReferenceOpts o;
  o.pde.base.space_base = 1200;
  o.pde.base.time_base = std::max(150, static_cast<int>(std::lround(300.0 * T)));
  o.pde.levels = 4;
  o.lattice.base.steps_base = 1000;
  o.lattice.levels = 5;
  return o;
}

// The derivative study's ladder (every bumped price is a full ladder): the first
// probe's, 800 / 200 per year.
[[nodiscard]] PdeLadder derivative_ladder(double T) {
  PdeLadder l;
  l.base.space_base = 800;
  l.base.time_base = std::max(100, static_cast<int>(std::lround(200.0 * T)));
  l.levels = 4;
  return l;
}

struct Case {
  std::string id;
  std::string group;
  double S = 100.0;
  double K = 100.0;
  double T = 1.0;
  double sigma = 0.25;
  double r = 0.04;
  double q = 0.0;
  Side side = Side::Put;
  CashProcess process = CashProcess::SpotLiquidator;
  std::vector<CashDividend> divs;
};

[[nodiscard]] Case make(std::string id, std::string group, double S, double K, double T,
                        double sigma, double r, Side side, std::vector<CashDividend> divs,
                        CashProcess process = CashProcess::SpotLiquidator) {
  Case c;
  c.id = std::move(id);
  c.group = std::move(group);
  c.S = S;
  c.K = K;
  c.T = T;
  c.sigma = sigma;
  c.r = r;
  c.side = side;
  c.process = process;
  c.divs = std::move(divs);
  return c;
}

[[nodiscard]] std::vector<CashDividend> quarterly(double first, double amount, double T) {
  std::vector<CashDividend> out;
  for (double t = first; t <= T + 1e-12; t += 0.25) {
    out.push_back({t, amount});
  }
  return out;
}

// The matrix of the lane brief, grouped A-I (doc section 3 explains each group).
[[nodiscard]] std::vector<Case> matrix() {
  std::vector<Case> m;
  constexpr double kSig = 0.25;
  constexpr double kR = 0.04;
  // A: ex-date alignment perturbations. T 0.5, one 2.00 at 0.25 = step 150.5 of
  // the 301 lattice (round-half-even -> 150); +-1 step; +-0.01 step around the
  // rounding boundary.
  {
    const double dt = 0.5 / 301.0;
    const std::array<std::pair<const char *, double>, 5> taus{
        {{"-1step", 0.25 - dt}, {"base", 0.25}, {"+1step", 0.25 + dt},
         {"150.49", 150.49 * dt}, {"150.51", 150.51 * dt}}};
    int i = 1;
    for (const auto &[tag, tau] : taus) {
      m.push_back(make("A" + std::to_string(i) + "p", std::string("A align ") + tag, 100, 100,
                       0.5, kSig, kR, Side::Put, {{tau, 2.0}}));
      m.push_back(make("A" + std::to_string(i) + "c", std::string("A align ") + tag, 100, 95,
                       0.5, kSig, kR, Side::Call, {{tau, 2.0}}));
      ++i;
    }
  }
  // B: dividend on the first step, on the last step before expiry, on expiry.
  {
    const double dt = 1.0 / 301.0;
    const std::array<std::pair<const char *, double>, 3> taus{
        {{"first step", 0.7 * dt}, {"last step", 1.0 - 0.4 * dt}, {"on expiry", 1.0}}};
    int i = 1;
    for (const auto &[tag, tau] : taus) {
      m.push_back(make("B" + std::to_string(i) + "p", std::string("B ") + tag, 100, 100, 1.0,
                       kSig, kR, Side::Put, {{tau, 3.0}}));
      m.push_back(make("B" + std::to_string(i) + "c", std::string("B ") + tag, 100, 95, 1.0,
                       kSig, kR, Side::Call, {{tau, 3.0}}));
      ++i;
    }
  }
  // C: multiple dividends, quarterly 1.00 from 0.10: 2, 4, 8 events.
  for (const double T : {0.5, 1.0, 2.0}) {
    const auto divs = quarterly(0.1, 1.0, T);
    const std::string n = std::to_string(divs.size());
    m.push_back(make("C" + n + "p", "C " + n + " events", 100, 100, T, kSig, kR, Side::Put, divs));
    m.push_back(make("C" + n + "c", "C " + n + " events", 100, 95, T, kSig, kR, Side::Call, divs));
  }
  // D: LEAPS. T 3 with 12 quarterly 1.00; an SPY-scale long chain (the L3f worst
  // contract's shape: S 772.98, T 2.182, sigma 0.35, nine quarterly 1.8311).
  {
    const auto divs = quarterly(0.1, 1.0, 3.0);
    m.push_back(make("D12p", "D LEAPS 3y", 100, 100, 3.0, kSig, kR, Side::Put, divs));
    m.push_back(make("D12c", "D LEAPS 3y", 100, 95, 3.0, kSig, kR, Side::Call, divs));
    const auto spy = quarterly(0.15, 1.8311, 2.182);
    m.push_back(make("DSPYp", "D LEAPS SPY", 772.98, 700, 2.182, 0.35, 0.047, Side::Put, spy));
    m.push_back(make("DSPYc", "D LEAPS SPY", 772.98, 858, 2.182, 0.35, 0.047, Side::Call, spy));
  }
  // E: large dividend / spot: 1 %, 5 %, 20 % at mid-life; and the review-m1
  // contract (dev200: S 6.375, K 5, cash 4.10 = 64 % of spot, 5.9 days out).
  for (const double D : {1.0, 5.0, 20.0}) {
    const std::string tag = std::to_string(static_cast<int>(D));
    m.push_back(make("E" + tag + "p", "E D/S " + tag + "%", 100, 100, 1.0, kSig, kR, Side::Put,
                     {{0.5, D}}));
    m.push_back(make("E" + tag + "c", "E D/S " + tag + "%", 100, 95, 1.0, kSig, kR, Side::Call,
                     {{0.5, D}}));
  }
  {
    const std::vector<CashDividend> m1{{0.016272795852273703, 4.0999999046325684}};
    m.push_back(make("E64p", "E D/S 64% (review m1)", 6.375, 5.0, 0.34715297818183899, 0.2,
                     0.046780478209257126, Side::Put, m1));
    m.push_back(make("E64c", "E D/S 64% (review m1)", 6.375, 5.0, 0.34715297818183899, 0.2,
                     0.046780478209257126, Side::Call, m1));
  }
  // F: exercise immediately before the jump (deep in the money).
  m.push_back(make("F1c", "F ITM pre-jump", 100, 70, 0.5, kSig, kR, Side::Call, {{0.45, 5.0}}));
  m.push_back(make("F1p", "F ITM pre-jump", 100, 130, 0.5, kSig, kR, Side::Put, {{0.45, 5.0}}));
  m.push_back(make("F2c", "F ITM pre-jump", 100, 80, 0.25, kSig, kR, Side::Call, {{0.24, 4.0}}));
  m.push_back(make("F2p", "F ITM pre-jump", 100, 125, 0.25, kSig, kR, Side::Put, {{0.24, 4.0}}));
  // G: r = 0.
  m.push_back(make("G1p", "G r=0", 100, 100, 1.0, kSig, 0.0, Side::Put, {{0.5, 3.0}}));
  m.push_back(make("G1c", "G r=0", 100, 95, 1.0, kSig, 0.0, Side::Call, {{0.5, 3.0}}));
  m.push_back(make("G4p", "G r=0", 100, 100, 1.0, kSig, 0.0, Side::Put, quarterly(0.1, 1.0, 1.0)));
  m.push_back(make("G4c", "G r=0", 100, 95, 1.0, kSig, 0.0, Side::Call, quarterly(0.1, 1.0, 1.0)));
  // H: the escrowed convention, against the Escrowed* lattices.
  m.push_back(make("H1p", "H escrowed", 100, 100, 1.0, kSig, kR, Side::Put, {{0.5, 3.0}},
                   CashProcess::Escrowed));
  m.push_back(make("H1c", "H escrowed", 100, 95, 1.0, kSig, kR, Side::Call, {{0.5, 3.0}},
                   CashProcess::Escrowed));
  m.push_back(make("H4p", "H escrowed", 100, 100, 1.0, kSig, kR, Side::Put,
                   quarterly(0.1, 1.0, 1.0), CashProcess::Escrowed));
  m.push_back(make("H4c", "H escrowed", 100, 95, 1.0, kSig, kR, Side::Call,
                   quarterly(0.1, 1.0, 1.0), CashProcess::Escrowed));
  // I: cash that can exceed spot — liquidator (the kernel's) vs survivor.
  for (const CashProcess process : {CashProcess::SpotLiquidator, CashProcess::SpotSurvivor}) {
    const char *tag = (process == CashProcess::SpotLiquidator) ? "L" : "S";
    m.push_back(make(std::string("I") + tag + "p",
                     std::string("I cash>spot ") + atx::vol::ddiv_ref::to_string(process), 10, 8,
                     0.5, 0.8, 0.03, Side::Put, {{0.25, 6.0}}, process));
    m.push_back(make(std::string("I") + tag + "c",
                     std::string("I cash>spot ") + atx::vol::ddiv_ref::to_string(process), 10, 8,
                     0.5, 0.8, 0.03, Side::Call, {{0.25, 6.0}}, process));
  }
  return m;
}

[[nodiscard]] Problem problem_of(const Case &c) {
  Problem p;
  p.S = c.S;
  p.K = c.K;
  p.T = c.T;
  p.sigma = c.sigma;
  p.r = c.r;
  p.q = c.q;
  p.side = c.side;
  p.exercise = ExerciseStyle::American;
  p.process = c.process;
  p.dividends = c.divs;
  return p;
}

// Reference vega for unit conversion only (IV bp = USD / vega): one PDE pair at a
// fixed common grid, sigma +- 1e-3. Its own error (~1e-6 relative) is irrelevant
// at the precision the conversion is quoted.
[[nodiscard]] double reference_vega(const Case &c) {
  Problem up = problem_of(c);
  Problem dn = up;
  up.sigma += 1.0e-3;
  dn.sigma -= 1.0e-3;
  PdeGrid g;
  const PdeLadder l = derivative_ladder(c.T);
  g.space_base = l.base.space_base;
  g.space_level = 2;
  g.time_base = l.base.time_base;
  g.time_level = 2;
  g.domain_sigma = c.sigma;
  const auto pu = pde_price(up, g);
  const auto pd = pde_price(dn, g);
  if (!pu || !pd) {
    return std::nan("");
  }
  return (pu->price - pd->price) / 2.0e-3;
}

// The kernel's columns for one case. Spot conventions: the 301-step SplicedCrr
// default and the refined comparator (SplicedBbs 2,401 + Richardson vs 1,201) with
// its claimed uncertainty |fine - coarse|. Escrowed: EscrowedLeisenReimer at 301
// and at 2,401 — the kernel has no escrowed comparator, so the refined column is
// that scheme refined and carries no claim.
struct KernelRow {
  double p301 = std::nan("");
  double pcmp = std::nan("");
  double cmp_claim = std::nan(""); // the comparator's own |fine - coarse|
  double iv301_bp = std::nan("");  // (sigma the 301 lattice implies from the REFERENCE - sigma) bp
  std::string iv_note;
  DiscreteDivScheme scheme301 = DiscreteDivScheme::SplicedCrr;
};

[[nodiscard]] KernelRow kernel_row(const Case &c, double ref, DiscreteDivWorkspace &ws) {
  KernelRow k;
  const bool escrowed = c.process == CashProcess::Escrowed;
  k.scheme301 = escrowed ? DiscreteDivScheme::EscrowedLeisenReimer : DiscreteDivScheme::SplicedCrr;
  const auto p301 = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs,
                                                kDiscreteDivDefaultSteps, ExerciseStyle::American,
                                                k.scheme301, ws);
  EXPECT_TRUE(p301.has_value()) << c.id;
  if (p301) {
    k.p301 = *p301;
  }
  if (escrowed) {
    const auto fine = american_discrete_div_price(
        c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs, kDiscreteDivConvergedSteps,
        ExerciseStyle::American, DiscreteDivScheme::EscrowedLeisenReimer, ws);
    EXPECT_TRUE(fine.has_value()) << c.id;
    if (fine) {
      k.pcmp = *fine;
    }
  } else {
    const auto conv = american_discrete_div_converged_price(c.S, c.K, c.T, c.sigma, c.r, c.q,
                                                            c.side, c.divs,
                                                            ExerciseStyle::American, ws);
    EXPECT_TRUE(conv.has_value()) << c.id;
    if (conv) {
      k.pcmp = conv->price;
      k.cmp_claim = std::abs(conv->fine - conv->coarse);
    }
  }
  DiscreteDivIvOpts opts;
  opts.seed_sigma = c.sigma;
  opts.tol = kIvContract;
  opts.tier = DiscreteDivTier::Accurate;
  const auto iv = american_discrete_div_implied_vol(ref, c.S, c.K, c.T, c.r, c.q, c.side, c.divs,
                                                    kDiscreteDivDefaultSteps, k.scheme301, opts,
                                                    ws);
  if (iv) {
    k.iv301_bp = (iv->sigma - c.sigma) * 1.0e4;
  } else {
    k.iv_note = std::string(atx::core::to_string(iv.error().code()));
  }
  return k;
}

struct Row {
  Case c;
  Reference ref;
  KernelRow k;
  double vega = 0.0;
  double seconds = 0.0;
  std::string status;

  // The kernel models the liquidator; a survivor row measures the convention gap.
  [[nodiscard]] bool kernel_model() const noexcept {
    return c.process != CashProcess::SpotSurvivor;
  }
  [[nodiscard]] bool iv_identified() const noexcept {
    return vega > kVegaIdentifiedRel * std::max(std::abs(ref.value), 1.0);
  }
};

// RESOLVED: the two methods agree and the reference uncertainty is inside the
// 1e-7 IV contract. UNRESOLVED otherwise, with the reason. A row whose IV is not
// identified is PRICE-ONLY (its USD uncertainty is in the table); a survivor row is
// a CONVENTION GAP (the kernel prices the liquidator).
[[nodiscard]] std::string status_of(const Row &row) {
  if (!row.ref.methods_agree) {
    return "UNRESOLVED (methods disagree)";
  }
  if (!row.kernel_model()) {
    return "CONVENTION GAP (kernel = liquidator)";
  }
  if (!row.iv_identified()) {
    return "PRICE-ONLY (IV not identified)";
  }
  if (!(row.ref.uncertainty <= kIvContract * row.vega)) {
    return "UNRESOLVED (u > 1e-7 IV)";
  }
  return "RESOLVED";
}

[[nodiscard]] bool counts_as_resolved(const Row &row) {
  return row.status == "RESOLVED" || row.status.rfind("PRICE-ONLY", 0) == 0 ||
         row.status.rfind("CONVENTION GAP", 0) == 0;
}

[[nodiscard]] std::size_t event_count(const Case &c) {
  std::size_t n = 0;
  for (const CashDividend &d : c.divs) {
    n += (d.tau > 0.0 && d.tau <= c.T * (1.0 + atx::vol::kTauAtExpiryRelTol)) ? 1U : 0U;
  }
  return n;
}

[[nodiscard]] double max_cash_ratio(const Case &c) {
  double best = 0.0;
  for (const CashDividend &d : c.divs) {
    best = std::max(best, d.amount / c.S);
  }
  return best;
}

[[nodiscard]] std::string fmt(const char *f, double v) {
  if (!std::isfinite(v)) {
    return "n/a";
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, f, v);
  return buf;
}

[[nodiscard]] std::string axis_text(const atx::vol::ddiv_ref::Axis &a) {
  std::string s;
  for (int i = 0; i < a.n; ++i) {
    s += fmt("%.9f", a.values[static_cast<std::size_t>(i)]);
    s += (i + 1 < a.n) ? " " : "";
  }
  s += " | p " + fmt("%.2f", a.observed_order) + (a.asymptotic ? "" : " (non-asym)") + " | u " +
       fmt("%.1e", a.uncertainty);
  return s;
}

void emit(std::FILE *out, const std::string &line) {
  std::fputs(line.c_str(), stdout);
  std::fputs("\n", stdout);
  if (out != nullptr) {
    std::fputs(line.c_str(), out);
    std::fputs("\n", out);
  }
}

// The opt-in output directory (ATX_VOL_DDIV_REF_OUT); empty when unset. The
// _dupenv_s form on Windows, as tests/corpus_test.cpp reads its own switch.
[[nodiscard]] std::string out_dir() {
#if defined(_WIN32)
  char *value = nullptr;
  std::size_t size = 0U;
  std::string dir;
  if (::_dupenv_s(&value, &size, "ATX_VOL_DDIV_REF_OUT") == 0 && value != nullptr) {
    dir = value;
  }
  std::free(value);
  return dir;
#else
  const char *value = std::getenv("ATX_VOL_DDIV_REF_OUT");
  return value != nullptr ? std::string(value) : std::string();
#endif
}

[[nodiscard]] std::FILE *open_out(const char *name) {
  const std::string dir = out_dir();
  if (dir.empty()) {
    return nullptr;
  }
  const std::string path = dir + "/" + name;
  std::FILE *f = nullptr;
#if defined(_WIN32)
  if (fopen_s(&f, path.c_str(), "wb") != 0) {
    f = nullptr;
  }
#else
  f = std::fopen(path.c_str(), "wb");
#endif
  return f;
}

void write_tables(const std::vector<Row> &rows, std::FILE *md) {
  emit(md, "<!-- BEGIN D0 COMPARISON TABLE -->");
  emit(md, "| id | group | side | T | events | max D/S | reference (PDE) +- u | lattice ref +- u "
           "| agree | P301 | d301 USD | d301 IV bp (inv) | refined cmp | dcmp USD | dcmp bp "
           "(lin) | cmp claim | status |");
  emit(md, "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|");
  for (const Row &r : rows) {
    const double d301 = r.k.p301 - r.ref.value;
    const double dcmp = r.k.pcmp - r.ref.value;
    std::string claim = "n/a";
    if (std::isfinite(r.k.cmp_claim) && r.kernel_model()) {
      claim = (std::abs(dcmp) <= r.k.cmp_claim + r.ref.uncertainty) ? "holds" : "VIOLATED";
      claim += " (" + fmt("%.1e", r.k.cmp_claim) + ")";
    }
    const bool iv_ok = r.kernel_model() && r.iv_identified();
    const std::string iv301 = !iv_ok                ? std::string("n/a")
                              : r.k.iv_note.empty() ? fmt("%+.3f", r.k.iv301_bp)
                                                    : r.k.iv_note;
    const std::string bpcmp = iv_ok ? fmt("%+.3f", 1.0e4 * dcmp / r.vega) : std::string("n/a");
    emit(md, "| " + r.c.id + " | " + r.c.group + " | " + (r.c.side == Side::Put ? "P" : "C") +
                 " | " + fmt("%.4g", r.c.T) + " | " + std::to_string(event_count(r.c)) + " | " +
                 fmt("%.3f", max_cash_ratio(r.c)) + " | " + fmt("%.7f", r.ref.value) + " +- " +
                 fmt("%.1e", r.ref.uncertainty) + " | " + fmt("%.7f", r.ref.lattice.value) +
                 " +- " + fmt("%.1e", r.ref.lattice.uncertainty) + " | " +
                 (r.ref.methods_agree ? "yes" : "NO") + " | " + fmt("%.7f", r.k.p301) + " | " +
                 fmt("%+.2e", d301) + " | " + iv301 + " | " + fmt("%.7f", r.k.pcmp) + " | " +
                 fmt("%+.2e", dcmp) + " | " + bpcmp + " | " + claim + " | " + r.status + " |");
  }
  emit(md, "<!-- END D0 COMPARISON TABLE -->");
  emit(md, "");
  emit(md, "<!-- BEGIN D0 CONVERGENCE TABLE -->");
  emit(md, "| id | PDE space (finest time; Nx 1200..9600) | PDE time (finest space; nominal "
           "base..8x base) | lattice (steps 1000..16000) | u_ref USD | u_ref IV bp | vega | PDE "
           "Howard max iter / tie steps / worst LCP ulps | wall s |");
  emit(md, "|---|---|---|---|---|---|---|---|---|");
  for (const Row &r : rows) {
    const Estimate &pde = r.ref.pde;
    emit(md, "| " + r.c.id + " | " + axis_text(pde.space) + " | " + axis_text(pde.time) + " | " +
                 axis_text(r.ref.lattice.time) + " | " + fmt("%.1e", r.ref.uncertainty) + " | " +
                 (r.iv_identified() ? fmt("%.2e", 1.0e4 * r.ref.uncertainty / r.vega)
                                    : std::string("n/a")) +
                 " | " + fmt("%.4g", r.vega) + " | " + std::to_string(pde.max_policy_iterations) +
                 " / " + std::to_string(pde.lcp_rounding_ties) + " / " +
                 fmt("%.0f", pde.max_lcp_violation / std::numeric_limits<double>::epsilon()) +
                 " | " + fmt("%.1f", r.seconds) + " |");
  }
  emit(md, "<!-- END D0 CONVERGENCE TABLE -->");
}

void write_summary(const std::vector<Row> &rows, std::FILE *md) {
  struct Bucket {
    const char *events;
    const char *tenor;
    std::size_t ev_lo, ev_hi;
    double t_lo, t_hi;
  };
  const std::array<Bucket, 6> buckets{{{"1", "<= 0.5 y", 1, 1, 0.0, 0.5},
                                       {"1", "0.5-1.5 y", 1, 1, 0.5, 1.5},
                                       {"2-4", "<= 1.5 y", 2, 4, 0.0, 1.5},
                                       {"8", "1.5-2.5 y", 5, 99, 1.5, 2.5},
                                       {"9-12", "> 2 y", 9, 99, 2.0, 99.0},
                                       {"all", "all", 0, 99, 0.0, 99.0}}};
  emit(md, "<!-- BEGIN D0 SUMMARY TABLE -->");
  emit(md, "| events | tenor | n | max abs d301 USD | max abs d301 IV bp (inv) | max abs dcmp USD "
           "| max abs dcmp bp | cmp claim violated | UNRESOLVED |");
  emit(md, "|---|---|---|---|---|---|---|---|---|");
  for (const Bucket &b : buckets) {
    std::size_t n = 0;
    std::size_t unresolved = 0;
    std::size_t violated = 0;
    double d301 = 0.0;
    double b301 = 0.0;
    double dcmp = 0.0;
    double bcmp = 0.0;
    for (const Row &r : rows) {
      const std::size_t ev = event_count(r.c);
      // Group I's survivor rows (a convention the kernel does not implement) stay
      // out of the kernel-error buckets; the comparison table carries them.
      if (!r.kernel_model() || ev < b.ev_lo || ev > b.ev_hi || !(r.c.T > b.t_lo) ||
          r.c.T > b.t_hi) {
        continue;
      }
      ++n;
      unresolved += counts_as_resolved(r) ? 0U : 1U;
      const double err_cmp = std::abs(r.k.pcmp - r.ref.value);
      const bool claimed = std::isfinite(r.k.cmp_claim);
      violated += (claimed && err_cmp > r.k.cmp_claim + r.ref.uncertainty) ? 1U : 0U;
      d301 = std::max(d301, std::abs(r.k.p301 - r.ref.value));
      dcmp = std::max(dcmp, err_cmp);
      if (r.iv_identified()) {
        if (std::isfinite(r.k.iv301_bp)) {
          b301 = std::max(b301, std::abs(r.k.iv301_bp));
        }
        bcmp = std::max(bcmp, 1.0e4 * err_cmp / r.vega);
      }
    }
    emit(md, std::string("| ") + b.events + " | " + b.tenor + " | " + std::to_string(n) + " | " +
                 fmt("%.2e", d301) + " | " + fmt("%.3f", b301) + " | " + fmt("%.2e", dcmp) +
                 " | " + fmt("%.3f", bcmp) + " | " + std::to_string(violated) + " | " +
                 std::to_string(unresolved) + " |");
  }
  emit(md, "<!-- END D0 SUMMARY TABLE -->");
}

void write_csv(const std::vector<Row> &rows) {
  std::FILE *csv = open_out("matrix.csv");
  if (csv == nullptr) {
    return;
  }
  std::fprintf(csv, "id,group,side,process,S,K,T,sigma,r,q,events,max_cash_ratio,ref,ref_u,"
                    "pde_value,pde_u,pde_finest,lattice_value,lattice_u,methods_agree,vega,"
                    "p301,iv301_bp,pcmp,cmp_claim,status,seconds\n");
  for (const Row &r : rows) {
    std::fprintf(csv,
                 "%s,%s,%s,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%zu,%.17g,%.17g,%.17g,%.17g,"
                 "%.17g,%.17g,%.17g,%.17g,%d,%.17g,%.17g,%.17g,%.17g,%.17g,%s,%.3f\n",
                 r.c.id.c_str(), r.c.group.c_str(), r.c.side == Side::Put ? "P" : "C",
                 atx::vol::ddiv_ref::to_string(r.c.process), r.c.S, r.c.K, r.c.T, r.c.sigma,
                 r.c.r, r.c.q, event_count(r.c), max_cash_ratio(r.c), r.ref.value,
                 r.ref.uncertainty, r.ref.pde.value, r.ref.pde.uncertainty, r.ref.pde.finest,
                 r.ref.lattice.value, r.ref.lattice.uncertainty, r.ref.methods_agree ? 1 : 0,
                 r.vega, r.k.p301, r.k.iv301_bp, r.k.pcmp, r.k.cmp_claim, r.status.c_str(),
                 r.seconds);
  }
  std::fclose(csv);
}

// ── The derivative subset: delta and vega by bump-and-revalue ON the reference
// (each bumped price a full PDE ladder), with a bump-size study, against the 301
// lattice's own greeks (delta off the tree, vega by its 10 % relative bump).
void derivative_study(const std::vector<Row> &rows, std::FILE *md) {
  const std::array<const char *, 6> ids{"A2p", "B3p", "C4c", "D12p", "E20p", "F1c"};
  emit(md, "<!-- BEGIN D0 DERIVATIVE TABLE -->");
  emit(md, "| id | greek | bump study (h: value) | extrapolated +- u | grid (PDE finest) | "
           "lattice 301 | d301 |");
  emit(md, "|---|---|---|---|---|---|---|");
  for (const Row &r : rows) {
    if (std::find(ids.begin(), ids.end(), r.c.id) == ids.end()) {
      continue;
    }
    PdeLadder ladder = derivative_ladder(r.c.T);
    ladder.base.domain_sigma = r.c.sigma; // common-mode grid for the sigma bumps
    const auto bumped = [&](double dS, double dSigma) {
      Problem p = problem_of(r.c);
      p.S += dS;
      p.sigma += dSigma;
      const auto e = pde_reference(p, ladder);
      EXPECT_TRUE(e.has_value()) << r.c.id;
      return e.has_value() ? *e : Estimate{};
    };
    const auto bundle = american_discrete_div_greek_bundle(r.c.S, r.c.K, r.c.T, r.c.sigma, r.c.r,
                                                           r.c.q, r.c.side, r.c.divs);
    EXPECT_TRUE(bundle.has_value()) << r.c.id;
    PdeGrid finest = ladder.base;
    finest.space_level = ladder.levels - 1;
    finest.time_level = ladder.levels - 1;
    const auto grid = pde_price(problem_of(r.c), finest);
    for (const bool is_delta : {true, false}) {
      const std::array<double, 3> hs = is_delta
                                           ? std::array<double, 3>{0.02 * r.c.S, 0.01 * r.c.S,
                                                                   0.005 * r.c.S}
                                           : std::array<double, 3>{0.01, 0.005, 0.0025};
      std::array<double, 3> values{};
      double noise = 0.0;
      std::string study;
      for (std::size_t i = 0; i < hs.size(); ++i) {
        const double h = hs[i];
        const Estimate up = is_delta ? bumped(h, 0.0) : bumped(0.0, h);
        const Estimate dn = is_delta ? bumped(-h, 0.0) : bumped(0.0, -h);
        values[i] = (up.value - dn.value) / (2.0 * h);
        noise = (up.uncertainty + dn.uncertainty) / (2.0 * h); // the smallest h's, last
        study += fmt("%.4g", h) + ": " + fmt("%.8f", values[i]) + (i + 1 < hs.size() ? "; " : "");
      }
      const atx::vol::ddiv_ref::Axis axis = richardson_axis(values, 2.0);
      const double est = values[2] + axis.correction;
      const double u = axis.uncertainty + noise;
      const double lattice = bundle ? (is_delta ? bundle->delta : bundle->vega) : std::nan("");
      const double grid_value = grid ? (is_delta ? grid->delta : std::nan("")) : std::nan("");
      emit(md, "| " + r.c.id + " | " + (is_delta ? "delta" : "vega") + " | " + study + " | " +
                   fmt("%.8f", est) + " +- " + fmt("%.1e", u) + " | " + fmt("%.8f", grid_value) +
                   " | " + fmt("%.8f", lattice) + " | " + fmt("%+.2e", lattice - est) + " |");
      EXPECT_TRUE(std::isfinite(est) && std::isfinite(u)) << r.c.id;
    }
  }
  emit(md, "<!-- END D0 DERIVATIVE TABLE -->");
}

TEST(DiscreteDivReferenceMatrix, FullMatrixAgainstTheLatticeKernel) {
  const auto t_all = std::chrono::steady_clock::now();
  DiscreteDivWorkspace ws(kDiscreteDivConvergedSteps);
  std::vector<Row> rows;
  for (const Case &c : matrix()) {
    const auto t0 = std::chrono::steady_clock::now();
    Row row;
    row.c = c;
    const auto ref = reference_price(problem_of(c), matrix_opts(c.T));
    ASSERT_TRUE(ref.has_value()) << c.id << ": " << ref.error().to_string();
    row.ref = *ref;
    row.vega = reference_vega(c);
    row.k = kernel_row(c, row.ref.value, ws);
    row.status = status_of(row);
    row.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(std::isfinite(row.ref.value) && std::isfinite(row.ref.uncertainty)) << c.id;
    EXPECT_GT(row.ref.uncertainty, 0.0) << c.id;
    // Not > 0: a survivor put's vega is legitimately negative (more volatility, more
    // chance the dividend is skipped) and a put inside its exercise region has none.
    EXPECT_TRUE(std::isfinite(row.vega)) << c.id;
    std::printf("[ddiv-matrix] %s ref %.10f u %.2e pde %.10f+-%.2e lat %.10f+-%.2e agree %d "
                "p301 %.10f pcmp %.10f iv301_bp %s vega %.4f %s (%.1f s)\n",
                c.id.c_str(), row.ref.value, row.ref.uncertainty, row.ref.pde.value,
                row.ref.pde.uncertainty, row.ref.lattice.value, row.ref.lattice.uncertainty,
                row.ref.methods_agree ? 1 : 0, row.k.p301, row.k.pcmp,
                row.k.iv_note.empty() ? fmt("%+.4f", row.k.iv301_bp).c_str()
                                      : row.k.iv_note.c_str(),
                row.vega, row.status.c_str(), row.seconds);
    std::fflush(stdout);
    rows.push_back(std::move(row));
  }
  std::FILE *md = open_out("tables.md");
  write_tables(rows, md);
  emit(md, "");
  write_summary(rows, md);
  emit(md, "");
  derivative_study(rows, md);
  if (md != nullptr) {
    std::fclose(md);
  }
  write_csv(rows);
  std::size_t unresolved = 0;
  std::size_t disagree = 0;
  std::size_t price_only = 0;
  for (const Row &r : rows) {
    unresolved += counts_as_resolved(r) ? 0U : 1U;
    disagree += r.ref.methods_agree ? 0U : 1U;
    price_only += (r.status.rfind("PRICE-ONLY", 0) == 0) ? 1U : 0U;
  }
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all).count();
  std::printf("[ddiv-matrix] SUMMARY cases %zu unresolved %zu price_only %zu methods_disagree %zu "
              "wall %.1f s\n",
              rows.size(), unresolved, price_only, disagree, wall);
  EXPECT_EQ(rows.size(), 50U);
}

} // namespace
