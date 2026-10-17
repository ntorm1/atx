# P9-H native driver and read plan

Before any H measurement, R15-WAVE-COMMIT and R15-WAVE-READ govern this plan. Registration `818e0750`
predates those rulings; it remains unchanged. Native driver source commits `5352c820` and `5bbe1231` only
add optional literal Git metadata and reject nonnegative/targetless exclusions. Frozen producer exes remain unchanged.
Independent review APPROVE `a8f06490` and PM R15-H-LAUNCH released attempt1, which stopped before any data phase
on the retained textual requires. R15-H-READY closes those four prerequisites; R15-H-RESTART authorizes the
two metadata clones committed at `708ab6ce`. Attempt2 remains unlaunched pending PM verification of the
committed preparation handback. Original registration, generated spec, console and all eleven stopped files remain.

Exact environment in the native wave process:

```text
PYTHONDONTWRITEBYTECODE=1
ATX_WAVE_COMMIT_TRAILER=Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
ATX_WAVE_COMMIT_EXCLUDE=:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache
```

The code's supported CLI remains the measurement route, from C:/atx-wt/pool-2:

```text
"C:/Program Files/Python312/python.exe" -u scripts/research_cycle.py wave run scripts/specs/p9/waves/p9h-cov-hedge-attempt2.json --seal-allow 20261003=R15-SEAL-C
```

The R15-SEAL-C suffix above follows the actual preserved-source rescan: eight logs, exactly two compact
20261003 hits in the mandated standalone Git exclusion argv token, both class C, OTHER=0. The checker passed
21 focused controls, including exact-span/token/source boundaries and unchanged A/B behavior. This allowance
covers only that provenance source. Any other source remains a stop even if it contains the same date token.
Exact clone hashes/delta are in `cells/p9h-attempt2-registration-commit.txt`; final control, actual scan and
preservation evidence are `cells/logs/p9h-sealc-controls-final.txt`, `p9h-sealc-attempt1-rescan-final.txt` and
`p9h-attempt2-preservation.txt`.

State free RAM and C: space immediately before launch, C: >=15 GB; no RAM wait or optional admission/host-budget
arguments. Registered phase caps and the 512 MiB floor remain. No auto retry, cache index or engine timing flag.
Every actual data stage remains the native bounded route. No custom executor, hand calibration or extra statistic.
Run the native command with stdout/stderr redirected unread into `cells/logs/p9h-attempt2-native-console.txt`;
save outer wall/exit in `cells/logs/p9h-attempt2-native-wall.txt`; expose only
exit code and externally measured elapsed seconds. Poll the raw console only through `cells/tools/p9h_read_filter.py
metadata <raw-log>`, which reconstructs allowlisted stage labels and top-level receipt seconds/file hashes.
It echoes no raw line, arbitrary error payload, verdict, return, dSR or IC figure. Any failure stops immediately;
inspect only the specific failed stage's diagnostic needed to identify/refine the stop and report it to the PM.

After successful native completion, in order:

1. Source-classify all seal hits; verify frozen pins/CRT, input identities, ledger count/prefix, generated commit
   scopes/trailers and cache flags/content scope. Inspect only the verify receipt's six mechanics rows and their
   fixed v8-mech limits, plus binding/seal/exe metadata. Any failed gate stops before paired figures.
2. Run `cells/tools/p9h_read_filter.py rho`. It reads the existing bundle, checks its hash against the judge
   receipt and the frozen eval producer, and verifies exact H/L paths. It prints only paired.sessions, rho,
   one_minus_rho and degenerate plus provenance. Save its output as `cells/logs/p9h-attempt2-rho-first.txt` and record it
   before opening any other paired figure. No engine/statistical run is involved. `nav_summ.py:259-272` omits
   one_minus_rho and degenerate for nondegenerate pairs. Under R15-V2-RHO both absent means the threshold flag is
   FALSE, explicitly inferred from that documented encoding, while numeric stable difference stays `not supplied
   by bundle`. A supplied degenerate key must be true with a valid supplied stable difference; inconsistent
   encoding, invalid structure/producer/pair or a rounding-boundary concern stops before other paired figures.
3. Read only pm7-34's deciding dSR and mechanics and the registered paired SE/p lines, then H's registered
   printed rows: IC hedge trace per theme/block, turnover per gross, C++ cost per traded dollar, capacity 4x net
   Sharpe. Predictions compare C++ S2 ann_vol ratio <1 and ann_mean_gross ratio in [.90,1.10]; ex-ante line stays cut.
   C.11's degenerate fallback remains point dSR>0 plus mechanics; refused SE/t/p are na and G-B7 skips that step.
4. Appendix A with registered counts/producer, output and receipt hashes, bridge flag and void checklist;
   record native judge's one automatic trial and exact before/after ledger evidence. No manual trial write.
5. Run `research_cycle.py scoreboard --results build-equity/waves/p9h-cov-hedge-attempt2/wave-result.json --ledger
   build-equity/trials.jsonl --timings`, preserving full output unread and extracting only timing sections.
   No default all-wave glob or unrelated historical results. Record actual roster size, one rule/zero admission
   candidates, process/phase/stage timing and external wall. Assess <=300s target/<=600s hard factually; reused
   phase receipts are identified separately from this invocation. Add one factual integration-log row.

G-P1 zero-spec-edits-on-retry is UNMET under R15-H-RESTART. G-P1 is not awarded by a later successful run.
Read filters are postprocessing only and never orchestrate a research stage.
The actual kind rule has 67 inherited candidates, zero new candidates and one registered rule, counted from the
pinned library configuration before launch; it is not equated to the plan's 73-member add-alpha example. Cache
before launch has 686 files, no index.sqlite and no research-store. Any stop/checkpoint
or required retry is recorded and cannot be presented as an uninterrupted end-to-end pass. P9-M remains unreleased.

Policy validation: exact node `scripts/tests/test_wave_commit_policy.py::test_wave_commit_policy_real_git` under
`-m pytest -q -p no:cacheprovider --basetemp C:/Users/natha/AppData/Local/Temp/c1hcommit3c` passed (1 test).
Real Git scope/trailer/unrelated staged files, no-op reuse, default/empty argv and invalid exclusion are checked.
The initial test fixture lacked Wave's thread state and failed before Git mutation; corrected fixture then passed,
and the same test passed again after negative-exclusion validation. All three logs preserved; no suite or build.
