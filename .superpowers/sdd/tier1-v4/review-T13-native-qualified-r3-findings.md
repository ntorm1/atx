# Task13 native qualification round3: independent scoped review

Commit52bb3669; reviewer /root/t12_terminal_events (GPT6.1 Sol), independent of
Task13 author. Scope previous continuation I1-I4 and regressions in supplied
review-T13-native-qualified-r3.diff. Prior R2 addressed findings and unchanged
foreign/shared code were not reopened. Read supplied diff, prior findings,
current changed modules/tests and Task13 report; prior brief/gap/Task4/R21/global
constraints remain binding. Exact changed worktree paths match reviewed commit.

## Spec compliance

- I1 NOT fully addressed. Dividend-local regular status/cadence now avoids the
  unrelated annual-meeting example and supports honest NULL for the named
  subject-to/if/conflicting examples. But qualification still depends on a small
  condition keyword list rather than supported unconditional native semantics;
  remaining Important I1 below reproduces false/conditional positive rates.
- I2 ADDRESSED in the supported narrow changed route. accepted_clock matches
  native authoritative accession/issuer and a unique native Accepted stamp;
  exact original index member URI or SGML matching FILENAME/TEXT body is required.
  Ancillary receipt URIs must identify the actual accession index/full submission.
  Related-accession prose and caller metadata alone no longer substitute.
  Unsupported layouts remain unqualified, not fabricated original-clock facts.
- I3 ADDRESSED. Reconstructed acceptance/final availability are checked against
  known native declaration date before emitting a qualified annual rate. The
  production future-date fixture retains valid earlier price/link evidence so
  missing linkage no longer masks the chronology test.
- I4 NOT fully addressed. Explicit independently pinned TZif and compiled
  ZoneInfo.from_file remove ambient TZPATH/cache drift. New pointer checks catch
  replacement entrypoints, but miss changed code inside the same loaded timezone
  parser function; remaining Important I4 below publishes a changed UTC clock
  with unchanged independent expected identities.

Fixed annual source value remains distinct from separately unqualified quote
currency and incomplete carry/history. Original gates0PASS/3FAIL-unmeasured,
native buyback checks0/200, complete global history/cancellation/carry admission
and vendor/member-line cash denominators remain unchanged and openly missing.
No fixture/engineering result certifies actual source acceptance or full Task13.
R21/source construction does not relax PIT or authorize evaluation/forward labels.

## Issues

No Critical finding. Two remaining Important findings; no new unrelated findings.

### Remaining Important I1: Unsupported scope still certifies unconditional rate

Locations: atx-db/src/atx_db/alpha_panel/distributions_qualification.py:133-140,167;
production promotion distributions.py:179-188. The new global regex recognizes
some conditions but ignores others and ignores scoped negation. With the supplied
native original fixture, suffix `, only when earnings targets are achieved` or
`, as long as regulatory approval is granted` still produces a qualified2.4
USD/share/year rate. Prefix `It is false that ` also qualifies2.4 even though the
original explicitly denies the declaration. Each input still matches the local
cadence and the extraction replay; those checks establish reproducibility, not
an affirmative unconditional fact.

Production reproduction: use the existing inputs helper with its original
statement suffixed `, only when earnings targets are achieved`, update the native
declaration body, and run the supplied production test's positive case in a
fresh temporary directory. Every positive producer/public assertion passes,
including conditionally_qualified_events=1 for the conditional2.4 rate. All
original/receipt/admission pins derive from the changed source, so this is not
stale bytes or a forged output. Other existing negative/tamper assertions also
complete. The as-long-as and explicitly false variants were reproduced directly
through native qualify; no actual source was read.

Fix: bind fixed-rate qualification to complete supported affirmative unconditional
original clause/context. Unknown conditions, negative/quoted/disputed scope must
preserve NULL/reasons; extending a finite keyword list alone cannot establish
truth of selected phrases. Preserve the dividend-local cadence fix and positive
supported declarations. Add the named variants through production and independent
public replay, including forged-positive rejection.

### Remaining Important I4: Same loaded timezone function can change under frozen pins

Locations: distributions_qualification.py:216-229;
distributions_native.py:18-39. assert_runtime compares
`_zoneinfo_common.load_data is _COMMON_LOADER`, but does not verify its actual
loaded __code__. A function keeps its identity when its implementation changes.
On-disk parser hashes and all pointer checks consequently stay unchanged while
ZoneInfo.from_file consumes changed parsing semantics.

Narrow reproduction with original loaded function identity preserved:

```python
loader = Q._zoneinfo_common.load_data
saved = loader.__code__
pins = N.code_pins()
def utc_loader(stream):
    return (), (), (0,), (0,), ('UTC',), b'UTC0'
try:
    loader.__code__ = utc_loader.__code__
    assert N.code_pins() == pins
    # Same admitted New York TZif, original index and declaration now give10Z.
    assert Q.qualify(event, text, **args)['available_at'].hour == 10
finally:
    loader.__code__ = saved
```

Production reproduction injects this change at the supplied normalizer_drift
fixture's hook after the v2 admission is already frozen, rather than replacing
Q.ZoneInfo. The expected-rejection assertion reports DID NOT RAISE because build
succeeds. Public validate_product then reports one qualified2.4 rate with
available_at2021-01-04 10:00Z (correct unchanged implementation gives15:00Z), and
expected admission code_sha256 equals N.code_pins throughout. Original TZif,
source/admission files and on-disk code are unchanged. Restored parser code in
finally; no persistent runtime alteration.

Fix: independently freeze/check actual loaded parser semantics, not only its
function pointer/on-disk file. Include stable loaded-code identity for the
normalizer dependency and verify before/after producer and public replay.
Reject same-function code drift under the unchanged full expected admission.
This is the previously named new normalization trust boundary; unchanged other
readers are not reopened.

## Assessment and validation

Spec compliance: Needs fixes. Task quality: Needs fixes. I2/I3 addressed;
I1/I4 remain. Strengths include real original filing/member/issuer bindings,
chronology checks after native clock reconstruction, admitted explicit TZif,
and relevant missing-proof/forged-positive regressions. Rule v4 refreeze and
actual external gate/prerequisite reporting remain honest.

Targeted synthetic inline `python -` only, from C:/atx/atx-db with
OPENBLAS_NUM_THREADS=1 and PYTHONPATH=src using .venv/Scripts/python.exe. Imported
existing test_distributions_qualification.py helpers read-only. Direct named
scope and loaded-loader probes completed in2.15s. One targeted temporary
production/public invocation per remaining risk completed below60s; original
adapter DuckDB128MB/one thread. No broad or author-suite reruns; author report's
interrupted combined invocation is not counted as a pass. No actual source,
data/hash/metadata/HTTP jobs, commits, producer edits, agents or live tools.
Temporary synthetic files were deleted, and changed loaded parser restored.
This findings report is the only written workspace file. Full original numerical
gates, denominators and 200 native handchecks remain unmeasured/incomplete.
