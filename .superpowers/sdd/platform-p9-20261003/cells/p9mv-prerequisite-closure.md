# P9-MV prerequisite closure and registration proposal

Preparation only under resume15-ledger-24 / cells-prompt-5, starting HEAD
`fa76811ab571bb585556685125153854b71c4fca`. C is rejected; H remains accepted.
No MV spec mutation, diagnostic, probe, scientific computation or trial write occurred.
`start-state-5.txt` records the read-only restart: frozen54/CRT/six references match,
ledger137 `21522bb42c9adf7859dbf042878338b3b0374b2f920055a8f2e59980c2a9a76b`,
prefix136 `c98dd0c4d59bada990fb34c7bf5efc3ae59873acc313de771a2d025a449013b7`.
N_c66, N_hand127, M110, N_tot237; P9 admissions+0. No RAM wait or unchanged-tool test.

## Five exact template guards and proof

1. **Exact:** "lane COV-MV merged in root and reviewed APPROVE before the P9 cells start (Ruling COV-7; else P9-MV lapses at 0, PRE-14)"

   **Proof:** merge `5c811b56` includes review `25e11b32` of `942f499b`;
   `task-COV-MV-rereview-1.md` verdict APPROVE, 0 Required, 4 Suggested.
   `root-wave3-merge-report.md:560-629` records the merge, target-row-only addition,
   guard and synthetic checks. Both commits are ancestors of current HEAD.
   Proposed satisfied; no historical check repeated.

2. **Exact:** "P9-MV registered by the PM in the P9 prereg with its store pin build-equity/p9-m2i-dcov-covv1 manifest SHA-256 ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3 (Rulings W3-10, PRE-29), the registration's and this template's SHA-256 and the spo-v2 line printed beside it as the prior of record (Ruling PRE-24a)"

   **Proof:** prereg B.5 and Appendix IV17 carry original registration/template/store
   pins and the spo-v1 / spo-v2 prior; PRE-2BII-RULED and CELLS-4 retain MV RUNS.
   Original pins independently rehashed below. Proposed scientific prerequisite
   satisfied; operational parent/hash closure and adjacent PRE-40 new-pin record await PM.
   The prior remains: spo-v1 failed mechanics through its internal gross cap;
   spo-v2 was a research rejection. MV is the distinct gross-normalised closed-form
   target, without an optimiser, cost term or internal gross cap. No prior is rerun.

3. **Exact:** "root checks universe.delisting.returns_applied == false on the role the store was fitted on (Rulings COV-9, S9) and re-pins the lineage parent to the wave-3 P9-B0 references (W1-I2) before the cell"

   **Proof:** covv1 manifest binds `build-equity/train-2020-2023-lo3/manifest.json`,
   SHA `e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4`;
   streaming hash matches and selected `universe.delisting.returns_applied` is false.
   Re-pin `2078f877` and FLAGS `f8d852f3` are ancestors. Restart rehashed all six
   P9-B0 listings (126 files), frozen54 and CRT with zero differences. Proposed satisfied.

4. **Exact:** "root sets parent to the last accepted cell, whose NAV runs --rule aim-partial-v5 without --two-speed, --adv-hold-q, --hold-band, --construction-grid, an spo rule or aim-partial-v6, and runs the cell as an atx.research-wave/v2 kind rule wave with gross_match pm6-6-engine whose rules block is [{name: mv-aim-v1, params: {risk_model: build-equity/p9-m2i-dcov-covv1, risk_model_sha256: ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3}}] (Rulings ALCOMB-1, PRE-9)"

   **Proof:** scratch24 rejects C and retains H at `bd837403`, template
   `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json`, SHA
   `1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f`.
   Read-only resolver: NAV `build-equity/mega-nav-p9-theme-cov-hedge-gm`, L1.1674,
   aim-partial-v5, cadence1/trade-fraction.05, fixed rate (no --rate), none of the
   forbidden flags/rules. --ann-mean-gross and --info-clock-print each inherited once.
   Flag-absent fitter remains ew-theme-std-v1 + theme-erc-v1 + theme-cov-hedge-v1.
   u `mega-p9l-train-u-label-terminal`, fit `mega-weights-p9-theme-cov-hedge`,
   card `mega-cards-p9-theme-cov-hedge`, w `mega-p9w-train-theme-cov-hedge`
   (all under build-equity) keep their completed, hash-verified receipts; supported
   native reuse, no fit metadata/statistics read. Proposed satisfied conditional on
   the exact parent/native manifest below. H does not inherit rejected C's rate.

5. **Exact:** "the NAV exe pinned by the cell (exes_sha256, E1-REUSE-a2) is a Release build of a head containing mv-aim-v1 (M2c-RED-2 (e)), and its nav --list-rules --json lists the target row mv-aim-v1"

   **Proof:** frozen Release p9-3f source `5f48a25d`, NAV SHA
   `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565`.
   Current `nav --list-rules --json` metadata-only invocation exits0, empty stderr,
   one mv-aim-v1 target row. `atx-impl/src/book/nav_rules.cpp:109-137` agrees:
   x-flag `--target-rule mv-aim-v1`, required closed params risk_model/string
   (`--risk-model`) and risk_model_sha256/string (`--risk-model-sha256`,
   `^[0-9a-f]{64}$`), additionalProperties=false. `wave_kinds.py:482-554`
   derives these three flags once, leaves nav.rule aim-partial-v5. No alternate route.
   Proposed satisfied; no real-data NAV or probe run.

The registration file's four historical `requires` strings remain untouched. Their
merge/registration/role/re-pin obligations have the evidence above. Its historical
identity wording is read with the gate's explicit cuts: Y-F0 flag-absent identity
holds, X-5 is CUT under FIELDS-X5, Debug/Release canaries hold (root-gate-report
final table, gate-report-review PROCEED). It is not a claim that X-5 was rerun.
No prior real-role MV run is introduced by preparation.

COMPOSE-1a gate ACCEPT was a store-backed parse check; later config/input checks
remain native preflight. The combined clock+target case is absent with fixed-rate H.
Preserve COMPOSE-PRE's zero-trial lapse where applicable; an unrelated fields,
resource or driver stop is reported for PM disposition, not automatically a lapse.

## Original bytes, store and exact proposed delta

At starting HEAD, template blob `63917df794f55696830021c330e0e5f93b9f12a2`,
SHA `83391f2c526b28008d48e250f0e7d26e7c3f8661786d43a8f3d76439f664d870`;
rule blob `31bcf217be335e768658e3e490155f2322e207f6`,
SHA `4b4af6e238b529d5d7eabccd2699022dc653da52fa45801eb3f61896bd6680b4`.

Only three existing JSON paths proposed, not applied:

- template `/parent`: null -> `theme-cov-hedge-v1-attempt2-gm.json`.
- template `/requires`: the exact five strings above -> [] **only on explicit PM
  closure**, separately requested beyond COVMV-PIN-2's existing two-path allowance.
- rule `/manifest_fragment/rule_cell/template_sha256`: original template SHA ->
  SHA of the registered template.

Simulated JSON serialization (two-space indentation, ensure_ascii=false, LF final
newline) produces template SHA `d4dc2ff7916fdda924590da597e7c8325358cc369a6c567f2c1b9d7df145e5c6`
and rule SHA `d88e393a49c8efc9bf08af019eaf427af106092feec23a9d47151ce34cdd8712`.
No files were written for this simulation. Registration must verify these hashes
and the closed JSON diff before launch, preserve original blobs, and put COMPLETE
old/new leaves, full manifest, pins and inherited flag binding in its commit body.
No scientific figures enter commit subject/body.

No description, nominal_parent, printed_only/ex-ante sentence, manifest_note,
constant, prediction, registration historical requires or source date changes.
source_sample_end2010 is not flagged. Existing null eigen encoding is disclosed in
the scalar plan; no imputation/new statistic. Adjacent B.5/Appendix IV17 new pins
will be recorded in PRE-40 form after release.

Only risk store: `build-equity/p9-m2i-dcov-covv1`, manifest
`ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`;
container root `20919f7b338dfbd786994cbbcb506bdaabd9c3af3266d4394d4d2e07d90c9f8e`;
model.atxcov SHA `0f90bf7c78203ca8c93d6003d58c8b2894747045485137756fbc13a32e3b642a`,
510678016 bytes. Streaming hashes and manifest container identity match. Never rebuild.
Library v8ysb SHA `41010b0bbc662922e6e873057e1fc39d046d6f01a0a5124dd8a601c25a1308f5`.
If MV is accepted, P9-F0 may carry risk while H remains P9-F0h/history-readable;
no era risk store, no history risk-model read, and later leverage starts from F0h.
No W/X/V/adoption work before explicit MV disposition.

## Proposed native manifest (new file only after release)

Path: `scripts/specs/p9/waves/p9mv-mv-aim.json`.
Printed native comparisons decide nothing; primary vol/gross-alpha and exact MV
summary leaves are selected separately as specified in `p9mv-mv-aim-read-plan.md`.

```json
{
  "schema": "atx.research-wave/v2",
  "wave": "p9mv-mv-aim",
  "kind": "rule",
  "description": "P9-MV prereg B.5: mv-aim-v1 on accepted H; pinned covv1, pm7-34, pm6-6-engine; immediate bounded native route.",
  "parent": {
    "spec": "scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json",
    "library": "v8ysb"
  },
  "fields": {
    "dir": "build-equity/train-2020-2023-lo3-fields-v15",
    "manifest_sha256": "26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09"
  },
  "acceptance": {
    "rule": "pm7-34",
    "printed": ["turnover-per-gross-not-higher", "capacity-4x-higher", "cost-bps-lower"]
  },
  "gross_match": "pm6-6-engine",
  "budget": {"id": "p9-prereg-construction", "construction_cap": 71},
  "ledger": "build-equity/trials.jsonl",
  "expect": {"n_before": 66},
  "out_dir": "build-equity/waves/p9mv-mv-aim",
  "record": {"copy_to": ".superpowers/sdd/platform-p9-20261003/cells/waves"},
  "driver": {"lock_exes": true, "receipt_digest": "content", "keep_verdicts": true, "timings": true},
  "rule_cell": {
    "template": "scripts/specs/p9/templates/mv-aim.json",
    "template_sha256": "d4dc2ff7916fdda924590da597e7c8325358cc369a6c567f2c1b9d7df145e5c6",
    "name": "mv-aim-v1",
    "source_sample_end": 2010,
    "predicted_mechanism": "V^{-1} tilt lowers ex-ante and realised vol at equal gross alpha",
    "data_class": "H"
  },
  "rules": [{
    "name": "mv-aim-v1",
    "params": {
      "risk_model": "build-equity/p9-m2i-dcov-covv1",
      "risk_model_sha256": "ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3"
    }
  }]
}
```

No admissions; completed u/fit/card/w native reuse. Native judge alone writes one
v2 construction trial; no DERIV. The proposal requires only PM ruling on the five
closures, closed delta, exact manifest and precommitted read/source plan. No missing
scientific prerequisite found. Registration and measurement remain NOT RELEASED.
