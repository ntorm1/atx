### Task T33b: `build_terminal_events.py` + `terminal_*` fields (Python)

**Lane:** C · **Pool:** pool-5 (`git checkout -B feat/mega-alpha-v5-delisting-20260927 <pool-2 HEAD>`) · **Model:** Opus 5.5 · **Depends on:** T33a = GO · **Tokens:** FIELDS · **Peak:** 0.3 GiB

**Files:**
- Create: `atx-engine/tools/build_terminal_events.py`, `atx-engine/tools/terminal_events_schema.md`, `atx-engine/tools/test_build_terminal_events.py`
- Modify: `atx-engine/tools/prepare_research_fields.py:884` (`tickerhistory_fields`), `:1879-1905` (manifest), `:344-396` field list; `test_prepare_research_fields.py:1208-1211` (field count 40 → 42)

**Interfaces:**
- Produces: `build-equity/terminal-events-v1/{terminal_events.parquet, manifest.json}` with columns `cik, instrument_id, terminal_session, kind ∈ {mna, performance, unknown}, venue ∈ {nyse, amex, nasdaq, unknown}, replacement_return, source_accession, source_accepted_utc`; fields `terminal_kind` (0 none, 1 mna, 2 performance, 3 unknown) and `terminal_return` (η: 0 / −0.30 / −0.55 / −0.35; NaN when none) on the instrument's **last present session**, known only at/after `source_accepted_utc` + 1 session (PIT).
- CLI: `build_terminal_events.py --submissions <parquet|duckdb ro> --bridge <dir> --role <TRAIN role> --out <dir> --seal 2023-01-01` (refuses any role past 2022-12-31 for TRAIN builds; VAL build is a root-only, later, disclosed step).

- [ ] **Step 1:** Implement classification exactly as T33a's SQL; write the schema doc with the η table from §4.D.
- [ ] **Step 2:** In `prepare_research_fields.py`, add `--terminal-events <dir>` (optional; absent → fields NaN, manifest notes "terminal: none"); emit the two fields with the +1-session PIT lag; manifest records the events manifest SHA.
- [ ] **Step 3:** Fixtures: synthetic 3-name role with one M&A, one Form-25, one unknown termination → expected `terminal_kind`/`terminal_return` on the right sessions; a termination whose 8-K is accepted after the last session must appear one session after acceptance, never earlier.
- [ ] **Step 4:** Report (root command: rebuild TRAIN fields as `…-fields-v7` with the same 40 fields + 2; expect byte-identical 40 legacy columns) and commit: `feat(fields): terminal events producer and terminal_kind/terminal_return fields (T33b)`.

**Acceptance (root, T39):** fields-v7 TRAIN manifest lists 42 fields; the 40 v6 columns are SHA-identical; count of non-NaN `terminal_return` equals T33a's classifiable count.

---

