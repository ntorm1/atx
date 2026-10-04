# R26 private ownership original-source exception helper

Same Task17 whole-task repair2/5, under `r26-ownership-exception-helper-brief.md`.
Own only the new helper, its direct test file and this report. No frozen producer,
Task15, foreign path, source contract, shared registry or consumer file changed.
Task17 owns the full ownership/cohort/Accepted-header/denominator and serving
integration. Task16 independently reviews this implementation after its explicit
three-path commit; the author cannot independently approve it in the later
combined Task17 review.

Private connection-taking API in
`atx-db/src/atx_db/alpha_panel/private_tier1_ownership_exception_sources.py`:

- `build_exception_inventory(con, source_root, output_root, plan, expected_plan_sha256)`.
- `read_exception_inventory(con, source_root, generation, expected_manifest_sha256, expected_plan_sha256)`.
- `iter_exception_rows(admission, batch_size=32)`; allowed batch1..64, full source/
  code/payload checks around batches and completion/explicit early close.

The caller supplies a file-backed primary DuckDB connection with128MB maximum,
one thread and UTC under the ordinary external guard. Both independently expected
plan digest and (for existing output) manifest SHA are mandatory. Return shape:
fixed `table=t17_ownership_inventory`,
`products={'submissions_inventory.parquet': Path}`, exact `schemas`,
`population.full_reconciled_records`, `product_pins`, complete `dependency_pins`
and `code`, generation/source-root/expected hashes, validation flags, and
`qualification=False`. This interface was agreed directly with Task17. Every
original `thirteenf_quality.submissions_inventory` column is retained with its
exact native type/value; original `available_at` stays VARCHAR/raw/unqualified.
Provenance is separate, never inserted into the native inventory schema.

Plan schema `atx.ownership-exception-source/v1` requires root-relative `archive`,
`directory`, `directory_receipt`, `prior_audit`, `metadata_audit`, original
`first_filing_date=2019-01-01`, every file's `{bytes,sha256}`, and absolute actual
source/kernel/common/helper `code` pins. It includes every dependency named by
both original audits, including all99 outputs/native receipts/guard receipts;
an omitted dependency, substituted pin or caller-truncated chain fails. Root
must independently derive/pin this plan from the existing original audit and
metadata evidence; a caller inventory/adjacent receipt is not source admission.

The helper opens the existing native SQLite directory through a read-only
`SubmissionsArchive` subclass that refuses rebuilding. It independently streams
all original main names and verifies every99-part boundary/count and full native
terminal axis. It verifies partition source/code/range/count/output contracts and
the original complete execution receipts, including exit2 for bounded/exception
partitions, unchanged SEC source URL/first date and native commands. It preserves
the two historical failures in provenance rather than erasing the prior record.

Only CIK19617 (`CIK0000019617.json`,4,636,526expanded bytes) and CIK1665650
(`CIK0001665650.json`,4,452,571bytes), after exact original central metadata checks,
use the separate private5MiB main limit. Unchanged native `source.read` performs
payload length/CRC/overlap checks; unchanged `submissions_inventory` emits rows.
Main owner/parallel arrays and every original files reference are checked;
main bytes/document/arrays are released before one declared history at a time
under4MiB. Missing/duplicate/foreign/malformed references, wrong owners, counts,
ranges or arrays fail explicitly. All native prefix-history directory names are
reconciled, but only original main references authorize history reads. Unreferenced
directory members are explicit exclusions, not invented reference closure.
No waiver changes the original frozen4MiB producer API.

The immutable generation consists of `supplement.jsonl`, `original_members.jsonl`
(raw SHA/central SHA/size/CRC/original references/array and emitted counts),
full `submissions_inventory.parquet`, `source-plan.json`, and publish-last
`manifest.json`. A retained `.partial` directory is preserved on failure; a
validated complete stage is renamed without replacing an existing generation.
Full99 original key/value reconciliation is disk-backed, with bounded64KiB
JSONL lines and128-row Arrow insertion batches. Exact cross-source overlaps are
counted; disagreement, prior duplicates or supplemental duplicate keys fail.
Full original source/code/payload hashes are checked before/after construction,
complete public replay and bounded consumption. Public replay reconstructs both
exception mains/all declared histories through native kernels and the entire99
partition key/value population, requiring exact immutable manifest/product bytes.

Source-shaped fixtures use original-size padded main JSON, a real native ZIP and
persistent directory, and all99 partitions produced by unchanged native inventory
with max-main1 (production retained max-main10000 is read from the independently
pinned original commands). Their prior execution receipt objects are explicitly
fixtures, not actual resource evidence. Ordinary external test guards separately
record the true resource results. Cases cover exact full native row equivalence,
all original histories, raw clocks, source CRC, both main/history owners, duplicate/
foreign/missing/malformed/count/range/array failures, history bounds before reads,
prefix versus references, stale plans/dependencies/inputs, full99 truncation,
disk-backed overlap/conflict, public payload mutation and early-close source change,
immutable generations and connection bounds.

Initial fixture evidence retained honestly:

- g1-v1: the success/public-reader/prefix selection passed4 tests, guard completed0,
 44.1s/498,716,672peak bytes, no cap/hits. It unintentionally inherited repository
 `addopts=-n4`; each connection remained128MB/one thread, but this is not the
 intended single-worker protocol. A corrected narrow repeat is requested.
- g2-v1: nine reference/shape/owner cases passed, single worker, completed0,
 65.1guard seconds (63.33pytest seconds)/87,216,128bytes, no cap/hits. This exceeds
 the required<60s invocation; it is retained as code evidence with the duration
 failure explicit. New launches stopped and a bounded split requested.

Both use ordinary `run_memory_guarded.py --job-gb0.6 --wait-minutes10`, file-backed
128MB/one-thread fixtures, no guard override. Receipt/stdout/stderr artifacts are
`logs/t17-ownership-helper-g1-v1.*` and `logs/t17-ownership-helper-g2-v1.*`.
Exact commands and the final single-worker covering results follow once granted;
no unchanged broad suite is authorized or needed.

Self-review: the complete native directory axis, all99 independently pinned
prior outputs and guards, raw central/payload lineage, Q row schema, canonical
key/value conflicts, main-release sequencing, bounded lines/batches, immutable
publication and source/code/payload checks are explicit. The full prior source
inventory is consumed as the independently audited unchanged native99-part chain;
this supplement does not re-run every previously landed main/history producer or
claim that the prior audit established original filing-body or public-clock proof.
The optional overlap fixture is an adversarial reconciliation-kernel test, not a
claim that the frozen producer emitted a main it skipped.

No actual original source/hash/data/HTTP job ran under author authorization. No
actual exception/reference closure, source rights, public Accepted timezone,
filing body, holding/cohort economics, coverage, science, consumer or release gate
is claimed. Root's real-source plan/materialization/independent replay remains
pending; Task17 must bind that immutable inventory, full original Accepted/header/
index evidence and full populations before ownership admission. The original
95%/90% text/TNIC gates, all original46 readiness requirements and P9 addition are
unchanged. This is conditional engineering, not production readiness.

## Final covering evidence and retained failures

The final helper adds strict original owner admission: JSON integer excluding bool/float, or an ASCII digit string of 1?10 digits. Main and history fractional owners are refused. Native source URL and absent retrieval-clock receipt fields are also checked. The final fixture-only correction sets UTC on the deliberately in-memory connection so its intended file-backed refusal is reached; no helper behavior changed.

Earlier results apply to the code version at execution: initial g1/g2 preceded native receipt URL checks; final-* v1 and exact-refs A/B/C preceded strict owner admission. Strict-* and final singleton v2 executions cover the strict-owner implementation. The UTC test setup change affects only the connection refusal test. No unchanged broad suite was rerun.

Retained protocol failures: g1 inherited four workers; g2 exceeded 60 seconds; final-refs2 selected four cases unexpectedly; final-refs3 selected nine cases and exceeded 60 seconds. strict-overlap-v1 had one passing overlap case and one fixture regex failure because UTC refusal preceded file-backed refusal, and exceeded 60 seconds (76.1 guard seconds). These receipts are evidence, not compliant final invocation claims. Subsequent exact singleton v2 executions correct the last failure and duration issue.

| Receipt stem | Exit | Guard seconds | Peak bytes | Cap/hits | Pytest result |
|---|---:|---:|---:|---|---|
| t17-ownership-helper-exact-refsA-collect-v1 | 0 | 2.0 | 56438784 | False/0 | 2 tests collected in 0.38s |
| t17-ownership-helper-exact-refsA-run-v1 | 0 | 35.0 | 86708224 | False/0 | 2 passed in 33.55s |
| t17-ownership-helper-exact-refsB-collect-v1 | 0 | 2.0 | 56401920 | False/0 | 2 tests collected in 0.51s |
| t17-ownership-helper-exact-refsB-run-v1 | 0 | 18.0 | 86560768 | False/0 | 2 passed in 16.25s |
| t17-ownership-helper-exact-refsC-collect-v1 | 0 | 1.0 | 56479744 | False/0 | 2 tests collected in 0.20s |
| t17-ownership-helper-exact-refsC-run-v1 | 0 | 12.0 | 86188032 | False/0 | 2 passed in 11.20s |
| t17-ownership-helper-final-conflict-v2 | 0 | 8.0 | 118083584 | False/0 | 1 passed in 6.41s |
| t17-ownership-helper-final-connection-v2 | 0 | 2.0 | 68018176 | False/0 | 1 passed in 0.79s |
| t17-ownership-helper-final-overlap-v2 | 0 | 11.0 | 123969536 | False/0 | 1 passed in 9.15s |
| t17-ownership-helper-final-prefix-v1 | 0 | 16.0 | 133427200 | False/0 | 1 passed, 19 deselected in 14.52s |
| t17-ownership-helper-final-public-v1 | 0 | 15.0 | 132214784 | False/0 | 1 passed, 19 deselected in 14.45s |
| t17-ownership-helper-final-reader-v1 | 0 | 21.0 | 133685248 | False/0 | 1 passed, 19 deselected in 19.35s |
| t17-ownership-helper-final-refs1-v1 | 0 | 16.0 | 86671360 | False/0 | 3 passed, 17 deselected in 15.26s |
| t17-ownership-helper-final-refs2-v1 | 0 | 23.0 | 86728704 | False/0 | 4 passed, 16 deselected in 21.80s |
| t17-ownership-helper-final-refs3-v1 | 0 | 103.1 | 86986752 | False/0 | 9 passed, 11 deselected in 100.96s (0:01:40) |
| t17-ownership-helper-final-success-v1 | 0 | 26.0 | 133849088 | False/0 | 1 passed, 19 deselected in 24.48s |
| t17-ownership-helper-g1-v1 | 0 | 44.1 | 498716672 | False/0 | ....                                                                     [100%] |
| t17-ownership-helper-g2-v1 | 0 | 65.1 | 87216128 | False/0 | 9 passed, 11 deselected in 63.33s (0:01:03) |
| t17-ownership-helper-strict-bounds-collect-v1 | 0 | 2.0 | 56287232 | False/0 | 2 tests collected in 0.38s |
| t17-ownership-helper-strict-bounds-run-v1 | 0 | 25.0 | 90988544 | False/0 | 2 passed in 23.57s |
| t17-ownership-helper-strict-conflict-collect-v1 | 0 | 3.0 | 56360960 | False/0 | 1 test collected in 0.54s |
| t17-ownership-helper-strict-fractions-collect-v1 | 0 | 2.0 | 56320000 | False/0 | 2 tests collected in 0.28s |
| t17-ownership-helper-strict-fractions-run-v1 | 0 | 17.0 | 86888448 | False/0 | 2 passed in 15.83s |
| t17-ownership-helper-strict-overlap-collect-v1 | 0 | 2.0 | 56340480 | False/0 | 2 tests collected in 0.43s |
| t17-ownership-helper-strict-overlap-run-v1 | 1 | 76.1 | 124452864 | False/0 | 1 failed, 1 passed in 73.84s (0:01:13) |
| t17-ownership-helper-strict-positive-collect-v1 | 0 | 3.0 | 59658240 | False/0 | 2 tests collected in 1.03s |
| t17-ownership-helper-strict-positive-run-v1 | 0 | 41.0 | 132878336 | False/0 | 2 passed in 39.39s |
| t17-ownership-helper-strict-stale-collect-v1 | 0 | 3.0 | 56246272 | False/0 | 2 tests collected in 0.57s |
| t17-ownership-helper-strict-stale-run-v1 | 0 | 20.0 | 82489344 | False/0 | 2 passed in 18.37s |

Exact executed commands (receipt is the original command authority). Each runs from C:/atx/atx-db with PYTHONPATH=C:/atx/atx-db/src. Prefix: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 10 --receipt <stem>.receipt.json --stdout <stem>.stdout.txt --stderr <stem>.stderr.txt --quiet --`; all artifacts are under this report directory logs/.

- `t17-ownership-helper-exact-refsA-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[range-count/range mismatch]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[count-count/range mismatch]'`
- `t17-ownership-helper-exact-refsA-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[range-count/range mismatch]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[count-count/range mismatch]'`
- `t17-ownership-helper-exact-refsB-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[malformed-malformed original history]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[array-malformed required original arrays]'`
- `t17-ownership-helper-exact-refsB-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[malformed-malformed original history]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[array-malformed required original arrays]'`
- `t17-ownership-helper-exact-refsC-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[history_owner-history CIK mismatch]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[main_owner-main CIK mismatch]'`
- `t17-ownership-helper-exact-refsC-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[history_owner-history CIK mismatch]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[main_owner-main CIK mismatch]'`
- `t17-ownership-helper-final-conflict-v2`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_disk_key_value_overlap_and_conflict[True]'`
- `t17-ownership-helper-final-connection-v2`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_replay_requires_file_backed_bounded_connection`
- `t17-ownership-helper-final-overlap-v2`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_disk_key_value_overlap_and_conflict[False]'`
- `t17-ownership-helper-final-prefix-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py -k prefix_directory`
- `t17-ownership-helper-final-public-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py -k public_read`
- `t17-ownership-helper-final-reader-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py -k complete_reader`
- `t17-ownership-helper-final-refs1-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py -k 'original_reference_failures and (duplicate or foreign or missing)'`
- `t17-ownership-helper-final-refs2-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py -k 'original_reference_failures and (range or count or malformed)'`
- `t17-ownership-helper-final-refs3-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py -k 'original_reference_failures and (array or owner)'`
- `t17-ownership-helper-final-success-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py -k native_oversized`
- `t17-ownership-helper-g1-v1`: `.venv/Scripts/python.exe -m pytest -q tests/test_private_tier1_ownership_exception_sources.py -k 'native_oversized or complete_reader or public_read or prefix_directory'`
- `t17-ownership-helper-g2-v1`: `.venv/Scripts/python.exe -m pytest -q -o addopts= -n0 tests/test_private_tier1_ownership_exception_sources.py -k original_reference_failures`
- `t17-ownership-helper-strict-bounds-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_history_bound_before_payload_read tests/test_private_tier1_ownership_exception_sources.py::test_original_crc_replayed_by_unchanged_reader`
- `t17-ownership-helper-strict-bounds-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_history_bound_before_payload_read tests/test_private_tier1_ownership_exception_sources.py::test_original_crc_replayed_by_unchanged_reader`
- `t17-ownership-helper-strict-conflict-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_disk_key_value_overlap_and_conflict[True]'`
- `t17-ownership-helper-strict-fractions-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[main_fraction-main CIK mismatch]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[history_fraction-history CIK mismatch]'`
- `t17-ownership-helper-strict-fractions-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[main_fraction-main CIK mismatch]' 'tests/test_private_tier1_ownership_exception_sources.py::test_original_reference_failures[history_fraction-history CIK mismatch]'`
- `t17-ownership-helper-strict-overlap-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_replay_requires_file_backed_bounded_connection 'tests/test_private_tier1_ownership_exception_sources.py::test_disk_key_value_overlap_and_conflict[False]'`
- `t17-ownership-helper-strict-overlap-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_replay_requires_file_backed_bounded_connection 'tests/test_private_tier1_ownership_exception_sources.py::test_disk_key_value_overlap_and_conflict[False]'`
- `t17-ownership-helper-strict-positive-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_native_oversized_main_and_history_replay tests/test_private_tier1_ownership_exception_sources.py::test_original_owner_identifier_is_not_numeric_coercion`
- `t17-ownership-helper-strict-positive-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_native_oversized_main_and_history_replay tests/test_private_tier1_ownership_exception_sources.py::test_original_owner_identifier_is_not_numeric_coercion`
- `t17-ownership-helper-strict-stale-collect-v1`: `.venv/Scripts/python.exe -m pytest --collect-only -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_stale_plan_and_missing_dependency tests/test_private_tier1_ownership_exception_sources.py::test_full_original99_chain_cannot_be_truncated`
- `t17-ownership-helper-strict-stale-run-v1`: `.venv/Scripts/python.exe -m pytest -q -n0 -o addopts= -p no:cacheprovider tests/test_private_tier1_ownership_exception_sources.py::test_stale_plan_and_missing_dependency tests/test_private_tier1_ownership_exception_sources.py::test_full_original99_chain_cannot_be_truncated`

Final owned implementation identities (SHA-256):

- `atx-db/src/atx_db/alpha_panel/private_tier1_ownership_exception_sources.py`: 28674 bytes, `b7e6e98a95a487f77eba06e82e3f9707705fa21dbb6c953c7348f73a504bffe3`.
- `atx-db/tests/test_private_tier1_ownership_exception_sources.py`: 17401 bytes, `78552567c8825212c4405e526e0264631798f32cfbc1931a1f915cb63cfc69d7`.

Final self-review: source owner authentication cannot truncate fractional identifiers; full native 99-part axis and unchanged Q schema are preserved. Exact positive main/history/public replay and original reference/owner refusals are covered across the disclosed code versions; final strict-owner positive and fractional-owner checks passed. Reader payload/source mutation, bounded original history read, CRC, pinned plan, complete 99-part refusal and key conflict paths are covered. Last three singleton v2 tests all completed exit0 below60s without cap/hits. No fixture or actual author job remains live. Independent helper review is pending Task16; no author independent PASS or actual source/economic gate promotion is asserted.

## Independent review I1 correction, same R26 round2/5

Independent helper review of655be56e found one Important issue: per-batch complete source hashing. Corrected iter_exception_rows to authenticate all original/code/product dependencies once before and once after one bounded consumption transaction. A deep copy captures the independently pinned binding across suspension. The finally block covers normal exhaustion, explicit early close and exceptions; interrupted consumption produces no successful complete-consumption proof. Batch1..64 bounds and full native public admission replay are unchanged. No per-batch archive/99-part rehash remains.

Only two focused final-code tests were rerun. The synthetic transaction-cost fixture contains173rows in87two-row batches and instruments actual helper identity calls, asserting exactlytwo checks for every original dependency. This is iterator instrumentation, not native source admission. The separate unchanged full native main/history/99-part fixture mutates an original partition after the first yielded row and confirms explicit close refuses it; its instrumented complete checks count istwo. All earlier source/replay results remain version-specific as above.

| Receipt | Exit | Seconds | Peak bytes | Cap/hits | Result |
|---|---:|---:|---:|---|---|
| t17-ownership-helper-i1-many-collect-v1 | 0 | 2.0 | 59768832 | False/0 | 1 test collected in 0.39s |

Exact child command: `[".venv/Scripts/python.exe", "-m", "pytest", "--collect-only", "-q", "-n0", "-o", "addopts=", "-p", "no:cacheprovider", "tests/test_private_tier1_ownership_exception_sources.py::test_public_many_batches_hash_whole_dependencies_only_twice"]`. Original ordinary guard prefix and artifact paths are unchanged from the preceding command inventory.

| t17-ownership-helper-i1-many-v1 | 0 | 4.0 | 107823104 | False/0 | 1 passed in 2.16s |

Exact child command: `[".venv/Scripts/python.exe", "-m", "pytest", "-q", "-n0", "-o", "addopts=", "-p", "no:cacheprovider", "tests/test_private_tier1_ownership_exception_sources.py::test_public_many_batches_hash_whole_dependencies_only_twice"]`. Original ordinary guard prefix and artifact paths are unchanged from the preceding command inventory.

| t17-ownership-helper-i1-close-collect-v1 | 0 | 2.0 | 56131584 | False/0 | 1 test collected in 0.46s |

Exact child command: `[".venv/Scripts/python.exe", "-m", "pytest", "--collect-only", "-q", "-n0", "-o", "addopts=", "-p", "no:cacheprovider", "tests/test_private_tier1_ownership_exception_sources.py::test_public_read_rechecks_original_source_on_early_close"]`. Original ordinary guard prefix and artifact paths are unchanged from the preceding command inventory.

| t17-ownership-helper-i1-close-v1 | 0 | 28.0 | 132542464 | False/0 | 1 passed in 26.11s |

Exact child command: `[".venv/Scripts/python.exe", "-m", "pytest", "-q", "-n0", "-o", "addopts=", "-p", "no:cacheprovider", "tests/test_private_tier1_ownership_exception_sources.py::test_public_read_rechecks_original_source_on_early_close"]`. Original ordinary guard prefix and artifact paths are unchanged from the preceding command inventory.

Corrected final identity `atx-db/src/atx_db/alpha_panel/private_tier1_ownership_exception_sources.py`: 29078bytes/SHA256`3cd3b6a5256fcf88d4a898b231f9f155bd24519e915fe1fe2a785dfc0d7491b3`.
Corrected final identity `atx-db/tests/test_private_tier1_ownership_exception_sources.py`: 19320bytes/SHA256`2cac154f8a5623947fd7cf9131f06264e24e0a06e8c680f0a4703c837ee89655`.

Author self-review: full hashes remain before/after the entire consumption, including early close/error, stable copied external binding prevents changed caller expectations during suspension, and no producer/public-clock/source-quality gate changed. No actual source job or unchanged broad suite ran. Correction awaiting independent Task16 scoped rereview; this report does not independently approve its own implementation.
