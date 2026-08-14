#pragma once

// Option-chain PANEL fixtures for the Vola-parity harness.
//
// The atx-vol repo commits NO option-chain data, and the SpiderRock Parquet
// loader (`load_spiderrock_parquet` in data.hpp) is deliberately deferred /
// NotImplemented (a byte-level Thrift re-decode is incompatible with the atx
// house rules, and no `.parquet` fixture is committed). This module supplies
// the two fixture paths the parity harness actually uses instead:
//
//   (a) a deterministic KNOWN-TRUTH synthetic American-equity option-chain
//       generator (`make_synthetic_american_panel`), and
//   (b) a self-contained CSV chain loader (`load_chain_csv`) — no Arrow /
//       Parquet dependency.
//
// Both build a `QuoteFrame` of the exact shape `data_install` consumes (see
// data.hpp and the canonical in-memory builder in tests/data_test.cpp).
//
// ## Known-truth construction (the q_eff bridge)
//
// For each expiry the "truth" is an arbitrage-free S3/SSVI smile in
// European-equivalent vols (s3.hpp), anchored on a forward built by the hybrid
// dividend model (dividend.hpp). The American mid prices are produced by the
// American pricer (american.hpp), which parameterises dynamics by a CONTINUOUS
// dividend yield `q` (its forward is F_pricer = S * e^{(r - q) * T}). To make
// the pricer's forward equal the hybrid forward F we solve for the effective
// yield
//
//     q_eff = r - log(F / spot) / T,
//
// so F_pricer == F bit-for-bit and the emitted American mid is priced against
// exactly the forward the harness re-derives from `hybrid_forward`. The truth
// vol carried per row is the European-equivalent `s3_iv(log(K/F), T, truth)`.
//
// ## Determinism
//
// The generator uses NO randomness — it is a clean, reproducible fixture. Two
// calls with the same `SynthPanelSpec` produce byte-identical output. (If quote
// noise is ever added it must take an explicit `uint64_t` seed and use
// `std::mt19937_64`, never a time-based source; the default remains no noise.)
//
// ## Ownership / thread-safety
//
// Every type here owns its storage by value (RAII, Rule of Zero). Both entry
// points are pure functions of their inputs (the CSV loader additionally reads
// the named file) — safe to call concurrently on distinct arguments.

#include <string>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"    // american_price, AmericanMethod
#include "atx/vol/api/marketdata/data.hpp"        // QuoteFrame, QuoteRow, iso_to_ns, build_expiry_inputs
#include "atx/vol/api/pricing/dividend.hpp"    // hybrid_forward, HybridDivParams
#include "atx/vol/api/pricing/rates_curve.hpp" // DividendEvent
#include "atx/vol/api/storage/s3.hpp"          // S3Params, s3_iv
#include "atx/vol/api/core/types.hpp"       // Result, Side

namespace atx::vol {

// ── (a) Synthetic known-truth panel ─────────────────────────────────────────

// One expiry's truth definition: its ISO date, its year-fraction, and the
// arbitrage-free S3/SSVI smile (in European-equivalent vols) that defines the
// truth IV at every strike. `truth.sigma0` is the ATF (at-the-forward) vol and
// is also stamped as the per-expiry source ATM vol.
struct SynthExpiry {
  std::string expiry_iso; // "YYYY-MM-DD"
  double T = 0.0;         // year-fraction to expiry (> 0)
  S3Params truth{};       // truth smile params for this expiry
};

// Full specification for a synthetic American-equity option chain. `strikes`
// are ABSOLUTE strike prices, shared across every expiry.
struct SynthPanelSpec {
  std::string uid = "SYNTH";
  std::string snapshot_iso = "2026-06-19";
  double spot = 100.0;
  double r = 0.05;    // continuously-compounded flat rate
  double borrow = 0.0; // continuous borrow cost (> 0 lowers the forward)
  HybridDivParams hyb{};                // dividend model (default: pure escrowed cash)
  std::vector<DividendEvent> cash_divs; // discrete cash dividends
  std::vector<SynthExpiry> expiries;
  std::vector<double> strikes;          // absolute strikes, shared across expiries
  double half_spread_frac = 0.01;       // bid-ask half width as a fraction of mid
  double min_half_spread = 0.02;        // absolute floor on the half-spread (price units)
  AmericanMethod method = AmericanMethod::AndersenLake;
};

// The generated panel: a `QuoteFrame` ready for `data_install`, plus the truth
// the parity harness checks fitted output against.
struct SynthPanel {
  QuoteFrame frame; // ready for data_install

  // Per emitted row (frame.rows order: expiry-major, then strike, then
  // side = {Call, Put}) the European-equivalent truth vol used to price it.
  std::vector<double> truth_iv;

  // Per expiry (spec.expiries order) the hybrid forward used to build q_eff.
  std::vector<double> truth_forward;
};

// Build a deterministic known-truth synthetic American-equity option chain.
//
// For each expiry the hybrid forward F = hybrid_forward(spot, r, borrow, T,
// cash_divs, expiry, snapshot, hyb) is computed; then for each strike K and
// BOTH sides:
//   truth_iv = s3_iv(log(K / F), T, truth);
//   mid      = american_price(spot, K, T, truth_iv, r, q_eff, side, method)
//              with q_eff = r - log(F / spot) / T  (see header: the q_eff bridge);
//   hw       = max(min_half_spread, half_spread_frac * mid);
//   bid      = max(0, mid - hw),  ask = mid + hw.
// Each becomes a `QuoteRow` in `frame.rows`. The frame's spot, snapshot,
// flat-r yield-curve pillars, dividend schedule, and per-(uid, expiry)
// source-input table are all populated so the frame installs directly.
//
// @return InvalidArgument if spot/strike/expiry-T is non-positive or non-finite,
//         a smile sigma0 is non-positive, an ISO date fails to parse, or the
//         expiry/strike lists are empty.
// @return Internal if a truth vol or American mid comes out non-finite/non-positive.
// @return the American pricer's own error, propagated, if a price fails.
[[nodiscard]] Result<SynthPanel> make_synthetic_american_panel(const SynthPanelSpec& spec);

// ── (b) CSV chain loader (self-contained; no Arrow/Parquet) ──────────────────
//
// Documented header (the first data line whose first field is "uid" — any case
// — is skipped as a header; blank lines and lines starting with '#' are ignored):
//
//   uid,snapshot_iso,spot,expiry_iso,strike,side,bid,ask,bid_size,ask_size,under_spot
//
// with two OPTIONAL trailing columns, in this order, appended per row:
//
//   [,rate[,ddiv]]
//
// so each data row carries 11, 12, or 13 comma-separated fields. `side` is
// "C"/"P" or "Call"/"Put" (case-insensitive). `rate` populates
// `QuoteRow::rate_source`; `ddiv` populates `QuoteRow::ddiv_source` (an empty
// optional field is treated as absent). The FIRST data row's `snapshot_iso` and
// `spot` set the frame-level `snapshot_iso` / `snapshot_ts_ns` / `spot` /
// `spot_ts_ns`. Whitespace around fields is trimmed. Embedded commas / quoted
// fields are not supported (fixture format only).
//
// The yield-curve pillars come from the spec (not the CSV); supply non-empty
// pillars if the resulting frame is to be installed via `data_install`. When any
// row carries a rate/ddiv, the per-(uid, expiry) source-input table is built.
struct CsvChainSpec {
  std::string path;
  std::vector<double> yc_pillar_t; // yield-curve pillar year-fractions
  std::vector<double> yc_pillar_r; // yield-curve pillar zero rates
};

// Load a CSV option chain into a `QuoteFrame` (see the header format above).
//
// @return IoError    if the file cannot be opened.
// @return ParseError on a malformed row (wrong column count, a non-numeric
//         numeric field, an unrecognised side, or an empty uid), naming the
//         1-based line number.
[[nodiscard]] Result<QuoteFrame> load_chain_csv(const CsvChainSpec& spec);

} // namespace atx::vol
