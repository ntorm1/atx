# Task 2 fix round 3 prebuild review

Reviewed commits `28ec2918f6faf29f68ea3dd9b74404b9b99f73dc`, `386378e5faa1f3ee97ca274255736177e03cf788`, and `4f3045db424fea43eedb7c4937ce740f40559a37` through the supplied 972-line package. Read REVIEWER.md, GC1–GC9, resume-contract.md, Task 2 brief, gap-analysis §2 item 1, round-2 findings, and the newest task-2-report.md section. This remains round 3/5. Graph tools are unavailable; targeted fallback was authorized. Code outside the diff was inspected only for the named retained-object verification/selection risk in sec_http.py. No HTTP, real-data replay, build, git mutation, children, or broad test suite was run.

### Spec Compliance — ❌ Needs fixes

| Scoped requirement | Disposition |
|---|---|
| R2-1 annual-plan extension preserves earlier vintages | ADDRESSED in engineering scope: issuer_history.py:111 retains prior observations before new selections; both preparation paths use it. Conflicting rediscovered identities stop preparation. Fixtures exercise annual/audit/follow-up retention and plan→load→universe preservation. Final source measurement remains pending. |
| R2-2 pending/conditional positives and terminations | Original three reproductions ADDRESSED: full sentence context rejects preceding/following conditions, including semicolons and dotted acronyms. No-event/no-transition integration fixtures are meaningful. Completed-election clause handling remains defective as R3-2. |
| Operational cache identity, typing/checksum, clocks, complete keys | Mostly implemented: full row/name/receipt/retained-object/code identities, strictly Boolean/nullable assertion payload, checksum, fresh header clocks, unique keys and complete assessment set are present. Prepublication source immutability is incomplete as R3-1. |
| Interrupted fresh-loader resume | Supported by supplied fixture: first row checkpoint survives interruption, no source outputs are written during interrupted parsing, fresh loader processes only the remaining assertion, cached/uncached table and assessment equality are asserted. This does not certify interruption during final publication. |
| GC5 scope | Package paths do not intersect foreign-dirty-files.txt; no shared-registry edit. |
| Final canonical sources/universe, hand evidence and gates | PENDING. Report explicitly withholds certification after two exit-137 host-headroom stops and marks the older universe snapshot stale under R9. No final round-3 evidence is claimed. French remains FAIL, 10/60 months outside ±5%, retained I4. |

### Strengths

The append-only acquisition-plan composition fixes the actual discovery-to-loader failure, rather than testing only append operations on an already-built table. Conditional source assertions stay Unknown and cause no synthetic false event. Scratch checkpoints bind exact compressed/raw objects and reconstruct header availability rather than caching clocks. Corrupt or changed cache entries recompute from verified originals; duplicate/incomplete keys, plan changes, receipt changes and later completed-source changes stop publication. The report separates stopped attempts and intermediate parser evidence from final publication, preserves the closed SEC budget, and keeps French FAIL and downstream hold visible.

### Issues — Critical / Important / Minor

Critical: 0. Important: 2 new engineering issues, plus retained I4 accuracy obligation. Minor: 0.

**R3-1 — Important: source identity is captured after the header clock has already been consumed, and error/unknown rows escape final source revalidation.** `atx-db/src/atx_db/alpha_panel/issuer_classes.py:192`, `:198`, `:201`, `:203`, `:209`; `atx-db/src/atx_db/alpha_panel/issuer_class_replay.py:57`, `:74`.

The loader reads and verifies the header, reconstructs its clock/name and appends incorporation before AssertionReplay captures the row's source identity. A retained header changed in that interval is therefore certified as the baseline for the old clock. Focused offline reproduction used the existing synthetic lake fixture: a Store.read wrapper returned the verified first header while replacing its compressed retained bytes with a different acceptance timestamp before returning to the loader. Plan and ledger were unchanged. `K.load(cache)` published once, reported two completed assertions, and published the original `2021-05-28 14:00:00+00:00` acceptance clock. A subsequent verified read of the same retained header failed `raw SHA mismatch`. This is a reproduced publication-integrity defect, not a claim that live retained sources were altered.

Additionally, finish iterates only successful assertion completions. Rows skipped on header failure/unbound identity or primary read/parser failure are absent. Primary failure rows can already contribute incorporation output, so an identity count covering successful assertions is insufficient to prove the complete source snapshot used by publication.

Fix: capture immutable source/receipt/object identity before any header or primary read, verify it after consumption, and retain/recheck the identity of every planned assessment before publication, including Unknown/error paths. Bind reconstructed clocks/name to that same verified identity. Add regressions for header replacement immediately after read, between clock reconstruction and assertion, and mutation of an earlier Unknown/primary-failure row while a later row is processed; each must refuse publication. Preserve valid fresh-loader resume and cached/uncached equality.

**R3-2 — Important: a genuine completed election is suppressed by an independent future-termination sentence.** `atx-db/src/atx_db/alpha_panel/issuer_classes.py:89`.

The improved sentence-aware conditional check is followed by the retained 180-character trailing election filter. That filter searches beyond sentence boundaries for `commencing|beginning|effective|will`. A uniquely CIK-bound synthetic disclosure `We have elected to be taxed as a REIT. We will terminate our election next year.` returns `{status: None, reason: no_unambiguous_issuer_status_assertion, evidence: None}`. Its current completed election should remain True at this disclosure's availability; the separate future termination is not a current false event. The control `We have elected to be taxed as a REIT and intend to maintain our qualification.` returns True. This directly concerns the round's requested completed-election/future-clause handling; it is not an unrelated historical issue.

Fix: scope prospective initial-election qualifiers to the actual election clause/sentence; separate future termination must neither erase the present positive nor create a premature negative. Add completed-election plus independent future-termination regression and a loader/universe test asserting the genuine positive is present and unchanged until an independently dated completed termination. Keep pending initial elections, conditional positives and pending terminations Unknown. Do not infer an earlier source clock from a historical effective date.

### Verification and ⚠️ Cannot verify from diff

Reviewer execution consisted only of two focused offline Python stdin checks under `PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`, interpreter `atx-db/.venv/Scripts/python.exe`: five direct parser cases, and the header-mutation reproduction using `runpy.run_path('atx-db/tests/test_issuer_class_replay.py')`, its temporary synthetic lake and pytest MonkeyPatch. Both commands completed exit 0 and reproduced the defects described above. Synthetic fixture files were temporary; only this assigned findings artifact persists. No source/test files were changed. Supplied suite results (76 tests/14.46 seconds; broader 114 tests/14.32 seconds) were not rerun and are reported evidence, not reviewer execution.

Final required measurement remains: guarded canonical replay of all retained planned sources with exact raw hashes/current code identities; explicit supersession of the intermediate 704-original parser result; complete source manifests and actual SHA/size/key/clock verification; final universe build followed by all output SHA/size/file-set verification and input/code binding under R9; current hand/sample evidence binding and any necessary changed-claim recheck; all five binding gates and annual 2020–2025 30-field coverage tables measured with exact commands/receipts. French 10/60 FAIL remains open until independently dated source remediation and remeasurement or a concrete controller carry-forward ruling. Later 2023–2026 status refresh and documented unknown/source coverage obligations remain explicit. No return statistic or aggregate return product at/after 2023 may be computed to close these checks.

### Assessment — Task quality: Needs fixes

**Engineering prebuild approval: Needs fixes**, because R3-1 can publish a source/clock snapshot that its retained original no longer verifies, and R3-2 suppresses an unambiguous completed election. **Task spec acceptance: Needs fixes/pending final measurement**; fixing these engineering issues does not approve stale universe publication or waive retained French I4. Task 2x and previously closed unrelated findings remain unchanged.
