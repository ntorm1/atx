# Companyfacts per-CIK replacement audit

Static source review, 2026-09-20. No database, imports, tests, probes, or network requests were run. Only this report was written. This brief concerns the archive ingestion failure, not the downstream fundamentals stages.

## Confirmed failure

The controller reports that the first archive target failed at 19:23:14 UTC. The preserved traceback identifies `SecCompanyFactsDataset._replace_facts`, `atx-db/src/atx_db/fundamentals.py:1206`: the `DELETE FROM fundamental_points` statement, before the raw-fact DELETE or inserts. DuckDB could not allocate 256 KiB at **953.5 MiB / 953.6 MiB**. The stage receipt reports **68.375 seconds**, failed status, and zero returned rows. The 3 GiB guard did not terminate the process; its native peak job memory was **1.838 GiB**, and the process exited with code 1.

Evidence read: `activation-companyfacts-archive1-launch2.err`, `.log`, and `-memory.json` under this report's directory. The command used `archive_members`, replacement mode, `1GB`, and one thread. The initial progress line lists 20,390 targets. Progress is logged before every 25th target (`fundamentals.py:1035–1038`), so absence of the next progress line alone cannot identify an individual target or prove a hang. The traceback does identify the failed SQL statement. No execution plan was obtained; the precise failing DuckDB operator is unknown.

## Source path and scope

`SecCompanyFactsDataset.load` resolves archive targets (`963–968`), lazily parses one ZIP member (`_CompanyFactsZipFetcher.__call__`, `916–927`), normalizes it (`1067–1070`), resolves its PIT identities (`1084–1086`), synchronizes archive point IDs with resolved raw-fact IDs (`1096–1099`), and calls `_replace_facts` (`1100`).

- `normalize_companyfacts` (`600–686`) creates two Python row lists and two DataFrames for **one CIK**. The parsed member also remains resident. `resolve_company_facts_identifiers` copies that member's facts (`406`) and may build one unresolved dictionary per member fact (`511–535`). These are concrete largest-member memory multipliers, not archive-wide fact materialization and not the demonstrated failing operation.
- Identity resolution registers distinct `(CIK, available_at)` lookup rows from that member (`413–422`). Its `fetchall()` (`503`) returns at most one resolved identity row per such key, not all warehouse facts. History-presence CTEs (`426–429`, `462–465`) range over the identity history table; they may incur repeated metadata work, but archive mode disables both current fallback branches. Do not weaken those PIT checks to accelerate replacement.
- Archive target lists are O(number of CIKs), and accumulated unresolved frames are reduced to one row per CIK (`535`, `1088`, `1131`). There is no Python fetch of the entire `sec_company_facts` or `fundamental_points` surface in this replacement-mode loop.

## Proven failing query and smallest repair

The point DELETE at `1208–1228` combines an outer warehouse points relation with a correlated `EXISTS` against raw facts, a numeric/regexp CIK filter, six NULL-safe key comparisons, and an identity `OR` containing another ticker subquery. This formulation permits substantial work over the full points relation despite replacing only one CIK. The OOM is now demonstrated for this statement; its exact physical expansion is not established by this audit.

Materialize a **small SQL deletion-key relation for the old contents of the requested CIK**, then use a direct `DELETE ... USING` join. Keep the entire operation inside the existing per-CIK transaction at `1200`.

1. Capture that CIK's existing fact rows, or just their distinct deletion keys, with the **unchanged** predicate `regexp_full_match(trim(cik), '[0-9]+') AND try_cast(cik AS BIGINT) = cast(? AS BIGINT)`. Only issuer-sized results enter the temporary relation; do not fetch them into Python. Materialize before deleting old raw facts.
2. Build a deduplicated legacy-ID relation containing the passed `security_id`, `cik_security_id(cik)`, and every `sec_company_tickers.security_id` whose ticker CIK satisfies the same guarded numeric predicate. NULL identities need not match: the existing security comparisons use ordinary equality/IN, not NULL-safe equality.
3. Construct allowed deletion tuples as the union of:
   - each old fact's **own** `security_id` plus its six filing keys;
   - each legacy ID crossed with this CIK's distinct six-key tuples.
4. Delete points by ordinary equality on `security_id`, `p.source = SOURCE_NAME`, and `IS NOT DISTINCT FROM` on each of the six key fields. The fields are accession number, taxonomy, metric/concept, unit, period end, and period start. This removes the nested correlated identity OR from the large-table DELETE while preserving its truth conditions.
5. Retain the existing raw-fact CIK deletion (`1232–1235`), candidate cleanup (`1236–1241`), and both inserts (`1243–1245`) in the same transaction. Drop temporary relations on success/failure using the project's normal cleanup pattern. Database errors must remain fatal rather than become skipped archive members (`1045`, `1084–1100`).

The expanded relation is bounded by this issuer's distinct old key count times its deduplicated legacy-ID count, plus the issuer's own ID/key tuples. If an issuer has unusually many legacy identities, two direct DELETE joins can avoid the cross product: one matching old identity/key tuples, and one joining points to the small legacy-ID and issuer-key relations. Both DELETEs must remain in the same transaction. The implementer should choose the simplest shape that demonstrates low-memory behavior on the regression fixture.

**Do not** collect all old security IDs and cross them with all old keys: the old-fact-ID branch in the existing predicate ties a specific ID to a specific fact key. Broadening it changes which points are deleted. Likewise, do not delete every point for a legacy ID, delete by accession alone, filter only the new incoming facts, use ordinary equality for nullable keys, or replace the numeric CIK predicate with equality on a single padded spelling.

## Semantics and focused validation for the implementation task

No new schema, persistent job/registry, activation edit, or index change is needed for this first repair. Validate these behaviors in a small isolated fixture when the controller grants the test slot:

- Padded, unpadded, whitespace-padded, and extra-leading-zero numeric spellings identify the same old CIK; junk/non-numeric CIKs remain untouched.
- Old PIT IDs, the supplied archive unresolved ID, `cik_security_id(cik)`, and matching current ticker IDs receive the same deletion treatment as the existing predicate.
- An old resolved ID with a different old fact's filing key is retained unless that ID independently belongs to the legacy-ID branch. This detects accidental broadening of ID/key ownership.
- NULL accession/taxonomy/unit/period fields match with the original NULL-safe semantics; nonmatching keys and points from other sources survive.
- Another CIK's points that share filing keys but have an unrelated identity survive. Where data for different CIKs has exactly the same source, security ID, and all six keys, points carry no CIK and the existing predicate cannot distinguish them; preserve the existing predicate rather than claim a stronger ownership guarantee.
- Empty or allowlist-empty replacement still removes that CIK's old matching facts/points. A CIK absent from old facts produces no point deletion merely because a fallback ID exists.
- An injected error after point deletion, raw deletion, or either insert rolls back the entire CIK replacement, including candidate cleanup. Other CIKs remain unchanged. Temporary-name reuse works on retry.
- Add many unrelated old points/facts with few target keys and verify the replacement stays under a deliberately low query-memory limit. This is the regression that checks the demonstrated scaling failure, beyond tiny row-parity cases.

Current `DuckDBStore.transaction` rolls back ordinary exceptions (`connection.py:270–279`). The OOM follows that exception path. Preserving the same transaction keeps both old surfaces intact if any replacement step fails; this audit did not open the database to independently measure recovery. `record_source_file` runs after `_replace_facts` returns (`1117`), outside that replacement transaction, and should not be pulled into an unrelated redesign.

## Residual throughput concern, separate from the OOM repair

The raw-fact DELETE (`1233–1234`) repeats a regexp/trim/numeric-CIK predicate for every target. The failed point DELETE repeats that same fact predicate; staging the old issuer keys removes that repetition from the correlated form. The repository defines fact indexes on `(security_id, filed_date)` and `(entity_id, filed_date)` and a point index on `(metric, as_of_date)` (`schema.py:1898–1899`, `1931`), with no CIK lookup index found in the searched schema/migrations. Neither fact nor point table has a primary key (`schema.py:804–827`, `1357–1375`). Actual scan pruning and join plans are unmeasured.

Even after the OOM fix, repeated whole-table eligibility scans across 20,390 CIKs may dominate runtime. Do not assume the direct join eliminates those scans. First measure progress from the guarded retry. If scans are the demonstrated next bottleneck, a separate bounded task can reuse transaction-local old-fact row locators for deletion, or create a once-per-load SQL CIK locator strategy while preserving numeric normalization and atomic replacement. That is a follow-up decision, not a prerequisite or authorization for a new persistent index/framework now.

Only the correlated point-deletion repair is supported by the current failure evidence.
