# E2 catalog and column-bin GBT bounded qualification

2026-09-26. Production/configured source `b6b80d2b`; clean combined build
`3e467fce` differs only in documentation. Test-only correction `42677f24`
follows the initial configuration-fixture failure described below.

**41 distinct checks now pass**, including all 18 new owning checks:
GBT7, catalog5, preregistration2, actual IC-stage2 and root configuration2.
No unresolved failure, error or skip remains. These counts overlap earlier
packets and are not additive to the prior 249-check qualification.

| Artifact suffix (`w2-epoch-gbt-…-qualified`) | Result | Native seconds |
|---|---|---:|
| learn | 17/17 | 0.305 |
| catalog | 5/5 | 0.110 |
| flags | 5/6 initially | 0.030 |
| stage | 13/13 | 20.944 |
| flags-fix | corrected case 1/1 | 0.004 |

The GBT checks cover independent squared-error/missing-right arithmetic,
byte-identical forests at one/four workers, histogram subtraction, importance,
date-centered loss, resource refusal, explicit legacy dispatch, actual bounded
dataset fitting, future mutation/maturity and augmentation recipe identity.
Existing dataset and date-CPCV consumers also pass after the exact three-helper
CPP extraction `4f27a8f9`.

Catalog checks cover durable pending declarations, exact-token idempotency,
alias deduplication, retained-cell proof, numerical mutations, stale anchors,
torn-record preservation and admission bounds. Actual IC-stage runs pass both
completed manifest/reservation/terminal bindings and failure finalization when
legacy pre-append or family VM execution fails. The existing legacy stage and
runtime-preregistration checks pass with E2 inactive.

The new flags fixture incorrectly expected `parse_config_file` itself to reject
a wrong subcommand. Existing file helpers deliberately allow partial
configurations; dispatch validates after merging CLI and file settings.
`42677f24` corrects only the fixture to exercise `validate_cross_flags` at that
boundary. Production behavior is unchanged. Equity-IC still refuses `--config`
at stage dispatch; this parser/merge check does not claim stage adoption.
The historical failed XML and receipt remain intact. Only the failed case was
rerun after correction: 42 executions across 41 distinct names.

## Build and attribution

- Production leaves: **51.4449585 s**, Jobs2, seven C++ actions including
  15.3 s configuration. All new production sources compiled on the first attempt.
- Four focused targets: **239.6642778 s**, Jobs3, 52 C++ actions and six links.
  No PCH or dependency rebuild. The broad API batch passed on its first attempt.
- Fixture correction: **24.4074442 s**, Jobs3, exactly one test TU and one link.
  No production rebuild, reconfigure, or numerical change.

These are measured shared-host build durations, not a controlled speedup claim.
No benchmark, actual market payload or long mining run was performed.

The companion JSON binds each source, filter, native exit, XML/log and executable
hash. The complete local case index is
`build-equity/w2-epoch-gbt-qualification-index.json`, SHA256
`c5dd670502014d01cf16fbdbd4020d9882e0e9530c147378ce1053f71ba25a66`.
Three unaffected binaries still match their original receipts. The initial
flags receipt belongs to the pre-correction executable; the relinked executable
matches the correction receipt. No rerun of its other five cases is implied.

## Remaining DAG scope

E2 remains one opt-in declaration consumer, without historical imports,
all-stage migration, compatible-PnL evidence attachment or cluster-N/DSR
reconciliation. GBT remains a bounded selected-window fit with fixed-right
missing routing and no inner-purged early stopping. Large-fit performance,
LightGBM comparison and automatic wrapper/model publication remain open.
This packet establishes no tradeable-alpha or full-wave acceptance.
