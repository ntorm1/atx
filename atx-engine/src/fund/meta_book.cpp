// meta_book.cpp — P2-S2-5 private implementation (USER DIRECTIVE: the driver body and
// all private helpers live in the .cpp / anonymous namespace, not the header; .agents/cpp
// §6). MetaBook::run reads as orchestration; every numeric step is a named helper below.
//
// Composes the four S2 units (Sleeve, MetaAllocator, cross_sleeve_risk, netting) plus
// combine::compute_metrics and the S1 MultiHorizonOptimizer — it re-implements none of
// them. The two passes (§4.5): PASS 1 runs each sleeve independently (net-after-optimize,
// causal); PASS 2 builds the TRAILING Ω from sleeve P&L strictly before s (R2), allocates
// capital, nets the period-s books, and accrues the report. Attribution sums to the fund
// (R4); the R7 one-sleeve pin is byte-identical to the sleeve's own MultiHorizonResult.
//
// Determinism (R1): every reduction is ascending-index order-fixed; no RNG, no clock, no
// unordered containers. The Meucci PCA uses Eigen's deterministic SelfAdjointEigenSolver.
// This is a COLD driver — the per-period Ω / trailing-slice / report allocations are at
// rebalance cadence, not a hot tick path (documented, accepted).

#include "atx/engine/fund/meta_book.hpp"

#include <cmath>   // std::fabs, std::sqrt, std::log, std::isfinite
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

#include <Eigen/Dense> // Eigen::Index, SelfAdjointEigenSolver (Meucci PCA)

#include "atx/core/linalg/linalg.hpp" // MatX (the trailing Ω — internal to the driver, not the public header)

#include "atx/engine/fund/cross_sleeve_risk.hpp" // sleeve_return_cov, fund_risk (representative RC)
#include "atx/engine/fund/netting.hpp"           // net_fund_book (NetResult)

namespace atx::engine::fund {

namespace {

// ===========================================================================
//  realized_sleeve_pnl — r_s[p] = Σ_i books[p][i]·returns_at(p)[i] (S2-5 resolution).
//
//  Sleeve s's realized return holding its period-p book through period p. Order-fixed
//  ascending i (R1). Length = #periods. A returns span shorter than the book is treated
//  as 0 past its end (defensive; the contract is length M, but we never read OOB).
// ===========================================================================
[[nodiscard]] std::vector<atx::f64>
realized_sleeve_pnl(const risk::MultiHorizonResult &sr,
                    const std::function<std::span<const atx::f64>(atx::usize)> &returns_at,
                    const risk::RebalanceSchedule &sched) {
  const atx::usize n = sched.periods.size();
  std::vector<atx::f64> r(n, 0.0);
  for (atx::usize p = 0U; p < n; ++p) {
    const std::span<const atx::f64> ret = returns_at(sched.periods[p]);
    const std::vector<atx::f64> &book = sr.books[p];
    atx::f64 acc = 0.0;
    const atx::usize m = (book.size() < ret.size()) ? book.size() : ret.size();
    for (atx::usize i = 0U; i < m; ++i) {
      acc += book[i] * ret[i];
    }
    r[p] = acc;
  }
  return r;
}

// ===========================================================================
//  trailing_pnl_slice — the TRAILING window r_s[ max(0, s−lookback) , s ) for sleeve si.
//
//  STRICTLY periods p < s (causal, R2): c[s] reads no P&L realized at or after s. Returns
//  the per-sleeve sub-series so sleeve_return_cov can build Ω over the window. Empty at
//  s == 0 (degenerate ⇒ the allocator fallback fires).
// ===========================================================================
[[nodiscard]] std::vector<atx::f64> trailing_pnl_slice(const std::vector<atx::f64> &pnl,
                                                       atx::usize s, atx::usize lookback) {
  const atx::usize lo = (s > lookback) ? (s - lookback) : 0U;
  std::vector<atx::f64> out;
  out.reserve(s - lo);
  for (atx::usize p = lo; p < s; ++p) { // p < s ⇒ strictly before the current period (R2)
    out.push_back(pnl[p]);
  }
  return out;
}

// ===========================================================================
//  diag_sqrt — per-sleeve vol σ_s = sqrt(Ω(s,s)) (the allocator's inverse-vol input).
//
//  A non-positive / non-finite diagonal ⇒ σ_s = 0 (the allocator treats an unusable vol
//  as equal-weight). Order-fixed ascending s.
// ===========================================================================
[[nodiscard]] std::vector<atx::f64> diag_sqrt(const atx::core::linalg::MatX &omega) {
  const auto s = static_cast<atx::usize>(omega.rows());
  std::vector<atx::f64> vol(s, 0.0);
  for (atx::usize i = 0U; i < s; ++i) {
    const atx::f64 d = omega(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i));
    vol[i] = (d > 0.0 && std::isfinite(d)) ? std::sqrt(d) : 0.0;
  }
  return vol;
}

// ===========================================================================
//  build_trailing_omega — Ω over the trailing P&L window of ALL sleeves at period s.
//
//  Slices each sleeve's r_s to { p < s, within lookback } (R2) and defers to
//  sleeve_return_cov (S2-3). At s == 0 every slice is empty ⇒ a 0-length panel of S
//  series ⇒ Ω is S×S all-zero ⇒ degenerate ⇒ the allocator fallback (equal / inverse-cap).
// ===========================================================================
[[nodiscard]] atx::core::Result<atx::core::linalg::MatX>
build_trailing_omega(const std::vector<std::vector<atx::f64>> &sleeve_pnl, atx::usize s,
                     atx::usize lookback) {
  std::vector<std::vector<atx::f64>> slices;
  slices.reserve(sleeve_pnl.size());
  for (const std::vector<atx::f64> &pnl : sleeve_pnl) {
    slices.push_back(trailing_pnl_slice(pnl, s, lookback));
  }
  std::vector<std::span<const atx::f64>> spans;
  spans.reserve(slices.size());
  for (const std::vector<atx::f64> &v : slices) {
    spans.emplace_back(v);
  }
  return sleeve_return_cov(std::span<const std::span<const atx::f64>>(spans));
}

// ===========================================================================
//  to_spans — non-owning span view over a vector-of-vectors (the netting inputs).
// ===========================================================================
[[nodiscard]] std::vector<std::span<const atx::f64>>
to_spans(const std::vector<std::vector<atx::f64>> &books) {
  std::vector<std::span<const atx::f64>> spans;
  spans.reserve(books.size());
  for (const std::vector<atx::f64> &b : books) {
    spans.emplace_back(b);
  }
  return spans;
}

// ===========================================================================
//  l1_abs / signed_sum — Σ_i |x_i| (gross) and Σ_i x_i (net) over a fund book.
// ===========================================================================
[[nodiscard]] atx::f64 l1_abs(const std::vector<atx::f64> &w) noexcept {
  atx::f64 s = 0.0;
  for (const atx::f64 x : w) {
    s += std::fabs(x);
  }
  return s;
}
[[nodiscard]] atx::f64 signed_sum(const std::vector<atx::f64> &w) noexcept {
  atx::f64 s = 0.0;
  for (const atx::f64 x : w) {
    s += x;
  }
  return s;
}

// ===========================================================================
//  meucci_effective_bets — N_Ent diversification over Ω, c (A5).
//
//  PCA Ω = EΛEᵀ (Eigen SelfAdjointEigenSolver, deterministic); principal weights
//  ω̃ = Eᵀc; diversification distribution p_i = (ω̃_i²·λ_i)/Σ_j(ω̃_j²·λ_j); N_Ent =
//  exp(−Σ_i p_i·ln p_i) with the 0·ln0 = 0 guard. Empty / degenerate Ω (Σ ω̃²λ ≤ 0)
//  ⇒ 0 (no measurable diversification). Order-fixed ascending i.
// ===========================================================================
[[nodiscard]] atx::f64 meucci_effective_bets(const atx::core::linalg::MatX &omega,
                                             const std::vector<atx::f64> &c) {
  const auto s = static_cast<atx::usize>(omega.rows());
  if (s == 0U || c.size() != s) {
    return 0.0; // empty/degenerate Ω ⇒ no measurable diversification (A5 guard)
  }
  Eigen::SelfAdjointEigenSolver<atx::core::linalg::MatX> es(omega);
  if (es.info() != Eigen::Success) {
    return 0.0; // a PCA failure is degenerate ⇒ 0 (never throws into the report)
  }
  const atx::core::linalg::MatX &E = es.eigenvectors(); // columns are principal directions
  const atx::core::linalg::VecX &lam = es.eigenvalues();

  // Principal weights ω̃ = Eᵀc, then the per-axis variance share ω̃_i²·λ_i (order-fixed).
  std::vector<atx::f64> share(s, 0.0);
  atx::f64 total = 0.0;
  for (atx::usize i = 0U; i < s; ++i) {
    atx::f64 wt = 0.0;
    for (atx::usize j = 0U; j < s; ++j) { // (Eᵀc)_i = Σ_j E(j,i)·c[j]
      wt += E(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) * c[j];
    }
    const atx::f64 li = lam[static_cast<Eigen::Index>(i)];
    const atx::f64 axis = wt * wt * ((li > 0.0) ? li : 0.0); // clamp tiny-negative λ to 0
    share[i] = axis;
    total += axis;
  }
  if (!(total > 0.0)) {
    return 0.0; // all-zero c / zero-variance Ω ⇒ no diversification signal
  }

  // N_Ent = exp(−Σ_i p_i ln p_i), p_i = share_i / total (0·ln0 = 0 guarded).
  atx::f64 entropy = 0.0;
  for (atx::usize i = 0U; i < s; ++i) {
    const atx::f64 p = share[i] / total;
    if (p > 0.0) {
      entropy -= p * std::log(p);
    }
  }
  return std::exp(entropy);
}

// ===========================================================================
//  compute_equity_curve — Π_p (1 + r_fund[p]) per period (compounded growth, documented).
//
//  r_fund[0] is a REAL period-0 return (NOT the combine structural zero) so it compounds.
// ===========================================================================
[[nodiscard]] std::vector<atx::f64> compute_equity_curve(const std::vector<atx::f64> &r_fund) {
  std::vector<atx::f64> eq(r_fund.size(), 0.0);
  atx::f64 cum = 1.0;
  for (atx::usize p = 0U; p < r_fund.size(); ++p) {
    cum *= (1.0 + r_fund[p]);
    eq[p] = cum;
  }
  return eq;
}

// ===========================================================================
//  flatten_books — the fund_book schedule period-major → one flat span for compute_metrics.
//
//  compute_metrics's turnover term reads positions period-major then instrument-minor
//  (length n_periods·M); we hand it the realized fund books exactly that way.
// ===========================================================================
[[nodiscard]] std::vector<atx::f64>
flatten_books(const std::vector<std::vector<atx::f64>> &books, atx::usize m) {
  std::vector<atx::f64> flat;
  flat.reserve(books.size() * m);
  for (const std::vector<atx::f64> &row : books) {
    for (const atx::f64 v : row) {
      flat.push_back(v);
    }
  }
  return flat;
}

} // namespace

// ---------------------------------------------------------------------------
//  PASS 1 (§0.7, net-after-optimize) — run each sleeve INDEPENDENTLY and causally.
//
//  Each Sleeve::run is a self-contained S1 MultiHorizonOptimizer walk over its own
//  scripted sources (sources_at(j, ·)); the sleeves are blind to one another. Errors
//  propagate. Then realized P&L r_s is formed per sleeve from returns_at (S2-5).
// ---------------------------------------------------------------------------
namespace {
[[nodiscard]] atx::core::Result<void>
run_pass1(const MetaBook &mb, const risk::RebalanceSchedule &sched,
          const std::function<risk::HorizonSources(atx::usize, atx::usize)> &sources_at,
          const std::function<const risk::FactorModel &(atx::usize)> &model_at,
          const std::function<std::span<const atx::f64>(atx::usize)> &returns_at,
          const book::CostInputs &cost, MetaBookResult &out,
          std::vector<std::vector<atx::f64>> &sleeve_pnl) {
  out.sleeve_results.reserve(mb.sleeves.size());
  sleeve_pnl.reserve(mb.sleeves.size());
  for (atx::usize j = 0U; j < mb.sleeves.size(); ++j) {
    // Bind sleeve j's per-period sources; the sleeve is causal and blind to the rest.
    const auto sj = [&, j](atx::usize p) { return sources_at(j, p); };
    ATX_TRY(risk::MultiHorizonResult sr, mb.sleeves[j].run(sched, sj, model_at, cost));
    sleeve_pnl.push_back(realized_sleeve_pnl(sr, returns_at, sched));
    out.sleeve_results.push_back(std::move(sr));
  }
  return atx::core::Ok();
}
} // namespace

// ---------------------------------------------------------------------------
//  PASS 2 (the fund overlay) — trailing allocate (R2), net, accrue the report rows.
//
//  For each period s ascending: build the TRAILING Ω (P&L strictly before s), allocate
//  capital, gather the period-s sleeve books, net them into the fund book (deltas vs the
//  s−1 sleeve books), and record the per-period report rows. prev_books carries the
//  period-(s−1) sleeve books forward (empty at s=0 ⇒ first move from flat).
// ---------------------------------------------------------------------------
namespace {
[[nodiscard]] atx::core::Result<void>
run_pass2(const MetaBook &mb, const risk::RebalanceSchedule &sched, const book::CostInputs &cost,
          const std::vector<std::vector<atx::f64>> &sleeve_pnl, MetaBookResult &out,
          atx::core::linalg::MatX &final_omega, std::vector<atx::f64> &caps) {
  const atx::usize n = sched.periods.size();
  out.fund_books.reserve(n);
  out.capital.reserve(n);
  out.report.gross_leverage.reserve(n);
  out.report.net_exposure.reserve(n);
  out.report.turnover_net.reserve(n);
  out.report.turnover_gross.reserve(n);
  out.report.crossing_benefit_bps.reserve(n);

  std::vector<std::vector<atx::f64>> prev_books; // sleeve books at s−1 (empty ⇒ flat, s=0)
  for (atx::usize s = 0U; s < n; ++s) {
    // (1) TRAILING Ω over P&L strictly before s (R2) → per-sleeve vols.
    ATX_TRY(atx::core::linalg::MatX omega,
            build_trailing_omega(sleeve_pnl, s, mb.cfg.risk_lookback));
    const std::vector<atx::f64> sleeve_vol = diag_sqrt(omega);
    final_omega = omega; // the last assignment is the FINAL period's Ω (report effective_bets)

    // (2) allocate capital from the trailing budget (degenerate Ω ⇒ fallback, §0.8).
    ATX_TRY(CapitalWeights cw,
            MetaAllocator{mb.cfg.alloc}.allocate(
                omega, std::span<const atx::f64>(sleeve_vol), std::span<const atx::f64>(caps)));

    // (3) gather the period-s sleeve books and net them into ONE fund book.
    std::vector<std::vector<atx::f64>> books_s;
    books_s.reserve(mb.sleeves.size());
    for (atx::usize j = 0U; j < mb.sleeves.size(); ++j) {
      books_s.push_back(out.sleeve_results[j].books[s]);
    }
    const std::vector<std::span<const atx::f64>> cur_spans = to_spans(books_s);
    const std::vector<std::span<const atx::f64>> prev_spans = to_spans(prev_books);
    ATX_TRY(NetResult nr,
            net_fund_book(std::span<const std::span<const atx::f64>>(cur_spans),
                          std::span<const std::span<const atx::f64>>(prev_spans),
                          std::span<const atx::f64>(cw.c), cost));

    // (4) record the per-period schedule + report rows.
    out.report.gross_leverage.push_back(l1_abs(nr.fund_book));
    out.report.net_exposure.push_back(signed_sum(nr.fund_book));
    out.report.turnover_net.push_back(nr.turnover_net);
    out.report.turnover_gross.push_back(nr.turnover_gross);
    out.report.crossing_benefit_bps.push_back(nr.crossing_benefit_bps);
    out.fund_books.push_back(std::move(nr.fund_book));
    out.capital.push_back(std::move(cw));
    prev_books = std::move(books_s); // carry the period-s sleeve books forward to s+1
  }
  return atx::core::Ok();
}
} // namespace

// ---------------------------------------------------------------------------
//  Attribution (R4) — return / risk / crossing, each SUMMING to the fund total.
//
//  return_contrib[s] = Σ_p c[p][s]·r_s[p]   (Σ_s == R_fund = Σ_p r_fund[p], exact/linear).
//  risk_contrib[s]   = the Euler RC from a REPRESENTATIVE Ω,c: the full-sample Ω (over the
//    WHOLE realized P&L panel) + the FINAL period's c, via fund_risk (S2-3); Σ_s == sqrt
//    (cᵀΩc) exactly (documented choice).
//  crossing_credit[s] = total crossing benefit pro-rata by the gross volume sleeve s
//    contributed to trading (Σ_p c[p][s]·Σ_i |Δw_{s,i}[p]|); Σ_s == total benefit.
// ---------------------------------------------------------------------------
namespace {
// Σ_p c[p][s]·r_s[p] — the realized return sleeve s contributed (linear; R4 additivity).
[[nodiscard]] std::vector<atx::f64>
return_contributions(const std::vector<std::vector<atx::f64>> &sleeve_pnl,
                     const std::vector<CapitalWeights> &capital) {
  const atx::usize S = sleeve_pnl.size();
  std::vector<atx::f64> rc(S, 0.0);
  for (atx::usize s = 0U; s < S; ++s) {
    atx::f64 acc = 0.0;
    for (atx::usize p = 0U; p < capital.size(); ++p) {
      acc += capital[p].c[s] * sleeve_pnl[s][p];
    }
    rc[s] = acc;
  }
  return rc;
}

// The per-sleeve gross trading volume credited pro-rata the total crossing benefit. For
// sleeve s: Σ_p c[p][s]·Σ_i |w_{s,i}[p] − w_{s,i}[p−1]| (the gross it pushed into netting,
// weighted by its capital). The total benefit is split by this volume; an all-zero volume
// ⇒ equal split (defensible, avoids 0/0). Σ_s crossing_credit == total benefit (R4).
[[nodiscard]] std::vector<atx::f64>
crossing_credits(const MetaBookResult &out, const std::vector<CapitalWeights> &capital) {
  const atx::usize S = out.sleeve_results.size();
  const atx::usize n = capital.size();
  std::vector<atx::f64> vol(S, 0.0);
  atx::f64 vol_total = 0.0;
  for (atx::usize s = 0U; s < S; ++s) {
    const std::vector<std::vector<atx::f64>> &books = out.sleeve_results[s].books;
    atx::f64 acc = 0.0;
    for (atx::usize p = 0U; p < n; ++p) {
      atx::f64 g = 0.0;
      const std::vector<atx::f64> &cur = books[p];
      for (atx::usize i = 0U; i < cur.size(); ++i) {
        const atx::f64 prev = (p == 0U) ? 0.0 : books[p - 1U][i];
        g += std::fabs(cur[i] - prev);
      }
      acc += capital[p].c[s] * g; // capital-weighted gross volume (what hit netting)
    }
    vol[s] = acc;
    vol_total += acc;
  }
  atx::f64 total_benefit = 0.0;
  for (const atx::f64 b : out.report.crossing_benefit_bps) {
    total_benefit += b;
  }
  std::vector<atx::f64> credit(S, 0.0);
  if (S == 0U) {
    return credit;
  }
  if (!(vol_total > 0.0)) { // no measurable trading volume ⇒ equal split (Σ preserved)
    const atx::f64 eq = total_benefit / static_cast<atx::f64>(S);
    for (atx::usize s = 0U; s < S; ++s) {
      credit[s] = eq;
    }
    return credit;
  }
  for (atx::usize s = 0U; s < S; ++s) {
    credit[s] = total_benefit * (vol[s] / vol_total); // pro-rata by contributed volume
  }
  return credit;
}

// The representative Euler risk_contrib: full-sample Ω over the whole P&L panel + final c.
[[nodiscard]] atx::core::Result<std::vector<atx::f64>>
representative_risk_contrib(const std::vector<std::vector<atx::f64>> &sleeve_pnl,
                            const std::vector<CapitalWeights> &capital,
                            const risk::FactorModel &V_final) {
  const atx::usize S = sleeve_pnl.size();
  // Full-sample Ω over the entire realized P&L panel (a representative covariance, R4).
  std::vector<std::span<const atx::f64>> panel;
  panel.reserve(S);
  for (const std::vector<atx::f64> &r : sleeve_pnl) {
    panel.emplace_back(r);
  }
  ATX_TRY(atx::core::linalg::MatX omega_full,
          sleeve_return_cov(std::span<const std::span<const atx::f64>>(panel)));

  // fund_risk needs the per-sleeve BOOKS (length M) for its V-based fields; the Euler RC
  // it returns depends ONLY on (Ω, c), so we pass any consistent same-period sleeve books.
  // Use the final period's sleeve books so M / shapes are valid. c = the final period's c.
  const std::vector<atx::f64> &c_final = capital.back().c;
  const atx::usize m = V_final.n_instruments();
  std::vector<std::vector<atx::f64>> dummy(S, std::vector<atx::f64>(m, 0.0));
  // A zero book is shape-valid; fund_risk's RC is over (Ω,c) only, so the book content
  // does not affect risk_contrib — but we still must satisfy the M-length boundary check.
  const std::vector<std::span<const atx::f64>> book_spans = to_spans(dummy);
  ATX_TRY(FundRisk fr, fund_risk(std::span<const std::span<const atx::f64>>(book_spans),
                                 std::span<const atx::f64>(c_final), V_final, omega_full));
  return atx::core::Ok(std::move(fr.risk_contrib));
}
} // namespace

// ---------------------------------------------------------------------------
//  MetaBook::run — the two-pass driver entry point (orchestration only; §4.5).
// ---------------------------------------------------------------------------
atx::core::Result<MetaBookResult> MetaBook::run(
    const risk::RebalanceSchedule &sched,
    const std::function<risk::HorizonSources(atx::usize, atx::usize)> &sources_at,
    const std::function<const risk::FactorModel &(atx::usize)> &model_at,
    const std::function<std::span<const atx::f64>(atx::usize)> &returns_at,
    const book::CostInputs &cost) const {
  namespace co = atx::core;

  // --- boundary validation ---
  if (sleeves.empty()) {
    return co::Err(co::ErrorCode::InvalidArgument, "MetaBook::run: sleeves must be non-empty");
  }
  if (!sources_at || !model_at || !returns_at) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "MetaBook::run: sources_at / model_at / returns_at callbacks must be present");
  }

  MetaBookResult out;
  // Degenerate (NOT an error): an empty schedule ⇒ Ok with empty result (still run pass-1
  // to populate the per-sleeve results, which are themselves empty over no periods).
  if (sched.periods.empty()) {
    std::vector<std::vector<atx::f64>> sleeve_pnl;
    ATX_TRY_VOID(run_pass1(*this, sched, sources_at, model_at, returns_at, cost, out, sleeve_pnl));
    return co::Ok(std::move(out));
  }

  // --- PASS 1: independent, causal per-sleeve walks + realized P&L (S2-5) ---
  std::vector<std::vector<atx::f64>> sleeve_pnl;
  ATX_TRY_VOID(run_pass1(*this, sched, sources_at, model_at, returns_at, cost, out, sleeve_pnl));

  // Per-sleeve capacity box (from each sleeve's cfg.capacity_gross) — the allocator's caps.
  std::vector<atx::f64> caps;
  caps.reserve(sleeves.size());
  for (const Sleeve &sl : sleeves) {
    caps.push_back(sl.cfg.capacity_gross);
  }

  // --- PASS 2: trailing allocate (R2) + net + per-period report rows ---
  atx::core::linalg::MatX final_omega(0, 0);
  ATX_TRY_VOID(run_pass2(*this, sched, cost, sleeve_pnl, out, final_omega, caps));

  // --- report assembly ---
  const atx::usize n = sched.periods.size();
  const risk::FactorModel &V_final = model_at(sched.periods[n - 1U]);
  const atx::usize m = V_final.n_instruments();

  // r_fund[p] = Σ_i fund_book[p][i]·returns_at(p)[i] (= Σ_s c[p][s]·r_s[p], linear).
  std::vector<atx::f64> r_fund(n, 0.0);
  for (atx::usize p = 0U; p < n; ++p) {
    const std::span<const atx::f64> ret = returns_at(sched.periods[p]);
    const std::vector<atx::f64> &fb = out.fund_books[p];
    atx::f64 acc = 0.0;
    const atx::usize mm = (fb.size() < ret.size()) ? fb.size() : ret.size();
    for (atx::usize i = 0U; i < mm; ++i) {
      acc += fb[i] * ret[i];
    }
    r_fund[p] = acc;
  }

  out.report.equity_curve = compute_equity_curve(r_fund);
  // The ONE fund Sharpe: compute_metrics over r_fund + the flattened fund_book schedule.
  // DOCUMENTED: compute_metrics excludes index 0 as a structural zero (combine convention).
  const std::vector<atx::f64> flat = flatten_books(out.fund_books, m);
  out.report.fund_metrics = combine::compute_metrics(std::span<const atx::f64>(r_fund),
                                                     std::span<const atx::f64>(flat), m, 1.0);
  out.report.effective_bets = meucci_effective_bets(final_omega, out.capital.back().c);

  // Attribution (R4) — each vector SUMS to the fund total.
  out.report.attribution.return_contrib = return_contributions(sleeve_pnl, out.capital);
  ATX_TRY(std::vector<atx::f64> risk_rc,
          representative_risk_contrib(sleeve_pnl, out.capital, V_final));
  out.report.attribution.risk_contrib = std::move(risk_rc);
  out.report.attribution.crossing_credit = crossing_credits(out, out.capital);

  return co::Ok(std::move(out));
}

} // namespace atx::engine::fund
