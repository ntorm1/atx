# v8y research loop: one wave, one command (lane YINFRA, 2026-10-02)

Owner goal (2026-10-02): reusable, modular and fast infrastructure for recursive self improvement of the core mega
alpha. Until now one wave cost the integrator hours: a hand-written plan, N add-alpha calls from scratch scripts,
`run --screen`, the PM7-35 reading by eye, a b library, `gmspec.py` for gross matching, `mech.py` before any return,
`bundle.sh`, `cellstats.py`, a verdict paragraph, a ledger check and a log section. This plan replaces those hand steps
with code that enforces the same discipline. No trial was run to build it; every stage is tested on synthetic fixtures
(`scripts/tests/test_research_wave.py`, `test_wave_queue.py`, `test_wave_scoreboard.py`, `test_wave_speed.py`,
`atx-engine/tools/test_stage_chain.py`).

## 1. The loop

```
signal lane            PM                         root                                   anyone
-----------            --                         ----                                   ------
candidates new  --->   candidates pin   --->      candidates emit --> commit -->         scoreboard
(proposed)             (pinned, ruling)           wave run MANIFEST (9 stages) -->       (lineage, timings)
                                                  wave-result.json + log section
                                                  (queue: screened/admitted/dropped/in-book; N advanced)
```

### 1.1 A lane proposes

A signal lane writes one file per candidate into `scripts/specs/v8/candidates/<id>.json` instead of a prose table:

```
python scripts/research_cycle.py candidates new --id wq_035 --dsl "<frozen string>" --theme reversal_seasonality \
    --tier B --prior-sign 1 --citation "Kakushadze 2016, #35" --origin prior --hypothesis wq-035 --by XWQ \
    [--kind replace --replaces ID [--rescreen]] [--removes ID] [--fields open_adj,high_adj] [--note "..."]
```

The file carries the frozen DSL and its SHA-256, theme, tier, prior sign, citation, origin, hypothesis id, add or
replace, status `proposed` and a history. `candidates validate` (run by every `new`, `pin` and `emit`) refuses:

- a file that is not a valid registration (missing key, a DSL whose SHA-256 is not the pin, a bad sign or origin);
- a DSL SHA-256 queued twice, or registered in `atx-impl/strategies/alphas/registry.json` under another id, or an id
  registered there with another DSL (a registered alpha is immutable);
- a second variant of one hypothesis id, unless the variant carries the `ruling` that allowed it.

### 1.2 The PM pins

```
python scripts/research_cycle.py candidates pin --id wq_035 --id wq_030 --by PM --ruling PM8-1
```

Only `pinned` candidates can enter a wave. Lifecycle: `proposed -> pinned -> screened | admitted | dropped | in-book`
(`proposed -> dropped` is a withdrawal); every later status is final, so a re-test is a new variant with a ruling.

### 1.3 Root runs one wave with one command

1. Emit the wave manifest from a head file (the wave's non-candidate keys) and the pinned selection, in roster order;
   `--after` takes the parent and `expect.n_before` from the previous wave's result (N advanced by code):

   ```
   python scripts/research_cycle.py candidates emit --head waves/x7-head.json --select wq_035,wq_030,... \
       --output scripts/specs/v8/waves/x7.json --after build-equity/waves/x5/wave-result.json
   git add scripts/specs/v8/waves/x7.json && git commit -m "wave x7: manifest (pre-registration)"
   ```

2. Look, then run:

   ```
   python scripts/research_cycle.py wave plan   scripts/specs/v8/waves/x7.json      # every stage's argv, nothing runs
   python scripts/research_cycle.py wave run    scripts/specs/v8/waves/x7.json      # the wave; re-run = resume
   python scripts/research_cycle.py wave status scripts/specs/v8/waves/x7.json      # done / stale / pending per stage
   python scripts/research_cycle.py scoreboard --timings                            # the accepted lineage
   ```

`wave run --until STAGE` stops after a stage (e.g. `--until screen` to read the screen before the cell); `--dry-run` is
`plan`. Exit codes: 0 done, 2 manifest, 3 a pin or a stale receipt, 4 a hard stop of a stage.

The manifest (`atx.research-wave/v1`, `scripts/wave_manifest.py`) declares the wave id, the parent book spec and
library, the fields dir and its manifest pin, the candidates (or a `rule_cell`: template + pin + constants), the
acceptance rule by name with its printed-only criteria, the sign rule, the gross-matching mode, the budget, the ledger,
`expect.n_before`, the state dir and where the result is copied. A rule wave (X-5 / X-6 style) names a template and
its constants instead of candidates.

## 2. What the code enforces (stage by stage)

The stages run as a resumable chain (`atx-engine/tools/stage_chain.py`, generic): each stage writes
`<out_dir>/receipts/NN-<stage>.json` (inputs, outputs, timing); it runs only after the previous stage's ok receipt
(whose SHA-256 is one of its inputs) and only while every input it recorded is unchanged; a failed stage leaves
`NN-<stage>.failed-k.json` and is retried by the next run; an ok receipt is never overwritten and a changed input is
refused (exit 3), never silently re-run.

| stage | what runs (root only for data) | what is enforced |
|---|---|---|
| preflight | nothing | manifest committed (its commit is the pre-registration); code pathspec clean; parent spec loads, has no run refusal, has its NAV, scores to the wave's ledger; fields manifest = pin, complete, `seal.exclusive_end` <= the research seal (`research_window.py`), candidate fields present; no path names a sealed year; ledger chain verifies and N == `expect.n_before`; budget holds the new admission trials and the cell; queued candidates are pinned with the manifest's DSL |
| register | `add-alpha` per frozen string into library NAME, `--save-plan` (K1 plan of record), one commit | the strings verbatim; a replacing wave's marginal on the pool only (PM6-8 (i), PM7-32) |
| screen | `research_cycle.py run lib-NAME.json --screen` | the gate ledgers the admission trials; the sign rule (PM7-35) by code on the admission rows; gate exit 10 = no cell |
| spec | none, or `add-alpha --name NAME+b` on the kept strings + commit, or the rule cell written + `lock --write` + commit | the cell is the screen library, the b library (same trial ids) or the rule cell; its ledger is the wave's; its paired reference is the parent's NAV; a template is used once |
| run | `run CELL --stop-after nav` (b library: `run CELL --screen` first) | the calibration NAV only: no summ, no ledger line |
| match | mechanics reader (keys only) on calibration + parent NAV; if needed `<cell>-gm.json` + `lock` + commit + `run --stop-after nav` + reader | PM6-6: within .005 stands, else one correction L' = L x G_parent / G (4 decimals); the matched run must be within .005, else a stop for the PM |
| verify | nothing | mechanics (all-rows gross in [.90, 1.05], abs net <= .02, tau mean <= .20, p95 <= .30, accounting within the NAV's tolerance) read before any return; the NAV's C-13 binding; no date token at or after the seal in any run log; a failure stops for a ruling with no ledger line |
| judge | `run CELL` (monitor, summ: nav_summ scores and ledgers the cell), `nav_summ --protocol v8 --bundle PARENT CELL`, book reader | verdict by the named rule (PM7-34: dSR > 0 AND mechanics; printed criteria computed, deciding nothing) |
| record | nothing | ledger chain re-read, the cell's line present, N advanced by exactly the cell (else a stop); `wave-result.json`, `wave-log.md`, copies; queue statuses set and committed |

The two readers (`scripts/wave_readers.py`) run under the bounded runner (180 s / 1,536 MiB / 512 MiB free) and write
new files only. `mechanics` writes construction keys only (no return, NAV, Sharpe or P&L figure): it is what gross
matching and the mechanics gate read. `book` (after the mechanics passed) writes the scoreboard line: net and gross
Sharpe, net annual mean, CAGR, volatility, max drawdown, gross of cost (net + annualised |trade cost|, |borrow|,
|long financing|), turnover, cost per traded dollar, the 4x net Sharpe from `capacity_curve.csv`.

`wave-result.json` (`atx.wave-result/v1`, `scripts/wave_result.py`) holds the screen rows and decision, the cell, the
marginal rows (report only), mechanics, the book stats of cell and parent, the paired block, the bundle, the DSR block,
PBO, the verdict, the ledger before / after, the next parent and every phase's wall seconds and peak. The scoreboard
reads only these files and the ledger (never a NAV file) and checks every cell's ledger line and s2_net_sr.

## 3. What stays a human ruling

The code stops (exit 3 or 4, a failed receipt naming why) and waits for a ruling in every case the integrator used to
stop for the PM:

- pinning a candidate, allowing a second variant of a hypothesis (`ruling`), raising a budget cap, re-pinning
  `expect.n_before` when another trial was ledgered first;
- a mechanics failure, a gross match still outside .005 after one correction, a seal-scan hit, a cell whose series
  equals an earlier trial (identity re-run), a record-stage N mismatch: the cell ran, no ledger line was written by the
  driver beyond nav_summ's own, and the ruling (defect line, re-run, acceptance) is the PM's / owner's;
- the choice of parent when the lineage branches, new named rules (a new sign, acceptance or criterion rule is a code
  change in `scripts/wave_rules.py`, reviewed, never prose), and every PM / owner ruling the rules encode.

## 4. Speed

Three largest wall-clock costs of a wave (integration log, X batch 1; Debug build v8-14):

1. **The marginal IC verb, twice per wave with a sign-rule drop** (screen + b library), report only: X-2 141.1 + 134.6 s
   (log lines 5622, 5647), X-3 175.5 + 167.0 s (5723, 5741), X-4 157.7 + 143.8 s (5814, 5830): 57-59% of each wave's
   phase wall.
2. **NAV replays**: the gross-matching pair (X-3 41.6 + 45.5 s, 5744-5745; X-5 45.1 + 45.4 s, 5866 / 5876; X-6 47.1 +
   46.7 s, 5913 / 5918) or nav + ref (X-2 41.5 + 43.3 s, 5649-5651).
3. **IC passes on the Debug build**: u twice + w (X-3 16.2 + 26.6 + 34.7 s, 5720 / 5739 / 5743; X-4 18.2 + 26.3 + 41.5
   s, 5812 / 5828 / 5832); the card twice (33.7-37.2 s) and summ (28.4-30.5 s) follow.

Implemented in Python (manifest `speed`, both default on, no decision input moves):

- `reuse_screen_marginal`: the b library's cell spec has no marginal phase; the screen's rows of the kept strings are
  carried into `wave-result.json` (source named; computed on the screen library, a superset, so `max_rho_member` may
  name a dropped string). Saves the second pass, 134.6-167.0 s per wave with a drop.
- `screen_first`: a b library runs `--screen` before its cell, so its u pass is the screen's (no unread u-pass blend,
  the IC exe's `--no-composition`) and the cell resumes it, as every screen library's cell already does (X-3: the b
  u pass took 26.6 s against the screen's 16.2 s).
- Hash-identical work is never redone: the stage chain skips every stage whose inputs are unchanged, the readers reuse
  their output while the NAV's daily CSV hashes as read, research_cycle resumes every output by name, and the cycle's
  own rule skips ref when the fields equal the parent's.
- `scoreboard --timings` prints wall seconds and peak by phase per wave from the results, so every later speed change
  is measured against the same table.

Design notes (C++, not coded; root builds; each needs its flag-absent identity):

- **Marginal subset** (`atx-impl/src/strategy_marginal_ic.cpp`, `.hpp`, `MarginalIcConfig`, `dispatch_marginal_ic` /
  `run_marginal_ic`; argv built by `scripts/research_cycle.py` `marginal_step` from `marginal.flags`): a
  `--candidates ID,...` option residualises only the listed (new) candidates and forms the pairwise rho only for pairs
  with a listed id; the parent members' rows come from the parent's wave. With 3-8 new of 52-60 members the
  residualisation work falls by about an order of magnitude; the payload stream for rho stays C per date. Absent, the
  verb is unchanged. Add `--candidates` to `MARGINAL_SPEC_FLAGS` (a list value) in research_cycle.
- **In-process gross calibration** (`atx-impl/src/strategy_nav_v7.cpp`, `strategy_nav_replay.cpp`, `config.cpp` /
  `config.hpp`): a `--calibrate-gross TARGET --calibrate-tolerance .005` mode that replays at L, computes the all-rows S2
  gross, and if outside tolerance replays once at L' in the same process (fields, liquidity cache and the combined
  signal loaded once) and skips the `--capacity-curve` books (5 extra replays) in the calibration replay. The NAV must
  stay on the Debug build: Release differs by one ULP in the modeled cost columns (log 3359-3365).
- **Release IC executable**: u and w are byte-identical Debug vs Release and 59% / 82% faster in CPU (log 3344-3358); the
  marginal verb lives in the same exe. A cell spec can already name `exes.ic` = `build-equity-rel/bin/...` with
  `exes.nav` on the Debug build (`research_cycle.effective_exes` keeps a path as given). Needed: build
  `atx-equity-strategy-ic` in `build-equity-rel` at the current tag, the flag-absent identity (the parent's u and w
  re-run byte-identical, the u-compare rows), and a ruling; the candidate cache is keyed by the VM identity, so the first
  Release u pass is cold once.

## 5. Files

- Generic: `atx-engine/tools/stage_chain.py` (+ `test_stage_chain.py`).
- Wave: `scripts/research_wave.py` (CLI), `wave_context.py`, `wave_stages.py`, `wave_steps.py` (argv and spec writers),
  `wave_rules.py` (named rules), `wave_manifest.py`, `wave_readers.py`, `wave_result.py`.
- Queue and scoreboard: `scripts/wave_queue.py`, `scripts/wave_scoreboard.py`.
- Existing files touched: `scripts/research_cycle.py` (verbs `wave`, `candidates`, `scoreboard`: dispatch only),
  `scripts/research_add_alpha.py` (opt-in `--save-plan`; absent, the files written are byte-identical: test).
