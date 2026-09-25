# Independent review: replay disclosure at consumer boundaries

Decision: **APPROVE** the scoped correction at
`1bdeed388b8819d539175df65c202fd33b6f2d51`. The integrated whole-impl gate remains
root-owned. This review does not approve diagnostic performance as alpha evidence.

Reviewed the complete three-file patch, surrounding nested-report validation,
fresh-directory/pending-marker failure handling, and final manifest publication.
Baseline verifies the replay summary against the nested manifest before copying
it; book now does the same. Both propagate all ten eligibility/count/PnL fields
without changing signed stress losses or numerical replay performance. Assumed
liquidations fail qualification in the summary, manifest and stage metadata;
baseline's independent shape failure remains intact. Each top-level summary is
included in the outer manifest's hashed files before final identity computation
and publication. Parse, hash or publication errors preserve failure handling and
do not release the pending marker as a successful artifact.

No blocker or high-severity finding remains in this patch. The existing synthetic
tests check all ten copied fields, exact nested performance equality, ineligibility,
negative assumed PnL, failed qualification/reason, and both root summary hashes.
The no-assumption case still discloses unverified evidence with zero assumptions.

Independently inspected owner's synchronized 43-step build log, two sequential
production PCH-off check logs and final focused CTest log: **32/32 passed**, zero
failures, **40.32 seconds**. Source was clean at report commit
`d4b86cd23581e5d4bba92b02181001cd0dce36f7`; report-only changes follow the reviewed
code. PCH-off checks preceded line wrapping only; the final normal build includes
the exact committed code. Logs are under `C:/atx-wt/pool-4/build-equity/`:
`w0-summary-build-final.log`, `w0-summary-pch-off-baseline.log`,
`w0-summary-pch-off-book.log`, and `w0-summary-focused.log`.

Independent read-only spot run of the built impl executable passed both
`StageEquityBaseline.FixedUnfitBlendReplaysOnlyEvaluationAndBindsAncestry` and
`StageEquityBaseline.CorrectedDefaultCompletesOriginalWindowAndConstrainedBook`:
**2/2 passed, 1.820 seconds, exit 0**. Optional real-data environment inputs were
cleared. Executable SHA256:
`251EF25832F2EF556171A04F8EEB5720157D48DB7BBD05CA1496C74E4E5A662B`.
Receipt: `C:/atx-wt/pool-5/build-equity/w0-summary-independent.log`.

## Benchmark freeze compatibility

The summary correction changes two `atx-impl` stage TUs and one impl test;
`atx-engine-bench` links no impl library. The separately approved native ASan
commit `370f7af4ad3fadf1551ca045c82e85539461b0d1` changes only an opt-in preset,
default-OFF test target, fixture, and runner. Its instrumentation is target-local;
the `equity-bench` preset is unchanged. Root independently executed that ASan gate;
this review did not duplicate it.

Root tree `e51f9550d1e4bc8401b0c5f052eed428cf22fa22` has no differences from the
benchmark production freeze `b185d056440704e7ebcfe2b9395601d7e5264269` under
`atx-engine/{include,src,bench}`, `atx-engine/CMakeLists.txt`, `atx-core`, or
`atx-tsdb`. The disclosure patch is also outside those paths. A final path/hash
receipt will bind the completed benchmark to the integrated tree.
