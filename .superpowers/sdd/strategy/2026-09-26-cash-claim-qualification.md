# Cash-claim execution qualification and third real rehearsal

At clean source `ab049a505aa530dec2b7b31c5d9aa14ec323b110`, all 32 focused
native cases pass, with zero failures/skips, in 1.738 seconds (guard 1.875s,
sampled peak 18,931,712 bytes). Twelve cases are new: seven engine cash-claim,
four runner claim/admission and one calendar-turnover case. This qualifies
bounded implementation behavior, not event coverage or achieved alpha.

## Integrated sources and review

Core `c0be402e` -> `eb67e4d5`; source clock `1ef2524d` -> `8ae8294c`;
locked-capital correction `440d538f` -> `963adbc0`. Engine fixture sources
`25ff0a88`/`3d1ef20a`/`47696031` imported as
`a2fe3280`/`9d8a718f`/`f8d1062f`. Independent source review
`eeb6dc03` -> `88f1e2c3`.

Runner `80ebf709` -> `1ab4a490`; decision-clock mask `5e1fa849` ->
`be2ce0ad`; fixtures `a86b274d` -> `010cc1a8`; whole-run retained-report
budget `5e6dcb75` -> `9926c524`; refusal fixture `8a9f254c` -> `c09df61c`.
Independent review `a8c23c56` -> `4e96aed3` approves both closed findings.
Compile correction `ab049a50` supplies the API's owned vector from the mask
view; the existing memory admission already includes that owned copy.

Monthly report `779add70` -> `494ecc75`, fixture `91329304` -> `6a05ec3f`,
include `b011e3c5` -> `bc294127`, report `cf4621e6` -> `10f3e98f`.
Calendar turnover uses actual execution dates and includes initial deployment;
calendar-year returns use realized endpoints. The old 21-session estimate
remains labeled as an approximation.

## Iteration cost

The focused two-target build at `c09df61c` used three workers, 2422 MiB free
physical memory and 4452 MiB commit headroom at launch. It stopped after
51.129s on the mask span/vector API mismatch, retaining eleven successfully
compiled objects. After the one-line correction, the incremental resume at
`ab049a50` completed in 21.906s with three workers, nine C++ objects and four
links. Total elapsed build time was 73.035s across both attempts. Existing PCH
and dependencies were retained. This is measured shared-host iteration time,
not a controlled speedup claim. Configured provenance remains `c09df61c`;
the actual compiled source including the correction is `ab049a50`.

## Real rehearsal: accounting advances to a stock conversion

The pre-registered third attempt uses the unchanged 24-alpha library, weights,
sign algorithm, $1bn scenario, Q1 TRAIN and Q2 development-check manifests.
The sole execution-policy extension is the pinned five-event cash-claim
document `257c6d645292b9f9464e6401ed1f9adb5e41a66089fdc4f7e23db0724f6da510`.
All five records are primary-source reconstructions; payment dates and verified
historical delivery remain unknown. The first run reaches January10 after
the MDCO/WAIR endpoints. The later three supplied cash events have not yet
been reached by a completed real trial.

`recent-dev-rehearsal-v3` stopped in 4.828s, sampled peak 219,181,056 bytes,
on the positive orientation of `momentum_12_1_s21`:

```
missing/guarded held return
date_index=405 mark_time_ns=1578693600000000000 (2020-01-10 22:00 UTC)
instrument_id=4997008 (historical source ticker JAG)
held_dollars=-151822.37061726692; source_present=0; price=NaN
previous_date_index=404; previous_price=8.2299995422363281
previous_present=1; guard_crossed=0
```

Context SHA256 is
`9f78b823d27d06f2337b34ae31669e537dfb7e2589eb66d63fb1b94a6a41e5f2`;
recipe SHA256 is
`c37c469efcf082cf6f58e3e0f359938e6795e8bc969e158c3f95c8f125caee05`.
The failed short cannot be turned into a disappearance gain or zero return.
The primary-source JAG investigation identifies a stock conversion requiring
successor identity, prices and explicit delivery accounting. That work is now
prioritized over unrelated sprint items. The existing synthetic transition
planner is arithmetic substrate, not a ready real-event application route.

Cumulative count is **three real trial attempts, zero completed**. Q2 is still
unscored, 2023-24 validation unread, 2025+ reserved. No portfolio Sharpe,
turnover target, common-stock coverage or tradability acceptance is claimed.

## Exact evidence

`cash-qualification-20260926/index.json` binds 49 byte-preserved artifacts
(759,485 bytes) using per-file SHA256 and `* -text`. This includes both build
receipts/logs, native XML/binary hash, the third run command/config/source/log
bindings and failed-trial ledger, source-gap forensics and read-only warehouse
metadata/coverage checks. Existing event, terminal-return and universe-type
tables were empty; no warehouse mutation occurred. The optional question
about another corporate-action or historical type source remains unanswered.

The 38-ID raw-source audit is data diagnosis only: 10.5s, peak 126,365,696
bytes, output SHA256
`037234f9c3f427dd45160a8f19ab7ecbc40dd941699cc73f56c94f717f11ed56`.
No future-presence filter, fabricated price or source repair was applied.
