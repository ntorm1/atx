"""Audited OpenFIGI v3 mapping for consensus-signal CUSIPs."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import logging
import os
import time
import uuid
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import requests

from .connection import DuckDBStore, resolve_data_dir
from .thirteenf_signals import DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER
from .warehouse import record_source_file

OPENFIGI_MAPPING_URL = "https://api.openfigi.com/v3/mapping"
OPENFIGI_SOURCE = "OpenFIGI v3"
LOGGER = logging.getLogger(__name__)
SUPPORTED_SECURITY_TYPES = {"ADR", "COMMON STOCK", "DEPOSITARY RECEIPT", "REIT"}
SUPPORTED_US_EXCHANGES = {"UA", "UN", "US", "UW"}


@dataclass(frozen=True)
class OpenFigiSignalMapOptions:
    start: dt.date | None = None
    end: dt.date | None = None
    api_key: str | None = field(default_factory=lambda: os.getenv("OPENFIGI_API_KEY"))
    cache_dir: Path = field(default_factory=lambda: resolve_data_dir() / "cache" / "openfigi")
    request_timeout: int = 60
    max_attempts: int = 5
    maximum_rank_per_quarter: int = DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER
    include_stress_quarters: bool = False
    replace: bool = False
    run_id: str | None = None


@dataclass(frozen=True)
class OpenFigiSignalMapResult:
    requested_cusips: int
    mapped_cusips: int
    unmatched_cusips: int
    candidate_rows: int
    elapsed_seconds: float


def ensure_openfigi_signal_schema(store: DuckDBStore) -> None:
    store.con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_signal_instrument_candidates (
            mapping_id VARCHAR PRIMARY KEY,
            cusip VARCHAR NOT NULL,
            figi VARCHAR,
            composite_figi VARCHAR,
            share_class_figi VARCHAR,
            ticker VARCHAR,
            name VARCHAR,
            exch_code VARCHAR,
            market_sector VARCHAR,
            security_type VARCHAR,
            security_type2 VARCHAR,
            candidate_rank INTEGER,
            selected BOOLEAN NOT NULL,
            mapping_status VARCHAR NOT NULL,
            warning VARCHAR,
            available_at TIMESTAMP NOT NULL,
            source VARCHAR NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    store.con.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_13f_signal_instrument_selected
        ON thirteenf_signal_instrument_candidates(cusip, selected)
        """
    )
    store.con.execute(
        """
        CREATE OR REPLACE VIEW v_thirteenf_signal_instruments AS
        SELECT *
        FROM thirteenf_signal_instrument_candidates
        WHERE is_latest_revision AND selected
        """
    )


def _candidate_score(candidate: dict[str, Any]) -> tuple[int, int, int, int, str]:
    security_type2 = str(candidate.get("securityType2") or "").upper()
    exchange = str(candidate.get("exchCode") or "").upper()
    market = str(candidate.get("marketSector") or "").upper()
    return (
        0 if market == "EQUITY" else 1,
        0 if security_type2 in {"COMMON STOCK", "REIT", "ADR"} else 1,
        0 if candidate.get("compositeFIGI") else 1,
        0 if exchange in {"US", "UN", "UA", "UW"} else 1,
        str(candidate.get("figi") or ""),
    )


def rank_openfigi_candidates(candidates: Sequence[dict[str, Any]]) -> tuple[dict[str, Any], ...]:
    eligible = [
        candidate
        for candidate in candidates
        if candidate.get("figi")
        and candidate.get("ticker")
        and str(candidate.get("marketSector") or "").upper() == "EQUITY"
        and str(candidate.get("securityType2") or "").upper() in SUPPORTED_SECURITY_TYPES
        and str(candidate.get("exchCode") or "").upper() in SUPPORTED_US_EXCHANGES
    ]
    return tuple(sorted(eligible, key=_candidate_score))


def _signal_cusips(store: DuckDBStore, options: OpenFigiSignalMapOptions) -> list[str]:
    if options.maximum_rank_per_quarter < 1:
        raise ValueError("maximum_rank_per_quarter must be positive")
    predicates = [
        "s.is_latest_revision",
        f"s.signal_rank <= {int(options.maximum_rank_per_quarter)}",
    ]
    if not options.include_stress_quarters:
        predicates.append("NOT s.is_stress_quarter")
    if options.start is not None:
        predicates.append(f"s.report_period >= DATE '{options.start.isoformat()}'")
    if options.end is not None:
        predicates.append(f"s.report_period <= DATE '{options.end.isoformat()}'")
    if not options.replace:
        predicates.append(
            "NOT EXISTS (SELECT 1 FROM thirteenf_signal_instrument_candidates m "
            "WHERE m.cusip = s.cusip AND m.is_latest_revision)"
        )
    rows = store.con.execute(
        f"""
        SELECT DISTINCT upper(trim(s.cusip))
        FROM thirteenf_consensus_amendment_signals s
        WHERE {' AND '.join(predicates)}
        ORDER BY 1
        """
    ).fetchall()
    return [str(row[0]) for row in rows]


def _mapping_id(cusip: str, candidate: dict[str, Any] | None) -> str:
    if candidate is None:
        payload = f"{cusip}|UNMATCHED"
    else:
        payload = "|".join(
            (cusip, str(candidate.get("figi") or ""), str(candidate.get("exchCode") or ""))
        )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _post_batch(
    session: requests.Session,
    cusips: Sequence[str],
    *,
    timeout: int,
    max_attempts: int,
) -> tuple[list[dict[str, Any]], requests.Response]:
    jobs = [{"idType": "ID_CUSIP", "idValue": cusip, "marketSecDes": "Equity"} for cusip in cusips]
    for attempt in range(1, max_attempts + 1):
        response = session.post(OPENFIGI_MAPPING_URL, json=jobs, timeout=timeout)
        if response.status_code == 429:
            if attempt == max_attempts:
                response.raise_for_status()
            reset_seconds = float(response.headers.get("ratelimit-reset", "2.5"))
            time.sleep(max(reset_seconds, 0.25))
            continue
        if response.status_code >= 500:
            if attempt == max_attempts:
                response.raise_for_status()
            time.sleep(min(2 ** (attempt - 1), 8))
            continue
        response.raise_for_status()
        payload = response.json()
        if not isinstance(payload, list) or len(payload) != len(cusips):
            raise RuntimeError("OpenFIGI returned a response that is not positionally aligned with the request")
        return payload, response
    raise AssertionError("unreachable")


def map_signal_cusips(
    store: DuckDBStore,
    options: OpenFigiSignalMapOptions,
    *,
    session: requests.Session | None = None,
) -> OpenFigiSignalMapResult:
    started = time.perf_counter()
    ensure_openfigi_signal_schema(store)
    cusips = _signal_cusips(store, options)
    if not cusips:
        return OpenFigiSignalMapResult(0, 0, 0, 0, time.perf_counter() - started)

    own_session = session is None
    session = session or requests.Session()
    session.headers.update({"Content-Type": "application/json", "Accept": "application/json"})
    if options.api_key:
        session.headers["X-OPENFIGI-APIKEY"] = options.api_key
    batch_size = 100 if options.api_key else 5
    minimum_interval = 0.25 if options.api_key else 2.5
    batch_count = (len(cusips) + batch_size - 1) // batch_size
    options.cache_dir.mkdir(parents=True, exist_ok=True)
    available_at = dt.datetime.now(dt.UTC).replace(tzinfo=None)
    mapped: set[str] = set()
    candidate_count = 0
    run_id = options.run_id or f"openfigi-signals-{uuid.uuid4()}"

    try:
        for offset in range(0, len(cusips), batch_size):
            batch_started = time.perf_counter()
            batch = cusips[offset : offset + batch_size]
            payload, response = _post_batch(
                session,
                batch,
                timeout=options.request_timeout,
                max_attempts=options.max_attempts,
            )
            cache_key = hashlib.sha256("\n".join(batch).encode("ascii")).hexdigest()[:20]
            cache_path = options.cache_dir / f"mapping-{cache_key}.json"
            cache_path.write_text(
                json.dumps({"requests": batch, "responses": payload}, sort_keys=True),
                encoding="utf-8",
            )
            rows: list[tuple[Any, ...]] = []
            for cusip, result in zip(batch, payload, strict=True):
                raw_candidates = [
                    candidate
                    for candidate in result.get("data") or []
                    if candidate.get("figi") and candidate.get("ticker")
                ]
                ranked = rank_openfigi_candidates(raw_candidates)
                ranked_ids = {
                    _mapping_id(cusip, candidate): rank
                    for rank, candidate in enumerate(ranked, start=1)
                }
                candidates_by_id = {
                    _mapping_id(cusip, candidate): candidate for candidate in raw_candidates
                }
                if ranked:
                    mapped.add(cusip)
                if not candidates_by_id:
                    warning = str(result.get("warning") or result.get("error") or "No eligible equity mapping")
                    rows.append(
                        (
                            _mapping_id(cusip, None),
                            cusip,
                            None,
                            None,
                            None,
                            None,
                            None,
                            None,
                            None,
                            None,
                            None,
                            None,
                            False,
                            "unmatched",
                            warning,
                            available_at,
                            OPENFIGI_SOURCE,
                            run_id,
                        )
                    )
                    continue
                for mapping_id, candidate in candidates_by_id.items():
                    rank = ranked_ids.get(mapping_id)
                    rows.append(
                        (
                            mapping_id,
                            cusip,
                            candidate.get("figi"),
                            candidate.get("compositeFIGI"),
                            candidate.get("shareClassFIGI"),
                            candidate.get("ticker"),
                            candidate.get("name"),
                            candidate.get("exchCode"),
                            candidate.get("marketSector"),
                            candidate.get("securityType"),
                            candidate.get("securityType2"),
                            rank,
                            rank == 1,
                            "mapped" if rank is not None else "ineligible",
                            result.get("warning")
                            if rank is not None
                            else "Not an eligible US-listed common equity, ADR, or REIT",
                            available_at,
                            OPENFIGI_SOURCE,
                            run_id,
                        )
                    )
            with store.transaction():
                placeholders = ",".join("?" for _ in batch)
                store.con.execute(
                    f"DELETE FROM thirteenf_signal_instrument_candidates WHERE cusip IN ({placeholders})",
                    batch,
                )
                store.con.executemany(
                    """
                    INSERT INTO thirteenf_signal_instrument_candidates (
                        mapping_id, cusip, figi, composite_figi, share_class_figi,
                        ticker, name, exch_code, market_sector, security_type,
                        security_type2, candidate_rank, selected, mapping_status,
                        warning, available_at, source, run_id
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    rows,
                )
            record_source_file(
                store,
                dataset_id="openfigi_signal_map",
                source_url=OPENFIGI_MAPPING_URL,
                cache_path=cache_path,
                status="loaded",
                compute_hash=True,
                metadata={
                    "cusips": batch,
                    "rate_limit": response.headers.get("ratelimit-limit"),
                    "rate_limit_remaining": response.headers.get("ratelimit-remaining"),
                    "run_id": run_id,
                },
            )
            candidate_count += len(rows)
            batch_number = offset // batch_size + 1
            if batch_number == 1 or batch_number % 10 == 0 or batch_number == batch_count:
                LOGGER.info(
                    "mapped OpenFIGI batch %s/%s (%s/%s CUSIPs)",
                    batch_number,
                    batch_count,
                    min(offset + len(batch), len(cusips)),
                    len(cusips),
                )
            delay = minimum_interval - (time.perf_counter() - batch_started)
            if delay > 0 and offset + batch_size < len(cusips):
                time.sleep(delay)
    finally:
        if own_session:
            session.close()

    return OpenFigiSignalMapResult(
        requested_cusips=len(cusips),
        mapped_cusips=len(mapped),
        unmatched_cusips=len(cusips) - len(mapped),
        candidate_rows=candidate_count,
        elapsed_seconds=time.perf_counter() - started,
    )
