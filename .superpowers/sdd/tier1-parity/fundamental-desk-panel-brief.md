# DS2: desk screen from the validated fundamental panel

The old prepared fundamental-desk-screen-acceptance.sql predates DL1/FQ1.
It joins accounting metric rows directly to trading security_id and manually
reconstructs only the current EPS operand. That neither handles distinct
issuer/trading owners nor proves prior-year/dependency CIKs for four metrics.
It has never been run live. Replace this acceptance path using the reviewed
generic FQ1 panel/proof contract rather than another reconstructed SQL lineage.

Desk question: at the latest observed decision session in an explicitly
selected completed default FQ1 run, which historically qualified US common
names have positive EPS YoY growth and positive operating-margin change, and
what are their total accruals, net-debt/EBITDA, market cap, earnings yield and
CFO/EV yield? This is a descriptive screen, not alpha inference. Clearly
report panel as-of date, chosen decision date and22UTC decision cutoff; do not
claim to include later weekend disclosures just because snapshot is Sunday.
The arbitrary content_as_of issuer API remains the way to inspect such facts.

Own only atx-db/sql/research/fundamental-desk-screen-acceptance.sql,
new atx-db/scripts/read_fundamental_desk_screen.py,
new atx-db/tests/test_fundamental_desk_panel.py, and
atx-db/docs/FUNDAMENTAL_DESK_SCREEN_ACCEPTANCE.md. Parent owns controller docs;
IQ2 agent owns api/service.py, asof/fundamentals.py, derived_lineage.py and issuer
docs/tests/catalog changes. Do not edit any of those or FQ1/FQ2 modules.

Requirements:
- Explicit existing DB, completed FQ1 build run ID and report as-of date; no
  implicit initialize/migrate, writes, network, or calendar clock reads in SQL.
  Use the existing public validate_fundamental_signal_panel in a single
  read-only transaction before any numeric screen output. It may stream its
  existing digest; do not fetch the full panel into Python. Distinguish stored
  manifest observations from validated content; unknown/failed/blocked or
  malformed runs yield a controlled diagnostic, never unvalidated candidates.
- Require exactly the default frozen signal specifications and a build snapshot
  no later than the requested report date. Inspect actual FQ1 schemas/validator
  return fields. Do not trust a signal ID alone to mean the default formula.
  Keep default200day accounting freshness or reject a looser build contract.
- Parameterized prepared SQL, read-only. Select the latest decision session
  recorded in that run, identify it explicitly, and read four single-signal
  inputs/eligible scores from that run. Avoid mixing run IDs or score directions
  (low-accrual/leverage scores are signed hypotheses, not raw accounting ratios).
  Preserve source state IDs, fiscal anchors, clocks and proof diagnostics where
  useful, without copying arbitrary JSON or full source-leaf payloads.
- Join the canonical daily market row on actual trading security_id and the
  SAME decision session, with availability/as_of <= decision cutoff/date.
  Select whole latest-visible states before NULL/value screening; no current
  ticker directory or backward-filled market row. Missing market fields get
  independent counts even if another field fails. Do not qualify price/market
  lineage beyond what the source actually proves.
- Emit one aggregate diagnostic even for no usable names, plus deterministic
  bounded preview (<=999 names, <=1000 total rows). Counts cover the entire
  selected cohort, not the preview. No eligible historical common cohort is
  an explicit missing-data result, never a vacuous pass. Preserve rejected/
  unavailable FQ1 reasons in bounded diagnostics, not fabricated zero values.
- The raw SQL is an inspection contract requiring the runner's prior FQ1
  validation; it must not by itself advertise digest certification. Output
  records SQL hash, build/panel hash, selected date/cutoff, row/byte caps,
  validation outcome and source limits. Fresh output path, refuse overwrite;
  no output file on partial failure, or atomic diagnostic-only artifact.
- Configured256MB/one-thread read-only DuckDB, bounded spill and result bytes.
  All runtime remains under parent's external memory guard. No whole-universe
  Python frames. Existing standalone script patterns can guide output safety.

Tiny focused fixtures256MB/one-thread, no full-schema bootstrap. Cover correct
raw metric signs and owner-independent security join from FQ1, missing market
fields and NULL invalidation, default-spec mismatch, no complete panel, empty
cohort, run isolation and output caps. Include meaningful validator sequencing
or actual tiny FQ1 integration; do not replace the production validator with
an always-success stub as sole integrity proof. Static only until root grants
the single runtime slot. One independent Codex review, Important fixed on
report, Critical rereview only; no full suite. Prescribed trailer/pathspec only.
Report in fundamental-desk-panel-result.md, await commit coordination.
