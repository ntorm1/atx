# atx-db Tier-1 Parity — Sprint 1 (Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a complete atx-db warehouse buildable from zero by one idempotent, resumable, deterministic command (`atx-db activate`), and remove the wall-clock reads that make a rerun non-reproducible.

**Architecture:** A new `atx_db.activation` module holds one pure stage function per build step with the uniform signature `(store: DuckDBStore, options: ActivationOptions) -> StageResult`, plus a ladder runner that records every stage in a new `activation_stage_runs` table (migration 0300) and skips stages already `completed` unless `--force`. Every stage reuses an existing, proven code path (`run_governed_migrations`, `SecurityMasterDataset`, `NasdaqSymbolDirectoryDataset`, `publish_bulk_ticker_history`, `SecSubmissionsBulkDataset`, `SecCompanyFactsDataset`, `refresh_fundamental_statement_points` …); the ladder only sequences and ledgers them. Network access is confined to three stages, all of which take an injected `Downloader` callable so tests substitute a fake and never touch the network. Determinism is fixed at the source: one `atx_db.clock.resolve_as_of_date(explicit, *, source_max_date)` helper replaces nine `date.today()` / `utcnow()` defaults, and the wall clock is read in exactly one place (`atx_db.clock.utc_today()`), only at CLI/script edges.

**Tech Stack:** Python 3.12, DuckDB 1.5.x, pandas 3.x, pytest 9 + pytest-xdist, ruff, mypy --strict, GitHub Actions.

**Spec:** `C:\atx\docs\superpowers\specs\2026-09-19-tier1-parity-design.md`

**Supporting audits:** `C:\atx\.superpowers\sdd\tier1-parity\audit-atx-db.md` (§1, §4, §5, §6, §9), `C:\atx\.superpowers\sdd\tier1-parity\audit-ticker-zip.md`

## Global Constraints

Copy these verbatim into your working notes. Every task's requirements implicitly include this section.

- Python 3.12 venv at `C:\atx\atx-db\.venv\Scripts\python.exe`.
- Run tests from `C:\atx\atx-db` with `.venv\Scripts\python.exe -m pytest <file> -n 0 -q`.
- No network in tests.
- No wall-clock reads in derived/ingest paths except through `atx_db.warehouse.now_utc_naive()` for load stamps.
- Next migration number is 0300 (registry head is 0299 in `src/atx_db/migrations/registry.py`; `0297` is a gap inside `bodies_0296.py`, `0138-0139`, `0168-0175`, `0184` are gaps; **0300 is free**). Follow the `bodies_NNNN.py` + `registry.py` registration pattern.
- Commit per task with a conventional-commit subject `feat(db): ...` / `fix(db): ...`.
- Git commits end with the line `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Deterministic ordering: `ORDER BY` on every insert-select that feeds a hash or a published table.
- Never delete existing public tables.

## Network-access policy for this sprint

Network is confined to exactly three activation stages — `security_master`, `symbol_directory`, `sec_bulk_download` — and all three route through the injected `Downloader` protocol on `ActivationOptions`. Every other stage is offline. Tests inject `FakeDownloader`; no test ever constructs the real `requests_downloader`.

## File structure

| File | Responsibility |
| --- | --- |
| `src/atx_db/clock.py` (new) | The only place the wall clock is read (`utc_today`) and the only as-of-date resolution contract (`resolve_as_of_date`). |
| `src/atx_db/ticker_history_extract.py` (new) | Streaming ZIP-member → staging TSV extraction: size gate, sha256 sidecar, progress, resume. |
| `src/atx_db/activation.py` (new) | `StageResult`, `ActivationOptions`, `Downloader`, the stage registry, the stage ledger, `run_activation`. |
| `src/atx_db/migrations/bodies_0300.py` (new) | `activation_stage_runs` table + catalog + quality-check registration. |
| `scripts/warehouse_activate.py` (new) | Operator entry point; argparse → `run_activation`. |
| `src/atx_db/cli.py` (modify) | `atx-db activate` subcommand. |
| `.github/workflows/atx-db.yml` (modify) | Full non-slow suite + ruff/mypy over the new modules. |
| `docs/PRODUCTION_RUNBOOK.md` (modify) | "Activation from scratch" section. |
| 10 existing modules (modify) | Determinism fixes flagged by audit §9. |

---

### Task 1: `atx_db.clock` and determinism group A

**Files:**
- Create: `src/atx_db/clock.py`
- Modify: `src/atx_db/valuation_multiples.py:1643-1646`
- Modify: `src/atx_db/delisting.py:248`
- Modify: `src/atx_db/identifier_resolution.py:216`
- Modify: `src/atx_db/identifier_decisions.py:165`
- Modify: `src/atx_db/fundamentals.py:508-519`, `src/atx_db/fundamentals.py:1147`
- Test: `tests/test_clock.py` (new)
- Test: `tests/test_determinism_as_of.py` (new)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `atx_db.clock.resolve_as_of_date(explicit: dt.date | None, *, source_max_date: dt.date | None = None) -> dt.date` — returns `explicit` if given, else `source_max_date` if given, else raises `ValueError`.
  - `atx_db.clock.utc_today() -> dt.date` — the single sanctioned wall-clock date read, for CLI/script edges only.
  - `atx_db.fundamentals._unresolved_cik_candidates(unresolved, *, run_id, as_of_date)` — `as_of_date` is now keyword-only and required.
  - `atx_db.identifier_resolution.IdentifierResolutionOptions.as_of_date: dt.date | None = None`
  - `atx_db.identifier_decisions.IdentifierResolutionDecisionOptions.as_of_date: dt.date | None = None`

- [ ] **Step 1: Write the failing test for the clock contract**

Create `tests/test_clock.py`:

```python
"""The single as-of-date resolution contract for deterministic ingest paths."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.clock import resolve_as_of_date, utc_today


def test_explicit_as_of_date_wins_over_source_max_date():
    resolved = resolve_as_of_date(dt.date(2020, 1, 2), source_max_date=dt.date(2024, 6, 30))
    assert resolved == dt.date(2020, 1, 2)


def test_source_max_date_is_used_when_no_explicit_date():
    resolved = resolve_as_of_date(None, source_max_date=dt.date(2024, 6, 30))
    assert resolved == dt.date(2024, 6, 30)


def test_missing_both_raises_with_an_actionable_message():
    with pytest.raises(ValueError) as excinfo:
        resolve_as_of_date(None)
    message = str(excinfo.value)
    assert "as_of_date is required" in message
    assert "source_max_date" in message


def test_datetime_source_max_date_is_narrowed_to_a_date():
    resolved = resolve_as_of_date(None, source_max_date=dt.datetime(2024, 6, 30, 22, 0, 0))
    assert resolved == dt.date(2024, 6, 30)
    assert not isinstance(resolved, dt.datetime)


def test_utc_today_matches_the_utc_calendar_date():
    assert utc_today() == dt.datetime.now(dt.timezone.utc).date()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_clock.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.clock'`

- [ ] **Step 3: Create `src/atx_db/clock.py`**

```python
"""The warehouse as-of-date contract.

Determinism rule (see docs/PRODUCTION_RUNBOOK.md "Determinism"): no derived or
ingest path may read the wall clock. A row's ``as_of_date`` is either supplied
explicitly by the operator or derived from the source data itself, so rerunning
the same inputs produces byte-identical rows.

``now_utc_naive()`` in ``atx_db.warehouse`` remains the only sanctioned wall-clock
*timestamp* read, and only for lineage stamps (``source_loaded_at``) that never
feed a signal. ``utc_today()`` here is the only sanctioned wall-clock *date* read,
and only at a CLI or script edge, where it is passed down explicitly.
"""

from __future__ import annotations

import datetime as dt

__all__ = ["resolve_as_of_date", "utc_today"]


def utc_today() -> dt.date:
    """Return today's UTC calendar date.

    Call this ONLY from a CLI/script edge and pass the result down explicitly.
    Library code must take an ``as_of_date`` argument instead.
    """
    return dt.datetime.now(dt.timezone.utc).date()


def resolve_as_of_date(
    explicit: dt.date | None,
    *,
    source_max_date: dt.date | dt.datetime | None = None,
) -> dt.date:
    """Resolve the ``as_of_date`` for a batch of rows without reading the clock.

    Precedence: an operator-supplied ``explicit`` date, then a date derived from
    the source data (``source_max_date`` — e.g. a vendor file's creation time or
    the maximum filing date in the batch). If neither is available the caller is
    asking for a non-reproducible run, which is an error.
    """
    if explicit is not None:
        if isinstance(explicit, dt.datetime):
            return explicit.date()
        return explicit
    if source_max_date is not None:
        if isinstance(source_max_date, dt.datetime):
            return source_max_date.date()
        return source_max_date
    raise ValueError(
        "as_of_date is required: pass an explicit date, or supply source_max_date "
        "derived from the source data. Ingest and derived paths must not read the "
        "wall clock (atx_db.clock.utc_today() is for CLI/script edges only)."
    )
```

- [ ] **Step 4: Run the clock test to verify it passes**

Run: `.venv\Scripts\python.exe -m pytest tests/test_clock.py -n 0 -q`
Expected: PASS — `5 passed`

- [ ] **Step 5: Write the failing tests for determinism group A**

Create `tests/test_determinism_as_of.py`:

```python
"""Audit §9 determinism fixes: no wall-clock reads on reproducible row builders."""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.fundamentals import _unresolved_cik_candidates
from atx_db.identifier_decisions import IdentifierResolutionDecisionOptions
from atx_db.identifier_resolution import IdentifierResolutionOptions
from atx_db.valuation_multiples import ValuationMultiplesOptions, _overlap_slice_row


def test_overlap_slice_row_falls_back_to_now_utc_naive_not_utcnow(monkeypatch):
    sentinel = dt.datetime(2024, 6, 30, 12, 0, 0)
    monkeypatch.setattr("atx_db.valuation_multiples.now_utc_naive", lambda: sentinel)
    row = _overlap_slice_row(ValuationMultiplesOptions(), {})
    assert row["available_at"] == sentinel
    assert row["as_of_date"] == dt.date(2024, 6, 30)


def test_overlap_slice_row_prefers_source_timestamps_over_the_clock(monkeypatch):
    monkeypatch.setattr(
        "atx_db.valuation_multiples.now_utc_naive",
        lambda: pytest.fail("clock read despite available source timestamps"),
    )
    row = _overlap_slice_row(
        ValuationMultiplesOptions(),
        {"as_of_ts": "2023-03-31T22:00:00", "max_valuation_trade_date": "2023-03-31"},
    )
    assert row["available_at"] == dt.datetime(2023, 3, 31, 22, 0, 0)
    assert row["as_of_date"] == dt.date(2023, 3, 31)


def test_unresolved_cik_candidates_requires_an_explicit_as_of_date():
    unresolved = pd.DataFrame(
        [{"cik": "0000320193", "security_id": "SEC-CIK-0000320193", "available_at": pd.Timestamp("2024-02-01 22:00:00")}]
    )
    with pytest.raises(TypeError):
        _unresolved_cik_candidates(unresolved, run_id="r1")  # type: ignore[call-arg]


def test_unresolved_cik_candidates_stamps_the_supplied_as_of_date():
    unresolved = pd.DataFrame(
        [{"cik": "0000320193", "security_id": "SEC-CIK-0000320193", "available_at": pd.Timestamp("2024-02-01 22:00:00")}]
    )
    frame = _unresolved_cik_candidates(unresolved, run_id="r1", as_of_date=dt.date(2024, 2, 1))
    assert list(frame["as_of_date"]) == [dt.date(2024, 2, 1)]


def test_identifier_resolution_options_expose_an_as_of_date():
    assert IdentifierResolutionOptions().as_of_date is None
    assert IdentifierResolutionOptions(as_of_date=dt.date(2024, 5, 1)).as_of_date == dt.date(2024, 5, 1)


def test_identifier_decision_options_expose_an_as_of_date():
    assert IdentifierResolutionDecisionOptions().as_of_date is None
    assert (
        IdentifierResolutionDecisionOptions(as_of_date=dt.date(2024, 5, 1)).as_of_date
        == dt.date(2024, 5, 1)
    )


def test_no_wall_clock_reads_remain_in_the_group_a_modules():
    import pathlib

    root = pathlib.Path(__file__).resolve().parents[1] / "src" / "atx_db"
    offenders: list[str] = []
    for name in (
        "valuation_multiples.py",
        "delisting.py",
        "identifier_resolution.py",
        "identifier_decisions.py",
        "fundamentals.py",
    ):
        text = (root / name).read_text(encoding="utf-8")
        for needle in ("date.today()", "datetime.utcnow()", "datetime.now("):
            if needle in text:
                offenders.append(f"{name}: {needle}")
    assert offenders == [], offenders
```

- [ ] **Step 6: Run the group A test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_determinism_as_of.py -n 0 -q`
Expected: FAIL — `AttributeError: module 'atx_db.valuation_multiples' has no attribute 'now_utc_naive'` plus dataclass-field and offender-list failures.

- [ ] **Step 7: Fix `valuation_multiples.py`**

At the top of `src/atx_db/valuation_multiples.py`, add `now_utc_naive` to the existing `from .warehouse import ...` line (create the import if the module does not already import from `.warehouse`):

```python
from .warehouse import now_utc_naive
```

Then replace the fallback at `valuation_multiples.py:1643-1646`:

```python
def _overlap_slice_row(options: ValuationMultiplesOptions, details: dict[str, object]) -> dict[str, object]:
    available_at = (
        _timestamp_from_iso(details.get("as_of_ts"))
        or _timestamp_from_iso(details.get("max_visible_available_at"))
        or now_utc_naive()
    )
```

- [ ] **Step 8: Fix `delisting.py`**

Replace line 248:

```python
    now = dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)
```

with a load stamp routed through the warehouse contract (the import already exists in this module; if not, add `from .warehouse import now_utc_naive`):

```python
    now = now_utc_naive()
```

- [ ] **Step 9: Fix `identifier_resolution.py`**

Add the option field to `IdentifierResolutionOptions` (`identifier_resolution.py:45-51`):

```python
@dataclass(frozen=True)
class IdentifierResolutionOptions:
    source_dataset_id: str = "sec_13f"
    source_period: str | None = None
    min_confidence: float = 0.8
    include_already_mapped: bool = True
    source: str = SOURCE_NAME
    as_of_date: dt.date | None = None
    run_id: str | None = None
```

Add the import near the other `atx_db` imports:

```python
from .clock import resolve_as_of_date
```

Replace line 216:

```python
            as_of_date = row.max_filing_date or row.max_report_date or dt.date.today()
```

with:

```python
            as_of_date = resolve_as_of_date(
                row.max_filing_date or row.max_report_date or None,
                source_max_date=options.as_of_date,
            )
```

- [ ] **Step 10: Fix `identifier_decisions.py`**

Add the option field to `IdentifierResolutionDecisionOptions` (`identifier_decisions.py:31-42`), immediately before `run_id`:

```python
    as_of_date: dt.date | None = None
```

Add the import:

```python
from .clock import resolve_as_of_date
```

Replace line 165:

```python
            as_of_date = row.as_of_date or dt.date.today()
```

with:

```python
            as_of_date = resolve_as_of_date(
                row.as_of_date or None,
                source_max_date=options.as_of_date,
            )
```

- [ ] **Step 11: Fix `fundamentals.py`**

Change the signature at `fundamentals.py:508`:

```python
def _unresolved_cik_candidates(
    unresolved: pd.DataFrame,
    *,
    run_id: str | None,
    as_of_date: dt.date,
) -> pd.DataFrame:
```

Delete line 519 (`as_of_date = dt.date.today()`), keeping the line below it:

```python
    available_at = now_utc_naive()
```

At the call site (`fundamentals.py:1147`), derive the date from the source batch — the maximum `available_at` already carried by the unresolved rows — and fall back to the operator-supplied option:

```python
        candidates = _unresolved_cik_candidates(
            unresolved_frame,
            run_id=options.run_id,
            as_of_date=resolve_as_of_date(
                options.as_of_date,
                source_max_date=(
                    None
                    if unresolved_frame.empty
                    else pd.to_datetime(unresolved_frame["available_at"]).max().date()
                ),
            ),
        )
```

Add the import near the other `atx_db` imports in `fundamentals.py`:

```python
from .clock import resolve_as_of_date
```

- [ ] **Step 12: Run the group A tests to verify they pass**

Run: `.venv\Scripts\python.exe -m pytest tests/test_clock.py tests/test_determinism_as_of.py -n 0 -q`
Expected: PASS — `12 passed`

- [ ] **Step 13: Run the regression lane for the touched modules**

Run: `.venv\Scripts\python.exe -m pytest tests/test_valuation_multiples.py tests/test_delisting_returns.py tests/test_identifier_spine.py tests/test_fundamentals.py -n 0 -q`
Expected: PASS (no failures; `@pytest.mark.slow` cases report as skipped).

- [ ] **Step 14: Lint and typecheck the new module**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/clock.py tests/test_clock.py tests/test_determinism_as_of.py`
Expected: `All checks passed!`

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/clock.py`
Expected: `Success: no issues found in 1 source file`

- [ ] **Step 15: Commit**

```bash
git add src/atx_db/clock.py src/atx_db/valuation_multiples.py src/atx_db/delisting.py src/atx_db/identifier_resolution.py src/atx_db/identifier_decisions.py src/atx_db/fundamentals.py tests/test_clock.py tests/test_determinism_as_of.py
git commit -m "fix(db): remove wall-clock reads from derived and candidate row builders

Adds atx_db.clock.resolve_as_of_date as the single as-of-date contract and
routes valuation_multiples, delisting, identifier_resolution,
identifier_decisions, and fundamentals through it. Audit section 9 group A.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Determinism group B — option-plumbed ingest edges

**Files:**
- Modify: `src/atx_db/security_master.py:31-36` (options), `:438-449` (`upsert_security_master_from_frame`), `:699`
- Modify: `src/atx_db/symbol_directory.py:241`, `:290+` (`NasdaqListingEventsDataset.load`)
- Modify: `src/atx_db/identifiers_figi.py:258`
- Modify: `src/atx_db/identifiers_lei.py:422`
- Modify: `src/atx_db/finra.py:432-433`
- Modify: `scripts/backfill_finra_short_interest.py`, `scripts/download_finra_short_interest.py`
- Test: `tests/test_determinism_ingest_edges.py` (new)

**Interfaces:**
- Consumes: `atx_db.clock.resolve_as_of_date`, `atx_db.clock.utc_today` (Task 1).
- Produces:
  - `atx_db.security_master.SecurityMasterOptions.as_of_date: dt.date | None = None`
  - `atx_db.security_master.upsert_security_master_from_frame(store, frame, *, source, as_of_date, run_id=None) -> None` — `as_of_date` is keyword-only and required.
  - `atx_db.symbol_directory.NasdaqSymbolDirectoryDataset.load` derives `as_of_date` from the Nasdaq file-creation-time line when `options.as_of_date` is None.
  - `atx_db.finra.FinraShortInterestDataset.load` raises `ValueError` when `start_date`/`end_date` are both unset.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_determinism_ingest_edges.py`:

```python
"""Audit §9 group B: ingest loaders take an explicit or source-derived as_of_date."""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.finra import FinraShortInterestDataset, FinraShortInterestOptions
from atx_db.identifiers_figi import FigiLoadOptions
from atx_db.identifiers_lei import LeiLoadOptions
from atx_db.security_master import SecurityMasterOptions, upsert_security_master_from_frame
from atx_db.symbol_directory import (
    NasdaqSymbolDirectoryOptions,
    resolve_directory_as_of_date,
)

_DIRECTORY_TEXT = (
    "Symbol|Security Name|Market Category|Test Issue|Financial Status|Round Lot Size\n"
    "AAPL|Apple Inc. - Common Stock|Q|N|N|100\n"
    "File Creation Time: 0630202422:01|||||\n"
)


def test_security_master_options_expose_an_as_of_date():
    assert SecurityMasterOptions().as_of_date is None
    assert SecurityMasterOptions(as_of_date=dt.date(2024, 5, 1)).as_of_date == dt.date(2024, 5, 1)


def test_upsert_security_master_requires_an_explicit_as_of_date(tmp_store):
    frame = pd.DataFrame(
        [{"cik": "0000320193", "ticker": "AAPL", "title": "Apple Inc.", "security_id": "SEC-CIK-0000320193"}]
    )
    with pytest.raises(TypeError):
        upsert_security_master_from_frame(tmp_store, frame, source="unit-test")  # type: ignore[call-arg]


def test_upsert_security_master_stamps_the_supplied_as_of_date(tmp_store):
    frame = pd.DataFrame(
        [{"cik": "0000320193", "ticker": "AAPL", "title": "Apple Inc.", "security_id": "SEC-CIK-0000320193"}]
    )
    upsert_security_master_from_frame(
        tmp_store, frame, source="unit-test", as_of_date=dt.date(2021, 7, 4), run_id="r1"
    )
    row = tmp_store.con.execute(
        "SELECT min(as_of_date) FROM security_identifier_history WHERE source = 'unit-test'"
    ).fetchone()
    assert row is not None and row[0] == dt.date(2021, 7, 4)


def test_directory_as_of_date_comes_from_the_nasdaq_file_creation_time():
    assert resolve_directory_as_of_date(
        NasdaqSymbolDirectoryOptions(), _DIRECTORY_TEXT
    ) == dt.date(2024, 6, 30)


def test_explicit_directory_as_of_date_wins():
    assert resolve_directory_as_of_date(
        NasdaqSymbolDirectoryOptions(as_of_date=dt.date(2020, 1, 1)), _DIRECTORY_TEXT
    ) == dt.date(2020, 1, 1)


def test_directory_without_creation_time_or_explicit_date_raises():
    with pytest.raises(ValueError, match="as_of_date is required"):
        resolve_directory_as_of_date(NasdaqSymbolDirectoryOptions(), "Symbol|Security Name\nAAPL|Apple\n")


def test_figi_load_without_an_as_of_date_raises(tmp_store, tmp_path):
    from atx_db.identifiers_figi import FigiDataset

    figi_file = tmp_path / "figi.csv"
    figi_file.write_text("cusip,figi\n037833100,BBG000B9XRY4\n", encoding="utf-8")
    with pytest.raises(ValueError, match="as_of_date is required"):
        FigiDataset().load(tmp_store, FigiLoadOptions(figi_file=figi_file))


def test_lei_load_without_an_as_of_date_raises(tmp_store, tmp_path):
    from atx_db.identifiers_lei import LeiDataset

    lei_file = tmp_path / "lei.csv"
    lei_file.write_text("lei,cik\n549300FJ4DPCQZQCXQ36,0000320193\n", encoding="utf-8")
    with pytest.raises(ValueError, match="as_of_date is required"):
        LeiDataset().load(tmp_store, LeiLoadOptions(lei_file=lei_file))


def test_finra_load_without_a_date_window_raises(tmp_store):
    with pytest.raises(ValueError, match="start_date and end_date are required"):
        FinraShortInterestDataset().load(tmp_store, FinraShortInterestOptions())


def test_no_wall_clock_reads_remain_in_the_group_b_modules():
    import pathlib

    root = pathlib.Path(__file__).resolve().parents[1] / "src" / "atx_db"
    offenders: list[str] = []
    for name in (
        "security_master.py",
        "symbol_directory.py",
        "identifiers_figi.py",
        "identifiers_lei.py",
        "finra.py",
    ):
        text = (root / name).read_text(encoding="utf-8")
        for needle in ("date.today()", "datetime.utcnow()", "Timestamp.now("):
            if needle in text:
                offenders.append(f"{name}: {needle}")
    assert offenders == [], offenders
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_determinism_ingest_edges.py -n 0 -q`
Expected: FAIL — `ImportError: cannot import name 'resolve_directory_as_of_date' from 'atx_db.symbol_directory'`

- [ ] **Step 3: Fix `security_master.py`**

Add the import:

```python
from .clock import resolve_as_of_date
```

Add the option field to `SecurityMasterOptions` (`security_master.py:31-36`), before `run_id`:

```python
    as_of_date: dt.date | None = None
```

Change `upsert_security_master_from_frame` (`security_master.py:438-449`):

```python
def upsert_security_master_from_frame(
    store: DuckDBStore,
    frame: pd.DataFrame,
    *,
    source: str,
    as_of_date: dt.date,
    run_id: str | None = None,
) -> None:
    if frame.empty:
        return
    frame = ensure_security_frame_entity_ids(frame)
    today = as_of_date
    available_at = now_utc_naive()
```

(`now_utc_naive` is already imported from `.warehouse` in this module; if not, add it to the existing import list. `today` keeps its name so the rest of the function body is untouched.)

Update the caller at `security_master.py:699`:

```python
        upsert_security_master_from_frame(
            store,
            frame,
            source=self.source_name,
            as_of_date=resolve_as_of_date(options.as_of_date, source_max_date=None),
            run_id=options.run_id,
        )
```

- [ ] **Step 4: Fix `symbol_directory.py`**

Add the import:

```python
from .clock import resolve_as_of_date
```

Add a module-level resolver just after `_file_creation_time`:

```python
def resolve_directory_as_of_date(
    options: NasdaqSymbolDirectoryOptions | NasdaqListingEventsOptions,
    text: str,
) -> dt.date:
    """Resolve the snapshot date without reading the wall clock.

    The Nasdaq Trader files carry their own ``File Creation Time:`` trailer, so a
    reload of the same file always stamps the same ``as_of_date``.
    """
    created = _file_creation_time(text)
    return resolve_as_of_date(options.as_of_date, source_max_date=created)
```

In `NasdaqSymbolDirectoryDataset.load`, replace `as_of_date = options.as_of_date or dt.date.today()` (line 241) and defer resolution until after the first response body is available:

```python
    def load(self, store: DuckDBStore, options: NasdaqSymbolDirectoryOptions) -> DatasetLoadResult:
        session = requests.Session()
        session.headers.update({"User-Agent": options.user_agent, "Accept": "text/plain,*/*"})
        payloads: list[tuple[str, str, Any]] = []
        for url, normalizer in (
            (options.nasdaq_url, normalize_nasdaq_listed),
            (options.other_url, normalize_other_listed),
        ):
            response = session.get(url, timeout=options.request_timeout)
            response.raise_for_status()
            payloads.append((url, response.text, normalizer))
        as_of_date = resolve_directory_as_of_date(options, payloads[0][1])
        frames: list[pd.DataFrame] = []
        for url, text, normalizer in payloads:
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=url,
                status="fetched",
                metadata={"as_of_date": as_of_date.isoformat()},
            )
            frames.append(
                normalizer(
                    _read_directory_text(text),
                    as_of_date=as_of_date,
                    source_url=url,
                    run_id=options.run_id,
                )
            )
        frame = pd.concat([frame for frame in frames if not frame.empty], ignore_index=True)
        rows = self._replace_snapshot(store, frame, as_of_date)
```

(Leave the trailing `quality_check` / `DatasetLoadResult` block exactly as it is.)

Apply the same `resolve_directory_as_of_date(options, response.text)` substitution inside `NasdaqListingEventsDataset.load` wherever it currently defaults `as_of_date` to `dt.date.today()`.

- [ ] **Step 5: Fix `identifiers_figi.py`**

Add the import:

```python
from .clock import resolve_as_of_date
```

Replace line 258:

```python
        as_of_date = options.as_of_date or dt.date.today()
```

with (an OpenFIGI mapping file carries no date, so the operator must supply one):

```python
        as_of_date = resolve_as_of_date(options.as_of_date, source_max_date=None)
```

- [ ] **Step 6: Fix `identifiers_lei.py`**

Add the import:

```python
from .clock import resolve_as_of_date
```

Replace line 422:

```python
        as_of_date = options.as_of_date or dt.date.today()
```

with:

```python
        as_of_date = resolve_as_of_date(options.as_of_date, source_max_date=None)
```

- [ ] **Step 7: Fix `finra.py`**

Replace lines 432-433:

```python
            start_date = options.start_date or subtract_years(dt.date.today(), 5)
            end_date = options.end_date or dt.date.today()
```

with an explicit-window requirement:

```python
            if options.start_date is None or options.end_date is None:
                raise ValueError(
                    "start_date and end_date are required for a date-range FINRA load; "
                    "the CLI passes atx_db.clock.utc_today()-derived defaults explicitly"
                )
            start_date = options.start_date
            end_date = options.end_date
```

- [ ] **Step 8: Make the FINRA script edges explicit**

In `scripts/backfill_finra_short_interest.py` and `scripts/download_finra_short_interest.py`, add the import:

```python
from atx_db.clock import utc_today
from atx_db.finra import subtract_years
```

and give the `--start-date` / `--end-date` arguments concrete defaults computed at the edge:

```python
    today = utc_today()
    parser.add_argument("--start-date", type=dt.date.fromisoformat, default=subtract_years(today, 5))
    parser.add_argument("--end-date", type=dt.date.fromisoformat, default=today)
```

- [ ] **Step 9: Run the group B tests to verify they pass**

Run: `.venv\Scripts\python.exe -m pytest tests/test_determinism_ingest_edges.py -n 0 -q`
Expected: PASS — `10 passed`

- [ ] **Step 10: Run the regression lane for the touched modules**

Run: `.venv\Scripts\python.exe -m pytest tests/test_security_master.py tests/test_symbol_directory.py tests/test_identifiers.py tests/test_finra.py tests/test_identifier_spine.py -n 0 -q`
Expected: PASS (no failures).

- [ ] **Step 11: Lint**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/security_master.py src/atx_db/symbol_directory.py src/atx_db/identifiers_figi.py src/atx_db/identifiers_lei.py src/atx_db/finra.py tests/test_determinism_ingest_edges.py`
Expected: `All checks passed!`

- [ ] **Step 12: Commit**

```bash
git add src/atx_db/security_master.py src/atx_db/symbol_directory.py src/atx_db/identifiers_figi.py src/atx_db/identifiers_lei.py src/atx_db/finra.py scripts/backfill_finra_short_interest.py scripts/download_finra_short_interest.py tests/test_determinism_ingest_edges.py
git commit -m "fix(db): require an explicit or source-derived as_of_date in ingest loaders

Security master, Nasdaq directory, OpenFIGI, GLEIF, and FINRA loaders no
longer default as_of_date to the local wall-clock date; the Nasdaq snapshot
derives it from the file's own creation-time trailer and CLI edges pass
utc_today() explicitly. Audit section 9 group B.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Migration 0300 `activation_stage_runs` and the stage ledger

**Files:**
- Create: `src/atx_db/migrations/bodies_0300.py`
- Modify: `src/atx_db/migrations/registry.py` (2 lines)
- Create: `src/atx_db/activation.py` (ledger half only)
- Test: `tests/test_activation_ledger.py` (new)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - Table `activation_stage_runs(stage VARCHAR, run_id VARCHAR, status VARCHAR, started_at TIMESTAMP, finished_at TIMESTAMP, "rows" BIGINT, params_json VARCHAR, error VARCHAR)`, PK `(stage, run_id)`.
  - `atx_db.activation.STAGE_ORDER: tuple[str, ...]`
  - `atx_db.activation.StageResult(rows: int, detail: dict[str, object])`
  - `atx_db.activation.begin_stage(store, *, stage, run_id, params) -> dt.datetime`
  - `atx_db.activation.finish_stage(store, *, stage, run_id, started_at, status, rows=0, error=None) -> None`
  - `atx_db.activation.completed_stages(store) -> set[str]`
  - `atx_db.activation.select_stages(*, start=None, stop=None, only=()) -> tuple[str, ...]`

- [ ] **Step 1: Write the failing test**

Create `tests/test_activation_ledger.py`:

```python
"""Stage ledger for the resumable warehouse activation ladder."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.activation import (
    STAGE_ORDER,
    StageResult,
    begin_stage,
    completed_stages,
    finish_stage,
    select_stages,
)


def test_stage_order_is_the_documented_dependency_order():
    assert STAGE_ORDER == (
        "migrate",
        "security_master",
        "symbol_directory",
        "ticker_history_extract",
        "ticker_history_publish",
        "sec_bulk_download",
        "submissions_load",
        "companyfacts_load",
        "statement_points",
        "periods",
        "ttm",
        "calendarization",
        "standardized",
        "industry_templates",
        "reconciliation",
        "provider_coverage",
    )


def test_activation_stage_runs_table_exists_after_migration(tmp_store):
    row = tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE schema_name='main' AND table_name='activation_stage_runs'"
    ).fetchone()
    assert row is not None and row[0] == 1


def test_activation_stage_runs_has_the_charter_columns(tmp_store):
    columns = [
        name
        for (name,) in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() "
            "WHERE schema_name='main' AND table_name='activation_stage_runs' "
            "ORDER BY column_index"
        ).fetchall()
    ]
    assert columns == [
        "stage",
        "run_id",
        "status",
        "started_at",
        "finished_at",
        "rows",
        "params_json",
        "error",
    ]


def test_begin_then_finish_records_a_completed_stage(tmp_store):
    started = begin_stage(tmp_store, stage="migrate", run_id="run-1", params={"threads": 4})
    assert isinstance(started, dt.datetime)
    assert completed_stages(tmp_store) == set()
    finish_stage(
        tmp_store, stage="migrate", run_id="run-1", started_at=started, status="completed", rows=7
    )
    assert completed_stages(tmp_store) == {"migrate"}
    row = tmp_store.con.execute(
        'SELECT status, "rows", params_json, error FROM activation_stage_runs '
        "WHERE stage='migrate' AND run_id='run-1'"
    ).fetchone()
    assert row == ("completed", 7, '{"threads": 4}', None)


def test_failed_stage_is_not_reported_completed_and_carries_the_error(tmp_store):
    started = begin_stage(tmp_store, stage="periods", run_id="run-2", params={})
    finish_stage(
        tmp_store,
        stage="periods",
        run_id="run-2",
        started_at=started,
        status="failed",
        rows=0,
        error="boom",
    )
    assert completed_stages(tmp_store) == set()
    row = tmp_store.con.execute(
        "SELECT status, error FROM activation_stage_runs WHERE stage='periods'"
    ).fetchone()
    assert row == ("failed", "boom")


def test_a_later_completed_run_supersedes_an_earlier_failure(tmp_store):
    first = begin_stage(tmp_store, stage="periods", run_id="run-2", params={})
    finish_stage(tmp_store, stage="periods", run_id="run-2", started_at=first, status="failed", error="boom")
    second = begin_stage(tmp_store, stage="periods", run_id="run-3", params={})
    finish_stage(tmp_store, stage="periods", run_id="run-3", started_at=second, status="completed", rows=3)
    assert completed_stages(tmp_store) == {"periods"}


def test_select_stages_defaults_to_the_full_ladder():
    assert select_stages() == STAGE_ORDER


def test_select_stages_honours_start_and_stop():
    assert select_stages(start="periods", stop="standardized") == (
        "periods",
        "ttm",
        "calendarization",
        "standardized",
    )


def test_select_stages_only_is_order_normalized():
    assert select_stages(only=("standardized", "migrate")) == ("migrate", "standardized")


def test_select_stages_rejects_an_unknown_stage():
    with pytest.raises(ValueError, match="unknown activation stage"):
        select_stages(start="nope")


def test_stage_result_is_a_rows_plus_detail_pair():
    result = StageResult(rows=5, detail={"k": "v"})
    assert result.rows == 5
    assert result.detail == {"k": "v"}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_ledger.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.activation'`

- [ ] **Step 3: Write migration 0300**

Create `src/atx_db/migrations/bodies_0300.py`:

```python
"""Resumable warehouse-activation stage ledger."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _activation_stage_runs(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS activation_stage_runs (
            stage VARCHAR NOT NULL,
            run_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP,
            "rows" BIGINT,
            params_json VARCHAR,
            error VARCHAR,
            PRIMARY KEY (stage, run_id)
        );

        CREATE INDEX IF NOT EXISTS idx_activation_stage_runs_status
            ON activation_stage_runs(stage, status, started_at);
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "activation_stage_runs",
                "control",
                "activation_stage_run",
                "stage,run_id",
                "One row per attempt of a full-warehouse activation stage; the ladder "
                "skips stages whose newest attempt completed unless --force is given.",
                '["stage","run_id"]',
                "Operational lineage only. started_at/finished_at are warehouse load "
                "stamps and must never be used as a point-in-time availability key.",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                "activation_stage_runs_stuck_running",
                "warehouse_activation",
                "activation_stage_runs",
                "warning",
                0.0,
                "eq",
                True,
                "warning",
                "atx_tier1_parity",
            )
        ],
    )
    _catalog_fields_for_tables(conn, ("activation_stage_runs",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=300,
        name="activation_stage_runs",
        up=_activation_stage_runs,
    )
]
```

- [ ] **Step 4: Register the migration**

In `src/atx_db/migrations/registry.py`, add after line 125 (`from .bodies_0299 import ...`):

```python
from .bodies_0300 import MIGRATIONS as _MIGRATIONS_0300
```

and after line 248 (`*_MIGRATIONS_0299,`) inside the `MIGRATIONS` list:

```python
    *_MIGRATIONS_0300,
```

- [ ] **Step 5: Write the ledger half of `activation.py`**

Create `src/atx_db/activation.py`:

```python
"""Idempotent, resumable, deterministic full-warehouse activation.

The ladder runs one stage at a time in dependency order, records every attempt in
``activation_stage_runs``, and skips a stage whose newest attempt completed unless
``force`` is set. Every stage is a pure ``(store, options) -> StageResult``; the
ladder owns all ledgering, ordering, and reporting so a stage function stays
testable on its own.
"""

from __future__ import annotations

import datetime as dt
import json
from dataclasses import dataclass

from .connection import DuckDBStore
from .warehouse import now_utc_naive

STAGE_ORDER: tuple[str, ...] = (
    "migrate",
    "security_master",
    "symbol_directory",
    "ticker_history_extract",
    "ticker_history_publish",
    "sec_bulk_download",
    "submissions_load",
    "companyfacts_load",
    "statement_points",
    "periods",
    "ttm",
    "calendarization",
    "standardized",
    "industry_templates",
    "reconciliation",
    "provider_coverage",
)


@dataclass(frozen=True)
class StageResult:
    """What a stage did: a row count plus a JSON-serialisable detail payload."""

    rows: int
    detail: dict[str, object]


def select_stages(
    *,
    start: str | None = None,
    stop: str | None = None,
    only: tuple[str, ...] = (),
) -> tuple[str, ...]:
    """Resolve a ladder slice, always in ``STAGE_ORDER`` order."""
    for name in (*(() if start is None else (start,)), *(() if stop is None else (stop,)), *only):
        if name not in STAGE_ORDER:
            raise ValueError(f"unknown activation stage {name!r}; expected one of {STAGE_ORDER}")
    if only:
        chosen = set(only)
        return tuple(stage for stage in STAGE_ORDER if stage in chosen)
    first = 0 if start is None else STAGE_ORDER.index(start)
    last = len(STAGE_ORDER) - 1 if stop is None else STAGE_ORDER.index(stop)
    if last < first:
        raise ValueError(f"--stop-stage {stop!r} precedes --start-stage {start!r}")
    return STAGE_ORDER[first : last + 1]


def begin_stage(
    store: DuckDBStore,
    *,
    stage: str,
    run_id: str,
    params: dict[str, object],
) -> dt.datetime:
    """Record a ``running`` attempt and return its start stamp."""
    started_at = now_utc_naive()
    store.con.execute(
        """
        INSERT OR REPLACE INTO activation_stage_runs (
            stage, run_id, status, started_at, finished_at, "rows", params_json, error
        ) VALUES (?, ?, 'running', ?, NULL, NULL, ?, NULL)
        """,
        [stage, run_id, started_at, json.dumps(params, default=str, sort_keys=True)],
    )
    return started_at


def finish_stage(
    store: DuckDBStore,
    *,
    stage: str,
    run_id: str,
    started_at: dt.datetime,
    status: str,
    rows: int = 0,
    error: str | None = None,
) -> None:
    """Close out an attempt as ``completed``, ``failed``, ``skipped`` or ``dry_run``."""
    store.con.execute(
        """
        UPDATE activation_stage_runs
        SET status = ?, finished_at = ?, "rows" = ?, error = ?
        WHERE stage = ? AND run_id = ?
        """,
        [status, now_utc_naive(), int(rows), error, stage, run_id],
    )
    _ = started_at  # start stamp is already persisted by begin_stage


def completed_stages(store: DuckDBStore) -> set[str]:
    """Return the stages whose NEWEST attempt completed.

    A later failure supersedes an earlier success and vice versa, so a resumed
    ladder always reflects the most recent evidence.
    """
    rows = store.con.execute(
        """
        SELECT stage
        FROM (
            SELECT stage, status,
                   row_number() OVER (
                       PARTITION BY stage ORDER BY started_at DESC, run_id DESC
                   ) AS attempt_rank
            FROM activation_stage_runs
        )
        WHERE attempt_rank = 1 AND status = 'completed'
        ORDER BY stage
        """
    ).fetchall()
    return {stage for (stage,) in rows}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_ledger.py -n 0 -q`
Expected: PASS — `11 passed`

Note: the first run rebuilds the fingerprinted schema template (~180 s) because `migrations/*.py` changed. Subsequent runs reuse it.

- [ ] **Step 7: Run migration governance and schema contract**

Run: `.venv\Scripts\python.exe -m pytest tests/test_migration_governance.py tests/test_schema_contract_v2.py -n 0 -q --run-slow`
Expected: PASS (no failures).

- [ ] **Step 8: Lint and typecheck**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/activation.py src/atx_db/migrations/bodies_0300.py tests/test_activation_ledger.py`
Expected: `All checks passed!`

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/activation.py`
Expected: `Success: no issues found in 1 source file`

- [ ] **Step 9: Commit**

```bash
git add src/atx_db/migrations/bodies_0300.py src/atx_db/migrations/registry.py src/atx_db/activation.py tests/test_activation_ledger.py
git commit -m "feat(db): activation stage ledger and migration 0300

Adds activation_stage_runs plus the ledger primitives (begin_stage,
finish_stage, completed_stages, select_stages) the resumable full-warehouse
activation ladder is built on.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Streaming ticker-history ZIP → TSV extraction

**Files:**
- Create: `src/atx_db/ticker_history_extract.py`
- Test: `tests/test_ticker_history_extract.py` (new)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `atx_db.ticker_history_extract.TICKER_HISTORY_UNCOMPRESSED_BYTES: int = 11_084_562_320`
  - `atx_db.ticker_history_extract.TickerHistoryExtractResult(tsv_path: Path, member_name: str, expected_bytes: int, written_bytes: int, sha256: str, skipped: bool)`
  - `atx_db.ticker_history_extract.sidecar_path(tsv_path: Path) -> Path`
  - `atx_db.ticker_history_extract.extract_ticker_history_tsv(zip_path, tsv_path, *, expected_bytes=TICKER_HISTORY_UNCOMPRESSED_BYTES, progress=None, progress_every_bytes=1<<30) -> TickerHistoryExtractResult`

- [ ] **Step 1: Write the failing test**

Create `tests/test_ticker_history_extract.py`:

```python
"""Streaming extraction of the SpiderRock ticker-history archive member."""

from __future__ import annotations

import hashlib
import zipfile
from pathlib import Path

import pytest

from atx_db.ticker_history_extract import (
    TICKER_HISTORY_UNCOMPRESSED_BYTES,
    extract_ticker_history_tsv,
    sidecar_path,
)

_HEADER = "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tclosePr\tvolume\tshares\treturnFactor"
_ROWS = [
    "2024-01-02\t32952\tAAA\tAAA\t10.0\t10.5\t9.9\t10.2\t10.2\t1000\t1000000\t1.0",
    "2024-01-02\t32953\tBBB\tBBB\t20.0\t20.5\t19.9\t20.2\t20.2\t2000\t2000000\t1.0",
    "2024-01-02\t32954\tCCC\tCCC\t30.0\t30.5\t29.9\t30.2\t30.2\t3000\t3000000\t1.0",
]
_MEMBER = "tbltickerhistory3_10y.txt"


def _payload() -> bytes:
    return ("\r\n".join([_HEADER, *_ROWS]) + "\r\n").encode("utf-8")


@pytest.fixture
def synthetic_zip(tmp_path: Path) -> Path:
    zip_path = tmp_path / "tbltickerhistory3_10y.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(_MEMBER, _payload())
    return zip_path


def test_expected_uncompressed_size_is_the_audited_constant():
    assert TICKER_HISTORY_UNCOMPRESSED_BYTES == 11_084_562_320


def test_extraction_writes_the_member_and_records_its_sha256(synthetic_zip, tmp_path):
    tsv_path = tmp_path / "staging" / _MEMBER
    result = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert result.skipped is False
    assert result.member_name == _MEMBER
    assert result.written_bytes == len(_payload())
    assert tsv_path.read_bytes() == _payload()
    assert result.sha256 == hashlib.sha256(_payload()).hexdigest()
    assert sidecar_path(tsv_path).read_text(encoding="ascii").strip() == result.sha256


def test_extraction_is_resumable_and_skips_a_complete_staging_file(synthetic_zip, tmp_path):
    tsv_path = tmp_path / "staging" / _MEMBER
    first = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    mtime = tsv_path.stat().st_mtime_ns
    second = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert second.skipped is True
    assert second.sha256 == first.sha256
    assert tsv_path.stat().st_mtime_ns == mtime


def test_a_truncated_staging_file_is_re_extracted(synthetic_zip, tmp_path):
    tsv_path = tmp_path / "staging" / _MEMBER
    extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    tsv_path.write_bytes(_payload()[:10])
    result = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert result.skipped is False
    assert tsv_path.read_bytes() == _payload()


def test_a_size_mismatch_against_the_expected_constant_fails_fast(synthetic_zip, tmp_path):
    with pytest.raises(ValueError, match="uncompressed bytes"):
        extract_ticker_history_tsv(synthetic_zip, tmp_path / _MEMBER)


def test_progress_callback_reports_bytes_and_total(synthetic_zip, tmp_path):
    seen: list[tuple[int, int]] = []
    extract_ticker_history_tsv(
        synthetic_zip,
        tmp_path / _MEMBER,
        expected_bytes=len(_payload()),
        progress=lambda written, total: seen.append((written, total)),
        progress_every_bytes=1,
    )
    assert seen
    assert seen[-1] == (len(_payload()), len(_payload()))


def test_a_multi_member_archive_is_rejected(tmp_path):
    zip_path = tmp_path / "two.zip"
    with zipfile.ZipFile(zip_path, "w") as archive:
        archive.writestr("a.txt", b"a")
        archive.writestr("b.txt", b"b")
    with pytest.raises(RuntimeError, match="exactly one ticker-history member"):
        extract_ticker_history_tsv(zip_path, tmp_path / "out.txt", expected_bytes=None)


def test_a_missing_archive_raises_file_not_found(tmp_path):
    with pytest.raises(FileNotFoundError):
        extract_ticker_history_tsv(tmp_path / "nope.zip", tmp_path / "out.txt", expected_bytes=None)


def test_no_part_file_survives_a_successful_extraction(synthetic_zip, tmp_path):
    tsv_path = tmp_path / _MEMBER
    extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert not tsv_path.with_suffix(tsv_path.suffix + ".part").exists()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_ticker_history_extract.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.ticker_history_extract'`

- [ ] **Step 3: Write the module**

Create `src/atx_db/ticker_history_extract.py`:

```python
"""Stream the ticker-history ZIP member to a staging TSV.

``ticker_history_bulk.publish_bulk_ticker_history`` scans an *extracted* TSV twice
with DuckDB projection pushdown, so the 3.3 GiB archive has to be materialised as
its 10.32 GiB member first. That extraction is the longest single step of a
from-scratch activation, so it is resumable: a staging file whose size matches the
member's uncompressed size and which has a recorded sha256 sidecar is accepted
as-is. The sidecar keeps the checksum with the artifact, so a resumed run can
report the same digest without re-reading 10 GiB.

Measured archive facts (audit-ticker-zip.md): one DEFLATE member
``tbltickerhistory3_10y.txt``, 11,084,562,320 uncompressed bytes, tab-delimited,
CRLF, 31,464,423 data rows, 2012-03-26 .. 2026-06-15.
"""

from __future__ import annotations

import hashlib
import logging
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

LOGGER = logging.getLogger(__name__)

TICKER_HISTORY_MEMBER = "tbltickerhistory3_10y.txt"
TICKER_HISTORY_UNCOMPRESSED_BYTES = 11_084_562_320
_READ_CHUNK_BYTES = 1 << 24  # 16 MiB


@dataclass(frozen=True)
class TickerHistoryExtractResult:
    tsv_path: Path
    member_name: str
    expected_bytes: int
    written_bytes: int
    sha256: str
    skipped: bool


def sidecar_path(tsv_path: Path) -> Path:
    """Return the sha256 sidecar path for an extracted staging TSV."""
    return tsv_path.with_suffix(tsv_path.suffix + ".sha256")


def _sole_member(archive: zipfile.ZipFile) -> zipfile.ZipInfo:
    members = [info for info in archive.infolist() if not info.is_dir()]
    if len(members) != 1:
        raise RuntimeError(
            "expected exactly one ticker-history member, found "
            f"{len(members)}: {[info.filename for info in members]}"
        )
    return members[0]


def extract_ticker_history_tsv(
    zip_path: Path,
    tsv_path: Path,
    *,
    expected_bytes: int | None = TICKER_HISTORY_UNCOMPRESSED_BYTES,
    progress: Callable[[int, int], None] | None = None,
    progress_every_bytes: int = 1 << 30,
) -> TickerHistoryExtractResult:
    """Extract the sole ZIP member to ``tsv_path``, resuming when already complete.

    ``expected_bytes`` gates against a silently different archive; pass ``None``
    only for synthetic fixtures. Writes through a ``.part`` file and renames on
    success so a killed run never leaves a truncated file that the resume check
    would accept.
    """
    if not zip_path.is_file():
        raise FileNotFoundError(zip_path)
    if progress_every_bytes < 1:
        raise ValueError("progress_every_bytes must be positive")

    sidecar = sidecar_path(tsv_path)
    partial = tsv_path.with_suffix(tsv_path.suffix + ".part")
    digest = hashlib.sha256()
    written = 0

    with zipfile.ZipFile(zip_path) as archive:
        info = _sole_member(archive)
        member_bytes = int(info.file_size)
        if expected_bytes is not None and member_bytes != expected_bytes:
            raise ValueError(
                f"{zip_path} member {info.filename} is {member_bytes:,} uncompressed bytes; "
                f"expected {expected_bytes:,}"
            )
        if tsv_path.is_file() and tsv_path.stat().st_size == member_bytes and sidecar.is_file():
            LOGGER.info("reusing extracted ticker-history TSV at %s", tsv_path)
            return TickerHistoryExtractResult(
                tsv_path=tsv_path,
                member_name=info.filename,
                expected_bytes=member_bytes,
                written_bytes=member_bytes,
                sha256=sidecar.read_text(encoding="ascii").strip(),
                skipped=True,
            )

        tsv_path.parent.mkdir(parents=True, exist_ok=True)
        next_report = progress_every_bytes
        with archive.open(info) as source, open(partial, "wb") as target:
            while True:
                chunk = source.read(_READ_CHUNK_BYTES)
                if not chunk:
                    break
                target.write(chunk)
                digest.update(chunk)
                written += len(chunk)
                if progress is not None and written >= next_report:
                    progress(written, member_bytes)
                    while next_report <= written:
                        next_report += progress_every_bytes

    if written != member_bytes:
        partial.unlink(missing_ok=True)
        raise RuntimeError(f"extracted {written:,} bytes from {zip_path}, expected {member_bytes:,}")

    partial.replace(tsv_path)
    checksum = digest.hexdigest()
    sidecar.write_text(checksum + "\n", encoding="ascii")
    if progress is not None:
        progress(written, member_bytes)
    LOGGER.info("extracted %s (%s bytes, sha256 %s)", tsv_path, f"{written:,}", checksum)
    return TickerHistoryExtractResult(
        tsv_path=tsv_path,
        member_name=info.filename,
        expected_bytes=member_bytes,
        written_bytes=written,
        sha256=checksum,
        skipped=False,
    )
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `.venv\Scripts\python.exe -m pytest tests/test_ticker_history_extract.py -n 0 -q`
Expected: PASS — `9 passed`

- [ ] **Step 5: Lint and typecheck**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/ticker_history_extract.py tests/test_ticker_history_extract.py`
Expected: `All checks passed!`

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/ticker_history_extract.py`
Expected: `Success: no issues found in 1 source file`

- [ ] **Step 6: Commit**

```bash
git add src/atx_db/ticker_history_extract.py tests/test_ticker_history_extract.py
git commit -m "feat(db): resumable streaming extraction of the ticker-history archive

Streams the sole DEFLATE member to a staging TSV through a .part file with a
sha256 sidecar, gates on the audited 11,084,562,320-byte uncompressed size, and
skips re-extraction when the staging file is already complete.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Activation stages A — options, downloader, and the price/identity stages

**Files:**
- Modify: `src/atx_db/activation.py`
- Test: `tests/test_activation_stages_a.py` (new)

**Interfaces:**
- Consumes: `StageResult`, `STAGE_ORDER` (Task 3); `extract_ticker_history_tsv`, `TICKER_HISTORY_UNCOMPRESSED_BYTES` (Task 4); `resolve_as_of_date`, `utc_today` (Task 1); `SecurityMasterOptions.as_of_date`, `NasdaqSymbolDirectoryOptions.as_of_date` (Task 2).
- Produces:
  - `atx_db.activation.Downloader` — `Protocol` with `__call__(url: str, dest: Path, *, user_agent: str) -> int` returning bytes on disk.
  - `atx_db.activation.ActivationOptions` — frozen dataclass, fields listed in the code below.
  - `atx_db.activation.STAGES: dict[str, Callable[[DuckDBStore, ActivationOptions], StageResult]]`
  - `atx_db.activation.stage_migrate / stage_security_master / stage_symbol_directory / stage_ticker_history_extract / stage_ticker_history_publish`

- [ ] **Step 1: Write the failing test**

Create `tests/test_activation_stages_a.py`:

```python
"""Activation stages: migrate, identity load, ticker-history extract and publish."""

from __future__ import annotations

import datetime as dt
import zipfile
from pathlib import Path

import pytest

from atx_db.activation import (
    STAGES,
    ActivationOptions,
    stage_migrate,
    stage_ticker_history_extract,
    stage_ticker_history_publish,
)

_HEADER = "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tclosePr\tvolume\tshares\treturnFactor"
_MEMBER = "tbltickerhistory3_10y.txt"
_SYMBOLS = ("AAA", "BBB", "CCC")
_DATES = ("2024-01-02", "2024-01-03", "2024-01-04")


def _rows() -> list[str]:
    rows: list[str] = []
    for index, symbol in enumerate(_SYMBOLS, start=1):
        for day in _DATES:
            base = 10.0 * index
            rows.append(
                f"{day}\t{32950 + index}\t{symbol}\t{symbol}\t{base}\t{base + 0.5}\t"
                f"{base - 0.1}\t{base + 0.2}\t{base + 0.2}\t{1000 * index}\t{1_000_000 * index}\t1.0"
            )
    return rows


@pytest.fixture
def three_symbol_zip(tmp_path: Path) -> Path:
    zip_path = tmp_path / "tbltickerhistory3_10y.zip"
    payload = ("\r\n".join([_HEADER, *_rows()]) + "\r\n").encode("utf-8")
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(_MEMBER, payload)
    return zip_path


def _options(tmp_path: Path, zip_path: Path) -> ActivationOptions:
    return ActivationOptions(
        db_path=tmp_path / "warehouse.duckdb",
        as_of_date=dt.date(2024, 1, 4),
        ticker_history_zip=zip_path,
        staging_dir=tmp_path / "staging",
        cache_dir=tmp_path / "cache",
        sec_user_agent="atx-db test agent test@example.com",
        ticker_history_expected_bytes=None,
        minimum_rows=1,
        minimum_securities=1,
        minimum_latest_date_securities=1,
        run_id="test-run",
    )


def test_every_stage_name_has_a_registered_function():
    from atx_db.activation import STAGE_ORDER

    assert set(STAGES) >= {
        "migrate",
        "security_master",
        "symbol_directory",
        "ticker_history_extract",
        "ticker_history_publish",
    }
    assert set(STAGES).issubset(set(STAGE_ORDER))


def test_migrate_stage_reports_head_schema_and_applies_nothing_on_a_current_db(tmp_store, tmp_path, three_symbol_zip):
    from atx_db.migrations import MIGRATIONS

    result = stage_migrate(tmp_store, _options(tmp_path, three_symbol_zip))
    assert result.rows == 0
    assert result.detail["schema_version"] == max(m.version for m in MIGRATIONS)
    assert result.detail["applied_versions"] == []


def test_extract_stage_writes_the_staging_tsv(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    result = stage_ticker_history_extract(tmp_store, options)
    tsv_path = Path(str(result.detail["tsv_path"]))
    assert tsv_path.is_file()
    assert result.detail["skipped"] is False
    assert len(str(result.detail["sha256"])) == 64
    assert result.rows == result.detail["written_bytes"]


def test_extract_stage_is_idempotent(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    second = stage_ticker_history_extract(tmp_store, options)
    assert second.detail["skipped"] is True


def test_publish_stage_loads_bars_for_every_symbol(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    result = stage_ticker_history_publish(tmp_store, options)
    assert result.rows == len(_SYMBOLS) * len(_DATES)
    assert result.detail["securities"] == len(_SYMBOLS)
    row = tmp_store.con.execute(
        "SELECT count(*), count(DISTINCT symbol), max(trade_date) FROM equity_daily_bars"
    ).fetchone()
    assert row == (9, 3, dt.date(2024, 1, 4))


def test_publish_stage_is_idempotent_on_row_count(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    stage_ticker_history_publish(tmp_store, options)
    stage_ticker_history_publish(tmp_store, options)
    row = tmp_store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
    assert row is not None and row[0] == 9


def test_publish_stage_breadth_floors_come_from_activation_options(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    strict = ActivationOptions(**{**options.as_dict(), "minimum_securities": 10_000})
    with pytest.raises(RuntimeError, match="publication gate failed"):
        stage_ticker_history_publish(tmp_store, strict)


def test_default_breadth_floors_match_the_bulk_loader_defaults():
    from atx_db.ticker_history_bulk import BulkTickerHistoryOptions

    defaults = BulkTickerHistoryOptions(tsv_path=Path("unused.tsv"))
    options = ActivationOptions()
    assert options.minimum_rows == defaults.minimum_rows
    assert options.minimum_securities == defaults.minimum_securities
    assert options.minimum_latest_date_securities == defaults.minimum_latest_date_securities


def test_security_master_stage_uses_the_injected_downloader(tmp_store, tmp_path, three_symbol_zip, monkeypatch):
    from atx_db.activation import stage_security_master

    calls: list[str] = []

    def fake_downloader(url: str, dest: Path, *, user_agent: str) -> int:
        calls.append(url)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(
            '{"0":{"cik_str":320193,"ticker":"AAPL","title":"Apple Inc."}}', encoding="utf-8"
        )
        return dest.stat().st_size

    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "downloader": fake_downloader}
    )
    result = stage_security_master(tmp_store, options)
    assert calls == ["https://www.sec.gov/files/company_tickers.json"]
    assert result.rows == 1
    row = tmp_store.con.execute("SELECT count(*) FROM sec_company_tickers").fetchone()
    assert row is not None and row[0] == 1


def test_stages_fail_fast_without_a_sec_user_agent(tmp_store, tmp_path, three_symbol_zip):
    from atx_db.activation import stage_security_master

    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "sec_user_agent": None}
    )
    with pytest.raises(ValueError, match="ATX_SEC_USER_AGENT"):
        stage_security_master(tmp_store, options)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_stages_a.py -n 0 -q`
Expected: FAIL — `ImportError: cannot import name 'STAGES' from 'atx_db.activation'`

- [ ] **Step 3: Add options, downloader, and stages A to `activation.py`**

Append to `src/atx_db/activation.py` (keep the existing ledger code above):

```python
import os
from collections.abc import Callable
from dataclasses import asdict, field
from pathlib import Path
from typing import Protocol

import pandas as pd

from .clock import resolve_as_of_date
from .connection import DEFAULT_DB_PATH
from .security_master import SEC_COMPANY_TICKERS_URL, normalize_company_tickers, upsert_security_master_from_frame
from .ticker_history_bulk import BulkTickerHistoryOptions, publish_bulk_ticker_history
from .ticker_history_extract import (
    TICKER_HISTORY_MEMBER,
    TICKER_HISTORY_UNCOMPRESSED_BYTES,
    extract_ticker_history_tsv,
)
from .warehouse import record_source_file

COMPANYFACTS_ZIP_URL = "https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip"
SUBMISSIONS_ZIP_URL = "https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip"
NASDAQ_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
OTHER_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"
DEFAULT_TICKER_HISTORY_ZIP = Path.home() / "Downloads" / "tbltickerhistory3_10y.zip"


class Downloader(Protocol):
    """Fetch ``url`` to ``dest`` and return the number of bytes on disk.

    The only network seam in the activation ladder. Real runs pass
    ``requests_downloader``; tests pass a fake that writes a fixture payload.
    """

    def __call__(self, url: str, dest: Path, *, user_agent: str) -> int: ...


@dataclass(frozen=True)
class ActivationOptions:
    db_path: Path = DEFAULT_DB_PATH
    as_of_date: dt.date | None = None
    ticker_history_zip: Path = DEFAULT_TICKER_HISTORY_ZIP
    ticker_history_expected_bytes: int | None = TICKER_HISTORY_UNCOMPRESSED_BYTES
    staging_dir: Path = Path("data/staging/broad-bars")
    cache_dir: Path = Path("data/cache")
    sec_user_agent: str | None = None
    downloader: Downloader | None = None
    memory_limit: str = "8GB"
    threads: int = 4
    reconciliation_shards: int = 16
    minimum_rows: int = 30_000_000
    minimum_securities: int = 10_000
    minimum_latest_date_securities: int = 5_000
    companyfacts_limit: int | None = None
    companyfacts_progress_every: int = 25
    skip_loaded_companyfacts: bool = True
    dry_run: bool = False
    force: bool = False
    run_id: str = "warehouse-activate"

    def as_dict(self) -> dict[str, object]:
        """Shallow field mapping for ``ActivationOptions(**{**opts.as_dict(), ...})``."""
        return dict(asdict(self)) | {"downloader": self.downloader}

    def ledger_params(self) -> dict[str, object]:
        """JSON-safe option snapshot for ``activation_stage_runs.params_json``."""
        payload = {key: value for key, value in self.as_dict().items() if key != "downloader"}
        return {key: (str(value) if isinstance(value, Path) else value) for key, value in payload.items()}

    @property
    def tsv_path(self) -> Path:
        return Path(self.staging_dir) / TICKER_HISTORY_MEMBER

    @property
    def companyfacts_zip(self) -> Path:
        return Path(self.cache_dir) / "companyfacts.zip"

    @property
    def submissions_zip(self) -> Path:
        return Path(self.cache_dir) / "submissions.zip"


def require_sec_user_agent(options: ActivationOptions) -> str:
    """Return the SEC user agent or fail fast with an operator-actionable message."""
    agent = options.sec_user_agent or os.environ.get("ATX_SEC_USER_AGENT")
    if not agent or not agent.strip():
        raise ValueError(
            "ATX_SEC_USER_AGENT is required before any SEC request. Set it to a product "
            'name and a monitored contact address, e.g. ATX_SEC_USER_AGENT="atx-db/0.2 '
            'ops@example.com", or pass --sec-user-agent.'
        )
    return agent.strip()


def stage_migrate(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Bring the warehouse to head schema and assert it got there."""
    from .migrations import MIGRATIONS, apply_pending_migrations

    applied = apply_pending_migrations(store.con)
    row = store.con.execute(
        "SELECT max(try_cast(version AS INTEGER)) FROM schema_migrations"
    ).fetchone()
    if row is None or row[0] is None:
        raise RuntimeError("warehouse has no schema_migrations rows after migrate")
    version = int(row[0])
    target = max(migration.version for migration in MIGRATIONS)
    if version != target:
        raise RuntimeError(f"warehouse is at schema {version}, expected head {target}")
    _ = options
    return StageResult(
        rows=len(applied),
        detail={"schema_version": version, "applied_versions": list(applied)},
    )


def stage_security_master(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load SEC ``company_tickers.json`` into ``sec_company_tickers`` (network)."""
    user_agent = require_sec_user_agent(options)
    download = _require_downloader(options)
    cache_path = Path(options.cache_dir) / "company_tickers.json"
    byte_count = download(SEC_COMPANY_TICKERS_URL, cache_path, user_agent=user_agent)
    frame = normalize_company_tickers(json.loads(cache_path.read_text(encoding="utf-8")))
    as_of_date = resolve_as_of_date(options.as_of_date, source_max_date=None)
    upsert_security_master_from_frame(
        store,
        frame,
        source="SEC company_tickers",
        as_of_date=as_of_date,
        run_id=f"{options.run_id}-security-master",
    )
    record_source_file(
        store,
        dataset_id="sec_security_master",
        source_url=SEC_COMPANY_TICKERS_URL,
        cache_path=cache_path,
        status="available",
        metadata={"as_of_date": as_of_date.isoformat(), "bytes": byte_count},
    )
    count = store.con.execute("SELECT count(*) FROM sec_company_tickers").fetchone()
    return StageResult(
        rows=0 if count is None else int(count[0]),
        detail={"as_of_date": as_of_date.isoformat(), "bytes": byte_count},
    )


def stage_symbol_directory(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load the Nasdaq Trader symbol directory snapshot (network)."""
    from .symbol_directory import (
        _read_directory_text,
        normalize_nasdaq_listed,
        normalize_other_listed,
        resolve_directory_as_of_date,
        NasdaqSymbolDirectoryOptions,
    )
    from .warehouse import insert_frame

    download = _require_downloader(options)
    user_agent = require_sec_user_agent(options)
    directory_options = NasdaqSymbolDirectoryOptions(as_of_date=options.as_of_date)
    texts: list[tuple[str, str, Callable[..., pd.DataFrame]]] = []
    for url, normalizer, name in (
        (NASDAQ_LISTED_URL, normalize_nasdaq_listed, "nasdaqlisted.txt"),
        (OTHER_LISTED_URL, normalize_other_listed, "otherlisted.txt"),
    ):
        dest = Path(options.cache_dir) / name
        download(url, dest, user_agent=user_agent)
        texts.append((url, dest.read_text(encoding="utf-8"), normalizer))
    as_of_date = resolve_directory_as_of_date(directory_options, texts[0][1])
    frames = [
        normalizer(
            _read_directory_text(text),
            as_of_date=as_of_date,
            source_url=url,
            run_id=f"{options.run_id}-symbol-directory",
        )
        for url, text, normalizer in texts
    ]
    frame = pd.concat([f for f in frames if not f.empty], ignore_index=True)
    with store.transaction():
        store.con.execute("DELETE FROM nasdaq_symbol_directory WHERE as_of_date = ?", [as_of_date])
        insert_frame(store, frame, "nasdaq_symbol_directory", "activation_symbol_directory_insert")
    return StageResult(
        rows=int(len(frame)),
        detail={"as_of_date": as_of_date.isoformat(), "symbols": int(frame["symbol"].nunique())},
    )


def stage_ticker_history_extract(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Stream the ticker-history ZIP member into the staging TSV (offline)."""
    _ = store
    result = extract_ticker_history_tsv(
        Path(options.ticker_history_zip),
        options.tsv_path,
        expected_bytes=options.ticker_history_expected_bytes,
        progress=lambda written, total: LOGGER.info(
            "ticker-history extract %.1f%% (%s / %s bytes)",
            100.0 * written / total,
            f"{written:,}",
            f"{total:,}",
        ),
    )
    return StageResult(
        rows=result.written_bytes,
        detail={
            "tsv_path": str(result.tsv_path),
            "member_name": result.member_name,
            "expected_bytes": result.expected_bytes,
            "written_bytes": result.written_bytes,
            "sha256": result.sha256,
            "skipped": result.skipped,
        },
    )


def stage_ticker_history_publish(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Atomically publish the staging TSV into ``equity_daily_bars`` (offline)."""
    result = publish_bulk_ticker_history(
        store,
        BulkTickerHistoryOptions(
            tsv_path=options.tsv_path.resolve(),
            memory_limit=options.memory_limit,
            threads=options.threads,
            minimum_rows=options.minimum_rows,
            minimum_securities=options.minimum_securities,
            minimum_latest_date_securities=options.minimum_latest_date_securities,
            run_id=f"{options.run_id}-broad-bars",
        ),
    )
    return StageResult(
        rows=result.rows,
        detail={
            "securities": result.securities,
            "latest_date": result.latest_date.isoformat(),
            "latest_date_securities": result.latest_date_securities,
            "invalid_rows": result.invalid_rows,
            "duplicate_keys": result.duplicate_keys,
            "elapsed_seconds": round(result.elapsed_seconds, 3),
        },
    )


def _require_downloader(options: ActivationOptions) -> Downloader:
    if options.downloader is None:
        raise ValueError(
            "a Downloader is required for network stages; the CLI injects "
            "requests_downloader and tests inject a fake"
        )
    return options.downloader


STAGES: dict[str, Callable[[DuckDBStore, ActivationOptions], StageResult]] = {
    "migrate": stage_migrate,
    "security_master": stage_security_master,
    "symbol_directory": stage_symbol_directory,
    "ticker_history_extract": stage_ticker_history_extract,
    "ticker_history_publish": stage_ticker_history_publish,
}
```

Add `import logging` and `LOGGER = logging.getLogger(__name__)` at the top of the module if not already present, and `from dataclasses import asdict, dataclass` to the existing dataclass import.

If `security_master.py` does not already export a `normalize_company_tickers(payload) -> pd.DataFrame` helper, extract the existing payload-to-frame block out of `SecurityMasterDataset.load` into that named function and call it from both places — the stage must not re-fetch.

- [ ] **Step 4: Run the test to verify it passes**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_stages_a.py -n 0 -q`
Expected: PASS — `10 passed`

- [ ] **Step 5: Lint and typecheck**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/activation.py tests/test_activation_stages_a.py`
Expected: `All checks passed!`

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/activation.py`
Expected: `Success: no issues found in 1 source file`

- [ ] **Step 6: Commit**

```bash
git add src/atx_db/activation.py src/atx_db/security_master.py tests/test_activation_stages_a.py
git commit -m "feat(db): activation options, downloader seam, and price/identity stages

Adds ActivationOptions (including the bulk-loader breadth floors), the injected
Downloader protocol, the ATX_SEC_USER_AGENT fail-fast, and the migrate,
security_master, symbol_directory, ticker_history_extract and
ticker_history_publish stages.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Activation stages B — SEC bulk download, submissions, companyfacts

**Files:**
- Modify: `src/atx_db/activation.py`
- Test: `tests/test_activation_stages_b.py` (new)

**Interfaces:**
- Consumes: `ActivationOptions`, `Downloader`, `require_sec_user_agent`, `StageResult`, `STAGES` (Task 5).
- Produces:
  - `atx_db.activation.requests_downloader(url, dest, *, user_agent) -> int` — real resumable HTTP downloader (never called in tests).
  - `atx_db.activation.sha256_file(path: Path) -> str`
  - `atx_db.activation.stage_sec_bulk_download / stage_submissions_load / stage_companyfacts_load`
  - `STAGES` gains keys `sec_bulk_download`, `submissions_load`, `companyfacts_load`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_activation_stages_b.py`:

```python
"""Activation stages: SEC bulk download, submissions load, companyfacts load."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import zipfile
from pathlib import Path

import pytest

from atx_db.activation import (
    COMPANYFACTS_ZIP_URL,
    SUBMISSIONS_ZIP_URL,
    ActivationOptions,
    sha256_file,
    stage_companyfacts_load,
    stage_sec_bulk_download,
    stage_submissions_load,
)

_CIK = "0000320193"


def _companyfacts_payload() -> bytes:
    return json.dumps(
        {
            "cik": 320193,
            "entityName": "Apple Inc.",
            "facts": {
                "us-gaap": {
                    "Assets": {
                        "label": "Assets",
                        "description": "Total assets",
                        "units": {
                            "USD": [
                                {
                                    "end": "2023-09-30",
                                    "val": 352583000000,
                                    "accn": "0000320193-23-000106",
                                    "fy": 2023,
                                    "fp": "FY",
                                    "form": "10-K",
                                    "filed": "2023-11-03",
                                    "frame": "CY2023Q3I",
                                }
                            ]
                        },
                    }
                }
            },
        }
    ).encode("utf-8")


def _submissions_payload() -> bytes:
    return json.dumps(
        {
            "cik": _CIK,
            "name": "Apple Inc.",
            "filings": {
                "recent": {
                    "accessionNumber": ["0000320193-23-000106"],
                    "filingDate": ["2023-11-03"],
                    "reportDate": ["2023-09-30"],
                    "acceptanceDateTime": ["2023-11-02T18:08:27.000Z"],
                    "form": ["10-K"],
                    "primaryDocument": ["aapl-20230930.htm"],
                    "primaryDocDescription": ["10-K"],
                },
                "files": [],
            },
        }
    ).encode("utf-8")


@pytest.fixture
def companyfacts_zip(tmp_path: Path) -> Path:
    path = tmp_path / "fixtures" / "companyfacts.zip"
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"CIK{_CIK}.json", _companyfacts_payload())
    return path


@pytest.fixture
def submissions_zip(tmp_path: Path) -> Path:
    path = tmp_path / "fixtures" / "submissions.zip"
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"CIK{_CIK}.json", _submissions_payload())
    return path


def _seed_company_ticker(store) -> None:
    store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik, ticker, title, security_id, source, source_loaded_at)
        VALUES (?, 'AAPL', 'Apple Inc.', 'SEC-CIK-0000320193', 'unit-test', TIMESTAMP '2024-01-01 00:00:00')
        """,
        [_CIK],
    )


def _options(tmp_path: Path, downloader=None) -> ActivationOptions:
    return ActivationOptions(
        db_path=tmp_path / "warehouse.duckdb",
        as_of_date=dt.date(2024, 1, 4),
        staging_dir=tmp_path / "staging",
        cache_dir=tmp_path / "cache",
        sec_user_agent="atx-db test agent test@example.com",
        downloader=downloader,
        ticker_history_expected_bytes=None,
        run_id="test-run",
    )


def test_sha256_file_matches_hashlib(tmp_path):
    path = tmp_path / "x.bin"
    path.write_bytes(b"hello world")
    assert sha256_file(path) == hashlib.sha256(b"hello world").hexdigest()


def test_download_stage_fetches_both_archives_and_records_hashes(
    tmp_store, tmp_path, companyfacts_zip, submissions_zip
):
    requested: list[str] = []

    def fake_downloader(url: str, dest: Path, *, user_agent: str) -> int:
        requested.append(url)
        source = companyfacts_zip if url == COMPANYFACTS_ZIP_URL else submissions_zip
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(source.read_bytes())
        return dest.stat().st_size

    options = _options(tmp_path, fake_downloader)
    result = stage_sec_bulk_download(tmp_store, options)
    assert requested == [COMPANYFACTS_ZIP_URL, SUBMISSIONS_ZIP_URL]
    assert options.companyfacts_zip.is_file()
    assert options.submissions_zip.is_file()
    assert result.detail["companyfacts_sha256"] == sha256_file(companyfacts_zip)
    assert result.detail["submissions_sha256"] == sha256_file(submissions_zip)
    assert result.rows == 2


def test_download_stage_resumes_by_skipping_present_archives(
    tmp_store, tmp_path, companyfacts_zip, submissions_zip
):
    calls: list[str] = []

    def fake_downloader(url: str, dest: Path, *, user_agent: str) -> int:
        calls.append(url)
        source = companyfacts_zip if url == COMPANYFACTS_ZIP_URL else submissions_zip
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(source.read_bytes())
        return dest.stat().st_size

    options = _options(tmp_path, fake_downloader)
    stage_sec_bulk_download(tmp_store, options)
    result = stage_sec_bulk_download(tmp_store, options)
    assert calls == [COMPANYFACTS_ZIP_URL, SUBMISSIONS_ZIP_URL]
    assert result.detail["companyfacts_skipped"] is True
    assert result.detail["submissions_skipped"] is True


def test_download_stage_requires_a_sec_user_agent(tmp_store, tmp_path):
    options = ActivationOptions(**{**_options(tmp_path).as_dict(), "sec_user_agent": None})
    with pytest.raises(ValueError, match="ATX_SEC_USER_AGENT"):
        stage_sec_bulk_download(tmp_store, options)


def test_submissions_stage_loads_the_bulk_archive(tmp_store, tmp_path, submissions_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.submissions_zip.parent.mkdir(parents=True, exist_ok=True)
    options.submissions_zip.write_bytes(submissions_zip.read_bytes())
    result = stage_submissions_load(tmp_store, options)
    assert result.rows == 1
    row = tmp_store.con.execute(
        "SELECT count(*) FROM sec_submissions WHERE accession_number = '0000320193-23-000106'"
    ).fetchone()
    assert row is not None and row[0] == 1


def test_submissions_stage_is_idempotent(tmp_store, tmp_path, submissions_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.submissions_zip.parent.mkdir(parents=True, exist_ok=True)
    options.submissions_zip.write_bytes(submissions_zip.read_bytes())
    stage_submissions_load(tmp_store, options)
    stage_submissions_load(tmp_store, options)
    row = tmp_store.con.execute("SELECT count(*) FROM sec_submissions").fetchone()
    assert row is not None and row[0] == 1


def test_companyfacts_stage_targets_all_sec_company_tickers_ciks(tmp_store, tmp_path, companyfacts_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.companyfacts_zip.parent.mkdir(parents=True, exist_ok=True)
    options.companyfacts_zip.write_bytes(companyfacts_zip.read_bytes())
    result = stage_companyfacts_load(tmp_store, options)
    assert result.rows >= 1
    assert result.detail["symbol_source"] == "sec_company_tickers"
    assert result.detail["skip_loaded"] is True
    row = tmp_store.con.execute(
        "SELECT count(*) FROM sec_company_facts WHERE cik = ?", [_CIK]
    ).fetchone()
    assert row is not None and row[0] >= 1


def test_companyfacts_stage_is_idempotent_with_skip_loaded(tmp_store, tmp_path, companyfacts_zip):
    _seed_company_ticker(tmp_store)
    options = _options(tmp_path)
    options.companyfacts_zip.parent.mkdir(parents=True, exist_ok=True)
    options.companyfacts_zip.write_bytes(companyfacts_zip.read_bytes())
    stage_companyfacts_load(tmp_store, options)
    before = tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()
    stage_companyfacts_load(tmp_store, options)
    after = tmp_store.con.execute("SELECT count(*) FROM sec_company_facts").fetchone()
    assert before == after


def test_companyfacts_stage_fails_without_the_archive(tmp_store, tmp_path):
    with pytest.raises(FileNotFoundError):
        stage_companyfacts_load(tmp_store, _options(tmp_path))
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_stages_b.py -n 0 -q`
Expected: FAIL — `ImportError: cannot import name 'sha256_file' from 'atx_db.activation'`

- [ ] **Step 3: Add stages B to `activation.py`**

Append to `src/atx_db/activation.py`:

```python
def sha256_file(path: Path, *, chunk_bytes: int = 1 << 22) -> str:
    """Stream a file through sha256 without loading it into memory."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(chunk_bytes)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def requests_downloader(url: str, dest: Path, *, user_agent: str) -> int:
    """Resumable streaming HTTP download. Network — never called from tests."""
    import requests

    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_suffix(dest.suffix + ".part")
    existing = partial.stat().st_size if partial.is_file() else 0
    headers = {"User-Agent": user_agent, "Accept-Encoding": "identity"}
    if existing:
        headers["Range"] = f"bytes={existing}-"
    with requests.get(url, headers=headers, stream=True, timeout=600) as response:
        if existing and response.status_code == 200:
            existing = 0  # server ignored the range request; restart
        response.raise_for_status()
        mode = "ab" if existing else "wb"
        with open(partial, mode) as handle:
            for chunk in response.iter_content(chunk_size=1 << 20):
                if chunk:
                    handle.write(chunk)
    partial.replace(dest)
    return dest.stat().st_size


def _download_archive(
    store: DuckDBStore,
    options: ActivationOptions,
    *,
    url: str,
    dest: Path,
    dataset_id: str,
    user_agent: str,
) -> tuple[str, int, bool]:
    """Fetch ``url`` to ``dest`` unless it is already present; return (sha256, bytes, skipped)."""
    skipped = dest.is_file() and dest.stat().st_size > 0
    if not skipped:
        _require_downloader(options)(url, dest, user_agent=user_agent)
    checksum = sha256_file(dest)
    record_source_file(
        store,
        dataset_id=dataset_id,
        source_url=url,
        cache_path=dest,
        status="available",
        metadata={"sha256": checksum, "bytes": dest.stat().st_size, "resumed": skipped},
        compute_hash=False,
    )
    return checksum, int(dest.stat().st_size), skipped


def stage_sec_bulk_download(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Fetch companyfacts.zip and submissions.zip with resume + recorded sha256 (network)."""
    user_agent = require_sec_user_agent(options)
    facts_sha, facts_bytes, facts_skipped = _download_archive(
        store,
        options,
        url=COMPANYFACTS_ZIP_URL,
        dest=options.companyfacts_zip,
        dataset_id="sec_company_facts",
        user_agent=user_agent,
    )
    subs_sha, subs_bytes, subs_skipped = _download_archive(
        store,
        options,
        url=SUBMISSIONS_ZIP_URL,
        dest=options.submissions_zip,
        dataset_id="sec_submissions",
        user_agent=user_agent,
    )
    return StageResult(
        rows=2,
        detail={
            "companyfacts_path": str(options.companyfacts_zip),
            "companyfacts_sha256": facts_sha,
            "companyfacts_bytes": facts_bytes,
            "companyfacts_skipped": facts_skipped,
            "submissions_path": str(options.submissions_zip),
            "submissions_sha256": subs_sha,
            "submissions_bytes": subs_bytes,
            "submissions_skipped": subs_skipped,
        },
    )


def stage_submissions_load(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load SEC filing histories from the bulk submissions archive (offline)."""
    from .sec_submissions import SecSubmissionsBulkDataset, SecSubmissionsBulkOptions

    if not options.submissions_zip.is_file():
        raise FileNotFoundError(options.submissions_zip)
    result = SecSubmissionsBulkDataset().load(
        store,
        SecSubmissionsBulkOptions(
            zip_path=options.submissions_zip,
            run_id=f"{options.run_id}-submissions",
        ),
    )
    return StageResult(rows=int(result.rows_loaded), detail=dict(result.details))


def stage_companyfacts_load(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load XBRL company facts for every CIK in sec_company_tickers (offline)."""
    from .fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions

    if not options.companyfacts_zip.is_file():
        raise FileNotFoundError(options.companyfacts_zip)
    result = SecCompanyFactsDataset().run(
        store,
        SecCompanyFactsOptions(
            symbols=(),
            symbol_source="sec_company_tickers",
            symbol_limit=options.companyfacts_limit,
            skip_loaded_targets=options.skip_loaded_companyfacts,
            skip_failed_targets=True,
            companyfacts_zip=options.companyfacts_zip,
            as_of_date=options.as_of_date,
            refresh_derived_surfaces=False,
            progress_every_targets=options.companyfacts_progress_every,
            run_id=f"{options.run_id}-companyfacts",
        ),
    )
    detail = {key: value for key, value in result.details.items() if key != "failed_targets"}
    detail["symbol_source"] = "sec_company_tickers"
    detail["skip_loaded"] = options.skip_loaded_companyfacts
    return StageResult(rows=int(result.rows_loaded), detail=detail)


STAGES.update(
    {
        "sec_bulk_download": stage_sec_bulk_download,
        "submissions_load": stage_submissions_load,
        "companyfacts_load": stage_companyfacts_load,
    }
)
```

Add `import hashlib` to the module imports.

Note `refresh_derived_surfaces=False`: the derived surfaces are rebuilt once by the `statement_points` / `periods` / `ttm` stages in Task 7, exactly as `scripts/finalize_companyfacts_bulk.py` does after a batch load.

- [ ] **Step 4: Run the test to verify it passes**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_stages_b.py -n 0 -q`
Expected: PASS — `9 passed`

- [ ] **Step 5: Lint and typecheck**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/activation.py tests/test_activation_stages_b.py`
Expected: `All checks passed!`

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/activation.py`
Expected: `Success: no issues found in 1 source file`

- [ ] **Step 6: Commit**

```bash
git add src/atx_db/activation.py tests/test_activation_stages_b.py
git commit -m "feat(db): SEC bulk download, submissions, and companyfacts activation stages

Adds a resumable sha256-recording download stage for companyfacts.zip and
submissions.zip behind the injected Downloader, plus the offline bulk load
stages that target every CIK in sec_company_tickers with --skip-loaded.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Activation stages C, the ladder runner, and `scripts/warehouse_activate.py`

**Files:**
- Modify: `src/atx_db/activation.py`
- Create: `scripts/warehouse_activate.py`
- Test: `tests/test_activation_ladder.py` (new)

**Interfaces:**
- Consumes: everything from Tasks 3, 5, 6.
- Produces:
  - `atx_db.activation.stage_statement_points / stage_periods / stage_ttm / stage_calendarization / stage_standardized / stage_industry_templates / stage_reconciliation / stage_provider_coverage`
  - `atx_db.activation.run_activation(options, *, stages=STAGE_ORDER, emit=print_json) -> list[dict[str, object]]`
  - `atx_db.activation.print_json(payload: dict[str, object]) -> None`
  - `scripts/warehouse_activate.py` with `--db-path --start-stage --stop-stage --only --dry-run --force --sec-user-agent --as-of-date --ticker-history-zip --staging-dir --cache-dir --memory-limit --threads --shards --run-id`

- [ ] **Step 1: Write the failing test**

Create `tests/test_activation_ladder.py`:

```python
"""The activation ladder: ordering, ledgering, idempotence, dry-run, slicing."""

from __future__ import annotations

import datetime as dt
import json
import zipfile
from pathlib import Path

import pytest

from atx_db.activation import (
    STAGE_ORDER,
    STAGES,
    ActivationOptions,
    StageResult,
    completed_stages,
    run_activation,
)
from atx_db.connection import DuckDBStore

_HEADER = "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tclosePr\tvolume\tshares\treturnFactor"
_MEMBER = "tbltickerhistory3_10y.txt"
_SYMBOLS = ("AAA", "BBB", "CCC")
_DATES = ("2024-01-02", "2024-01-03", "2024-01-04")


@pytest.fixture
def three_symbol_zip(tmp_path: Path) -> Path:
    rows = [
        f"{day}\t{32950 + i}\t{sym}\t{sym}\t{10.0 * i}\t{10.0 * i + 0.5}\t{10.0 * i - 0.1}\t"
        f"{10.0 * i + 0.2}\t{10.0 * i + 0.2}\t{1000 * i}\t{1_000_000 * i}\t1.0"
        for i, sym in enumerate(_SYMBOLS, start=1)
        for day in _DATES
    ]
    payload = ("\r\n".join([_HEADER, *rows]) + "\r\n").encode("utf-8")
    zip_path = tmp_path / "tbltickerhistory3_10y.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(_MEMBER, payload)
    return zip_path


@pytest.fixture
def offline_options(built_warehouse, tmp_path, three_symbol_zip) -> ActivationOptions:
    return ActivationOptions(
        db_path=built_warehouse("activation.duckdb"),
        as_of_date=dt.date(2024, 1, 4),
        ticker_history_zip=three_symbol_zip,
        ticker_history_expected_bytes=None,
        staging_dir=tmp_path / "staging",
        cache_dir=tmp_path / "cache",
        sec_user_agent="atx-db test agent test@example.com",
        minimum_rows=1,
        minimum_securities=1,
        minimum_latest_date_securities=1,
        reconciliation_shards=1,
        run_id="ladder-test",
    )


_OFFLINE_SLICE = (
    "migrate",
    "ticker_history_extract",
    "ticker_history_publish",
    "statement_points",
    "periods",
    "ttm",
    "calendarization",
    "standardized",
    "industry_templates",
    "provider_coverage",
)


def test_every_stage_in_stage_order_is_registered():
    assert tuple(sorted(STAGES)) == tuple(sorted(STAGE_ORDER))


def test_every_stage_function_has_the_uniform_signature():
    import inspect

    for name, func in STAGES.items():
        params = list(inspect.signature(func).parameters)
        assert params == ["store", "options"], f"{name} has {params}"
        assert inspect.signature(func).return_annotation in (StageResult, "StageResult")


def test_ladder_emits_one_json_line_per_stage(offline_options):
    lines: list[dict[str, object]] = []
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["stage"] for line in lines] == list(_OFFLINE_SLICE)
    for line in lines:
        assert line["status"] == "completed"
        assert isinstance(line["rows"], int)
        assert isinstance(line["seconds"], float)
        json.dumps(line)  # every emitted payload must be JSON-serialisable


def test_ladder_records_every_stage_in_the_ledger(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    with DuckDBStore(offline_options.db_path) as store:
        assert completed_stages(store) == set(_OFFLINE_SLICE)


def test_rerunning_the_ladder_skips_completed_stages(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    second: list[dict[str, object]] = []
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=second.append)
    assert [line["status"] for line in second] == ["skipped"] * len(_OFFLINE_SLICE)
    with DuckDBStore(offline_options.db_path) as store:
        attempts = store.con.execute(
            "SELECT stage, count(*) FROM activation_stage_runs GROUP BY stage ORDER BY stage"
        ).fetchall()
    assert {stage for stage, _ in attempts} == set(_OFFLINE_SLICE)


def test_rerunning_the_ladder_leaves_published_row_counts_unchanged(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    with DuckDBStore(offline_options.db_path) as store:
        before = store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    with DuckDBStore(offline_options.db_path) as store:
        after = store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
    assert before == after == (9,)


def test_force_reruns_completed_stages(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    forced = ActivationOptions(**{**offline_options.as_dict(), "force": True})
    lines: list[dict[str, object]] = []
    run_activation(forced, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["status"] for line in lines] == ["completed"] * len(_OFFLINE_SLICE)


def test_dry_run_reports_the_plan_and_writes_nothing(offline_options):
    planned = ActivationOptions(**{**offline_options.as_dict(), "dry_run": True})
    lines: list[dict[str, object]] = []
    run_activation(planned, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["status"] for line in lines] == ["dry_run"] * len(_OFFLINE_SLICE)
    with DuckDBStore(planned.db_path) as store:
        rows = store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
        assert rows is not None and rows[0] == 0
        assert completed_stages(store) == set()


def test_a_failing_stage_stops_the_ladder_and_is_recorded(offline_options, monkeypatch):
    def boom(store, options):
        raise RuntimeError("synthetic periods failure")

    monkeypatch.setitem(STAGES, "periods", boom)
    lines: list[dict[str, object]] = []
    with pytest.raises(RuntimeError, match="synthetic periods failure"):
        run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["stage"] for line in lines][-1] == "periods"
    assert lines[-1]["status"] == "failed"
    with DuckDBStore(offline_options.db_path) as store:
        row = store.con.execute(
            "SELECT status, error FROM activation_stage_runs WHERE stage = 'periods'"
        ).fetchone()
    assert row is not None and row[0] == "failed" and "synthetic" in row[1]
    assert "ttm" not in [line["stage"] for line in lines]
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_ladder.py -n 0 -q`
Expected: FAIL — `ImportError: cannot import name 'run_activation' from 'atx_db.activation'`

- [ ] **Step 3: Add stages C and the runner to `activation.py`**

Append to `src/atx_db/activation.py`:

```python
def stage_statement_points(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Rebuild the concept catalog, fact revisions, and normalized statement points."""
    from .fundamental_statements import refresh_fundamental_statement_points
    from .fundamentals import refresh_fundamental_fact_revisions, refresh_xbrl_concept_catalog

    _ = options
    catalog_rows = refresh_xbrl_concept_catalog(store)
    revision_rows = refresh_fundamental_fact_revisions(store)
    point_rows = refresh_fundamental_statement_points(store)
    return StageResult(
        rows=int(point_rows),
        detail={
            "catalog_rows": int(catalog_rows),
            "revision_rows": int(revision_rows),
            "statement_point_rows": int(point_rows),
        },
    )


def stage_periods(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .fundamental_statements import refresh_fundamental_periods

    _ = options
    rows = refresh_fundamental_periods(store)
    return StageResult(rows=int(rows), detail={"period_rows": int(rows)})


def stage_ttm(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .fundamental_statements import refresh_fundamental_ttm_points

    _ = options
    rows = refresh_fundamental_ttm_points(store)
    return StageResult(rows=int(rows), detail={"ttm_rows": int(rows)})


def stage_calendarization(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .calendarization import CalendarizationOptions, run_calendarization_refresh

    summary = run_calendarization_refresh(
        store, CalendarizationOptions(run_id=f"{options.run_id}-calendarization")
    )
    rows = int(summary.get("calendar_ttm_rows", summary.get("map_rows", 0)) or 0)
    return StageResult(rows=rows, detail=dict(summary))


def stage_standardized(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .standardization import FundamentalStandardizationOptions, refresh_fundamental_standardized

    _configure_analytical_session(store, options)
    result = refresh_fundamental_standardized(
        store,
        FundamentalStandardizationOptions(
            materialize_result_limit=0,
            run_id=f"{options.run_id}-standardized",
        ),
    )
    return StageResult(
        rows=int(result.standardized_row_count),
        detail={
            "build_id": result.build_id,
            "rule_set_sha256": result.rule_set_sha256,
            "input_rows": int(result.input_row_count),
            "exception_rows": int(result.exception_row_count),
            "basis_counts": dict(result.basis_counts),
        },
    )


def stage_industry_templates(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .industry_templates import IndustryTemplateOptions, run_industry_template_refresh

    summary = run_industry_template_refresh(
        store, IndustryTemplateOptions(run_id=f"{options.run_id}-industry")
    )
    return StageResult(rows=int(summary.get("coverage_rows", 0) or 0), detail=dict(summary))


def stage_reconciliation(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Publish the reconciliation serving table in bounded symbol shards.

    Side effect: this stage CLOSES the ladder's connection and shells out to
    ``scripts/refresh_reconciliation_sharded.py``, which runs each shard in a fresh
    interpreter. A long-lived process was measured to degrade a shard from ~110s to
    ~840s at identical warehouse size, and DuckDB allows exactly one writer, so the
    ladder must yield the file for the duration. The connection is reopened before
    returning, so the stage's contract to its caller is unchanged.
    """
    script = Path(__file__).resolve().parents[2] / "scripts" / "refresh_reconciliation_sharded.py"
    db_path = str(options.db_path)
    store.close()
    try:
        completed = subprocess.run(
            [
                sys.executable,
                str(script),
                "--db-path",
                db_path,
                "--shards",
                str(options.reconciliation_shards),
                "--memory-limit",
                options.memory_limit,
                "--threads",
                str(options.threads),
                "--run-id-prefix",
                f"{options.run_id}-recon",
            ],
            check=False,
        )
    finally:
        store.reopen()
    if completed.returncode != 0:
        raise RuntimeError(
            f"sharded reconciliation publish failed with exit code {completed.returncode}"
        )
    row = store.con.execute("SELECT count(*) FROM fundamental_reconciliation_serving").fetchone()
    rows = 0 if row is None else int(row[0])
    return StageResult(rows=rows, detail={"shards": options.reconciliation_shards, "serving_rows": rows})


def stage_provider_coverage(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    from .provider_coverage import ProviderCoverageOptions, refresh_provider_coverage

    rows = refresh_provider_coverage(
        store,
        ProviderCoverageOptions(run_id=f"{options.run_id}-coverage"),
    )
    conditions = {
        condition: sum(row.condition == condition for row in rows)
        for condition in ("available", "degraded", "pending", "missing")
    }
    return StageResult(rows=len(rows), detail={"schema_count": len(rows), "conditions": conditions})


def _configure_analytical_session(store: DuckDBStore, options: ActivationOptions) -> None:
    store.con.execute("PRAGMA disable_progress_bar")
    store.con.execute("SET memory_limit = ?", [options.memory_limit])
    store.con.execute("SET threads = ?", [options.threads])
    store.con.execute("SET preserve_insertion_order = false")


STAGES.update(
    {
        "statement_points": stage_statement_points,
        "periods": stage_periods,
        "ttm": stage_ttm,
        "calendarization": stage_calendarization,
        "standardized": stage_standardized,
        "industry_templates": stage_industry_templates,
        "reconciliation": stage_reconciliation,
        "provider_coverage": stage_provider_coverage,
    }
)


def print_json(payload: dict[str, object]) -> None:
    """Emit one JSON line to stdout, flushed, so an operator can tail the ladder."""
    print(json.dumps(payload, default=str, sort_keys=True), flush=True)


def run_activation(
    options: ActivationOptions,
    *,
    stages: tuple[str, ...] = STAGE_ORDER,
    emit: Callable[[dict[str, object]], None] = print_json,
) -> list[dict[str, object]]:
    """Run the activation ladder, ledgering every stage and stopping on the first failure."""
    for stage in stages:
        if stage not in STAGES:
            raise ValueError(f"unknown activation stage {stage!r}; expected one of {STAGE_ORDER}")
    emitted: list[dict[str, object]] = []
    params = options.ledger_params()
    with DuckDBStore(options.db_path) as store:
        done = set() if options.force else completed_stages(store)
        for stage in stages:
            if options.dry_run:
                payload = {
                    "stage": stage,
                    "status": "dry_run",
                    "rows": 0,
                    "seconds": 0.0,
                    "detail": {"already_completed": stage in done},
                }
                emit(payload)
                emitted.append(payload)
                continue
            if stage in done:
                payload = {
                    "stage": stage,
                    "status": "skipped",
                    "rows": 0,
                    "seconds": 0.0,
                    "detail": {"reason": "already completed; pass --force to rerun"},
                }
                emit(payload)
                emitted.append(payload)
                continue
            begun = time.monotonic()
            started_at = begin_stage(store, stage=stage, run_id=options.run_id, params=params)
            try:
                result = STAGES[stage](store, options)
            except Exception as exc:  # noqa: BLE001 - recorded then re-raised
                finish_stage(
                    store,
                    stage=stage,
                    run_id=options.run_id,
                    started_at=started_at,
                    status="failed",
                    rows=0,
                    error=str(exc),
                )
                payload = {
                    "stage": stage,
                    "status": "failed",
                    "rows": 0,
                    "seconds": round(time.monotonic() - begun, 3),
                    "detail": {"error": str(exc)},
                }
                emit(payload)
                emitted.append(payload)
                raise
            finish_stage(
                store,
                stage=stage,
                run_id=options.run_id,
                started_at=started_at,
                status="completed",
                rows=result.rows,
            )
            payload = {
                "stage": stage,
                "status": "completed",
                "rows": result.rows,
                "seconds": round(time.monotonic() - begun, 3),
                "detail": result.detail,
            }
            emit(payload)
            emitted.append(payload)
    return emitted
```

Add `import subprocess`, `import sys`, and `import time` to the module imports.

- [ ] **Step 4: Add `close()` / `reopen()` to `DuckDBStore` if absent**

The reconciliation stage needs to yield the single-writer file. Add to `src/atx_db/connection.py` inside `class DuckDBStore` (skip if equivalent methods already exist):

```python
    def close(self) -> None:
        """Release the connection without discarding the store's configuration."""
        if self.connection is not None:
            self.connection.execute("CHECKPOINT")
            self.connection.close()
            self.connection = None

    def reopen(self) -> None:
        """Reacquire a configured connection after ``close()``."""
        if self.connection is None:
            self.connection = open_duckdb_connection(self.path, read_only=self.read_only)
            self._configure_session(self.connection)
```

- [ ] **Step 5: Run the ladder test to verify it passes**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_ladder.py -n 0 -q`
Expected: PASS — `9 passed`

- [ ] **Step 6: Write `scripts/warehouse_activate.py`**

```python
#!/usr/bin/env python
"""Activate a complete atx-db warehouse from zero, resumably.

Runs the governed activation ladder in dependency order, one JSON line per stage,
recording every attempt in ``activation_stage_runs``. A rerun skips stages whose
newest attempt completed, so an interrupted multi-hour build resumes exactly where
it stopped.

Stages (in order)
-----------------
  migrate, security_master, symbol_directory, ticker_history_extract,
  ticker_history_publish, sec_bulk_download, submissions_load,
  companyfacts_load, statement_points, periods, ttm, calendarization,
  standardized, industry_templates, reconciliation, provider_coverage

Network is limited to security_master, symbol_directory, and sec_bulk_download.
``ATX_SEC_USER_AGENT`` (or ``--sec-user-agent``) is required before any SEC
request; the ladder fails fast without it.

Usage
-----
  python scripts/warehouse_activate.py --dry-run
  python scripts/warehouse_activate.py
  python scripts/warehouse_activate.py --start-stage companyfacts_load
  python scripts/warehouse_activate.py --only standardized --only reconciliation --force
"""
from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.activation import (
    STAGE_ORDER,
    ActivationOptions,
    DEFAULT_TICKER_HISTORY_ZIP,
    requests_downloader,
    run_activation,
    select_stages,
)
from atx_db.clock import utc_today
from atx_db.connection import DEFAULT_DB_PATH


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--as-of-date", type=dt.date.fromisoformat, default=utc_today())
    parser.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--staging-dir", type=Path, default=Path("data/staging/broad-bars"))
    parser.add_argument("--cache-dir", type=Path, default=Path("data/cache"))
    parser.add_argument("--sec-user-agent", default=None)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--shards", type=int, default=16)
    parser.add_argument("--companyfacts-limit", type=int, default=None)
    parser.add_argument("--start-stage", choices=STAGE_ORDER, default=None)
    parser.add_argument("--stop-stage", choices=STAGE_ORDER, default=None)
    parser.add_argument("--only", action="append", choices=STAGE_ORDER, default=None)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--run-id", default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    run_id = args.run_id or f"warehouse-activate-{args.as_of_date.isoformat()}"
    options = ActivationOptions(
        db_path=args.db_path,
        as_of_date=args.as_of_date,
        ticker_history_zip=args.ticker_history_zip,
        staging_dir=args.staging_dir,
        cache_dir=args.cache_dir,
        sec_user_agent=args.sec_user_agent,
        downloader=requests_downloader,
        memory_limit=args.memory_limit,
        threads=args.threads,
        reconciliation_shards=args.shards,
        companyfacts_limit=args.companyfacts_limit,
        dry_run=args.dry_run,
        force=args.force,
        run_id=run_id,
    )
    stages = select_stages(
        start=args.start_stage,
        stop=args.stop_stage,
        only=tuple(args.only or ()),
    )
    run_activation(options, stages=stages)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 7: Verify the script's dry-run plan**

Run: `.venv\Scripts\python.exe scripts/warehouse_activate.py --db-path .pytest_cache/activate-smoke.duckdb --dry-run --only migrate --only provider_coverage`
Expected: exactly two JSON lines on stdout, e.g.
```
{"detail": {"already_completed": false}, "rows": 0, "seconds": 0.0, "stage": "migrate", "status": "dry_run"}
{"detail": {"already_completed": false}, "rows": 0, "seconds": 0.0, "stage": "provider_coverage", "status": "dry_run"}
```

- [ ] **Step 8: Lint and typecheck**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/activation.py src/atx_db/connection.py scripts/warehouse_activate.py tests/test_activation_ladder.py`
Expected: `All checks passed!`

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/activation.py src/atx_db/connection.py`
Expected: `Success: no issues found in 2 source files`

- [ ] **Step 9: Run the full activation test set**

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_ledger.py tests/test_activation_stages_a.py tests/test_activation_stages_b.py tests/test_activation_ladder.py tests/test_ticker_history_extract.py -n 0 -q`
Expected: PASS — `39 passed`

- [ ] **Step 10: Commit**

```bash
git add src/atx_db/activation.py src/atx_db/connection.py scripts/warehouse_activate.py tests/test_activation_ladder.py
git commit -m "feat(db): full-warehouse activation ladder and warehouse_activate script

Adds the derived-chain stages (statement points, periods, TTM, calendarization,
standardized, industry templates, sharded reconciliation, provider coverage) and
run_activation, which ledgers every stage, emits one JSON line per stage, and
supports --start-stage/--stop-stage/--only/--dry-run/--force.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: `atx-db activate` subcommand, CI, and the runbook

**Files:**
- Modify: `src/atx_db/cli.py` (imports, `_build_parser`, `main`)
- Modify: `.github/workflows/atx-db.yml`
- Modify: `docs/PRODUCTION_RUNBOOK.md`
- Test: `tests/test_cli_activate.py` (new)

**Interfaces:**
- Consumes: `run_activation`, `select_stages`, `ActivationOptions`, `requests_downloader`, `STAGE_ORDER`, `DEFAULT_TICKER_HISTORY_ZIP` (Task 7); `utc_today` (Task 1).
- Produces: `atx-db activate` subcommand with the same flags as `scripts/warehouse_activate.py`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cli_activate.py`:

```python
"""The `atx-db activate` subcommand."""

from __future__ import annotations

import json

import pytest

from atx_db import cli
from atx_db.activation import STAGE_ORDER


def test_activate_is_a_registered_subcommand():
    parser = cli._build_parser()
    action = next(a for a in parser._actions if a.dest == "command")
    assert "activate" in action.choices


def test_activate_dry_run_emits_one_json_line_per_selected_stage(built_warehouse, capsys):
    db_path = built_warehouse("cli_activate.duckdb")
    code = cli.main(
        [
            "activate",
            "--db-path",
            str(db_path),
            "--dry-run",
            "--only",
            "migrate",
            "--only",
            "provider_coverage",
        ]
    )
    assert code == 0
    lines = [json.loads(line) for line in capsys.readouterr().out.splitlines() if line.strip()]
    assert [line["stage"] for line in lines] == ["migrate", "provider_coverage"]
    assert {line["status"] for line in lines} == {"dry_run"}


def test_activate_dry_run_respects_start_and_stop(built_warehouse, capsys):
    db_path = built_warehouse("cli_activate_slice.duckdb")
    cli.main(
        ["activate", "--db-path", str(db_path), "--dry-run", "--start-stage", "periods", "--stop-stage", "standardized"]
    )
    lines = [json.loads(line) for line in capsys.readouterr().out.splitlines() if line.strip()]
    assert [line["stage"] for line in lines] == ["periods", "ttm", "calendarization", "standardized"]


def test_activate_rejects_an_unknown_stage(built_warehouse):
    db_path = built_warehouse("cli_activate_bad.duckdb")
    with pytest.raises(SystemExit):
        cli.main(["activate", "--db-path", str(db_path), "--only", "not-a-stage"])


def test_activate_exposes_every_ladder_stage_as_a_choice():
    parser = cli._build_parser()
    action = next(a for a in parser._actions if a.dest == "command")
    activate = action.choices["activate"]
    only = next(a for a in activate._actions if a.dest == "only")
    assert tuple(only.choices) == STAGE_ORDER
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_cli_activate.py -n 0 -q`
Expected: FAIL — `AssertionError: assert 'activate' in {...}`

- [ ] **Step 3: Wire the subcommand into `cli.py`**

Add to the import block at the top of `src/atx_db/cli.py`:

```python
from .activation import (
    DEFAULT_TICKER_HISTORY_ZIP,
    STAGE_ORDER,
    ActivationOptions,
    requests_downloader,
    run_activation,
    select_stages,
)
from .clock import utc_today
```

Add to `_build_parser()` immediately before `return parser`:

```python
    activate = commands.add_parser(
        "activate",
        help="Build a complete warehouse from zero, resumably, one JSON line per stage",
    )
    activate.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    activate.add_argument("--as-of-date", type=dt.date.fromisoformat, default=utc_today())
    activate.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    activate.add_argument("--staging-dir", type=Path, default=Path("data/staging/broad-bars"))
    activate.add_argument("--cache-dir", type=Path, default=Path("data/cache"))
    activate.add_argument("--sec-user-agent", default=None)
    activate.add_argument("--memory-limit", default="8GB")
    activate.add_argument("--threads", type=int, default=4)
    activate.add_argument("--shards", type=int, default=16)
    activate.add_argument("--companyfacts-limit", type=int, default=None)
    activate.add_argument("--start-stage", choices=STAGE_ORDER, default=None)
    activate.add_argument("--stop-stage", choices=STAGE_ORDER, default=None)
    activate.add_argument("--only", action="append", choices=STAGE_ORDER, default=None)
    activate.add_argument("--dry-run", action="store_true")
    activate.add_argument("--force", action="store_true")
    activate.add_argument("--run-id")
```

Add to `main()` immediately before the final `raise AssertionError(...)`:

```python
    if args.command == "activate":
        run_activation(
            ActivationOptions(
                db_path=args.db_path,
                as_of_date=args.as_of_date,
                ticker_history_zip=args.ticker_history_zip,
                staging_dir=args.staging_dir,
                cache_dir=args.cache_dir,
                sec_user_agent=args.sec_user_agent,
                downloader=requests_downloader,
                memory_limit=args.memory_limit,
                threads=args.threads,
                reconciliation_shards=args.shards,
                companyfacts_limit=args.companyfacts_limit,
                dry_run=args.dry_run,
                force=args.force,
                run_id=args.run_id or f"warehouse-activate-{args.as_of_date.isoformat()}",
            ),
            stages=select_stages(
                start=args.start_stage,
                stop=args.stop_stage,
                only=tuple(args.only or ()),
            ),
        )
        return 0
```

- [ ] **Step 4: Run the CLI test to verify it passes**

Run: `.venv\Scripts\python.exe -m pytest tests/test_cli_activate.py -n 0 -q`
Expected: PASS — `5 passed`

- [ ] **Step 5: Replace the CI workflow**

Replace the whole of `C:\atx\.github\workflows\atx-db.yml` with:

```yaml
name: atx-db

on:
  push:
    paths:
      - "atx-db/**"
      - ".github/workflows/atx-db.yml"
  pull_request:
    paths:
      - "atx-db/**"
      - ".github/workflows/atx-db.yml"

jobs:
  quality:
    runs-on: ubuntu-latest
    # The full non-slow suite is ~2m47s locally at -n 4 plus a ~180s cold schema
    # template build; budget ~3 min of test time and cap the job well above it.
    timeout-minutes: 25
    defaults:
      run:
        working-directory: atx-db
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.12"
          cache: pip
          cache-dependency-path: atx-db/pyproject.toml
      - run: python -m pip install --upgrade pip
      - run: python -m pip install -e ".[dev]"
      - name: Cache the warehouse schema template
        uses: actions/cache@v4
        with:
          path: atx-db/.pytest_cache/db_schema_templates
          key: >-
            atx-db-schema-${{ hashFiles('atx-db/src/atx_db/migrations/*.py',
            'atx-db/src/atx_db/seeds/*.csv', 'atx-db/src/atx_db/schema.py',
            'atx-db/src/atx_db/schema_contract.py', 'atx-db/src/atx_db/api/catalog.py',
            'atx-db/src/atx_db/connection.py', 'atx-db/src/atx_db/fundamental_statements.py',
            'atx-db/src/atx_db/parity.py') }}
      - name: Compile package
        run: python -m compileall -q src tests scripts
      - name: Lint productionized surfaces
        run: >-
          python -m ruff check
          src/atx_db/activation.py
          src/atx_db/clock.py
          src/atx_db/ticker_history_extract.py
          src/atx_db/migrations/bodies_0300.py
          src/atx_db/cli.py
          src/atx_db/connection.py
          src/atx_db/security_master.py
          src/atx_db/symbol_directory.py
          src/atx_db/thirteenf.py
          src/atx_db/thirteenf_archive.py
          src/atx_db/thirteenf_amendments.py
          src/atx_db/thirteenf_signals.py
          src/atx_db/openfigi_signals.py
          src/atx_db/thirteenf_backtest.py
          src/atx_db/thirteenf_analysis.py
          scripts/warehouse_activate.py
          tests/test_activation_ledger.py
          tests/test_activation_stages_a.py
          tests/test_activation_stages_b.py
          tests/test_activation_ladder.py
          tests/test_ticker_history_extract.py
          tests/test_clock.py
          tests/test_determinism_as_of.py
          tests/test_determinism_ingest_edges.py
          tests/test_cli_activate.py
      - name: Typecheck productionized surfaces
        run: >-
          python -m mypy --strict
          src/atx_db/activation.py
          src/atx_db/clock.py
          src/atx_db/ticker_history_extract.py
          src/atx_db/connection.py
          src/atx_db/cli.py
          src/atx_db/thirteenf_archive.py
          src/atx_db/thirteenf_amendments.py
          src/atx_db/thirteenf_signals.py
          src/atx_db/openfigi_signals.py
          src/atx_db/thirteenf_backtest.py
          src/atx_db/thirteenf_analysis.py
      - name: Full non-slow test suite
        run: python -m pytest -q -n 4 -m "not slow" --tb=short tests
```

- [ ] **Step 6: Validate the workflow YAML parses**

Run: `.venv\Scripts\python.exe -c "import yaml,pathlib; d=yaml.safe_load(pathlib.Path('../.github/workflows/atx-db.yml').read_text()); print(sorted(d['jobs'])); print(len(d['jobs']['quality']['steps']))"`
Expected: `['quality']` then `9`

- [ ] **Step 7: Reproduce the CI test command locally**

Run: `.venv\Scripts\python.exe -m pytest -q -n 4 -m "not slow" --tb=short tests`
Expected: PASS — all tests pass, `X passed, Y deselected` in roughly 3 minutes with a warm schema template.

- [ ] **Step 8: Add the runbook section**

Append to `C:\atx\atx-db\docs\PRODUCTION_RUNBOOK.md`, immediately after the "Deploy" section:

```markdown
## Activation from scratch

`atx-db activate` builds a complete warehouse from an empty directory. It is
idempotent, resumable, and deterministic: every stage is recorded in
`activation_stage_runs`, and a rerun skips stages whose newest attempt completed
unless `--force` is given. Each stage prints exactly one JSON line to stdout.

### Prerequisites

- The SpiderRock archive `tbltickerhistory3_10y.zip` on disk (3.30 GiB
  compressed; one DEFLATE member, 11,084,562,320 uncompressed bytes).
- `ATX_SEC_USER_AGENT` set to a product name and a monitored contact address.
  The ladder fails fast on any SEC stage without it.
- One durable volume with room for the disk budget below.

### Disk usage

| Artifact | Size |
| --- | --- |
| `tbltickerhistory3_10y.zip` (input, not written by the ladder) | 3.3 GB |
| `data/staging/broad-bars/tbltickerhistory3_10y.txt` (extracted TSV) | **11 GB** |
| `data/cache/companyfacts.zip` | **~1.3 GB** |
| `data/cache/submissions.zip` | **~1.5 GB** |
| `data/warehouse.duckdb` (built) | **30-40 GB** |
| DuckDB spill (`data/staging/broad-bars/duckdb-tmp/`, transient) | up to 8 GB |
| **Peak total** | **~65 GB** |

The staging TSV may be deleted after `ticker_history_publish` completes; keep the
`.sha256` sidecar so a later rerun can prove which extraction produced the bars.

### Commands

```powershell
$env:ATX_SEC_USER_AGENT = "atx-db/0.2 ops@example.com"
$env:ATX_DB_PATH = "D:\atx\data\warehouse.duckdb"

# 1. See the plan without touching anything.
atx-db activate --db-path $env:ATX_DB_PATH --dry-run

# 2. Run the whole ladder (multi-hour; safe to interrupt).
atx-db activate --db-path $env:ATX_DB_PATH `
  --ticker-history-zip $env:USERPROFILE\Downloads\tbltickerhistory3_10y.zip `
  --staging-dir D:\atx\data\staging\broad-bars `
  --cache-dir D:\atx\data\cache `
  --memory-limit 8GB --threads 4 --shards 16

# 3. Resume after an interruption (completed stages are skipped automatically).
atx-db activate --db-path $env:ATX_DB_PATH

# 4. Resume from an explicit point, or rerun one stage.
atx-db activate --db-path $env:ATX_DB_PATH --start-stage companyfacts_load
atx-db activate --db-path $env:ATX_DB_PATH --only standardized --force

# 5. Confirm the result.
atx-db status --db-path $env:ATX_DB_PATH --strict
```

### Stage order

`migrate` -> `security_master` -> `symbol_directory` -> `ticker_history_extract`
-> `ticker_history_publish` -> `sec_bulk_download` -> `submissions_load` ->
`companyfacts_load` -> `statement_points` -> `periods` -> `ttm` ->
`calendarization` -> `standardized` -> `industry_templates` -> `reconciliation`
-> `provider_coverage`.

Network is limited to `security_master`, `symbol_directory`, and
`sec_bulk_download`; every other stage is offline. `sec_bulk_download` resumes a
partial transfer and records each archive's sha256 in `raw_source_files`.
`reconciliation` shells out to `scripts/refresh_reconciliation_sharded.py`, which
runs each shard in a fresh interpreter (a long-lived process was measured to
degrade a shard from ~110s to ~840s).

### Determinism

No derived or ingest path reads the wall clock. `atx_db.warehouse.now_utc_naive()`
is the only sanctioned timestamp read and is used solely for `source_loaded_at`
lineage. `atx_db.clock.utc_today()` is the only sanctioned wall-clock date read
and is called only at CLI/script edges, which pass the value down explicitly.
Library code resolves its stamp through
`atx_db.clock.resolve_as_of_date(explicit, source_max_date=...)`, which raises
rather than silently producing a non-reproducible run.
```

- [ ] **Step 9: Lint the touched sources**

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/cli.py tests/test_cli_activate.py`
Expected: `All checks passed!`

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/cli.py`
Expected: `Success: no issues found in 1 source file`

- [ ] **Step 10: Run the full non-slow suite one more time**

Run: `.venv\Scripts\python.exe -m pytest -q -n 4 -m "not slow" --tb=short tests`
Expected: PASS — no failures.

- [ ] **Step 11: Commit**

```bash
git add src/atx_db/cli.py ../.github/workflows/atx-db.yml docs/PRODUCTION_RUNBOOK.md tests/test_cli_activate.py
git commit -m "feat(db): atx-db activate subcommand, full-suite CI, activation runbook

Wires the activation ladder into the CLI, replaces the 6-file CI lane with the
whole non-slow suite plus ruff/mypy over the new modules (~3 min with a cached
schema template), and documents activation from scratch with exact commands and
the ~65 GB peak disk budget.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review notes

**Spec coverage.** Sprint 1 is the foundation layer of the Tier-1 spec, not the whole spec. It covers: the spec's §"Deterministic, reproducible builds" (Tasks 1, 2), §"Market data & corporate actions" load path (Tasks 4, 5), and the §"Universe" / §"Quality gates" prerequisite that the full CIK universe and full price history actually be loaded (Tasks 5-7). Item breadth (spec §"Canonical item catalog"), the derived-metric engine (§"Derived metric catalog"), delisting returns, and the serving tier are explicitly **out of Sprint 1 scope** and belong to later sprints of this program — they are recorded in audit §8 gaps 2, 3, 4b, 5, 7, 8, 10.

**Type consistency.** `StageResult(rows: int, detail: dict[str, object])` is used identically in Tasks 3, 5, 6, 7. `ActivationOptions.as_dict()` is the only mutation idiom used in tests. `Downloader.__call__(url, dest, *, user_agent) -> int` is identical in Tasks 5 and 6. `resolve_as_of_date(explicit, *, source_max_date)` keeps the same keyword in Tasks 1 and 2. `STAGE_ORDER` is defined once in Task 3 and asserted against `STAGES` in Task 7.

**Known cross-task ordering.** Task 3 changes `migrations/*.py`, which invalidates the conftest schema-template fingerprint; the first test run after Task 3 pays ~180 s. Tasks 4-8 reuse the rebuilt template.

---

## Addendum A (controller-ruled, binding for Tasks 3 and 8)

### Task 3 addendum — equity_daily_bars uniqueness is a quality check, not a constraint

Inside `bodies_0300.py`'s quality-check `conn.executemany(...)` block, add a second tuple after `activation_stage_runs_stuck_running`:

```python
            (
                "duplicate_equity_daily_bar_keys",
                "tbltickerhistory_daily",
                "equity_daily_bars",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_tier1_parity",
            ),
```

The check counts rows where `(source, security_id, trade_date)` occurs more than once in `equity_daily_bars` (expected 0).

**Why a check and not a unique index.** `(source, symbol, trade_date)` is not unique by design: `ticker_history_bulk._create_line_map` splits a recycled ticker into two `security_id`s that both keep the same `symbol`, so a unique index would break `INSERT INTO equity_daily_bars SELECT * FROM equity_daily_bars_bulk_next`. `(source, security_id, trade_date)` *is* guaranteed by the bulk publisher but not by the pandas chunk path, which inserts before `disambiguate_vendor_collisions` repairs, and not by an existing live warehouse. A constraint added here would fail the migration and trigger a backup restore. The check makes the invariant visible without risking the activation.

### Task 8 addendum

- CI ruff list also includes `src/atx_db/factor_panel.py`, `src/atx_db/signal_eval.py`, `tests/test_determinism_panel_dedupe.py`, `src/atx_db/clock.py`.
- Append to the runbook `### Determinism` subsection: "`source_loaded_at` is lineage only and is never an ordering or dedupe key: the factor panel selects duplicates by `(available_at, run_id)` in both its pandas and SQL read paths, and `factor_breadth.available_at` is the max of its input availabilities, falling back to `as_of_date + 22h` rather than to the clock."

### Task 1 addendum
See `.superpowers/sdd/2026-09-19-tier1-s1-foundation/task-1-addendum.md` (factor_panel dedupe ordering + signal_eval.compute_breadth fallback; new test file `tests/test_determinism_panel_dedupe.py`).
