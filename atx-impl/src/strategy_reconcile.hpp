#pragma once

#include <iosfwd>
#include <string>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
// ---- broker reconciliation (v7 W4, review B8) ----
// Compares the book a prior decide expected after its orders filled (expected_holdings.csv,
// bound to that decide's decision.json and to the deploy manifest) with a broker positions
// file at the broker's as-of, after applying the corporate actions between the two
// sessions to the expected side. Plain CSV (comma-separated, no quoting); dates YYYY-MM-DD.
//
// Inputs
//   --deploy M     atx.book-deploy/v1; its SHA-256 must equal the decision's
//                  pins_verified.deploy_manifest_sha256 and its book the decision's book.
//   --expected P   a decide output directory, or its expected_holdings.csv (decision.json
//                  beside it): columns instrument_id, shares [, reference_price]; the file's
//                  SHA-256 must equal decision.json files["expected_holdings.csv"].
//   --broker B     columns shares and one key: instrument_id, else ticker, else cik (the
//                  first present). ticker/cik rows map to an instrument through --identity at
//                  the broker as-of; a row that maps to no instrument (or to several) is an
//                  unmapped break. An instrument listed twice is refused.
//   --asof D       the broker file's session (>= the decision's as-of).
//   --identity I   (optional) the identity bridge as CSV: instrument_id (or sr_id, the
//                  atx.identity-bridge/v1 column) with ticker and/or cik, optional start and
//                  end (or end_incl) inclusive dates, optional primary (P|J: on several
//                  matches the P line wins). A CSV export of the bridge's links.parquet is
//                  accepted as is.
//   --corporate-actions C  (optional) columns instrument_id|ticker|cik, date (or ex_date),
//                  ratio (new shares per old share on the ex-date: 2:1 split 2, 1:10 reverse
//                  split .1, 5% stock dividend 1.05; cash dividends change no share count and
//                  are not listed). Actions dated in (decision as-of, broker as-of] multiply
//                  the expected shares (in date order) and divide the reference price. A row
//                  that maps to no instrument is refused.
//   --tolerance-shares T (default .5), --tolerance-fraction F (default 0): a name matches
//                  when |broker - expected x ratio| <= max(T, F x |expected x ratio|).
// Statuses per instrument (ascending id): ok; explained (matches only after the corporate
// actions); missing (expected held, broker flat or absent); extra (broker held, expected
// flat or absent); quantity (both held, beyond tolerance, or an expected share count that is
// not finite); unmapped (a broker row without an instrument). Every status but ok and
// explained is an unexplained break.
// Output (--output NEWDIR, optional; reconcile.json LAST): reconciliation.csv
//   instrument_id,broker_key,status,expected_shares,ratio,expected_adjusted,broker_shares,
//   difference,reference_price,difference_notional
// and reconcile.json (atx.book-reconcile/v1: inputs with SHA-256, bindings, tolerance,
// counts by status).
inline constexpr const char* book_reconcile_schema = "atx.book-reconcile/v1";

struct ReconcileConfig {
  std::string deploy_path, expected_path, broker_path, asof;
  std::string identity_path, corporate_actions_path, output_directory; // optional
  atx::f64 tolerance_shares{0.5}, tolerance_fraction{0.0};
};
struct ReconcileOutcome {
  atx::usize compared{}, ok{}, explained{};
  atx::usize missing{}, extra{}, quantity{}, unmapped{};
  [[nodiscard]] atx::usize breaks() const noexcept { return missing + extra + quantity + unmapped; }
};
// Refusals (bad input, a binding that differs) are errors and write nothing; breaks are
// an outcome. `report` receives one summary line and one line per unexplained break.
[[nodiscard]] atx::core::Result<ReconcileOutcome> run_reconcile(const ReconcileConfig& cfg,
                                                                std::ostream& report);
// argv[0] is the "reconcile" verb. Exit codes: 0 reconciled (no unexplained break),
// 1 refused, 2 usage, 5 unexplained breaks.
[[nodiscard]] int dispatch_reconcile(int argc, char** argv, std::ostream& out,
                                     std::ostream& err);
} // namespace atx::impl::strategy
