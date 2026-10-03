# Root R0-7 report: Y-3 norm-score (gm template): STOPPED before stage 01

Status: **STOPPED, waiting for a ruling. Nothing ran.** Under the brief's stop rule "something needs a ruling", no stage of
Y-3 was started. Root: `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, HEAD at start `ff0c5552`. Date 2026-10-03,
13:07Z.

## Why it stopped

1. **There is no Y-3 wave manifest.**
   - `scripts/specs/v8/waves/` holds only `y-s.json` and `y-s.head.json`.
   - `git log --all -- scripts/specs/v8/waves/` shows only `d7c1c520` (Y-S manifest) and `4492f301` (R0-2 amendment).
   - No Y-3 manifest or head file exists on any branch.
2. **Writing one is a pre-registration act, and some of its choices have no ruling.**
   - The driver's preflight requires the manifest to be committed ("its commit is the pre-registration", v8y research
     loop section 2).
   - The registration fixes the template, its pin, the parent, PM7-34, PM6-6 and the budget arithmetic.
   - The registration does not fix:
     - the wave id;
     - the printed-criteria list;
     - the budget id;
     - the record directory;
     - how the template's on-disk bytes relate to its pin (item 3).
3. **The template's on-disk SHA-256 differs from its registered pin.**
   - The registered pin is `693c64f5471ceff6f21320c5bfee92d4f741226cbf7be295b1905e0a569e677f` (`v8y-prereg.md` section
     14). It equals the committed blob at `02633038` and at HEAD; the file is unchanged since then.
   - pool-2 has `core.autocrlf=true`, and `git ls-files --eol` shows `i/lf w/crlf`. So the bytes on disk hash to
     `73d16673f93e8ff0342072481e149ead9b3ecf6cd9b2e9ad065a27199af95183`.
   - The driver hashes the bytes on disk (`wave_context.Wave.sha` -> `stage_chain.sha256_file`;
     `wave_stage_preflight.py:174-178`).
   - So a manifest that pins the registered SHA would make preflight refuse: "rule_cell.template ... is not the pinned
     cell template".
   - Pinning `73d16673` instead would depart from the registered pin.
   - All four Y templates are `w/crlf` in pool-2: `y-norm-score`, `y-theme-tsmom`, `y-two-speed` and `y-vol-target`.
     By contrast, `y-s.json` and `lib-v8ysb-gm.json` are `w/lf`, and their disk SHA equals their blob SHA.

## What was checked (all read-only; no data, no exe)

| check | result |
|---|---|
| Parent (driver) | `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb, L 1.1828. This is the `next_parent` of `build-equity/waves/y-s/wave-result.json` |
| Y-3 defined on the parent (v8y section 6, "Undefined") | **Defined**: NAV rule `aim-partial-v5`; no `--hold-band`, `--vol-scale` or spo |
| Ledger | `build-equity/trials.jsonl` has 128 lines, the same as R0-6's count; N 57 |
| Template blob vs registration | `693c64f5` = the registered pin |
| Draft manifest (below; scratchpad only, not committed) | `wave plan` **exit 0**: 9 stages pending; the cell file would be `scripts/specs/v8/y-norm-score-y-3.json`, followed by `lock --write` and a commit; the match stage carries the PM6-6 -gm path. Draft SHA-256 `d4ece3a5bded754b...`. `plan` wrote nothing (no `build-equity/waves/y-3`; tree unchanged) |

## Per stage

None ran. There is no exit code, no receipt and no driver commit.

| commit | what |
|---|---|
| `3c42a2c8` | log: R0-7 STOP section appended to `platform-v8-20260929/integration-log.md` |
| (next) | this report (`git add -f`) |

- **Verdict and cell numbers:** none. No cell exists.
- **Trials added:** 0. The ledger is still 128 lines and N is still 57.
- **Next parent for R0-8:** unchanged, `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb). Y-3 has not run, so the
  registered order (PM8-10 (e)) still puts Y-3 before Y-2.

## Rulings needed (recommendation -- why -- cost if wrong)

**R0-7-MAN: who writes `scripts/specs/v8/waves/y-3.json`, and with what bytes.**
- Recommendation: root commits the draft below as `wave y-3: manifest (pre-registration of cell Y-3; v8y-prereg sections
  5, 6, 14)`, then runs `wave plan` and stage by stage.
- Why:
  - Every key either comes from the registration or the Y-S result, or copies the Y-S manifest's shared keys (fields
    v15 pin, ledger, record dir).
  - The printed list repeats Y-S's three named rules. They cover the registration's printed turnover per gross, cost per
    traded dollar and 4x row, and they decide nothing under PM7-34.
  - Y-3's own criterion (S2 gross-of-cost annual return per unit of all-rows gross) has no named rule, so it is written
    by hand from `wave-result.json` (YP-7).
- Cost if wrong: one amended pre-registration commit before preflight. No trial is spent.

**R0-7-EOL: template bytes vs the registered pin.**
- Recommendation: before the manifest commit, root restores the committed LF bytes of the template in pool-2's working
  tree, then confirms that its disk SHA-256 is `693c64f5`. For example: delete the file, then run
  `git -c core.autocrlf=false checkout -- scripts/specs/v8/y-norm-score.json`.
  - `git status` stays clean.
  - No hash is edited, and the pin stays the registered value.
  - The same restore should apply to the other three `y-*.json` templates before R0-8, R0-9 and R0-11.
- Why: the registration pins the committed bytes. The CRLF copy is a checkout artifact of `core.autocrlf`, not a
  different template.
- Cost if wrong: if the restore were skipped and the manifest pinned the CRLF SHA instead, every Y cell would record a
  pin different from section 14's.

**Heads-up for R0-9 (not R0-7's to resolve):**
- `scripts/specs/v8/y-two-speed.json` is `e29365d1` at HEAD.
- The registered pin is `69cf6134` (@ `e2ac7d63`).
- It was changed afterwards by `b3b5dab4` ("docs(two-speed): registered text follows the code and PM8...").
- `y-theme-tsmom` (`9d3548b6`) and `y-vol-target` (`4e190f54`) match their pins.

## The draft manifest (validated by `wave plan`, exit 0; not committed)

Path used: `<scratchpad>/y-3.draft.json`. Proposed home: `scripts/specs/v8/waves/y-3.json`.

```json
{
  "schema": "atx.research-wave/v1",
  "wave": "y-3",
  "description": "Cell Y-3 norm-score-v1 (v8y-prereg sections 5, 6, 14; PM8-10 (c), (e); YP-7): rule wave, template scripts/specs/v8/y-norm-score.json @ 02633038 (blob 693c64f5), no free constant, NAV-only; parent = Y-S cell lib-v8ysb-gm.json (library v8ysb, L 1.1828; wave-result 57e9f5ea next_parent); fields v15 lo3; acceptance PM7-34 (dSR > 0 AND mechanics); PM6-6 gross matching (gm template); budget construction N 57 + 1 <= 62 (N_c(X-F0) + 6), 0 admission trials.",
  "parent": {"spec": "scripts/specs/v8/lib-v8ysb-gm.json", "library": "v8ysb"},
  "fields": {"dir": "build-equity/train-2020-2023-lo3-fields-v15",
             "manifest_sha256": "26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09"},
  "rule_cell": {"template": "scripts/specs/v8/y-norm-score.json",
                "template_sha256": "693c64f5471ceff6f21320c5bfee92d4f741226cbf7be295b1905e0a569e677f"},
  "acceptance": {"rule": "pm7-34",
                 "printed": ["capacity-4x-higher", "turnover-per-gross-not-higher", "cost-bps-lower"]},
  "gross_match": "pm6-6",
  "budget": {"id": "v8y-construction-n57-plus-1", "construction_cap": 62},
  "ledger": "build-equity/trials.jsonl",
  "expect": {"n_before": 57},
  "out_dir": "build-equity/waves/y-3",
  "record": {"copy_to": ".superpowers/sdd/platform-v8-20260929/waves"}
}
```

(The validated draft's description began with "DRAFT (uncommitted; for the PM's ruling) --". Remove that prefix before
committing; it changes the manifest SHA, which is recorded only when committed.)

## Hard rules

- Nothing was built, run or launched; the only processes started were one read-only `wave plan` on the scratchpad draft
  and git, sha256sum and Python reads of specs and receipts.
- No stage started and no lock was taken. The seal scan was not needed because no run log exists.
- Nothing dated 2024-01-01 or later was opened; only specs, templates, the Y-S wave-result and the ledger's line count
  were read.
- `C:/atx` and `atx-db/` were not touched.
- No push, no worktree prune, no expected hash edited, no code changed, no subagents.
- The template's working-tree bytes were left as they are, pending R0-7-EOL.
- The untracked `docs/plans/2026-10-02-x5-equity-curve.png` was left as it is.
