# Independent review: A0 provenance and named SEC execution metadata

Spec verdict: **PASS**. Quality verdict: **PASS**, scoped to these bounded metadata controllers and their retained evidence. No corrections required. This review does not certify source payloads, a producing commit, D0 reproduction, SEC request accounting or any release gate.

Reviewed `p9-a0-manifest-preflight-v1.py`, `p9-a0-manifest-preflight-v2.py`, the selected evidence fields of `p9-a0-manifest-preflight-v2.json`, `p9-a0-manifest-preflight-report-v1.md`, and the three named guard receipts. Also reviewed `sec-text-additional-logs-audit-v1.py`, selected result fields, `sec-text-additional-logs-findings-v1.md` and its guard receipt. No controller was executed during review; no warehouse/source payload, original execution log or Parquet was opened, and no fresh source hash, HTTP request, fixture or index operation was performed.

## A0

The fixed, guarded controllers restrict reads to named JSON metadata and code, reject reparse paths and oversized inputs, hold the initial bytes and perform independent final reads. Controller v2 checks the exact twelve r4 declaration names in both retained manifest and build. Read-only historical Git objects are comparison evidence; raw, LF and CRLF comparisons are explicitly diagnostic and do not relax native byte guards.

The latest result supports seven metadata objects totaling 296,141 bytes, seventeen distinct code files and the exact requested published bridge manifest SHA256 `09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01`. Eight of twelve r4 declarations match historical raw or CRLF bytes; the four reported mismatches are `_fundamental_clock.py`, `identity_reconstruction.py`, `market_daily.py` and `market_owner_bridge.py`, in both declaration groups. The common.py mismatch and absent backfill/name code identities remain visible. The recorded HEAD and Stage I pass=true are producer declarations; the report correctly leaves the supported producing commit unknown and D0 unmeasured.

Receipt version and controller version are distinguished correctly: guard attempt v1 refused before a child at low free commit; attempt v2 ran controller v1, completed 0 in 1 second with peak 65,548,288 bytes; attempt v3 ran controller v2, completed 0 in 2 seconds with peak 65,740,800 bytes. The successful attempts report no cap hit. The refusal is not a metadata or producer failure. No rebuild, bridge overwrite, source admission or gate promotion follows.

## SEC execution metadata

The audit fixes the two additional log paths and sizes and the independently expected prior-audit identity, bounds individual lines and retained records, preserves unclassified/unkeyed records, and independently rechecks all named inputs before publication. Invocation grouping uses guard PID, admission time and the full command, preserving locations and distinct terminal records rather than counting snapshots as requests or inferring missing exits.

The result supports 107 plus four additional guard snapshots and six progress records, with no unclassified additional line. Together with 517 prior snapshots, it retains 628 guard records, including 138 explicitly unkeyed records. It identifies ten named invocations with one observed terminal each: nine low-commit stops and one completion. The newly identified guard 16428 and 10832 stop durations, 1,177.2 and 71.9 seconds, agree with the report. The audit guard completed 0 in 1 second at 29,757,440 peak bytes without a cap hit.

These named records do not prove complete invocation coverage, the original 25,762 baseline interval, rate-lock coverage, retries or requests lost before receipts. The result correctly retains complete_invocation_inventory=false, all_attempt_count=null, budget_upper_bound=null and remaining_budget_granted=0. The original 60,000 ceiling and SEC HTTP hold remain unchanged.

The smallest next steps are the report's bounded preparation/OPS/review-chain and exact producing-code recovery for A0, and reconciliation of the baseline interval and complete producer attempt-accounting evidence for SEC. Neither is authorization for an actual rebuild or HTTP operation.
