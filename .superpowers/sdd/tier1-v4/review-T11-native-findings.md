# Independent Task11 native checkpoint review: Needs fixes

Reviewer /root/t5_prebuild_review inspected0c92521b,29d2dd29,cb0e1137 against
Task11 brief/R16/owner addendum/GC. Four Important defects in implemented behavior;
unfinished full-family work and actual acceptance remain separate.

## Strengths

Source/submissions inventory is bounded and explicitly unqualified, not claimed
as retained13Ffiling bronze. Missing units/manager chains/amendments fail visibly.
NPORT event vintages retain original and late prior-report effects; return math
stays in pre2023 windows and missing flow components stay NULL. Insider rejected
prices retain monetary missingness through monthly aggregation.

## Important I1: historical coverage accepts future cutoffs

thirteenf_features.py:329-334 checks cutoff type/nullness but not relationship
to session; :264 uses it for visibility. Repro: session2020-05-18, cutoff2026-05-15,
feature published2026 yielded one qualified2020cell. Independently validate exact
cutoff against pinned calendar and declared daily/monthly policy; reject future/
mismatched clocks and test future-year/same-session cutoffs.

## Important I2: missing insider filing clock fabricates public clock

insider_measures.py:89-90 greatest(t.available_at, transaction_date+22h) ignores
NULL in DuckDB. Qualified-price trade with NULL filing availability becomes
available2020-05-01T22:00Z/value1000 without public filing evidence. Preserve
missing clock/exclude PIT aggregation, or separately evidence a conservative
filing floor. Test NULL-source-clock alongside actual same-day acceptance.

## Important I3: cohort completion ignores source disqualification

thirteenf_features.py:61-70 requires qualified_original but not source_qualified
for originals; prior/current originals with source_qualified=False yielded
prior_visible_baseline_complete=True. Both flags retained at:135. Require
affirmative source qualification for both original populations and arrived-cohort
completion. Missing/false remain incomplete; fix positive fixtures which omit
source qualification while expecting completion.

## Important I4: missing amendment/notice and clock lineage

thirteenf_features.py:49-56 visible amendments/notices block completion, but:73-74
source_accessions records selected originals only. Visible unqualified restatement
made completion false without its accession in lineage. Contributing source clocks
are also absent. Preserve relevant amendment/notice accessions, clocks and blocking
reasons plus selected-original clocks. Test immediately before/after public clock.

## Verification and assessment

Tiny reproductions128MB/one thread. Four selected positive regressions passed in
85.06s total: NPORT late amendment, coverage denominator/current-quarter, insider
price boundaries and monthly missingness. Existing tests do not cover defects.
No full suite, actual sources/hashes/acquisition/data jobs; no live review jobs.
No Critical findings. Scoped code quality Needs fixes. Full Task11 acceptance
NOT_MEASURED: bronze, type coverage, NPORT membership, stakes, insiderp99.9,
all-three-universe coverage, native position assembly and finalR15/v11 binding.
