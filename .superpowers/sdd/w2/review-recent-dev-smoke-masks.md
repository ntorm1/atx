# First real recent role: independent masks-only audit

Scope: read-only inspection of the exact role manifest and its session, instrument,
presence and membership payloads. No price/return fields, source Parquet, warehouse,
signals, performance, build or simulation were read/run. This is artifact QA, not
evidence that the strategy reaches its performance or capacity target.

The pinned manifest at
`C:/atx-wt/pool-2/build-equity/recent-dev-smoke-v1/manifest.json` matches
SHA256 `900839a1ea8e21edc0f5edd5e9cd8f2bc7884a9295a86d5d6f2de19a79aed36b`.
All four inspected payload sizes and hashes match it:

| Payload | Bytes | SHA256 |
|---|---:|---|
| sessions.i64 | 3,688 | f8375cf766422d1256a5f845eebe6d99bd2e6b62f6a8f2da8fc7db0781f6e214 |
| ids.u64 | 31,600 | f8f4e57e6dc1f75dc042de280acb040056eafa9e63de3eadcbe3cfc7bc87756e |
| present.u8 | 1,820,950 | e6d7bdd1e1db6657b1bc6af27a204cdf6755503b57e328043dd4af39fdd495c1 |
| member.u8 | 1,820,950 | c8d85e0deb05c2684bf0aa61f7e333ad34e3f2f2a9cca4cb7fd24666b05be658 |

The role has 461 dates and 3,950 positive, sorted, unique IDs. Dates are strictly
increasing midnight UTC labels spanning 2018-06-01 through 2020-03-31. The declared
score interval is [2020-01-01, 2020-04-01); its 62 observed sessions run from
2020-01-02 through 2020-03-31. All daily scored member counts match the manifest;
their minimum/maximum are 2,872/3,000 (mean 2,942.371). Both masks are binary.

The full 399-date warmup remains present. Its first 63 membership rows are empty,
leaving 336 usable historical-membership dates. Every union ID is a member on at
least one warmup or score date; 654 IDs are selected only during warmup. Thus the
artifact did not compact its instrument union to names selected in the scored
window. This verifies mask/axis preservation, not the underlying prior-ADV
calculation, which requires numerical fields outside this audit's scope.

Membership and physical presence remain independent: there are 356 member but
current-absent name-dates over the whole role, including 36 across 30 scored dates.
For the following diagnostic, an observed eligible name means
`member[d] && present[d]`. Only decision dates with both later sessions inside the
role are counted; the last two score sessions are excluded.

| Decision schedule | Dates | Observed eligible name-decisions | Missing presence d+1 | Missing presence d+2 | Missing either | Distinct affected IDs |
|---|---:|---:|---:|---:|---:|---:|
| Every mature score date | 60 | 176,422 | 35 | 68 | 70 | 35 |
| Every fifth date, starting at score_begin | 12 | 35,294 | 4 | 9 | 10 | 10 |

For every-date decisions, entry/endpoint gaps occur on 29/45 dates; for cadence5,
they occur on 4/6 dates. These counts flag potential strict execution refusals.
They do not prove a particular trade fails: signal finiteness, nonzero targets,
held positions, caps and actual fill state were not inspected. Off-cycle held
marks can also matter beyond scheduled entry checks. These future-presence QA
counts must never alter earlier universe selection, membership or candidates.

The manifest declares common-stock and historical-vintage qualification false;
this remains a research cohort. Its source SHA is declared here, not rehashed by
this masks-only audit. Numeric payload hashes and absent-cell NaN consistency
remain outside scope.

Reproduction: `audit_recent_role_masks.py` opens only the pinned manifest and four
named payloads above. The complete 461-date count receipt is
`recent-dev-smoke-mask-audit.json`, canonical LF SHA256
`3490ff51dd07a060ea1d072b69be6a271f610337381c3fc65a363d1762478a40`.
The derived receipt was explicitly serialized with LF for stable Git provenance;
the original role and all original payloads were untouched.
