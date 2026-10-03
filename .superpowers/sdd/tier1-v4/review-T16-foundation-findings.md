# Independent Task16 foundation review: Needs fixes

Reviewer: /root/t6_factors, independent of Task16. Commit5b86179d supplied full
package; no implementation/index/branch/source-data/report edits. The reported
15-test suite was not rerun. No real acceptance or runtime verified.

## Spec and strengths

Build independently binds finra_names/other reader products and side-product
provenance, verifies producer members, checks typed exact panel keys/year/session/
clocks, retains all nine years, rejects partial publication and preserves legacy
evidence. Native output paths are isolated. Documentation preserves byte versus
economic qualification. French historical10/60 FAIL, false source certification,
NOT_PRODUCTION_READY, pre2023 return boundary and full Task16 remain binding.

## Important I1: gate counters are not bound to payloads

universe_release.py:144-161 and166-175: check_daily checks date sets, fields and
integer bounds against whole-year row count, but not actual admitted rows.
daily_sha256 authenticates the supplied object against itself. Daily-band and
code-coverage gates directly consume these counters. Tiny valid one-row payload
with univ_all_common=true sealed with all counters zero: accepted_wrong_daily_
all_common0 while actual1. Recompute payload-derived daily counts during sealing
and admission, including all-common, NYSE, code presence/missing shares codes.
Independently bind/regenerate diagnostic counters needing native intermediates.
Compare per-day counts/relationships. Add self-consistent wrong-counter negatives;
positive fixtures currently claim rows_without_shrcd=1 with nonnull codes.

## Important I2: validator trusts abbreviated snapshot inventory (R15)

release_inputs.py:119-124, universe_release.py:292-313: recheck rebuilds whatever
contract, code paths and external bindings the frozen document supplies. Public
validate_generation never independently requires reader_contract(REQUIRED), code
inventory or binding/French/hand evidence. Synthetic nine-year generation with
exact valid keys passed with only9panels+calendar, zero code and external binds:
accepted_incomplete_reader_contract10 code_bindings0 external_bindings0.
Enforce snapshot version and exact independently derived reader/code/external
contracts before bytes; reject omissions/substitutions, add full-generation
negative cases. Normal build creates more, but consumer/rollback admission must
independently enforce the promised contract.

## Important I3: unsupported qualification/status accepted (R15)

universe_release.py:292-314,327-336: validator does not require stage, schema,
complete status, NOT_PRODUCTION_READY, BYTE_IDENTITY_BOUND, false economic
certification or original gate definitions. Same synthetic generation passed as
arbitrary_stage / partial / PRODUCTION_READY / source_quality_certified=True.
Rollback delegates to it. Require complete manifest/evidence schema, invariant
qualification, original gate definitions and full measurement populations.
Reject partial/wrong stage/changed criteria/escalated certification while retaining
legitimate measured gate FAILs as publishable candidate evidence.

## Minor M1: coverage adapter root isolation incomplete

universe_release.py:30-45 and unchanged universe.py:502-503: cloned _p/calendar
use supplied root; coverage uses shared C.build_root to discover history. Another
context can choose years from the wrong root. Bind private common-root accessor
or exact panel inventory; extend two-context coverage fixture.

## Minor M2: rollback lock and interrupted two-pointer swap

universe_release.py:327-336: rollback bypasses build writer lock. Interruption
after current.json but before previous.json loses reversible swap. Use same lock
and recoverable pointer transaction/journal plus interruption tests.

## Verification

Two named-risk stdin Python checks with synthetic Arrow, candidate_fixture/
payload helpers and C.connect(memory='128MB',threads=1). First sealed wrong zero
counters and compared actual count. Second built9valid tiny years with omitted
source/code/evidence and unsupported qualification, then validate_generation;
both accepted. Command atx-db/.venv/Scripts/python.exe -, PYTHONPATH=atx-db/src,
OPENBLAS_NUM_THREADS=1. No real files, broad suite, return calculation, HTTP or
live jobs. Assessment: Needs fixes before real build/downstream admission.
