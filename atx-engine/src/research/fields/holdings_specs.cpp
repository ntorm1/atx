// The declared specs of the holdings fields ported from research_fields_holdings.py (P9 lane
// A4): the text of each is its row of atx-engine/tools/field_registry.json (spec_text: units,
// clock, staleness, definition, source_columns; point_in_time, no non-PIT aspect, no domain) at
// revision 1, so formula_sha256(spec) is the row's formula_sha256 (gtest
// ResearchFieldsHoldingsFixture.FormulaFingerprintsAreTheRegistrys); the caveats are the Python
// HOLD_FIELDS' (the entry's caveats key). Generated once from those two sources and checked by the
// gtests; edit the registry row and the Python spec together, never this copy alone.
#include <string_view>

#include "atx/engine/research/fields/holdings_fields.hpp"

namespace atx::engine::research::fields {
namespace {

[[nodiscard]] FieldSpec make_ftd_shares_ratio21_spec() {
  FieldSpec s;
  s.name = "ftd_shares_ratio21";
  s.group = "ftd";
  s.formula_id = "sec-ftd-sum21-over-shares-v1";
  s.revision = 1;
  FieldDefinition &d = s.definition;
  d.units =
      "sum of daily fail balances / shares outstanding (decimal)";
  d.clock =
      "sec-ftd-latest-visible-21-v1: rows visible by available_at (stage "
      "ftd-publication-halfmonth-v1: nominal half-month publication + 7 d, or a later HTTP "
      "Last-Modified) < date(t-1) 22:00 UTC; the window is the 21 settlement dates ending at the "
      "latest prefix-visible settlement date S*(t)";
  d.staleness =
      "NaN when date(t) - S*(t) > 60 days (stage staleness), fewer than 21 settlement dates exist "
      "up to S*(t), or shares_out at the window's session is not finite and positive; session 0 -> "
      "NaN";
  d.definition =
      "sum over the 21 settlement dates ending at S*(t) of the SEC CNS fails-to-deliver quantity "
      "of the line (a settlement date without a row of the line counts 0: the SEC file lists every "
      "non-zero fail) / this run's shares_out at the last role session on or before S*(t); S*(t) = "
      "the latest settlement date such that every settlement date up to it is visible; settlement "
      "calendar = the distinct settlement dates of the stage. Formula id "
      "sec-ftd-sum21-over-shares-v1.";
  d.source_columns = {"settlement_date",
                      "security_id",
                      "quantity",
                      "available_at",
                      "map_basis",
                      "shares_out"};
  d.point_in_time = true;
  s.caveats = {
      "files are re-posted without revision history (vintage_risk on every row)",
      "rows the stage could not map (OTC and delisted symbols, collisions) count as no fail: "
      "96-98% of fail value maps",
      "two CUSIPs of one line on one date (CUSIP change) are summed"};
  return s;
}

[[nodiscard]] FieldSpec make_regsho_threshold_days63_spec() {
  FieldSpec s;
  s.name = "regsho_threshold_days63";
  s.group = "regsho";
  s.formula_id = "regsho-threshold-days63-listing-market-v1";
  s.revision = 1;
  FieldDefinition &d = s.definition;
  d.units =
      "count of sessions (0-63)";
  d.clock =
      "regsho-days63-t2-v1: list rows and list statuses visible by available_at (stage "
      "regsho-publication-v1) < date(t-1) 22:00 UTC; the window ends at session t-2, the newest "
      "session whose next-day file is visible before the t-1 mark; listing-market rows by their "
      "dissemination available_at under the same rule";
  d.staleness =
      "NaN when the listing market is unknown (no visible FINRA name row within 45 days, or a "
      "market without a list source such as IEX) or fewer than 60 of the market's 63 window lists "
      "are visible (this also enforces the stage's 10-day list staleness); session 0 -> NaN";
  d.definition =
      "number of the 63 sessions ending at t-2 (role calendar, extended before the role by the "
      "list dates) on which a visible Reg SHO threshold list carries the line on_list; finite only "
      "when the line's listing market (FINRA marketClassCode of its latest visible short-interest "
      "row no older than 45 days: NNM/SC -> nasdaq, NYSE/AMEX/ARCA -> nyse, BZX -> cboe_bzx, OTC "
      "-> finra_otc) has a visible list (status list or empty_list) on at least 60 of those 63 "
      "sessions. Formula id regsho-threshold-days63-listing-market-v1.";
  d.source_columns = {"lists.market",
                      "lists.list_date",
                      "lists.status",
                      "lists.available_at",
                      "threshold.list_date",
                      "threshold.market",
                      "threshold.security_id",
                      "threshold.on_list",
                      "threshold.available_at",
                      "finra_names.security_id",
                      "finra_names.available_at",
                      "finra_names.dissemination_date",
                      "finra_names.market_class"};
  d.point_in_time = true;
  s.caveats = {
      "the NYSE-family lists were still landing at stage build time (51 days of 2018 only): "
      "NYSE-listed lines are NaN by the coverage rule, never a false zero; a rebuild after the "
      "stage lands them fills them unchanged",
      "lists are re-posted without revision history (vintage_risk)",
      "symbols the stage could not map are absent (OTC symbols outside the vendor file)"};
  return s;
}

} // namespace

const FieldSpec &ftd_shares_ratio21_spec() {
  static const FieldSpec spec = make_ftd_shares_ratio21_spec();
  return spec;
}

const FieldSpec &regsho_threshold_days63_spec() {
  static const FieldSpec spec = make_regsho_threshold_days63_spec();
  return spec;
}

} // namespace atx::engine::research::fields
