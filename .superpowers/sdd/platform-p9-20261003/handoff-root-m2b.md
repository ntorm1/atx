# Handoff: root M2b (HYG-1, A3 post-flip checks, AL-SIG), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Code head **`6673f8d1`**; the docs commit that adds
this file sits on top of it. Detail: `root-wave2-merge-report.md` section M2b (and the A3 section of M2a, now
completed); one row per lane in `integration-log.md`; evidence logs in `m2b-logs/`. Nothing is half-merged; nothing of
mine is running. Next free build tag: **p9-1x**.

## Done

| step | result | commits |
|---|---|---|
| HYG-1 | 609 staged `wq-explore/.mypy_cache` paths unstaged; both cache dirs deleted by name. `tools/.mypy_cache` was **committed** (129 files, `749f9ac2`), so its deletion is a commit | `56060f84` |
| A3 post-flip | registry `ok 92` + round-trip (9 engine rows); real exe pytest 35p / 9p / 11p; DISK-2 pool-12: nothing left to delete | docs only |
| AL-SIG merge | `ebb081ab` -> `b1406e14`; 7 shared-tail conflicts with A3, all keep-both (kind table now 11) | `b1406e14` |
| ALSIG-9 re-proof | p9-1w exit 0, 0 warnings; gtests 33p filter, 92/92 whole (M1a-RED pair green); pytest 39 / 16 / 11 / 9 p; candidates validate 1 valid; admission refusal lines read | - |
| SQL2-CLS | `atx.p9-blocked-registration/v1` -> new class `blocked-registration`; `atx.alpha-panel.indexes/v1` -> `legacy_allow` (the guard's actual red); fixture chain `d32f7655...4e69` unchanged | `6673f8d1` |
| W1-MIN M-2 | seam holds: no theme added, registry themes = 13 built-in; theme-pinning pytests 31p | - |
| DISK-2 pool-13 | nothing to delete (lane already removed its object tree) | - |

Trial ledger at the end: `build-equity/trials.jsonl` **133 lines, sha256 `27e40f9f...`** (unchanged; 0 trials).

## Known-red now

- pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2's M1a-RED).
- Release `BookNormalScore.*` x2 (M1c-RED, C2).

No new red. Not re-run in M2b: the E2 pytest and the Release pair (no merge here touches them).

## Not done / next

- Remaining wave-2 merges (lanes in fix rounds at dispatch): E2, S2, B2, D2, then AL-COMB (after D2; re-check the
  W1-MIN M-2 theme seam there: AL-COMB may append a theme), C2, COV, T2 last as a unit (W2-ORDER).
- Wave-2 gate items unchanged: M2A-4 (real-tree bounded catalog run + X-5 opt-in cache identity on the final head);
  the store test group stays in `build-equity` (M2A-3).
- ALSIG-3 (carried): `prepare_research_fields_engine.ENGINE_FIELDS` does not route `gia_13f` / `russell_recon`; they
  build only via `atx-research-fields build --registry` with a v2 spec carrying `thirteenf` / `indexes`.
- `build-equity/p9-a3-train-{py,engine}{,-run}` and `build-equity/p9-a3-train-identity/` kept in pool-2 (owner
  question on the six-field TRAIN identity pending, RESUME 4).

## Uncommitted state

- `progress.md` modified (PM's; not touched). Untracked PNGs under `docs/plans/` (four; not mine; left alone).
- Session scratch (`m2b/` in my session scratchpad: basetemps, scratch catalogs, `keep_both.py`) deleted by name.

## Open questions for the PM

1. **SQL2-CLS class choice:** `atx.p9-blocked-registration/v1` got its own class row (`blocked-registration`, glob
   `**/blocked/*.json`, `spec_doc` kind `candidate`, render null) rather than joining `wave-candidate`, whose glob,
   typed candidate ingest and render rule do not fit hand-written blocked registrations. Accept, or name another home
   (one-line move; no fixture carries it)?
2. **SQL2-CLS literal named in the ledger was not the guard's red:** the guard scans only Python / C++, so it flagged
   `atx.alpha-panel.indexes/v1` (AL-SIG's reader) instead; registered as a dated `legacy_allow` row like its sibling
   `atx.alpha-panel.thirteenf/v1` (expires 2027-03-31). Accept?
3. **HYG-1 extension:** the committed `tools/.mypy_cache` was removed by a root commit (`56060f84`) rather than only
   deleted on disk (deleting a tracked dir otherwise leaves 129 dirty entries). Accept?
4. FYI: `handoff-root-m2a.md`'s registry-check command passed `fr.python_modules(d)` (module names) to
   `bind_modules` (expects modules); the check used `fr.bind(vars(b), d)` (the wave-1 recipe).
