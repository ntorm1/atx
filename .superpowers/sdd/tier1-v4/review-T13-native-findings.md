# Task13 initial independent review

Reviewer /root/t16_release_foundation, commit539ee6ff: Needs fixes. Original90%,
95%, and200 handchecks with>=95% agreement thresholds remain unchanged/unmeasured.
No Critical findings; five Important engineering findings. Actual source/native
handcheck prerequisites remain open and must not be invented.

I1 distributions.py50-78: first USD/share amount is selected without old/new
clause distinction. Focused increase fromUSD0.50 toUSD0.60 emitted0.50 and annual
2.0 instead of0.60/2.4. Associate the amount with the actual declaration/change
clause; ambiguous multiple amounts/classes must refuse rather than take first.

I2 distributions.py113-115: daily cutoff uses preceding calendar day, making a
Saturday announcement visible Monday rather than enforcing preceding exchange
session Friday22:00. Require independently admitted calendar for previous session
and monthly stamped-session convention.

I3 events_buyback.py293-326: only exact column captions are inspected for scale.
A surrounding table declaration of shares/authorization amounts in millions was
accepted as2shares/$5. Resolve full table unit/scale scope or reject scaled and
ambiguous sections with keyed unsupported reasons.

I4 distributions.py185-202 and events_buyback.py402-421,450-486: distribution code
identity first captured after extraction; buyback publication trusts parsed-file
SHA without native objects/catalog/parser at-compute provenance. Fixture publishes
manufactured rows using only a hash receipt. No public validator independently
checks complete reader/code provenance, schemas/keys and native semantics. Bind
exact reader/code before computation, recheck after, require authenticated parse
provenance and read-only public admission/replay; reject stale/abbreviated proofs.

I5 distributions.py169-177,194-202: duplicate catalog records publish2rows sharing
1event_id with statuscomplete. Reject conflicts or explicitly reconcile exact
duplicates/counts, and independently enforce nonnull unique keys before any
manifest/current publication.

Strengths: unknown cadence/special/class/currency does not invent annual rates;
duration XBRL facts separate; executions not first authorizations; immutable older
products preserved, actual gates FAIL. These do not resolve the five defects.
Two focused offline checks completed2.01s and2.80s. Author tests not rerun. Review
read-only, no actual source/data/hash/HTTP access or live jobs. Root fixround1/5.
