"""Listing-source ingestion: ladder order, pinned snapshots, source dating, actions.

Covers the pre-run5 A1 contracts: the activation ladder builds listing inputs
before their consumers, pinned-snapshot sources never re-download or overwrite
retained files by default, Nasdaq files are dated by their own File Creation Time
(the operator date is only a cutoff), a re-dated reload supersedes -- never
deletes -- earlier evidence, and Nasdaq action spellings map to one vocabulary.
"""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
import os
from pathlib import Path

import pytest

from atx_db.activation import (
    STAGE_DEPENDENCIES,
    STAGE_ORDER,
    ActivationOptions,
    run_activation,
    select_stages,
    stage_listing_events,
    stage_listing_status,
    stage_symbol_directory,
    validate_stage_dependencies,
)
from atx_db.clock import utc_today
from atx_db.listing_status import event_action_outcome
from atx_db.symbol_directory import (
    APPROVED_USER_AGENT,
    NASDAQ_LISTED_URL,
    OTHER_LISTED_URL,
    NasdaqListingEventsOptions,
    NasdaqSymbolDirectoryOptions,
    SnapshotAfterCutoffError,
    _read_directory_text,
    normalize_nasdaq_action,
    normalize_nasdaq_listed,
    normalize_other_listed,
    text_sha256,
)
from atx_db.warehouse import insert_frame, now_utc_naive, record_source_file

SRC = Path(__file__).resolve().parents[1] / "src" / "atx_db"
SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "warehouse_activate.py"

RUN5_SUFFIX = (
    "statement_points", "periods", "ttm", "calendarization", "standardized", "entity_classification",
    "industry_templates", "reconciliation", "derived_metrics", "market_daily", "equity_price_metrics", "listing_events",
    "listing_status", "legacy_liquid_universe", "factor_projections", "delisting_evidence",
    "universe_us_listed", "delisting_terminal_returns", "trading_calendar", "survivorship_forward_returns",
    "item_coverage", "provider_coverage", "quality",
)

FOOTER_0918 = "File Creation Time: 0918202621:31"
NASDAQ_LISTED_TXT = (
    "Symbol|Security Name|Market Category|Test Issue|Financial Status|Round Lot Size|ETF|NextShares\n"
    "AAA|AAA Corp - Common Stock|Q|N|N|100|N|N\n"
    "BBB|BBB Inc. - Class A Common Stock|G|N|N|100|N|N\n"
    f"{FOOTER_0918}|||||||\n"
)
OTHER_LISTED_TXT = (
    "ACT Symbol|Security Name|Exchange|CQS Symbol|ETF|Round Lot Size|Test Issue|NASDAQ Symbol\n"
    "CCC|CCC Corp Common Stock|N|CCC|N|100|N|CCC\n"
    f"{FOOTER_0918}||||||\n"
)
ADDS_DELETES_HEADER = "Symbol|Company Name|NASDAQ Action|BX Action|PSX Action|Effective Date|Primary Listing Market"


def _adds_deletes(rows: list[str], footer: str = FOOTER_0918) -> str:
    return "\n".join([ADDS_DELETES_HEADER, *rows, f"{footer}||||||"]) + "\n"


class RecordingDownloader:
    """Fake network seam: records every request and serves fixture payloads."""

    def __init__(self, payloads: dict[str, str] | None = None) -> None:
        self.payloads = payloads or {}
        self.calls: list[str] = []

    def __call__(self, url: str, dest: Path, *, user_agent: str) -> int:
        self.calls.append(url)
        if url not in self.payloads:
            raise AssertionError(f"unexpected network request: {url}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(self.payloads[url], encoding="utf-8")
        return dest.stat().st_size


def _options(tmp_path: Path, *, as_of: dt.date, downloader=None, **overrides) -> ActivationOptions:
    return ActivationOptions(
        db_path=tmp_path / "unused.duckdb",
        as_of_date=as_of,
        cache_dir=tmp_path / "cache",
        staging_dir=tmp_path / "staging",
        downloader=downloader,
        run_id="a1-test",
        **overrides,
    )


# The production retained files were received 2026-09-20 00:06 UTC (file mtime).
RECEIVED_0920 = dt.datetime(2026, 9, 20, 0, 6, 20)


def _retain(cache_dir: Path, name: str, text: str, received_at: dt.datetime = RECEIVED_0920) -> Path:
    """Place a retained cache file whose mtime is its original receipt time (UTC)."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    path = cache_dir / name
    path.write_text(text, encoding="utf-8")
    stamp = received_at.replace(tzinfo=dt.UTC).timestamp()
    os.utime(path, (stamp, stamp))
    return path


# --- ladder order -------------------------------------------------------------------


def test_stage_order_satisfies_declared_dependencies():
    validate_stage_dependencies()  # also enforced at import
    position = {stage: index for index, stage in enumerate(STAGE_ORDER)}
    assert set(STAGE_DEPENDENCIES) == set(STAGE_ORDER)
    for stage, inputs in STAGE_DEPENDENCIES.items():
        for dependency in inputs:
            assert position[dependency] < position[stage], f"{stage} runs before its input {dependency}"
    # First-run terminal returns read equity_price_metrics; listing inputs precede consumers.
    assert "equity_price_metrics" in STAGE_DEPENDENCIES["delisting_terminal_returns"]
    assert position["equity_price_metrics"] == position["market_daily"] + 1
    assert position["equity_price_metrics"] < position["delisting_terminal_returns"]
    assert position["listing_events"] < position["listing_status"] < position["legacy_liquid_universe"]
    assert position["listing_events"] < position["delisting_evidence"]
    assert STAGE_ORDER[-1] == "quality"


def test_misordered_ladder_is_rejected():
    swapped = list(STAGE_ORDER)
    i, j = swapped.index("equity_price_metrics"), swapped.index("delisting_terminal_returns")
    swapped[i], swapped[j] = swapped[j], swapped[i]
    with pytest.raises(RuntimeError, match="delisting_terminal_returns <- equity_price_metrics"):
        validate_stage_dependencies(tuple(swapped))


def test_listing_stages_declare_the_ticker_identity_rows_they_resolve_through():
    # AF1 (A1-review M4): listing_events and listing_status resolve symbols through the
    # security_identifier_history TICKER rows ticker_history_publish writes.
    assert "ticker_history_publish" in STAGE_DEPENDENCIES["listing_events"]
    assert "ticker_history_publish" in STAGE_DEPENDENCIES["listing_status"]
    moved = [stage for stage in STAGE_ORDER if stage != "listing_events"]
    moved.insert(moved.index("ticker_history_publish"), "listing_events")
    with pytest.raises(RuntimeError, match="listing_events <- ticker_history_publish"):
        validate_stage_dependencies(tuple(moved))


def test_market_daily_stage_detail_carries_the_owner_bridge_accounting(tmp_store, monkeypatch):
    # AF1 (A5-review M6): the stage detail reports the bridge accounting the refresh recorded.
    from atx_db import market_daily
    from atx_db.activation import ActivationOptions, stage_market_daily
    from atx_db.warehouse import quality_check

    bridge = {
        "source": market_daily.MARKET_DAILY_SOURCE_NAME, "rows": 7, "mode": "reconstructed",
        "identity_basis": "current_ticker_unverified", "availability_basis": "modeled",
        "linked_lines": 2, "unlinked_lines": 1, "unlinked_by_reason": {"no_current_ticker": 1},
        "linked_by_identity_basis": {"current_ticker_unverified": 2},
    }

    def fake_refresh(store, options):
        assert options.source == market_daily.MARKET_DAILY_SOURCE_NAME
        quality_check(store, dataset_id=market_daily.MarketDailyDataset.dataset_id,
                      table_name="market_daily_metrics", check_name=market_daily.OWNER_BRIDGE_CHECK_NAME,
                      status="warning", severity="warning", observed_value=2.0, threshold_value=3.0,
                      details=bridge)
        return 7

    monkeypatch.setattr(market_daily, "refresh_market_daily_metrics", fake_refresh)
    monkeypatch.setattr(market_daily, "shares_reconciliation_report", lambda store: {"shares_basis": "fixture"})
    result = stage_market_daily(tmp_store, ActivationOptions(run_id="af1"))
    assert result.rows == 7
    assert result.detail["shares_basis"] == "fixture"
    assert result.detail["owner_bridge"] == bridge
    assert result.detail["owner_bridge_current"] is True


def test_run5_suffix_is_the_23_stage_ladder():
    assert select_stages(start="statement_points") == RUN5_SUFFIX
    assert len(RUN5_SUFFIX) == 23


def test_dry_run_from_statement_points_emits_the_23_stage_suffix(built_warehouse, capsys):
    spec = importlib.util.spec_from_file_location("warehouse_activate_a1", SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    db_path = built_warehouse("dry-run-suffix.duckdb")

    def forbidden_migrations(path: Path) -> None:
        raise AssertionError("dry-run must not run governed migrations")

    exit_code = module.main(
        ["--dry-run", "--start-stage", "statement_points", "--db-path", str(db_path), "--as-of-date", "2026-09-20"],
        governed_migrations=forbidden_migrations,
    )
    lines = [json.loads(line) for line in capsys.readouterr().out.strip().splitlines() if line.startswith("{")]
    assert exit_code == 0
    assert tuple(line["stage"] for line in lines) == RUN5_SUFFIX
    assert {line["status"] for line in lines} == {"dry_run"}


# --- snapshot dating and non-destructive reloads ------------------------------------------


def _latest_directory(store) -> list[tuple]:
    return store.con.execute(
        """
        SELECT source_url, symbol, as_of_date FROM nasdaq_symbol_directory
        WHERE coalesce(is_latest_revision, true) ORDER BY source_url, symbol
        """
    ).fetchall()


def _simulate_operator_dated_load(store, cache_dir: Path, operator_date: dt.date) -> int:
    """Reproduce the pre-A1 stage: same retained files stamped with the operator date."""
    rows = 0
    for url, name, normalizer, text in (
        (NASDAQ_LISTED_URL, "nasdaqlisted.txt", normalize_nasdaq_listed, NASDAQ_LISTED_TXT),
        (OTHER_LISTED_URL, "otherlisted.txt", normalize_other_listed, OTHER_LISTED_TXT),
    ):
        path = _retain(cache_dir, name, text)
        frame = normalizer(_read_directory_text(text), as_of_date=operator_date, source_url=url, run_id="legacy")
        rows += insert_frame(store, frame, "nasdaq_symbol_directory", "legacy_directory_insert")
        sha = text_sha256(path.read_bytes())
        record_source_file(store, dataset_id="nasdaq_symbol_directory", source_url=url, cache_path=path,
                           sha256=sha, metadata={"as_of_date": operator_date.isoformat(), "sha256": sha})
    return rows


def test_redated_directory_reload_uses_file_creation_date_and_retains_prior_rows(tmp_store, tmp_path):
    operator_date = dt.date(2026, 9, 20)
    cache_dir = tmp_path / "cache"
    prior_rows = _simulate_operator_dated_load(tmp_store, cache_dir, operator_date)
    downloader = RecordingDownloader()  # retained files only: any request fails the test
    options = _options(tmp_path, as_of=operator_date, downloader=downloader)

    load_started = now_utc_naive()  # loaded days after the 2026-09-20 receipt
    result = stage_symbol_directory(tmp_store, options)

    assert downloader.calls == []
    assert result.detail["network_requests"] == 0
    assert result.detail["as_of_dates"] == ["2026-09-18"]
    assert result.rows == 3 and result.detail["superseded_rows"] == prior_rows == 3
    latest = _latest_directory(tmp_store)
    assert {row[2] for row in latest} == {dt.date(2026, 9, 18)}
    assert len(latest) == 3
    # available_at is the ORIGINAL receipt (file mtime), known by the cutoff session --
    # not the reload time; source_loaded_at records the reload.
    available, loaded = tmp_store.con.execute(
        """
        SELECT list_distinct(list(available_at)), min(source_loaded_at)
        FROM nasdaq_symbol_directory WHERE is_latest_revision
        """
    ).fetchone()
    assert available == [RECEIVED_0920]
    assert loaded > RECEIVED_0920 and load_started > RECEIVED_0920  # reloaded later than received
    assert {file["receipt_basis"] for file in result.detail["files"]} == {"file_mtime"}
    superseded = tmp_store.con.execute(
        """
        SELECT count(*), min(as_of_date), max(as_of_date), bool_and(NOT is_latest_revision)
        FROM nasdaq_symbol_directory WHERE as_of_date = DATE '2026-09-20'
        """
    ).fetchone()
    assert superseded == (prior_rows, operator_date, operator_date, True)

    # Reloading the same pinned files is a no-op: no new evidence, nothing superseded.
    again = stage_symbol_directory(tmp_store, options)
    assert again.rows == 3 and again.detail["rows_inserted"] == 0 and again.detail["superseded_rows"] == 0
    assert {file["source_status"] for file in again.detail["files"]} == {"unchanged"}
    total = tmp_store.con.execute("SELECT count(*) FROM nasdaq_symbol_directory").fetchone()[0]
    assert total == prior_rows + 3


def test_directory_file_created_after_the_cutoff_is_not_loaded(tmp_store, tmp_path):
    _retain(tmp_path / "cache", "nasdaqlisted.txt", NASDAQ_LISTED_TXT)
    _retain(tmp_path / "cache", "otherlisted.txt", OTHER_LISTED_TXT)
    options = _options(tmp_path, as_of=dt.date(2026, 9, 17), downloader=RecordingDownloader())
    with pytest.raises(SnapshotAfterCutoffError, match="2026-09-18"):
        stage_symbol_directory(tmp_store, options)
    assert tmp_store.con.execute("SELECT count(*) FROM nasdaq_symbol_directory").fetchone()[0] == 0


def test_network_refresh_archives_the_retained_file_instead_of_overwriting_it(tmp_store, tmp_path):
    cache_dir = tmp_path / "cache"
    _retain(cache_dir, "nasdaqlisted.txt", NASDAQ_LISTED_TXT)
    _retain(cache_dir, "otherlisted.txt", OTHER_LISTED_TXT)
    newer = NASDAQ_LISTED_TXT.replace("0918202621:31", "0921202621:31")
    downloader = RecordingDownloader({NASDAQ_LISTED_URL: newer, OTHER_LISTED_URL: OTHER_LISTED_TXT})
    options = _options(tmp_path, as_of=utc_today(), downloader=downloader, allow_network_refresh=True)

    result = stage_symbol_directory(tmp_store, options)

    assert downloader.calls == [NASDAQ_LISTED_URL, OTHER_LISTED_URL]
    acquisitions = {Path(f["cache_path"]).name: f for f in result.detail["files"]}
    assert acquisitions["nasdaqlisted.txt"]["acquisition"] == "refreshed"
    assert acquisitions["otherlisted.txt"]["acquisition"] == "refreshed_unchanged"
    archived = Path(acquisitions["nasdaqlisted.txt"]["superseded_path"])
    assert archived.read_text(encoding="utf-8") == NASDAQ_LISTED_TXT  # prior evidence kept on disk
    assert (cache_dir / "nasdaqlisted.txt").read_text(encoding="utf-8") == newer
    assert result.detail["as_of_dates"] == ["2026-09-18", "2026-09-21"]
    # A fresh download is available from its own receipt time, recorded next to the file;
    # identical re-downloaded bytes keep their original receipt.
    assert acquisitions["nasdaqlisted.txt"]["receipt_basis"] == "network_download"
    assert acquisitions["otherlisted.txt"]["receipt_basis"] == "file_mtime"
    receipt = json.loads((cache_dir / "nasdaqlisted.txt.receipt.json").read_text(encoding="utf-8"))
    assert dt.datetime.fromisoformat(receipt["received_at"]) > RECEIVED_0920
    available = dict(tmp_store.con.execute(
        "SELECT as_of_date, max(available_at) FROM nasdaq_symbol_directory WHERE is_latest_revision GROUP BY 1"
    ).fetchall())
    assert available == {dt.date(2026, 9, 18): RECEIVED_0920,
                         dt.date(2026, 9, 21): dt.datetime.fromisoformat(receipt["received_at"])}


def test_cache_receipt_takes_precedence_over_mtime_unless_stale(tmp_store, tmp_path):
    cache_dir = tmp_path / "cache"
    listed = _retain(cache_dir, "nasdaqlisted.txt", NASDAQ_LISTED_TXT, received_at=dt.datetime(2026, 9, 24, 12, 0))
    other = _retain(cache_dir, "otherlisted.txt", OTHER_LISTED_TXT, received_at=dt.datetime(2026, 9, 24, 12, 0))
    recorded = dt.datetime(2026, 9, 19, 1, 2, 3)
    for path, sha in ((listed, text_sha256(listed.read_bytes())), (other, "stale-sha")):
        (cache_dir / f"{path.name}.receipt.json").write_text(
            json.dumps({"received_at": recorded.isoformat(), "sha256": sha}), encoding="utf-8")
    result = stage_symbol_directory(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 25)))
    by_name = {Path(f["cache_path"]).name: f for f in result.detail["files"]}
    assert (by_name["nasdaqlisted.txt"]["receipt_basis"], by_name["nasdaqlisted.txt"]["available_at"]) == (
        "cache_receipt", recorded.isoformat())
    assert (by_name["otherlisted.txt"]["receipt_basis"], by_name["otherlisted.txt"]["available_at"]) == (
        "file_mtime", dt.datetime(2026, 9, 24, 12, 0).isoformat())


# --- receipt-vs-cutoff guard ----------------------------------------------------------


RECEIVED_0921 = dt.datetime(2026, 9, 21, 3, 0)


def test_directory_received_after_the_cutoff_day_fails_loudly(tmp_store, tmp_path):
    # Created 2026-09-18 (before the cutoff) but received 2026-09-21: cannot evidence 09-20.
    _retain(tmp_path / "cache", "nasdaqlisted.txt", NASDAQ_LISTED_TXT, received_at=RECEIVED_0921)
    _retain(tmp_path / "cache", "otherlisted.txt", OTHER_LISTED_TXT, received_at=RECEIVED_0921)
    downloader = RecordingDownloader()
    with pytest.raises(SnapshotAfterCutoffError, match=r"received 2026-09-21T03:00:00 \(receipt_basis=file_mtime\)"):
        stage_symbol_directory(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 20), downloader=downloader))
    assert downloader.calls == []
    assert tmp_store.con.execute("SELECT count(*) FROM nasdaq_symbol_directory").fetchone()[0] == 0
    # The same files are valid evidence for a cutoff on/after their receipt day.
    ok = stage_symbol_directory(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 21), downloader=downloader))
    assert ok.rows == 3 and ok.detail["as_of_dates"] == ["2026-09-18"]


def test_adds_deletes_received_after_the_cutoff_day_is_unavailable(tmp_store, tmp_path):
    _retain(tmp_path / "cache", "TradingSystemAddsDeletes.txt",
            _adds_deletes(["AAA|AAA Corp|Delete|||09/18/2026|Q"]), received_at=RECEIVED_0921)
    result = stage_listing_events(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 20),
                                                      downloader=RecordingDownloader()))
    assert result.rows == 0
    assert (result.detail["source_status"], result.detail["reason"]) == (
        "source_unavailable_for_snapshot", "received_after_cutoff")
    assert tmp_store.con.execute("SELECT count(*) FROM nasdaq_listing_events").fetchone()[0] == 0


def test_listing_status_ignores_evidence_known_after_the_cutoff_and_retires_stale_builds(tmp_store, tmp_path):
    for symbol, available in (("KNOWN", "2026-09-20 00:06:00"), ("LATE", "2026-09-21 03:00:00")):
        tmp_store.con.execute(
            f"""
            INSERT INTO nasdaq_symbol_directory (directory, symbol, security_name, market_category, exchange,
                as_of_date, source_url, available_at, is_latest_revision)
            VALUES ('nasdaqlisted', '{symbol}', '{symbol} Corp', 'Q', 'NASDAQ', DATE '2026-09-18', 'u',
                    TIMESTAMP '{available}', true)
            """
        )
    stage_listing_status(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 21)))
    assert tmp_store.con.execute("SELECT count(*) FROM listing_status_intervals").fetchone()[0] == 2
    result = stage_listing_status(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 20)))
    symbols = [row[0] for row in tmp_store.con.execute("SELECT symbol FROM listing_status_intervals").fetchall()]
    assert symbols == ["KNOWN"] and result.rows == 1
    # No evidence known by an earlier cutoff: the prior build must not survive.
    empty = stage_listing_status(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 17)))
    assert empty.rows == 0
    assert tmp_store.con.execute("SELECT count(*) FROM listing_status_intervals").fetchone()[0] == 0


def test_network_refresh_for_a_past_cutoff_is_refused_before_any_request(tmp_store, tmp_path):
    retained = _retain(tmp_path / "cache", "nasdaqlisted.txt", NASDAQ_LISTED_TXT)
    _retain(tmp_path / "cache", "otherlisted.txt", OTHER_LISTED_TXT)
    downloader = RecordingDownloader({NASDAQ_LISTED_URL: "newer", OTHER_LISTED_URL: "newer"})
    options = _options(tmp_path, as_of=dt.date(2026, 9, 20), downloader=downloader, allow_network_refresh=True)
    with pytest.raises(ValueError, match="past cutoff 2026-09-20"):
        stage_symbol_directory(tmp_store, options)
    with pytest.raises(ValueError, match="past cutoff 2026-09-20"):
        stage_listing_events(tmp_store, options)
    assert downloader.calls == []
    assert retained.read_text(encoding="utf-8") == NASDAQ_LISTED_TXT
    assert not (tmp_path / "cache" / "superseded").exists()


# --- Adds/Deletes: pinned run, actions, identity basis ------------------------------------


def test_pinned_run_without_retained_adds_deletes_makes_no_request(built_warehouse, tmp_path):
    downloader = RecordingDownloader()
    options = ActivationOptions(
        **{**_options(tmp_path, as_of=dt.date(2026, 9, 20), downloader=downloader).as_dict(),
           "db_path": built_warehouse("pinned-no-adds-deletes.duckdb")}
    )
    lines: list[dict[str, object]] = []
    run_activation(options, stages=("listing_events", "listing_status"), emit=lines.append)

    assert downloader.calls == []
    assert [(line["stage"], line["status"], line["rows"]) for line in lines] == [
        ("listing_events", "completed", 0),
        ("listing_status", "completed", 0),
    ]
    detail = lines[0]["detail"]
    assert isinstance(detail, dict)
    assert detail["source_status"] == "source_unavailable_for_snapshot"
    assert detail["network_requests"] == 0


def test_retained_adds_deletes_created_after_the_cutoff_is_unavailable(tmp_store, tmp_path):
    _retain(tmp_path / "cache", "TradingSystemAddsDeletes.txt",
            _adds_deletes(["AAA|AAA Corp|Delete|||09/18/2026|Q"]))
    downloader = RecordingDownloader()
    result = stage_listing_events(tmp_store, _options(tmp_path, as_of=dt.date(2026, 9, 17), downloader=downloader))
    assert downloader.calls == []
    assert result.rows == 0
    assert result.detail["source_status"] == "source_unavailable_for_snapshot"
    assert tmp_store.con.execute("SELECT count(*) FROM nasdaq_listing_events").fetchone()[0] == 0


@pytest.mark.parametrize(
    ("raw", "canonical"),
    [("A", "add"), ("Add", "add"), ("add", "add"), (" a ", "add"), ("D", "delete"), ("Delete", "delete"),
     ("DELETE", "delete"), ("d", "delete"), ("", None), (None, None), ("X", "X")],
)
def test_nasdaq_actions_normalize_to_one_vocabulary(raw, canonical):
    assert normalize_nasdaq_action(raw) == canonical


def test_listing_status_maps_every_action_spelling_and_counts_mixed_rows(tmp_store, tmp_path):
    tmp_store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik, ticker, title, security_id, source_loaded_at)
        VALUES ('0000000001', 'DDD', 'DDD Corp', 'SEC-CIK-0000000001', TIMESTAMP '2026-09-01 00:00:00')
        """
    )
    _retain(tmp_path / "cache", "TradingSystemAddsDeletes.txt", _adds_deletes([
        "DDD|DDD Corp|Delete|||09/10/2026|Q",       # spelled-out delete, effective before the snapshot
        "EEE|EEE Corp|D|||09/18/2026|N",            # single-letter delete
        "FFF|FFF Corp|Add|||09/21/2026|Q",          # spelled-out add, effective after the snapshot
        "GGG|GGG Corp|A|||09/18/2026|P",            # single-letter add
        "HHH|HHH Corp|Add|Delete||09/18/2026|Q",    # mixed add + delete across venues
    ]))
    options = _options(tmp_path, as_of=dt.date(2026, 9, 20), downloader=RecordingDownloader())

    events = stage_listing_events(tmp_store, options)
    status = stage_listing_status(tmp_store, options)

    assert events.detail["action_counts"] == {"add": 2, "delete": 2, "mixed": 1, "no_action": 0, "unrecognized": 0}
    assert events.detail["identity_basis"] == "current_ticker_unverified"
    stored = tmp_store.con.execute(
        "SELECT DISTINCT nasdaq_action FROM nasdaq_listing_events WHERE nasdaq_action IS NOT NULL ORDER BY 1"
    ).fetchall()
    assert stored == [("add",), ("delete",)]
    intervals = {
        row[0]: row[1:]
        for row in tmp_store.con.execute(
            """
            SELECT symbol, status, valid_from, security_id,
                   json_extract_string(details_json, '$.identity_basis'),
                   json_extract_string(details_json, '$.valid_from_basis'),
                   json_extract_string(details_json, '$.effective_date')
            FROM listing_status_intervals WHERE evidence_source_table = 'nasdaq_listing_events'
            """
        ).fetchall()
    }
    assert {symbol: values[0] for symbol, values in intervals.items()} == {
        "DDD": "inactive", "EEE": "inactive", "FFF": "active", "GGG": "active",
    }
    assert "HHH" not in intervals
    assert status.detail["event_action_outcomes"]["mixed"] == 1
    snapshot_date = dt.date(2026, 9, 18)
    assert all(values[1] >= snapshot_date for values in intervals.values())  # never before the snapshot
    assert intervals["DDD"][1:] == (snapshot_date, "SEC-CIK-0000000001", "current_ticker_unverified",
                                    "snapshot_date_floor", "2026-09-10")
    assert intervals["FFF"][1] == dt.date(2026, 9, 21) and intervals["FFF"][4] == "effective_date"
    assert intervals["EEE"][2] is None and intervals["EEE"][3] == "current_ticker_unverified"


def test_listing_status_accepts_legacy_stored_spellings(tmp_store):
    for event_id, symbol, action in (("e1", "LEG1", "Delete"), ("e2", "LEG2", "D"), ("e3", "LEG3", "A")):
        tmp_store.con.execute(
            """
            INSERT INTO nasdaq_listing_events (event_id, symbol, nasdaq_action, effective_date, as_of_date,
                                               source_url, available_at, is_latest_revision)
            VALUES (?, ?, ?, DATE '2026-09-18', DATE '2026-09-18', 'legacy', TIMESTAMP '2026-09-20 00:00:00', true)
            """,
            [event_id, symbol, action],
        )
    stage_listing_status(tmp_store, _options(Path("."), as_of=dt.date(2026, 9, 20)))
    statuses = dict(tmp_store.con.execute("SELECT symbol, status FROM listing_status_intervals").fetchall())
    assert statuses == {"LEG1": "inactive", "LEG2": "inactive", "LEG3": "active"}
    assert event_action_outcome({"nasdaq_action": "d", "bx_action": "Delete", "psx_action": None}) == "delete"


def test_redated_adds_deletes_reload_supersedes_prior_rows(tmp_store, tmp_path):
    text = _adds_deletes(["AAA|AAA Corp|D|||09/18/2026|Q", "BBB|BBB Corp|A|||09/18/2026|N"])
    path = _retain(tmp_path / "cache", "TradingSystemAddsDeletes.txt", text)
    # Pre-A1 load: raw spellings, operator date, same content receipt.
    for event_id, symbol, action in (("old-a", "AAA", "D"), ("old-b", "BBB", "A")):
        tmp_store.con.execute(
            """
            INSERT INTO nasdaq_listing_events (event_id, symbol, nasdaq_action, effective_date, as_of_date,
                source_file_created_at, source_url, available_at, is_latest_revision)
            VALUES (?, ?, ?, DATE '2026-09-18', DATE '2026-09-20', TIMESTAMP '2026-09-18 21:31:00', ?,
                    TIMESTAMP '2026-09-20 00:06:00', true)
            """,
            [event_id, symbol, action, "https://www.nasdaqtrader.com/dynamic/SymDir/TradingSystemAddsDeletes.txt"],
        )
    options = _options(tmp_path, as_of=dt.date(2026, 9, 20), downloader=RecordingDownloader())
    result = stage_listing_events(tmp_store, options)

    assert result.detail["superseded_rows"] == 2 and result.rows == 2
    rows = tmp_store.con.execute(
        "SELECT event_id, as_of_date, is_latest_revision FROM nasdaq_listing_events ORDER BY event_id"
    ).fetchall()
    assert len(rows) == 4  # nothing deleted
    assert {(r[1], r[2]) for r in rows if r[0].startswith("old-")} == {(dt.date(2026, 9, 20), False)}
    assert {(r[1], r[2]) for r in rows if not r[0].startswith("old-")} == {(dt.date(2026, 9, 18), True)}
    new_available = tmp_store.con.execute(
        "SELECT list_distinct(list(available_at)) FROM nasdaq_listing_events WHERE is_latest_revision"
    ).fetchone()[0]
    assert new_available == [RECEIVED_0920]  # original receipt, not the reload time
    again = stage_listing_events(tmp_store, options)
    assert again.detail["source_status"] == "unchanged" and again.detail["rows_inserted"] == 0
    assert path.read_text(encoding="utf-8") == text


# --- approved user agent ----------------------------------------------------------------


def test_symbol_directory_defaults_to_the_approved_user_agent():
    assert "@gmail" not in (SRC / "symbol_directory.py").read_text(encoding="utf-8")
    assert NasdaqSymbolDirectoryOptions().user_agent == APPROVED_USER_AGENT
    assert NasdaqListingEventsOptions().user_agent == APPROVED_USER_AGENT
    assert APPROVED_USER_AGENT == "atx-db/0.1 atx-research@example.com"
