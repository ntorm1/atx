# CF5: Resume the full companyfacts archive within the existing memory budget

## Measured failure and retained evidence

Archive3 exited normally with a fatal COMMIT memory error at 2026-09-20
22:02:44 UTC: 953.6 MiB of the 1 GB DuckDB allocation used. The 3 GiB process
guard did not kill it; native process-tree peak was 1.884941 GiB. Both dataset
and activation ledgers correctly say failed. No manual ledger repair needed.

Read `companyfacts-archive3-failure-inspection.json` (root read-only probe,
DuckDB1.5.5, peak0.4415GiB) and `companyfacts-archive3-failed-members.json`.
Dataset UUID: `758f7d5c-73ae-4b66-a8c9-f1996afa163a`.
Retained totals:38500008 facts and38500008 points,31959271 prices,
31934514 custom feature rows. This attempt retained20205629 facts across3750
loaded CIKs; receipts also show708empty,26unavailable,5sourceerrors. Last committed
CIK0001033905. Source SHAee099c7394a357f1996c728b7362f25158613b34f2ab1ad270cffc1befb917b6;
allowlist SHA ccfe31cf84b2f8c7eac978cfb19b6fd1e0494410cfcd0f969f98f030a7f6cdaf.

Five source failures are missing payload CIK, not malformed facts objects:
0000741313,0000759828,0000851693,0000855886,0000925683. Payloads have
entityName/facts and valid exact archive member names; three inspected tiny
payloads contain unsupported cef facts. The unconditional int(str(None))
identity check prevents ordinary empty/unsupported classification. Preserve
explicit conflicting/malformed payload CIK rejection. For an absent payload
CIK, use the validated archive member identity with inspectable provenance;
do not add current ticker fallback or accept mismatched identity.

## Required production repair

1. Bound connection/index lifetime across committed issuer replacements. Use
   the existing submissions reopen pattern as a reference, retain analytical
   caps and ensure no transaction/cursor/registered-frame survives a reopen.
   Choose a conservative small fixed interval and explain it. No RAM increase,
   no dropped required primary keys, no broad index migrations or global settings.
2. Add verified archive resume from the prior dataset UUID so the successful
   two-hour prefix is not blindly loaded again. Source digest, allowlist/options,
   exact target identity and successful receipt/retained fact+point evidence
   must justify each skip; empty/unavailable remain distinct and source errors
   are retried. Reject unsafe proof instead of silently declaring completeness.
   Preserve bounded SQL/count/identifier buffers. Consider interrupted commit vs
   missing receipt and overwritten/deleted receipts. No blind numeric offset.
3. Recover/retain unresolved-identity candidate output for skipped completed
   issuers: current loader accumulates candidates until the end, so the failed
   run did not finish that surface. A resumed stage must not silently omit it.
   Keep source/security identity rules and original filing availability intact.
4. Fix the missing payload CIK condition above. Prepare focused real-shape
   fixtures, explicit mismatch/invalid cases, and source completeness counters.
5. Ensure activation forwards its configured SEC User-Agent into companyfacts
   options. The archive run was offline, but persisted options inherited another
   configured contact because stage_companyfacts_load omitted this field. Use
   only the dummy atx-research@example.com for operational tests/commands. Never
   print, copy into reports, or send any configured personal contact externally.

## Ownership / process

Fresh Codex implementer; no subagents. Own actual fundamentals.py and a new
private companyfacts resume/helper module if needed, plus focused companyfacts
tests. No runtime tests/imports/DB/probes/production work: ROOT ONLY runs those.
No git commits or destructive/shared-tree operations.

activation.py remains P1-owned until root validates/commits P1. Prepare a small
separate activation integration patch (resume argument/validation + explicit UA)
and report its intended application; do not edit actual activation.py yet.
Do not touch registry/jobs/migrations, P1/Core/AF1 source, or other task files.

Write `companyfacts-archive3-repair-report.md`: exact paths, bounds, resume proof,
candidate recovery, pending focused commands and actual limitations. Root will
run focused checks once, obtain one independent Codex review, accept Important
fixes on report, rereview only Critical fixes, and commit pathspec-only with the
requested trailer. No production restart until verification/review and coherent
activation integration are complete. Preserve all archive/backup artifacts.
