"""S6a: 13F filer entity-resolution (`filer_13f_cik_alias`).

Resolves each 13F filing-manager CIK to a canonical *primary* CIK so that
institutional-ownership rollups can aggregate subadvisors, renamed entities, and
post-M&A continuations the way FactSet/WhaleWisdom do. The design is deliberately
conservative about cross-entity merges:

* **SELF** — every filer CIK maps to itself with its current normalized name.
  High precision; this is the spine.
* **NAME_HISTORY** — superseded prior names for the *same* CIK, with closed
  validity windows. Lets PIT queries resolve the name a filer used at a date.
* **NAME_MATCH_CANDIDATE** — cross-CIK links between filers that share an
  identical normalized name. Emitted at 0.5 confidence and **never** used by the
  default resolver, so unrelated "Smith Capital" filers do not silently merge.
* **SUBADVISOR / MA_CONTINUITY / MANUAL** — authoritative rollups supplied via an
  injectable curated seed CSV (the only path that asserts a real entity merge).

`resolve_primary_cik` walks the authoritative rollups visible at a point in time;
its default `min_confidence=1.0` excludes the candidate links entirely.
"""

from __future__ import annotations

import datetime as dt
import re
import uuid
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import file_sha256, now_utc_naive, quality_check, record_source_file


SOURCE_NAME = "ATX 13F filer entity-resolution builder"
DEFAULT_SOURCE = "atx_filer_13f_alias_v1"

AUTHORITATIVE_TYPES = ("SELF", "SUBADVISOR", "MA_CONTINUITY", "MANUAL")
SEED_TYPES = ("SUBADVISOR", "MA_CONTINUITY", "MANUAL")
CANDIDATE_CONFIDENCE = 0.5

# Pure legal-form / incorporation suffix tokens. Business words such as CAPITAL,
# MANAGEMENT, PARTNERS, ADVISORS are intentionally NOT here: stripping them would
# collapse genuinely distinct firms (e.g. "Acme Capital" vs "Acme Partners").
_LEGAL_SUFFIXES = {
    "LLC", "LLP", "LP", "LLLP", "LC", "PLLC",
    "INC", "INCORPORATED", "CORP", "CORPORATION", "CO", "COMPANY",
    "LTD", "LIMITED", "PLC", "NV", "SA", "AG", "GMBH", "AB", "BV",
    "KG", "SE", "OYJ", "ASA", "SAS", "SPA", "PTE", "PTY", "AS", "OY",
}

_INSERT_COLUMNS = [
    "alias_id", "primary_cik", "alias_cik", "alias_type", "manager_id",
    "normalized_name", "raw_name", "cluster_key", "valid_from", "valid_to",
    "is_current", "confidence", "evidence", "source", "as_of_date",
    "available_at", "run_id",
]


@dataclass(frozen=True)
class FilerAliasOptions:
    source: str = DEFAULT_SOURCE
    seed_file: Path | None = None
    seed_source: str = "injected_filer_alias_seed_v1"
    replace: bool = True
    run_id: str | None = None


def normalize_filer_name(name: str | None) -> str:
    """Normalize a filing-manager name for clustering.

    Uppercases, joins initialisms (drops periods so ``L.L.C.`` -> ``LLC``),
    maps remaining punctuation to spaces, collapses whitespace, then strips
    trailing legal-form suffix tokens. Always keeps at least one token.
    """
    if not name:
        return ""
    text = str(name).upper().replace(".", "")
    text = re.sub(r"[^0-9A-Z]+", " ", text).strip()
    text = re.sub(r"\s+", " ", text)
    if not text:
        return ""
    tokens = text.split(" ")
    while len(tokens) > 1 and tokens[-1] in _LEGAL_SUFFIXES:
        tokens.pop()
    return " ".join(tokens) if tokens else text


def _to_ts(value: dt.date) -> dt.datetime:
    return dt.datetime.combine(value, dt.time(0, 0))


def _alias_id(source: str, *parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"{source}|{payload}"))


def _coerce_date(value: object, fallback: dt.date) -> dt.date:
    if value in (None, "", pd.NaT):
        return fallback
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    parsed = pd.to_datetime(value, errors="coerce")
    return fallback if pd.isna(parsed) else parsed.date()


def _self_rows(store: DuckDBStore, source: str, today: dt.date, run_id: str | None) -> list[tuple]:
    rows = store.con.execute(
        """
        SELECT
            cik,
            arg_max(manager_id, coalesce(last_filing_date, last_report_period, DATE '1900-01-01')) AS manager_id,
            arg_max(manager_name, coalesce(last_filing_date, last_report_period, DATE '1900-01-01')) AS manager_name,
            min(coalesce(first_filing_date, first_report_period)) AS first_seen
        FROM thirteenf_managers
        WHERE cik IS NOT NULL AND coalesce(manager_name, '') <> ''
        GROUP BY cik
        ORDER BY cik
        """
    ).fetchall()

    out: list[tuple] = []
    self_meta: dict[str, tuple[str, dt.date]] = {}
    for cik, manager_id, manager_name, first_seen in rows:
        valid_from = _coerce_date(first_seen, today)
        normalized = normalize_filer_name(manager_name)
        self_meta[cik] = (normalized, valid_from)
        out.append((
            _alias_id(source, "SELF", cik),
            cik, cik, "SELF", manager_id,
            normalized, manager_name, normalized,
            valid_from, None, True, 1.0,
            "thirteenf_managers self identity", source,
            valid_from, _to_ts(valid_from), run_id,
        ))
    return out, self_meta  # type: ignore[return-value]


def _name_history_rows(
    store: DuckDBStore,
    source: str,
    self_meta: dict[str, tuple[str, dt.date]],
    run_id: str | None,
) -> list[tuple]:
    rows = store.con.execute(
        """
        SELECT cik, filing_manager_name, min(filing_date) AS first_filing
        FROM thirteenf_manager_reports
        WHERE cik IS NOT NULL
          AND coalesce(filing_manager_name, '') <> ''
          AND filing_date IS NOT NULL
        GROUP BY cik, filing_manager_name
        """
    ).fetchall()

    # Per CIK: distinct normalized names with their first-seen filing date.
    by_cik: dict[str, dict[str, tuple[str, dt.date]]] = {}
    for cik, raw_name, first_filing in rows:
        if first_filing is None:
            continue
        normalized = normalize_filer_name(raw_name)
        if not normalized:
            continue
        bucket = by_cik.setdefault(cik, {})
        existing = bucket.get(normalized)
        if existing is None or first_filing < existing[1]:
            bucket[normalized] = (raw_name, first_filing)

    out: list[tuple] = []
    for cik, names in by_cik.items():
        if len(names) < 2 and cik in self_meta:
            # Single reported name already covered by the SELF row.
            continue
        ordered = sorted(names.items(), key=lambda kv: kv[1][1])  # by first-seen date
        current_norm = self_meta[cik][0] if cik in self_meta else ordered[-1][0]
        for idx, (normalized, (raw_name, first_filing)) in enumerate(ordered):
            if normalized == current_norm:
                continue
            # Close the window the day before the next distinct name appeared.
            if idx + 1 < len(ordered):
                next_first = ordered[idx + 1][1][1]
                valid_to = next_first - dt.timedelta(days=1)
            else:
                valid_to = None
            out.append((
                _alias_id(source, "NAME_HISTORY", cik, normalized),
                cik, cik, "NAME_HISTORY", None,
                normalized, raw_name, normalized,
                first_filing, valid_to, False, 1.0,
                "thirteenf_manager_reports prior reported name", source,
                first_filing, _to_ts(first_filing), run_id,
            ))
    return out


def _candidate_rows(
    source: str,
    self_meta: dict[str, tuple[str, dt.date]],
    run_id: str | None,
) -> list[tuple]:
    clusters: dict[str, list[str]] = {}
    for cik, (normalized, _vf) in self_meta.items():
        if not normalized:
            continue
        clusters.setdefault(normalized, []).append(cik)

    out: list[tuple] = []
    for normalized, ciks in clusters.items():
        if len(ciks) < 2:
            continue
        representative = min(ciks)
        for cik in sorted(ciks):
            if cik == representative:
                continue
            valid_from = self_meta[cik][1]
            out.append((
                _alias_id(source, "NAME_MATCH_CANDIDATE", cik, representative),
                representative, cik, "NAME_MATCH_CANDIDATE", None,
                normalized, None, normalized,
                valid_from, None, True, CANDIDATE_CONFIDENCE,
                f"shared normalized name '{normalized}' (candidate, not authoritative)",
                source, valid_from, _to_ts(valid_from), run_id,
            ))
    return out


def _seed_rows(
    options: FilerAliasOptions,
    today: dt.date,
    run_id: str | None,
) -> tuple[list[tuple], str | None, int]:
    if options.seed_file is None:
        return [], None, 0
    seed_file = Path(options.seed_file)
    frame = pd.read_csv(seed_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(seed_file)
    out: list[tuple] = []
    for _, row in frame.iterrows():
        parent = (row.get("parent_cik") or "").strip()
        child = (row.get("child_cik") or "").strip()
        if not parent or not child:
            continue
        alias_type = (row.get("alias_type") or "MANUAL").strip().upper()
        if alias_type not in SEED_TYPES:
            alias_type = "MANUAL"
        valid_from = _coerce_date(row.get("valid_from"), today)
        valid_to = row.get("valid_to")
        valid_to = None if not valid_to else _coerce_date(valid_to, today)
        confidence_raw = (row.get("confidence") or "").strip()
        confidence = float(confidence_raw) if confidence_raw else 1.0
        available_raw = (row.get("available_at") or "").strip()
        available_at = (
            pd.to_datetime(available_raw).to_pydatetime()
            if available_raw
            else _to_ts(valid_from)
        )
        evidence = (row.get("evidence") or "").strip() or f"seed:{seed_file.name}"
        out.append((
            _alias_id(options.source, alias_type, child, parent, valid_from),
            parent, child, alias_type, None,
            None, None, None,
            valid_from, valid_to, True, confidence,
            evidence, options.source,
            valid_from, available_at, run_id,
        ))
    return out, source_hash, int(len(frame))


def refresh_filer_aliases(store: DuckDBStore, options: FilerAliasOptions | None = None) -> int:
    """Rebuild the filer alias spine from 13F managers plus an optional seed."""
    options = options or FilerAliasOptions()
    store.initialize()
    today = now_utc_naive().date()
    run_id = options.run_id

    self_rows, self_meta = _self_rows(store, options.source, today, run_id)
    history_rows = _name_history_rows(store, options.source, self_meta, run_id)
    candidate_rows = _candidate_rows(options.source, self_meta, run_id)
    seed_rows, seed_hash, seed_input_rows = _seed_rows(options, today, run_id)

    all_rows = self_rows + history_rows + candidate_rows + seed_rows
    # Deduplicate on the deterministic primary key (alias_id) to stay idempotent
    # even if a seed restates an auto-derived link.
    deduped: dict[str, tuple] = {row[0]: row for row in all_rows}
    rows = list(deduped.values())

    with store.transaction():
        if options.replace:
            store.con.execute("DELETE FROM filer_13f_cik_alias WHERE source = ?", [options.source])
        if rows:
            store.con.executemany(
                f"""
                INSERT INTO filer_13f_cik_alias ({", ".join(_INSERT_COLUMNS)})
                VALUES ({", ".join(["?"] * len(_INSERT_COLUMNS))})
                """,
                rows,
            )
        if options.seed_file is not None:
            record_source_file(
                store,
                dataset_id="filer_13f_cik_alias",
                source_url=str(options.seed_file),
                cache_path=Path(options.seed_file),
                sha256=seed_hash,
                metadata={"seed_rows": seed_input_rows, "source": options.seed_source},
            )

    return len(rows)


def resolve_primary_cik(
    store: DuckDBStore,
    cik: str,
    as_of_date: dt.date,
    *,
    min_confidence: float = 1.0,
    as_of_ts: dt.datetime | None = None,
    max_hops: int = 8,
) -> str:
    """Resolve a filer CIK to its primary (rollup) CIK at a point in time.

    Default ``min_confidence=1.0`` follows only authoritative rollups
    (SELF/SUBADVISOR/MA_CONTINUITY/MANUAL); lower it to <=0.5 to opt into
    cross-CIK NAME_MATCH_CANDIDATE links. Follows the rollup chain transitively
    with cycle protection.
    """
    as_of_ts = as_of_ts or dt.datetime.combine(as_of_date, dt.time.max)
    current = cik
    seen = {cik}
    for _ in range(max_hops):
        row = store.con.execute(
            """
            SELECT primary_cik
            FROM filer_13f_cik_alias
            WHERE alias_cik = ?
              AND primary_cik <> alias_cik
              AND confidence >= ?
              AND valid_from <= ?
              AND (valid_to IS NULL OR valid_to >= ?)
              AND available_at <= ?
            ORDER BY confidence DESC, valid_from DESC
            LIMIT 1
            """,
            [current, min_confidence, as_of_date, as_of_date, as_of_ts],
        ).fetchone()
        if not row:
            break
        nxt = row[0]
        if nxt in seen:
            break
        seen.add(nxt)
        current = nxt
    return current


class FilerAliasDataset(Dataset):
    dataset_id = "filer_13f_cik_alias"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FilerAliasOptions) -> DatasetLoadResult:
        rows = refresh_filer_aliases(store, options)
        by_type = store.con.execute(
            """
            SELECT alias_type, count(*)
            FROM filer_13f_cik_alias
            WHERE source = ?
            GROUP BY alias_type
            """,
            [options.source],
        ).fetchall()
        type_counts = {alias_type: int(count) for alias_type, count in by_type}
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="filer_13f_cik_alias",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "type_counts": type_counts},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"type_counts": type_counts, "seed_file": str(options.seed_file) if options.seed_file else None},
        )
