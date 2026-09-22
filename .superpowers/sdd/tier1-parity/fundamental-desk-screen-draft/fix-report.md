# Static review resolution

The five findings in `../fundamental-desk-screen-review.md` are addressed in
the isolated SQL and companion document. The warehouse remains unqueried;
every production count and the SQL binder result are still unmeasured.

1. **EPS owner.** The query now reconstructs the selected current
   `eps_diluted` input by the publisher's quarter bucket and complete
   `(period_end, available_at, source, rule_id, basis, standardized_id)` order
   at the derived event. It ranks all CIKs, then checks the winning row's CIK,
   security, basis, and period against the dated link and selected derived
   fiscal period. A wrong CIK has its own status and aggregate count. The
   opaque `inputs_hash` is reported but is not represented as a reversible
   source-row link.
2. **Freshness.** All four gates now use the selected `fiscal_period_end`,
   with missing or future operand ends classified as unavailable. Name rows
   report separate target-period and operand ages. Annual fallback cannot
   borrow a recent target quarter's date to pass the 200-day gate.
3. **Definition identity.** The four source expressions and input lists are
   frozen in SQL. It computes the publisher's SHA-256 fingerprint from those
   literals, version `1`, and the static empty annual plan; the registry must
   match the frozen literals. Selected whole metric states are ranked before
   hash comparison. A mismatched state cannot expose an older value and is
   counted explicitly.
4. **Market missingness.** The market disposition still identifies the first
   failure for readability. The aggregate now independently counts invalid
   or absent close, market cap, earnings yield, and CFO/EV yield, including
   when the whole session row is absent.
5. **Membership overlaps.** All visible interval rows are counted per
   security. More than one visible interval quarantines that security before
   market or fundamental joins, regardless of which interval ranks highest.
   The aggregate names the overlap failure and counts the affected members;
   up to 99 quarantined security IDs appear in a separate preview. Normal
   previews are capped at 300 per disposition, so total output is at most
   999 names plus the mandatory aggregate row (1000 rows).

The patch adds only the SQL and evidence document under `atx-db`. Static
`git apply --check` passes. No runtime, import, test, database access,
network call, live production edit, or commit was used in this fix pass.
