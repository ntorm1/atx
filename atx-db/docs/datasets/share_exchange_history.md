# share_exchange_history (`security_master/share_exchange_history.parquet`)

CRSP-style dated share codes (`shrcd`) and exchange codes (`exchcd`) per line (tier1-v3 S2.5 codes, module
`atx_db.alpha_panel.identity_v3 codes`). Rebuilt 2026-10-03 in tier1-v4 Task 1 (same 397,050 rows as the
2026-09-30 build; the manifest now binds the current code). Manifest
`security_master/share_exchange_history_manifest.json` (schema `atx.alpha-panel.share-exchange-history/v1`; no
done criterion is defined for it).

- Path: `data/alpha_panel/v1/security_master/share_exchange_history.parquet` (397,050 rows, 2,291,538 bytes).
- Keys: `(security_id, valid_from)`, unique; runs never overlap within a line (0 overlaps measured).
- Span: 2018-01-02 .. 2026-09-18, 22,704 lines. Delisted lines included up to their last panel session.
- Input: the v2 panel's point-in-time security flags (`panel/`, manifest SHA-256 in the manifest).

## Rule

One row per maximal run of consecutive calendar sessions with the same `(exchange, exchcd, shrcd, security_type)`.

- `exchcd`: 1 NYSE (`XNYS`), 2 NYSE American (`XASE`), 3 Nasdaq (`XNAS`), 4 NYSE Arca (`ARCX`), 5 Cboe BZX (`BATS`),
  6 IEX (`IEXG`) (5 and 6 are our extensions), 0 OTC; NULL when the panel `exchange` is NULL (unknown, not OTC).
- `shrcd` (first matching): 73 ETF / ETP (`is_etf`); 31 ADR (`is_adr` or `is_adr_likely`); 14 closed-end fund
  (`security_type` fund); 71 LP; 18 REIT common; 12 foreign private issuer common (`is_fpi`); 11 US common
  (`security_type` common / common_unverified); NULL otherwise (preferred, warrant, unit, right, note, SPAC, unknown).
  CRSP 10 is not used (11 covers ordinary common).

## PIT rule

The panel's flags on session d use data visible before d-1 22:00 UTC. A run is stamped `available_at` =
`valid_from` - 2 h (22:00 UTC of the calendar day before its first session). Under the strict lake rule
(`available_at < lake_cutoff(d)`) a run becomes visible one session after `valid_from`; consumers that need the
code of session d should interval-join on `valid_from <= d <= valid_to` (the flags themselves are point in time).

## Columns

| name | type | unit | definition | source | clock | staleness |
|---|---|---|---|---|---|---|
| security_id | BIGINT | id | vendor line | panel | - | - |
| valid_from / valid_to | DATE | date | first / last session of the run (inclusive) | panel | - | daily |
| sessions | BIGINT | count | sessions in the run | panel | - | - |
| exchcd | INTEGER | code | CRSP-style exchange code (above) | panel `exchange` | valid_from - 2 h | daily |
| exchange | VARCHAR | MIC | panel exchange (`XNYS`, `XNAS`, `XASE`, `ARCX`, `OTC`, NULL) | panel | valid_from - 2 h | daily |
| shrcd | INTEGER | code | CRSP-style share code (above) | panel flags | valid_from - 2 h | daily |
| security_type | VARCHAR | - | panel security type | panel | valid_from - 2 h | daily |
| available_at | TIMESTAMP | UTC | valid_from - 2 h | - | - | - |

## Measured

Line-sessions by `shrcd`: 11 10,604,499; 12 905,462; 18 554,816; 31 818,236; 71 176,661; 73 6,891,556; NULL
1,917,612. Runs by `exchcd`: 1 69,311 (6.59 M sessions); 2 12,775 (0.64 M); 3 94,665 (6.08 M); 4 40,968 (4.03 M);
0 6; NULL 179,325 (4.52 M); no BATS / IEX lines occur.

Panel `member_equity` cells 2019-2026: `shrcd` non-NULL 100% every year; `exchcd` in 1-6: 2019 98.4%, 2020 97.4%,
2021 95.1%, 2022 97.4%, 2023 97.6%, 2024 97.6%, 2025 96.4%, 2026 96.2% (the rest has a NULL panel exchange).
Member cells with `shrcd` 11: 82-83%; 12: 4.6-7.4%; 18: 4.5-6.4%.

## Known limits

- Runs are fragmented where the panel flags flicker (e.g. SPAC 92,209 runs averaging 6 sessions, common_unverified
  81,067 runs averaging 44): prefer the session-level join over run counts.
- `exchcd` NULL means the panel has no exchange that session, not OTC.
- The share code is derived from vendor / FINRA / SEC flags, not from CRSP; ADR detection includes the
  `is_adr_likely` heuristic (FPI filer with common-like line).
