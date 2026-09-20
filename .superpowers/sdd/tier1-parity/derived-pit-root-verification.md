# P1 /0315 root verification

2026-09-20 22:18UTC. The companyfacts writer is terminal; root ran verification
sequentially with the locked project Python/DuckDB1.5.5. No live warehouse
migration or derived build occurred.

The combined focused invocation covered test_derived_pit_revisions,
test_derived_metrics, test_market_daily, test_panel_export and
test_core_metric_breadth:65passed and1failed, native guard peak0.922688GiB
under2.5GiB. Every P1 case passed, including populated0314->315 upgrade,
legacy preservation, PK/nullability/reentry, original460->510 restatement,
invalid daily/factor states, first_reported API, stub dates and bounded frames.

The sole failure belonged to the additive Core fixture: its expected DSO/DIO/DPO
denominator used quarter indices0..3 at targetindex4. The correct TTM is1..4.
Core's implementer repaired only those expectations; root's single affected-case
rerun passed, native peak0.751171GiB. This did not change P1/source formulas.

P1 touched-file Ruff found only one extra blank line after imports and a test
regex that should be explicitly raw. Root made these mechanical changes;
the two-file Ruff recheck passed, all other P1 paths passed the initial check.
Strict mypy passed for both new source modules (_derived_pit and bodies_0315),
guard peak0.327911GiB. Path-scoped git diff --check passed.

Evidence: `derived-pit-focused-91a7f1a8c70b4fe1920b4b30c5b1231e-stdout.log`
and matching memory/stderr files; `derived-pit-mypy-0315.log` and receipt;
`core-metric-breadth-fix1-check.log` and receipt. Core's fix explanation is
`core-metric-breadth-fix1-report.md`.

One independent P1 source review already closed the original Critical;
ImportantI1/MinorM1 prepared cases have now passed root execution. The mechanical
lint edits and fixture correction do not require another review. P1 is accepted
for its pathspec commit, then Core separately. A committed-HEAD import/module/
schema check follows those commits; AF1 and CF5 remain separate tasks. The
full-universe runtime, measured coverage/quality, release and full sprint gate
are not proven by these focused fixtures.

Committed-HEAD check at0c52fdbd: isolated import resolved exclusively to the
git-archive export. All schema-contract cases and remaining module cases passed
(one preexisting slow skip). The sole failure was the intended new private
helper `_derived_pit` missing from the literal dir-surface snapshot. Root added
that exact name, without changing boundary rules or another task's snapshot
names. Native guard peak0.967030GiB. After the pin commit, rerun the isolated
module file only; the schema/source bytes are unchanged by this fixture update.
Evidence: pit-core-head-0c52fdbd.log/err/-memory.json.
