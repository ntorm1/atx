# Task12 fixround1 scoped independent review

Commit67a5f1ac; reviewer /root/t16_release_foundation, independent of author. Reviewed supplied R1 diff, initial findings, Task12 brief/report and binding R21. Scope I1-I5, changed-code regressions and the separate R21 source-only path; unchanged areas were not reopened.

## Spec Compliance

- **I1 ADDRESSED:** mna_financial_v2.py:39,131-138 adds conditional blockers and requires the supported monetary/stock patterns to consume the full conversion tail. Unknown qualification residue prevents consideration emission, rather than merely clearing the final return.
- **I2 NOT ADDRESSED completely:** exact quote matching fixes the original direct negation and selected contradictory statements, but quote occurrence remains an arbitrary substring of the original. Negated surrounding context can still certify an exact value; see remaining Important I2 below.
- **I3 ADDRESSED:** mna_native_v2.py:48-70 and199-202 derive complete typed auxiliary inventories with original/receipt hashes, URLs, successful status, byte counts and retrieval clocks, and compare them with independent expected inventories. Producer and per-key exact contracts now require those inventories before replay. Missing/extra/self-rehashed replacements cannot silently inherit the old expectation.
- **I4 ADDRESSED:** mna_publish_v2.py:332-341 compares exact independently derived physical schemas before canonical row replay, including empty tables. :351-354 rechecks output/manifest/runtime identities afterward. The same validation remains required for existing and pending-build reuse.
- **I5 ADDRESSED:** mna_runtime_v2.py adds loaded entrypoint/code fingerprints, actual compiled reader/binary/shared-library and Python identities, and exact New York TZif bytes. Native acceptance now uses those explicit bytes via ZoneInfo.from_file. Independent complete runtime dictionaries are included in before/after and public admission identity comparisons.

**R21 source-only policy: implemented in the reviewed changed path.** mna_exact_v2.py:47,82-88 recognizes the separately frozen source-only-native-dlret-r21 policy and removes only the construction holdout reason, preserving all remaining economic/source errors before division. The old pre2023 policy still leaves later values NULL. Publication includes dlret_policy and is_forward_label=False; terminal complete_window_eligible is unchanged and requires exit<2023-01-01, preserving complete forward-label boundaries. No evaluation API was relaxed by this diff. Raw source values do not prove residual-month treatment or complete labels.

Original full2267 population, continued candidates, 80% numeric gates and independent200>=95% handcheck requirement are unchanged. The superseded44.24% metadata ceiling is correctly retained as history rather than a release constraint. Actual source semantics, admissions, coverage/gates and source runs remain unqualified/unmeasured. No fixture or runtime/code hash constitutes actual economic acceptance.

## Strengths

The repair separates byte identity from semantics, preserves NULL qualification failures, binds auxiliary clock evidence to the immutable external contract, and strengthens physical-schema/resume admission. The R21 change is a distinct construction policy rather than removing the forward-window cutoff.

## Issues - Critical / Important / Minor

No Critical finding.

### Remaining Important I2 - Negated quote context still becomes economic proof

`atx-db/src/atx_db/alpha_panel/mna_price_native_v2.py:74,85-87`: fullmatch applies to the caller-selected quote, while original-document matching only checks substring occurrence. The contradiction expression does not cover negation outside that substring. An original economic fixture changed from:

`close is the unadjusted raw traded close.`

to:

`It is false that close is the unadjusted raw traded close.`

Keeping the selected quote as the original affirmative substring and independently pinning the changed synthetic original bytes still yielded dlret_exact=True and dlret=0.2 through full mna_exact_v2.replay_exact. The raw-basis claim is explicitly false in the original document, so quote/hash identity and exact quote syntax cannot establish raw-close comparability. Public native replay repeats the same economic error.

Fix: bind and validate the supported affirmative statement in its complete original sentence/context, refusing negative/conditional/contradictory scope rather than accepting a selected substring from such a sentence. Extend corresponding raw/conversion proof fixtures with scoped negation; preserve NULL/unqualified output on unsupported semantics. This is the original I2 trust boundary, not a demand for unavailable production sources.

No other new scoped Important/Minor findings identified.

## Validation evidence

One new narrow synthetic pytest case ran in a temporary file outside the workspace, using OPENBLAS_NUM_THREADS=1, with:

`atx-db/.venv/Scripts/python.exe -m pytest <temporary>/test_context_risk.py -n0 -o addopts= -p no:cacheprovider -q`

Result1passed in2.32s. The case intentionally asserts the erroneous acceptance above; it is a reproduction, not a product gate PASS. No author suites or unchanged tests were rerun. No actual source/data/hash/metadata/HTTP jobs were performed. Temporary fixture removed automatically; this report is the only persistent written file. No producer changes, commits, agents or live jobs.

## Assessment

**Task quality: Needs fixes.** I1/I3/I4/I5 are addressed and R21 construction/evaluation separation is implemented, but I2 still allows explicitly negated original price-basis evidence to certify an exact return. Actual source/gate certification remains separately open.
