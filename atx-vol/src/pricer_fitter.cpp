#include "atx/vol/pricer_fitter.hpp"

#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american_iv.hpp"  // american_implied_vol
#include "atx/vol/correction.hpp"   // AmericanCorrectionCaches (cached inversion hot path)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Deterministic block-partitioned fan-out over [0, n): each worker owns a
// contiguous index range and writes only its own output slots, so the result is
// identical for any thread count (the calibrate_pool determinism pattern). 0 =>
// hardware_concurrency; 1 => serial. `fn` must be safe to call concurrently over
// disjoint indices (pure const reads + disjoint writes).
template <class F>
void parallel_for(std::size_t n, unsigned n_threads, F&& fn) {
  if (n == 0) {
    return;
  }
  unsigned nt = n_threads;
  if (nt == 0) {
    nt = std::thread::hardware_concurrency();
    if (nt == 0) {
      nt = 1u;
    }
  }
  if (nt > n) {
    nt = static_cast<unsigned>(n);
  }
  if (nt <= 1u) {
    for (std::size_t i = 0; i < n; ++i) {
      fn(i);
    }
    return;
  }
  const std::size_t chunk = (n + nt - 1u) / nt;
  std::vector<std::jthread> workers;
  workers.reserve(nt);
  for (unsigned t = 0; t < nt; ++t) {
    const std::size_t lo = static_cast<std::size_t>(t) * chunk;
    if (lo >= n) {
      break;
    }
    const std::size_t hi = (lo + chunk < n) ? (lo + chunk) : n;
    workers.emplace_back([lo, hi, &fn] {
      for (std::size_t i = lo; i < hi; ++i) {
        fn(i);
      }
    });
  }
  // std::jthread joins on destruction — the loop below (scope exit) is the
  // barrier; every worker has finished before value_chain returns.
}

}  // namespace

std::optional<std::size_t> ChainValuation::row_of(OptionId id) const {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) {
      return i;
    }
  }
  return std::nullopt;
}

Status PricerFitter::fit(const OptionChain& chain) {
  SessionInputs in =
      make_session_inputs(cfg_.preset, chain.spot(), chain.rate(), chain.now_ns());
  if (!cfg_.cash_divs.empty()) {
    in.cash_divs = cfg_.cash_divs;
  }
  ATX_TRY(VolaSession sess, VolaSession::build(chain.underlying(), in));
  // FittedSurface's ctor is private (friend PricerFitter), so make_unique cannot
  // reach it — construct explicitly.
  surface_.reset(new FittedSurface(std::move(sess)));
  return Ok();
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain& chain,
                                                 OutputField fields,
                                                 unsigned n_threads) const {
  if (surface_ == nullptr) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::value_chain: no fitted surface; call fit() first");
  }
  const VolaSession& sess = surface_->session();
  const double S = chain.spot();
  const double r = chain.rate();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  ChainValuation val;
  val.ids = chain.ids();
  val.filled = fields;
  const std::size_t n = val.ids.size();

  if (has(fields, OutputField::ModelPrice)) {
    val.model_price.assign(n, nan);
  }
  if (has(fields, OutputField::ModelIV)) {
    val.model_iv.assign(n, nan);
  }
  if (has(fields, OutputField::BidIV)) {
    val.bid_iv.assign(n, nan);
  }
  if (has(fields, OutputField::AskIV)) {
    val.ask_iv.assign(n, nan);
  }
  if (has(fields, OutputField::MidIV)) {
    val.mid_iv.assign(n, nan);
  }
  if (has(fields, OutputField::Greeks)) {
    val.greeks.assign(n, AmericanGreeks{});
  }

  // Single-thread decode pass (cheap; keeps per-worker hot loops free of the
  // repeated underlying lookup `at` would do).
  std::vector<OptionRef> refs;
  refs.reserve(n);
  for (const OptionId id : val.ids) {
    const auto ref = chain.at(id);
    refs.push_back(ref.has_value() ? *ref : OptionRef{});
  }

  const unsigned nt = n_threads ? n_threads : cfg_.n_threads;
  const bool want_bands = has(fields, OutputField::BidIV) ||
                          has(fields, OutputField::AskIV) ||
                          has(fields, OutputField::MidIV);

  // The per-side correction caches the fit built. Routing the bid/ask/mid IV
  // inversions through them replaces the cold per-residual Andersen-Lake solve
  // (12 BAW root-finds + sweeps + quadrature + cold polish) with the cached hot
  // path (Black-76 + one Chebyshev evaluation) — the SOTA American-IV method (a
  // fast surrogate in the root-find, not a pricer). A null cache for a side
  // transparently falls back to the cold path (bit-identical, just slower).
  const AmericanCorrectionCaches caches = sess.correction_caches();

  const auto eval = [&](std::size_t i) {
    const OptionRef& o = refs[i];
    if (!(o.strike > 0.0) || !(o.T > 0.0)) {
      return;  // decode failed or degenerate expiry — leave the row NaN
    }
    const double q = sess.q_eff_at(o.T);
    if (has(fields, OutputField::ModelIV)) {
      val.model_iv[i] = sess.iv(o.strike, o.T);
    }
    if (has(fields, OutputField::ModelPrice)) {
      const auto fv = sess.fair_value(o.strike, o.T, o.side);
      val.model_price[i] = fv.has_value() ? *fv : nan;
    }
    if (has(fields, OutputField::Greeks)) {
      const auto g = sess.greeks(o.strike, o.T, o.side);
      if (g.has_value()) {
        val.greeks[i] = *g;
      } else {
        val.greeks[i].price = nan;
      }
    }
    if (!want_bands) {
      return;
    }
    // Parallel American-IV band inversions through the cached hot path. The
    // surface's own IV at (K, T) seeds all three (bid/ask/mid vols sit within a
    // spread's width of it), so each is 1-2 Newton steps.
    const CorrectionCache* cc = caches.for_side(o.side);
    const double miv = sess.iv(o.strike, o.T);
    const double ws = (std::isfinite(miv) && miv > 0.0) ? miv : 0.0;
    const auto invert = [&](double px) {
      return american_implied_vol(px, S, o.strike, o.T, r, q, o.side,
                                  AmericanMethod::AndersenLake, 1.0e-7, 64,
                                  std::nullopt, cc, ws);
    };
    if (has(fields, OutputField::BidIV) && o.bid > 0.0) {
      const auto iv = invert(o.bid);
      val.bid_iv[i] = iv.has_value() ? *iv : nan;
    }
    if (has(fields, OutputField::AskIV) && o.ask > 0.0) {
      const auto iv = invert(o.ask);
      val.ask_iv[i] = iv.has_value() ? *iv : nan;
    }
    if (has(fields, OutputField::MidIV) && o.mid > 0.0) {
      const auto iv = invert(o.mid);
      val.mid_iv[i] = iv.has_value() ? *iv : nan;
    }
  };

  parallel_for(n, nt, eval);
  return Ok(std::move(val));
}

}  // namespace atx::vol
