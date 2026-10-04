# Task13 fix round1 independent review

Reviewer /root/t16_release_foundation reviewed8f80ae3f: Needs fixes. I2-I5
addressed (exchange sessions, scale scope, native/code/readers at-compute and
public replay, nonnull unique keys). I1 partially addressed; new Important I6.
Actual source/class/clock semantics remain unqualified,0PASS/3FAIL.

I1 remains open, distributions.py57: the new amount matcher makes per-share
suffix optional and accepts a per-share phrase anywhere in the sentence. Native
fixture declaring a regular quarterly dividend totalingUSD600million with the
per-share amount to be determined emitted amount600 and regular_rate600, helper
annual2400. Production clears annualrate but retains wrong monetary fields.
Require a locally attached per-share unit (narrow explicit from/to exception),
reject aggregate scales and unresolved per-share values. Native replay cannot
correct a repeated economic parsing error.

I6 distributions_native.py144 and events_buyback.py397/464: document-hash
partitions cause first-announcement comparison to omit earlier authorizations
in other partitions. Focused SQL fixture marks later filingTrue alone versus
False with same authorization one month earlier. Validator repeats incomplete
comparison. Admit complete prior-history dependencies or defer classification
until all required partitions are available; incomplete meansunknown, notfirst.

Strengths: stored/original bytes checked before normalization; native public
replay/joins reject manufactured receipts; unqualified semantics and NULL rates
retained. No author suite/actual sources/data/HTTP, repository edits or live jobs.
Root dispatches original author fixround2/5; thresholds/denominators unchanged.
