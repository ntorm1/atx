# Archive3 terminal failure and recovery evidence

Root observed the actual worker9412 disappear at22:02:51UTC, then collected
terminal tool session6840 (exit1), guard receipt and both persisted ledgers.
Archive3 failed at22:02:44UTC after7155.515 seconds. It did not finish the corpus.
Failure was `TransactionContext Error: Failed to commit` at953.6MiB/953.6MiB of
the configured1GB DuckDB budget. The 3GiB guard did not terminate the job;
native process-tree peak1.884941GiB, final host physicalfree7.456GiB and
commitfree10.409GiB. No RAM increase or production restart has been performed.

Read-only recovery used the locked project Python/DuckDB1.5.5 under a2.5GiB
guard (peak0.441486GiB), with UTC/1GB/1thread and bounded report rows. It did
not import atx_db or apply the pending0315 migration. Evidence:
`companyfacts-archive3-failure-inspection.json` and its receipt/logs.

| Measurement | Retained result |
|---|---:|
| Total sec_company_facts / fundamental_points | 38500008 /38500008 |
| Price rows / custom feature rows | 31959271 /31934514 |
| Attempt fact rows / distinct loaded CIKs | 20205629 /3750 |
| Loaded / empty / unavailable / error receipts | 3750 /708 /26 /5 |
| Last loaded CIK | 0001033905 |
| Applied schema | 0314 |

Dataset UUID758f7d5c-73ae-4b66-a8c9-f1996afa163a and activation run
activation-companyfacts-archive3 both already say failed with finish clocks;
manual ledger writes are unnecessary. The4489 receipts include errors and
unavailable members; they are not4489 successfully loaded issuers. The final
attempt row count exceeds the last4475-member progress log because some further
commits succeeded before the failing transaction.

All five persisted source errors were ValueError from int(str(None)) at the
payload CIK validation. Bounded offline inspection of exact source members
confirmed entityName/facts objects with missing top-level CIK, not the empty{}
placeholder. Three tiny records contain cef facts. Their archive filenames
carry validated CIK identity. See `companyfacts-archive3-failed-members.json`.

The persisted options also exposed that activation omitted its explicit User-Agent
when constructing companyfacts options, allowing an unrelated configured contact
into local metadata. This run used the local archive and made no member HTTP
requests. The evidence JSON/log was redacted before commit; future helper output
redacts that field. CF5 includes explicit dummy-contact forwarding. No personal
contact belongs in published artifacts or external requests.

Next action is CF5 under companyfacts-archive3-repair-brief.md: bounded connection
lifetime, verified resume with source/allowlist/retained-evidence checks, recovery
of unfinished unresolved-candidate output, missing payload CIK handling, and
explicit activation contact. Root-only tests/review precede any live resume.
Meanwhile root is running queued P1/Core focused tests sequentially. AF1 remains
isolated until P1 then Core commits. The full universe activation, measured
coverage/quality, CF1 forward-return evaluation and release remain incomplete.
