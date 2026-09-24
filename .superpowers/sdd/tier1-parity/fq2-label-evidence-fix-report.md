# FQ2 selected-label evidence repair

The whole-branch review found that `is_stitched` affected selected-label validity but was absent from the selected-row SHA. Changing it on a selected non-delisted row could turn a valid label invalid, changing counts and possibly returns while leaving both the selected SHA and aggregate sample SHA unchanged. The source-row audit also found `source_loaded_at` absent: it orders revisions after `available_at`, so its value is part of the selected revision's identity and must be evidenced.

The `_fq2_join` projection and streaming selected-row digest now include `is_stitched` and `source_loaded_at`. All source operands used by the validity CASE are covered: row ID, basis, calculation version, raw/terminal/composed returns, end/delist dates, terminal source, observation ID, delisted and stitched flags, and availability time. Revision ordering uses the covered availability time, source load time, and row ID. The security ID is in each digest row; requested source, entry date, and horizon are pinned by the run configuration and evidence key. Scores and deciles come from the separately validated FQ1 panel. The digest still streams batches of 2,048 rows in security-ID order under the existing 256 MB/one-thread settings.

New runs record `evaluation_version=fq2_v2` and `label_evidence_version=selected_label_v2` in `config_json` and its SHA. Existing `fq2_v1` completed runs retain their stored hashes and prior coverage; their selected and sample hashes must not be interpreted as v2 evidence or recomputed with the v2 projection. The publication `label_version=forward_return_publication_v1` is unchanged because the publisher's label format did not change.

Changed paths:

- `atx-db/src/atx_db/fundamental_signal_evaluation.py`
- `atx-db/tests/test_fundamental_signal_evaluation.py`
- `atx-db/docs/FUNDAMENTAL_SIGNAL_EVALUATION.md`
- `.superpowers/sdd/tier1-parity/fq2-label-evidence-fix-report.md`

Focused commands for the root's guarded run from `C:\atx\atx-db`:

```powershell
python -m pytest -q tests/test_fundamental_signal_evaluation.py::test_selected_label_evidence_covers_revision_order_and_stitching
python -m pytest -q tests/test_fundamental_signal_evaluation.py
```

The first case evaluates a frozen fixture, changes only `source_loaded_at`, then changes `is_stitched`; each change must alter the selected and sample hashes, and stitching must lower the labeled decile count from 20 to 19. No tests, imports, Python code, database access, or production evaluation were executed during this static repair.

## Root acceptance

Root ran the complete evaluation file together with the disjoint issuer-lineage
file once under the unchanged 1.5 GiB guard: all 17 checks passed, exit 0, peak
0.630894 GiB (whole-branch-fixes-tests1-memory.json). The isolated single-case
command above was not run redundantly. Scoped Ruff passed on all four changed
Python files, peak 0.074970 GiB (whole-branch-fixes-ruff1-memory.json).
The Important finding is accepted on this field audit and focused evidence;
no Critical finding or repeat review is required. No live evaluation is claimed.
