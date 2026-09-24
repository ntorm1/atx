# DS2 independent review by parent Codex

The fresh review subagent failed before producing a review because its usage
limit was reached. The parent, who did not implement DS2, performed this one
independent static pass against the brief, FQ1 producer/validator, four DS2
files and their focused tests. No runtime or live measurement supports this
review. No Critical finding; the Important fixes below need implementer
reports and focused evidence, without another review cycle.

## Important

1. **Latest-session features are incorrectly gated on a future entry bar.**
   FQ1 `_publish_signal` stores `eligible=false`, score NULL and reason
   `missing_next_session` when an observed decision session has no later
   observed entry session by the build snapshot. Its accounting input can
   still be fully qualified. DS2 selects the latest decision session but
   `four_eligible` requires all four research scores to be eligible, so a
   normal current snapshot can never produce a passing desk screen. Select
   the same latest decision session; qualify the raw accounting input on its
   own `reason='valid'` and lineage status, keeping research score eligibility
   separate. Do not invent a future entry or choose an earlier decision date.
   This descriptive desk question does not require a future return label.

2. **Rejected raw accounting inputs are exposed as metric values.** FQ1
   `_stage_leg` and `fundamental_signal_inputs` intentionally retain `raw_value`
   alongside rejection reasons, including stale, foreign and unverified
   states. The DS2 preview checks only `isfinite(raw_value)`. A validated
   panel digest proves those stored diagnostics have not changed; it does
   not turn rejected raw values into qualified metrics. Expose a number only
   for a qualified input; keep NULL plus the exact bounded reasons/state IDs
   otherwise. Require all four qualified inputs for a complete screen.

3. **The 2 MB cap is enforced after transferring all JSON to Python.**
   `fetchmany(1001)` bounds rows, not JSON bytes. Stored market strings and
   identifiers are not a byte bound. Count bytes in SQL before fetching the
   result, in the same read transaction, or use an equally strict transfer
   bound. Retain final encoded-artifact checking and fail closed on excess.
   Include a small oversized-field case; do not allocate a large fixture.

4. **The runner has no successful end-to-end validator proof.** Current
   tests execute bare SQL, then exercise the real validator only when the
   build is absent; the whole successful `read_screen` path is untested.
   Add one tiny actual FQ1 build/validate/read case (existing tiny FQ1 fixture
   can be reused without editing its module), including its final session
   with no entry bar and a changed panel digest rejection. Also check a fresh
   output path/overwrite refusal and independent absent-market counts. Keep
   the isolated 256MB/one-thread budget and root-only runtime.

The run/date/source predicates, raw directional sign handling, latest whole
market state selection, aggregate-versus-preview counting and atomic link
installation are otherwise consistent with the requested design. No live
coverage, price lineage or alpha result is inferred.
