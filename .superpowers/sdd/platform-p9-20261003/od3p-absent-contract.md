# OD3P -> C++ lane: the IC exe's `--absent-members` contract (`absent-members-v1`)

Written by lane OD3P (pool-21, RESUME 7) under Ruling OD3P-1 (ledger `progress.md`, RESUME 7 block). It is the contract
between the Python side (done in OD3P: `scripts/research_roles.py`, `atx-impl/tools/carry_weights.py`) and the C++ lane
that consumes it: lane ABS (Ruling ABS-1; pool-28, branched from D3W's APPROVE head; merged in root at `898bf120`).
ABS's report (`task-ABS-report.md`) uses this file's spellings unchanged: flags `--absent-members` /
`--absent-members-sha256`, capability `absent-members-v1`, schema `atx.od3-flagged-members/v1`, and `summary.json`
`absent_members {rule, list_sha256, members}` (checked in OD3P's follow-up round, re-review 1 S-b). Synthetic data
only; nothing here reads a return. Source citations are at OD3P's merge of root `4dc831ac`; the C++ as built is cited
file:line at ABS's head in ABS's report.

## 1. What the ruling fixes (OD3P-1, quoted in substance)

- A dropped member is treated on every date exactly as the engine already treats a member with no data on that date:
  it adds nothing to the composite, and its mass stays in its theme ("missing-or-unoriented-mass-stays-in-theme").
- The pinned TRAIN weights, signs, theme blocks and the runner's verify are unchanged, so theme-erc-v1 and ic-shrink
  bases are accepted. A theme whose members are all dropped is treated as the engine treats a theme with no present
  member.
- Route: an opt-in list flag on the IC exe, `--absent-members <file>` with `--absent-members-sha256 <hex>`, capability
  `absent-members-v1`; flag absent = bytes unchanged; tested by the equivalence that defines it (section 5).
- The C++ must be merged and APPROVE at the wave-3 gate (EXE-FREEZE: the IC exe may not change after the re-pin);
  otherwise the clean-source line is printed "not read: no producer" and nothing else changes (the line decides
  nothing, PRE-31).

## 2. The list file

The file is the drop list itself: the spec's `fit.carry.drop.path`, in P9 `scripts/specs/p9/od3/flagged_members.json`
(`.gitattributes`: `eol=lf`, so its bytes and SHA-256 are the same in every checkout). No derived file is written.

| key | type | read by the exe | rule |
|---|---|---|---|
| `schema` | string | yes | must equal `atx.od3-flagged-members/v1` |
| `members` | array of strings | yes (the only payload) | non-empty; distinct; each matches `[A-Za-z0-9_]+`; each is the `id` of a candidate of the pass's `--library` |
| `rules`, `rules_pending`, `rule_source_sample_end`, `source`, `classes`, `description`, `ruling` | - | no | the Python side's: `carry_weights.py --drop-list` and the cycle check them (pending rules refused, a base applying a listed rule refused, a nav / ref flag naming a listed rule refused) |

The exe ignores every key but `schema` and `members` (a deliberate choice: the rule keys are not composition inputs).
Order of `members` is not semantic; the record (section 4) keeps it as listed.

Synthetic example (the committed list has the 12 PRE-31 ids):

```json
{"schema": "atx.od3-flagged-members/v1", "source": {"path": "flags.md", "sha256": "<64 hex>"},
 "members": ["old_alpha"], "rules": ["theme-cov-hedge-v1"], "rules_pending": []}
```

## 3. Flags and capability

- `--absent-members <path>` and `--absent-members-sha256 <64 lowercase hex>`: both or neither; each at most once.
  The SHA-256 is of the file's bytes as read (no newline normalisation).
- Capability: the string `absent-members-v1` in the `capabilities` array of `--list-rules --json` (schema
  `atx.composition-rules/v1`, K-P9-6; today's array is `exe_capabilities` at `strategy_ic_rules.cpp:545-546`, sorted).
  Offered if and only if the flags are implemented with the semantics of sections 4-5. The cycle reads it through
  `scripts/cycle/phases.py:45-60` `exe_capabilities` and refuses without it (section 6).
- Flag absent: every output byte of every pass is unchanged (the existing goldens and identity tests hold as they are).

## 4. Semantics (what the exe does with the list)

- Every listed member is treated, on every date of the pass, as a member whose data is missing on that date. The
  composition rule the book already states for a missing member applies unchanged: redistribution rows
  ("missing-or-unoriented-mass-stays-in-theme; ... theme-without-present-member-neutral",
  `strategy_ic_rules.cpp:387-389`), standardise rows ("theme-weighted-rank-sum-missing-neutral; ...",
  `:390-393`), and the no-theme blend (fixed denominator, `strategy_ic_runner.cpp:118-119`).
- Nothing else moves: the composition weights file is read and verified as without the flag (theme-erc-v1 / ic-shrink
  re-derivation, `strategy_ic_rules.cpp` verify rows), the theme masses are formed from the pinned weights
  (`strategy_ic_composition.cpp:256-257`, `:263-265`), signs and theme blocks are the file's.
- Reading of the source (non-binding; the C++ lane decides): the composer already skips a candidate added with sign 0
  or an empty signal (`strategy_ic_composition.cpp:314-318`) and every non-finite cell (`:333`), while the masses
  were fixed at create; so the effect is reachable without touching `IcComposition`.
- With the flag, `summary.json` gains one top-level key, absent without it:
  `"absent_members": {"rule": "absent-members-v1", "list_sha256": "<hex>", "members": [<ids as listed>]}`.
- Caches: the list changes no signal-cache or IC-result-cache entry (key or bytes), and a cache dir shared by runs
  with and without the flag never serves one run an entry whose use would change its bytes. The cycle shares one
  `ic.cache` between a book's runs with and without the list (the clean-source book and the other history lines).
- `--no-composition` with the flag: the C++ lane's choice (apply it as section 5 defines, or refuse the pair); the
  cycle does not send the pair in the OD-3 read (a screen is not a history read).

## 5. The defining equivalence (the test that defines the flag)

For a pass P with argv A on inputs I and a list L naming member m:

> the published outputs of `A + [--absent-members L --absent-members-sha256 S]` on I equal, byte for byte, the outputs
> of A on I', where I' is I with m's data removed (m's evaluated signal non-finite on every cell),

compared over every output file the pass publishes, excluding only: wall-clock and stage-seconds keys, cache
hit / miss and cache-entry keys (they differ between two cold caches when a field payload differs), and the
`absent_members` record of section 4. Both runs use fresh cache dirs.

Fixture guidance: tiny_world, a library in which m's DSL reads a field no other candidate reads; I' is the same fields
dir with that field non-finite on every cell. Cases the tests should cover (each against the equivalence, plus
flag-absent = unchanged bytes):

1. m in a theme with other members (the theme's mass stays: the others carry it);
2. every member of one theme listed (the theme with no present member);
3. a theme-erc-v1 base and an ic-shrink-v1 base (the verify passes; the weights file is the same bytes);
4. an ew-theme-std-v1 base (the standardise path, `add_standardised`);
5. a base without themes (fixed-denominator blend);
6. the u pass (no `--composition-weights`) and the w pass (`--composition-weights`).

The OD-3 read needs at least the composite outputs (the combined signal the NAV reads and the `__combined__` rows).
If the C++ lane finds that the per-member rows of the equivalence cannot hold as stated (for example because of the
IC-result cache), it states in its report the exact output set it proves and the PM rules; the cycle does not depend
on which (it reads only the capability).

Refusals (exit as the exe's other usage refusals, before any output is written): one of the two flags without the
other; a repeated flag; the file missing or unreadable; a SHA-256 mismatch; not JSON or not an object; `schema` not
`atx.od3-flagged-members/v1`; `members` missing, not an array, empty, holding a non-string, a repeated id or an id
not matching `[A-Za-z0-9_]+`; a listed id that is not a candidate of `--library`. Each refusal gets a test with teeth.

## 6. Where the cycle puts the flags (done in OD3P)

- Spec: `fit.carry.drop {path, sha256}` (lock fills `sha256`; the plan shows `# pin carry:drop`). Code:
  `scripts/research_roles.py` `RolesCycle.absent_members`, called from `RolesCycle.steps` after the carry step.
- Roles: every role that is a history era (`is_history`: era end on or before the TRAIN begin of the research window,
  2020-01-01 in `research-window-v2`); the TRAIN anchor and any role inside TRAIN get nothing.
- Passes: the u and the w IC passes (`ABSENT_PHASES`) of each such role. The NAV, fields, check and summ steps get
  nothing; the carry step gets `--drop-list <path> --drop-list-sha256 <pin>` (it checks and records the list and
  writes every weights file as without it: the carried weights are TRAIN's bytes).
- Position: the runner gets `--bind <path>` as its last bind before `--`; the IC exe's argv ends with
  `--absent-members <path> --absent-members-sha256 <pin>` (after `--candidate-cache` and any `--theme-registry`).
  `<path>` is the spec's repo-relative path, `<pin>` the verified SHA-256 of the file.
- Refusals in the cycle (exit 2, at plan / status / run, before any step runs): the IC exe does not offer
  `absent-members-v1` (the message names the capability, the history roles and OD3P-1, and says the clean-source
  line is then printed "not read: no producer"); a drop with no history role; a nav / ref flag or nav rule naming a
  listed rule. The carry tool refuses (publishing nothing) an unknown, repeated or malformed id, pending rules, a base
  applying a listed rule, a non-v1/v2 weights document and a list without `--era`.
- Added in OD3P fix round 1 (cycle side only; sections 2-5 unchanged): the carry tool's list and base checks also run
  at plan time (`check_drop_list`); a history u / w pass without the bounded runner's `--` is refused, never
  planned without the flags; a `fit.output` that exists and whose `carry.json` does not record exactly this carry is
  refused (review R-1: each history book carries into its own fresh `fit.output`, runbook B6).

Synthetic w-pass argv of a history era E1 (`fit.carry.drop.path` = `fit/flagged_members.json`):

```
<py> scripts/runner.py ... --role-id E1 --output out/WT-E1-run1 --bind bin/ic.exe ... --bind fit/flagged_members.json --
  bin/ic.exe --library lib/lib.json --library-sha256 <L> --train e1/manifest.json --train-sha256 <R1> ...
  --composition-weights out/W/composition_weights.E1.json --composition-weights-sha256 <W1>
  --absent-members fit/flagged_members.json --absent-members-sha256 <D>
```

## 7. What the C++ lane hands back

- `absent-members-v1` in the envelope; the flags, the record and the refusals of sections 3-5.
- Tests: the equivalence cases of section 5 and the flag-absent identity (every existing golden unchanged).
- Root at the merge: the cycle tests of OD3P (`scripts/tests/test_research_cycle_od3.py`) run against the real
  envelope by `exe_capabilities` on the built exe (the probe path is tested on a fake in OD3P), then the OD-3 runbook
  `p9-od3-runbook.md` step for the clean-source line runs; without the capability that step prints "not read: no
  producer".
