# Independent final D0 correctness review

**APPROVE D0 owning-target correctness** for production
`fcbcc9d1a351ccd339687389b118aca65e2812b5`, as reported by root
`932c10614f7b7184a371f2e213dc315908d821d4` in
`final-vwap-closure-report.md`. No blocker remains in the reviewed raw VWAP,
geometry, context-policy or capacity-identity repairs. This approval excludes the
pending L9 remeasurement and 81-case quiet performance gate; it is not a W0 wave
approval, observed intraday VWAP or a tradeable-alpha claim. Adjusted OHLC and
mixed-basis lint retain their documented W2-A3 boundary.

## Independent evidence checks

- Verified zero code/build drift from fcbcc9d1 to the report commit. Recomputed
  all three current root executable SHA256 values and native log hashes; each
  matches its owning test receipt and the report. Parsed every GoogleTest JSON:
  data 239 passed/1 skipped, book 128 passed, impl 605 passed/5 skipped, zero
  failures or disabled tests. Added to the independently qualified alpha 711/711,
  this is 1,683 passes and six skips from 1,689 executed tests.
- The data exclusions are line-for-line identical to the previous integrated
  gate; their recorded SHA256 is
  `2af3ee16dbacbd4dc414a5dd6c25ff83043f89f87746fce3692a9e209fd64d53`.
  The runner clears external-data/nightly opt-ins. The six JSON skips are exactly
  DataUniverse.SurvivorshipCaveatDocumentedOrDeferred;
  Alpha101Orats.RankBySharpeRiskParity and RankByOptimizerSharpe;
  AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha;
  FundamentalZoo.RealDataIcReport; SingleAlphaCapacity.SweepAndVerify.
  Their recorded reasons are the absent optional document or unavailable/opt-in
  real-data fixtures; none is a newly waived D0 assertion.
- Confirmed the native build receipt exit 0, 163 actual compiler actions and six
  links. The unchanged repeat has exit 0, 5.047 seconds and native
  `ninja: no work to do.`. The report correctly distinguishes native worker
  capping from the non-atomic process-count sample and makes no cache speedup claim.
- Independently compared all 84 recorded context first-party Git blobs with both
  integrated fcbcc9d1 and checked 30994375; all match, and the checked files have
  no local drift. Read the authoritative native exit-0 one-object PCH-off log and
  exit file. Verified the reusable real_panel TU plus 34 recorded headers against
  its compiled source and integrated candidate: all 35 match.
- Recomputed compile_commands.json SHA256 and the five impl command hashes;
  config, panel, discovery, progress sink and mining commands match the receipt
  and contain no PCH use. This is scoped production include evidence, not a
  claim that every header or the full tree was built standalone.

Independent parsed/hash receipts are in pool5/build-equity:
`w0-vwap-final-review-bindings.json` (including JSON hashes) and
`w0-vwap-final-review-hygiene.json`.

## Independent synthetic spot run

Executed the existing root binaries read-only, with all inherited ATX_* settings
removed and logs written only into pool5. No compiler, configure, full-suite
repeat or real-data operation was launched.

- Data **3/3 passed**, no skips: explicit context policy and moves before lazy
  caching, factor-resnapshot raw-field invariance, real-panel basis metadata.
- Impl **6/6 passed**, no skips: V1/V2 conditional resume identity, each active
  capacity knob, persisted active/off recipe, bound panel identity under rule
  mutation, empty V1 field order and closed CLI/default rule names.

Both native exits were zero and executable hashes remained unchanged. Exact argv,
source/executable/log/JSON hashes and cleared environment names are preserved in
`w0-vwap-independent-subset.receipt.json`; native logs/JSON use the
`w0-vwap-independent-atx-engine-data-tests` and
`w0-vwap-independent-atx-impl-tests` prefixes. The already-qualified independent
2520x128 legacy input oracle remains byte-identical with digest
`75b4a957ff30e9c3`; no redundant full alpha run was needed.
