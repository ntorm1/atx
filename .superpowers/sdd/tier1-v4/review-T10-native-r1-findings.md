# Independent Task10 repair-round1/5 rereview — 312c42f6

Scoped requirements/spec verdict: PASS for repairs I1–I3. Scoped code-quality verdict: PASS. I1, I2 and I3 are addressed; no new Critical or Important repair regression found. This verdict closes the three engineering findings only. Original Task10 acceptance remains 0 PASS / 7 FAIL-unmeasured, independent source handchecks 0/300, source_quality_certified=false and NOT_PRODUCTION_READY.

Scope: reviewed the prior findings, full 639-line review-T10-native-r1.diff, latest Task10 report R1 section, Task10 brief/R22, changed production code and new repair test source. Checked availability of graph discovery tools; none were exposed, so used targeted static reads/rg. Accepted recorded author test outcomes without rerunning unchanged suites. No new repro was needed. Only this report was written; no product edits, commits, actual dataset/source/hash/metadata/HTTP/return jobs, cleanup, agents or worktrees. No live jobs were started.

## I1 — addressed

Exact locations: `guidance.py:816-825`, `guidance.py:877`, `guidance.py:1013`; `guidance_native.py:52-53,70-71`; repair tests `test_alpha_panel_guidance_review_r1.py:33-46`.

The parser retains resolver explicitness separately from source origin. Table candidates now preserve `_pb` as extras.period_resolution instead of losing it under the table label; heading candidates preserve the resolved stated/inferred decision before replacing period_basis with heading. Native admission consumes this resolution while publishing the original period_basis and separate period_origin. Therefore explicit Q/FY table and heading labels can qualify through the existing unique independent evidence checks; inferred labels remain NULL. Candidate identities include the added provenance and code/config identity rejects old native generations/units.

Evidence: two recorded public parse/build/validate repair cases assert qualified table/heading endpoints, native revision and EPS surprise, exact origin/resolution and unchanged independent yearly denominator. Two inferred table/heading cases assert refusal and no invented comparator identities. Existing legacy parser tests reportedly remain 13/13 passing. Static review confirms the producer-to-native propagation and does not find a simple whitelist that would admit inferred table years.

## I2 — addressed

Exact locations: `guidance_native.py:54-67`, `guidance_native.py:153-154`; `guidance_kernels.py:17-30`; repair tests `test_alpha_panel_guidance_review_r1.py:48-72`.

Original stated target provenance now survives numeric-value refusal in comparator_identities. Independently clock-qualified proof mappings retain their union; consensus metadata remains visible without pretending conflicting identities form a qualified mapping. prior() includes affected original observations before choosing the latest clock, so duplicate/conflicting latest evidence reaches the existing unusable-midpoint refusal rather than disappearing behind exact comparator equality. Explicit source share language constrains the blocker, and different fiscal targets stay unaffected. A later uniquely qualified issue can supply subsequent comparisons without rehabilitating the earlier ambiguous issue.

Evidence: recorded native parse/build/public replay cases cover duplicate and conflicting accounting mappings, NULL EPS surprise, NULL later revision, preserved common identity, separate conflicting identities and unrelated quarter/basic-share exclusions. The new SQL retrieves at most257 proofs and explicitly refuses count>256 rather than silently truncating the affected identity union. Original missing-source releases remain in audit/sample; the newly frozen documented policy gives genuinely unknown targets no invented supersession identity. This closes the earlier reproduced stale-fallback defect while preserving explicit uncertainty as missing numeric values.

## I3 — addressed

Exact locations: `guidance_native.py:339-349`; repair test `test_alpha_panel_guidance_review_r1.py:76-81`.

Public replay now explicitly refuses NULL/absent-source issuer IDs, retains exact per-issuer multiset equality, accumulates the complete regenerated count and compares it with the global published feature count. Thus an extra absent-source issuer cannot bypass regeneration, and global population checks complement authenticated bytes/schema/rows.

Evidence: the recorded regression adds an absent-source issuer row, updates its legitimate producer file records and immutable/active fixture manifests, proves physical publication validation still succeeds, then requires native semantic validation to reject the orphan. Static inspection verifies the checks sit in the public feature-validation path before the precision replay/source recheck.

## Validation and retained acceptance limits

Author-recorded commands accepted without rerun (OPENBLAS_NUM_THREADS=1, PYTHONPATH=C:/atx/atx-db/src; fixture DuckDB128MB/one thread; each invocation<60s):

- `atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_alpha_panel_guidance_review_r1.py -n0 -o addopts='' -p no:cacheprovider -q --tb=short` — 8 passed,27.76s.
- `atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_alpha_panel_guidance.py -n0 -o addopts='' -p no:cacheprovider -q --tb=short` — 13 passed,1.53s.
- `atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_alpha_panel_guidance_native.py -k frozen_artifacts -n0 -o addopts='' -p no:cacheprovider -q --tb=short` — 1 passed,18 deselected,2.03s.

The declared R1 config is 94358ee94edc83b699bf00cc64216d61eac1a63b89cdf92d39cfa96adca26a33. Diff inspection confirms pre-measurement refreezing adds provenance/blocker/replay policy and the proof bound; EPS-only dimensionless surprise, native-unit revision, strict source issue/prior-price clocks, primary candidate/sign, precision300/90%, independent six-year member denominators and holdout limits remain unchanged. Fixtures provide engineering evidence, not actual original-text judgments or source certification. All seven actual gates and original manual0/300 remain open. Whole-task repair usage remains round1/5; it is not reset by this scoped PASS.
