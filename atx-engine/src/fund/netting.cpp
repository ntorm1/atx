// netting.cpp — P2-S2-4 private implementation (USER DIRECTIVE: the unit's numeric
// kernel lives in the .cpp, not the header; .agents/cpp §6).
//
// Defines net_fund_book, plus the anonymous-namespace shape-validation helper. The
// single compute pass is ascending name (outer) then ascending sleeve (inner), so the
// accumulation order is fixed for byte-reproducibility (R1); no RNG, no clock, no
// unordered containers. It consults only the current + prior sleeve books passed in —
// a same-timestamp aggregation of already-known targets, structurally PIT-safe (§0.9, R2).

#include "atx/engine/fund/netting.hpp"

#include <cmath>   // std::fabs
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

namespace atx::engine::fund {

namespace {

// ---------------------------------------------------------------------------
//  validate_shapes — boundary checks for net_fund_book (run BEFORE compute).
//
//  Returns a Status: Ok when shapes are consistent, else Err(InvalidArgument). The
//  caller has already established S == c.size(); this checks sleeve_books matches S,
//  every book is length M, and sleeve_prev is EITHER empty OR S spans each of length M.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::core::Status
validate_shapes(std::span<const std::span<const atx::f64>> sleeve_books,
                std::span<const std::span<const atx::f64>> sleeve_prev, atx::usize s,
                atx::usize m) {
  if (sleeve_books.size() != s) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "net_fund_book: sleeve_books.size() must equal c.size() (S)");
  }
  for (atx::usize i = 0U; i < s; ++i) {
    if (sleeve_books[i].size() != m) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "net_fund_book: every sleeve_books[s] must have the same length M");
    }
  }
  // sleeve_prev: EMPTY ⇒ "first move from flat" (all prev = 0); else it must mirror the
  // sleeve_books shape exactly (S spans, each length M) so the per-name delta is defined.
  if (!sleeve_prev.empty()) {
    if (sleeve_prev.size() != s) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "net_fund_book: non-empty sleeve_prev.size() must equal S");
    }
    for (atx::usize i = 0U; i < s; ++i) {
      if (sleeve_prev[i].size() != m) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "net_fund_book: every sleeve_prev[s] must have length M");
      }
    }
  }
  return atx::core::Ok();
}

} // namespace

// ---------------------------------------------------------------------------
//  net_fund_book — the public entry point (the honest crossing measurement, §4.4).
// ---------------------------------------------------------------------------
atx::core::Result<NetResult>
net_fund_book(std::span<const std::span<const atx::f64>> sleeve_books,
              std::span<const std::span<const atx::f64>> sleeve_prev,
              std::span<const atx::f64> c, const book::CostInputs& cost) {
  const atx::usize s = c.size();
  // M is the common book length; with no sleeves there is no universe ⇒ M = 0.
  const atx::usize m = sleeve_books.empty() ? 0U : sleeve_books[0].size();

  ATX_TRY_VOID(validate_shapes(sleeve_books, sleeve_prev, s, m));

  // An empty sleeve_prev ⇒ every prior weight is 0 (first move from a flat book).
  const bool flat_prev = sleeve_prev.empty();

  NetResult out;
  out.fund_book.assign(m, 0.0);

  atx::f64 t_gross = 0.0; // Σ_i Σ_s |c_s·Δw_{s,i}| (sleeves traded separately)
  atx::f64 t_net = 0.0;   // Σ_i |Σ_s c_s·Δw_{s,i}| (the netted fund trade)

  // Ascending name i (outer), ascending sleeve s (inner) — order-fixed for determinism
  // (R1). Per name: net_delta sums the SIGNED scaled deltas (offsetting flow cancels =
  // internal crossing); gross_delta sums their MAGNITUDES (no crossing). |net| ≤ gross is
  // the triangle inequality, so t_net ≤ t_gross by construction (R3).
  for (atx::usize i = 0U; i < m; ++i) {
    atx::f64 net_delta = 0.0;
    atx::f64 gross_delta = 0.0;
    atx::f64 w_i = 0.0;
    for (atx::usize si = 0U; si < s; ++si) {
      const atx::f64 prev = flat_prev ? 0.0 : sleeve_prev[si][i];
      const atx::f64 d = c[si] * (sleeve_books[si][i] - prev);
      net_delta += d;
      gross_delta += std::fabs(d);
      w_i += c[si] * sleeve_books[si][i];
    }
    out.fund_book[i] = w_i;
    t_net += std::fabs(net_delta);
    t_gross += gross_delta;
  }

  out.turnover_gross = t_gross;
  out.turnover_net = t_net; // ≤ t_gross by the triangle inequality (asserted invariant, R3)
  // Price the saving through the SAME calibrated cost field the sleeves use. ≥ 0 since
  // (t_gross − t_net) ≥ 0 and round_trip_cost_bps ≥ 0 (R3).
  out.crossing_benefit_bps = (t_gross - t_net) * cost.round_trip_cost_bps;
  // The internal-cross rate ∈ [0,1]; 0 when there was no turnover at all (guarded).
  out.crossed_fraction = (t_gross > 0.0) ? (t_gross - t_net) / t_gross : 0.0;

  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::fund
