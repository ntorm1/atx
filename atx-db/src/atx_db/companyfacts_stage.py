"""CF-R extract (tier-1 v2 node 1.4): the pinned SEC CompanyFacts archive to Parquet batches.

No warehouse is opened. The output feeds the ``companyfacts_rebuild`` batch stage (node 1.5),
which loads it append-only into ``__next`` tables and proves digest equality against the
lineage-verified CIKs (ruling R-3), so extraction reproduces the legacy archive loader exactly:

* member selection -- ``CIK##########.json`` members of the ZIP in CIK order, exactly like
  ``resolve_companyfacts_targets(symbol_source='archive_members')``;
* the exact two-byte ``{}`` placeholder rule (``_CompanyFactsZipFetcher``): disposition
  ``unavailable`` / ``empty_archive_placeholder``, no rows;
* the payload checks of ``SecCompanyFactsDataset.load``: an object, a ``facts`` object and
  ``_validate_archive_payload_cik``; a failed member is disposition ``error`` (legacy receipt
  status ``error``) with ``<ErrorType>: <message[:200]>``;
* ``normalize_companyfacts`` (``fundamentals.py``): taxonomy filter ``us-gaap``/``dei``, the
  concept allowlist, ``end <= filed`` (either missing drops the fact), ``available_at = filed +
  22h``, ``value`` numeric coercion, and the empty-member reasons of the loader
  (``unsupported_or_empty_taxonomy`` / ``allowlist_empty`` / ``no_valid_fact_rows``).

Per-member parsing uses the standard-library ``json`` (the loader's parser) into column lists that
become PyArrow arrays; one member is resident at a time and at most one 131,072-row row group is
buffered. The ``value`` coercion reproduces ``pd.to_numeric(errors='coerce')`` followed by the
DuckDB pandas scan (NaN -> NULL) for JSON numbers and nulls; any other JSON type in a typed
column (a string or boolean ``val``, a non-integer ``fy``, a non-string text field, an integer
outside BIGINT) has no pandas-free equivalent and fails the member closed
(``CompanyFactsParityError``) instead of guessing.

Layout of ``<out_root>/<archive_sha16>/``:

* ``plan.json`` -- immutable: archive identity and sha256, rule version, the pinned concept
  allowlist (and the loader's ``allowlist_sha256`` fingerprint), planner parameters, batches;
* ``batch-NNNN.parquet`` -- fact rows (zstd, 131,072-row row groups; written to ``.tmp``, renamed);
* ``batch-NNNN.json`` -- the batch's manifest row: file sha256, rows, row groups, per-member
  dispositions. It is written (``.tmp``, renamed) only after its Parquet file, so a batch is
  complete iff both exist and agree; resume skips exactly those;
* ``manifest.json`` and ``members.parquet`` -- assembled from the batch rows once every batch
  is complete (:func:`assemble_companyfacts_stage`).

Importing this module needs only the standard library (the orchestrator stays lean, index section 4
M6); PyArrow is imported by the functions that write or read Parquet.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import os
import re
import zipfile
from collections.abc import Collection, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

RULE_VERSION = "cf-extract-v1"
# Mirrors of the legacy loader's constants (fundamentals.py). The loader module imports pandas, so
# they are restated here; scripts/stage_companyfacts.py verify-sample asserts they are equal.
SOURCE_NAME = "SEC companyfacts"
SEC_COMPANY_FACTS_ZIP_URL = "https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip"
SUPPORTED_FACT_TAXONOMIES = ("us-gaap", "dei")
COMPANYFACTS_MEMBER_PATTERN = re.compile(r"CIK([0-9]{10})\.json")
UNRESOLVED_COMPANYFACTS_CIK_PREFIX = "SEC-COMPANYFACTS-UNRESOLVED-CIK-"
AVAILABLE_AT_OFFSET = dt.timedelta(hours=22)

DEFAULT_TARGET_UNCOMPRESSED_BYTES = 230_000_000
ROW_GROUP_ROWS = 131_072
PARQUET_OPTIONS: dict[str, Any] = {
    "compression": "zstd", "compression_level": 3, "use_dictionary": True, "write_statistics": True,
    "version": "2.6", "data_page_version": "1.0",
}
PLAN_FILE = "plan.json"
MANIFEST_FILE = "manifest.json"
MEMBERS_FILE = "members.parquet"

# Fact columns: every sec_company_facts column the archive determines. entity_id and the resolved
# security_id come from identity resolution and run_id from the loading run (node 1.5);
# ``security_id`` here is the loader's pre-resolution passthrough (``UNRESOLVED_..._PREFIX || cik``),
# which unresolved facts keep. The fundamental_points projection reads the same columns
# (metric = concept, as_of_date = filed_date, symbol = NULL for archive members).
FACT_COLUMNS: tuple[tuple[str, str], ...] = (
    ("source", "string"), ("security_id", "string"), ("cik", "string"), ("taxonomy", "string"),
    ("concept", "string"), ("label", "string"), ("description", "string"), ("unit", "string"),
    ("period_start", "date32"), ("period_end", "date32"), ("filed_date", "date32"),
    ("fiscal_year", "int32"), ("fiscal_period", "string"), ("form", "string"),
    ("accession_number", "string"), ("frame", "string"), ("value", "float64"),
    ("available_at", "timestamp_us"), ("source_url", "string"),
)
MEMBER_COLUMNS: tuple[tuple[str, str], ...] = (
    ("cik", "string"), ("member", "string"), ("crc32", "int64"), ("compressed_bytes", "int64"),
    ("uncompressed_bytes", "int64"), ("disposition", "string"), ("reason", "string"),
    ("rows", "int64"), ("payload_cik", "int64"),
)
DISPOSITIONS = ("loaded", "empty", "unavailable", "error")
PLACEHOLDER_REASON = "empty_archive_placeholder"
_INT32 = (-(2 ** 31), 2 ** 31 - 1)
_INT64 = (-(2 ** 63), 2 ** 63 - 1)


class CompanyFactsParityError(ValueError):
    """A JSON value whose legacy (pandas + DuckDB) coercion this pandas-free path cannot reproduce."""


@dataclass(frozen=True)
class CompanyFactsBatch:
    batch_id: int
    first_cik: int
    last_cik: int
    members: tuple[str, ...]          # zip member names in CIK order
    uncompressed_bytes: int


# ---------------------------------------------------------------------------
# Archive identity and planning (ZIP central directory only)
# ---------------------------------------------------------------------------

def archive_sha256(zip_path: Path) -> str:
    with Path(zip_path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def _member_infos(archive: zipfile.ZipFile) -> tuple[list[zipfile.ZipInfo], list[str]]:
    """CIK members in CIK order, plus the ignored (non-CIK) member names. Duplicates fail closed."""
    members = [info for info in archive.infolist() if COMPANYFACTS_MEMBER_PATTERN.fullmatch(info.filename)]
    ignored = sorted(info.filename for info in archive.infolist()
                     if not COMPANYFACTS_MEMBER_PATTERN.fullmatch(info.filename))
    names = [info.filename for info in members]
    if len(names) != len(set(names)):
        raise ValueError("companyfacts archive has duplicate CIK member names; no unique disposition exists")
    return sorted(members, key=lambda info: info.filename), ignored


def _cik_of(member: str) -> str:
    match = COMPANYFACTS_MEMBER_PATTERN.fullmatch(member)
    if match is None:
        raise ValueError(f"not a companyfacts CIK member: {member!r}")
    return match.group(1)


def plan_companyfacts_batches(zip_path: Path,
                              target_uncompressed_bytes: int = DEFAULT_TARGET_UNCOMPRESSED_BYTES,
                              ) -> list[CompanyFactsBatch]:
    """Contiguous CIK ranges, each closed before it would exceed the uncompressed-byte target."""
    if target_uncompressed_bytes <= 0:
        raise ValueError("target_uncompressed_bytes must be positive")
    with zipfile.ZipFile(zip_path) as archive:
        infos, _ignored = _member_infos(archive)
    batches: list[CompanyFactsBatch] = []
    current: list[zipfile.ZipInfo] = []

    def close() -> None:
        batches.append(CompanyFactsBatch(
            batch_id=len(batches), first_cik=int(_cik_of(current[0].filename)),
            last_cik=int(_cik_of(current[-1].filename)),
            members=tuple(info.filename for info in current),
            uncompressed_bytes=sum(info.file_size for info in current)))
        current.clear()

    size = 0
    for info in infos:
        if current and size + info.file_size > target_uncompressed_bytes:
            close()
            size = 0
        current.append(info)
        size += info.file_size
    if current:
        close()
    return batches


def allowlist_fingerprint(concepts: Iterable[str]) -> str:
    """The legacy loader's ``allowlist_sha256`` (``SecCompanyFactsDataset.load``), byte for byte."""
    payload = {"concepts": sorted(set(concepts)), "taxonomies": SUPPORTED_FACT_TAXONOMIES}
    return hashlib.sha256(json.dumps(payload, sort_keys=True, default=str).encode()).hexdigest()


def _canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, indent=1, ensure_ascii=True) + "\n"


def _write_json_atomic(path: Path, value: Any) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(_canonical_json(value), encoding="utf-8", newline="\n")
    os.replace(tmp, path)


def stage_directory(out_root: Path, archive_digest: str) -> Path:
    return Path(out_root) / archive_digest[:16]


def write_companyfacts_plan(zip_path: Path, out_root: Path, *, concepts: Collection[str],
                            concepts_source: str,
                            target_uncompressed_bytes: int = DEFAULT_TARGET_UNCOMPRESSED_BYTES,
                            archive_digest: str | None = None) -> Path:
    """Write ``<out_root>/<sha16>/plan.json`` once; an existing plan must be identical (else refuse)."""
    zip_path = Path(zip_path).resolve()
    stat = zip_path.stat()
    digest = archive_digest or archive_sha256(zip_path)
    if not concepts:
        raise ValueError("the concept allowlist is empty; the legacy loader never ran unfiltered archive loads")
    with zipfile.ZipFile(zip_path) as archive:
        infos, ignored = _member_infos(archive)
    batches = plan_companyfacts_batches(zip_path, target_uncompressed_bytes)
    if zip_path.stat().st_size != stat.st_size or zip_path.stat().st_mtime_ns != stat.st_mtime_ns:
        raise ValueError("companyfacts archive changed while planning")
    plan = {
        "rule_version": RULE_VERSION,
        "archive": {
            "path": str(zip_path), "sha256": digest, "bytes": stat.st_size,
            "member_count": len(infos) + len(ignored), "cik_member_count": len(infos),
            "ignored_members": ignored, "uncompressed_bytes": sum(info.file_size for info in infos),
        },
        "allowlist": {
            "source": concepts_source, "count": len(set(concepts)),
            "allowlist_sha256": allowlist_fingerprint(concepts), "concepts": sorted(set(concepts)),
        },
        "normalization": {
            "source_name": SOURCE_NAME, "taxonomies": list(SUPPORTED_FACT_TAXONOMIES),
            "member_pattern": COMPANYFACTS_MEMBER_PATTERN.pattern,
            "placeholder": "exact two-byte {} member -> unavailable(empty_archive_placeholder)",
            "available_at": "filed_date + 22h", "end_le_filed": True,
            "source_url": SEC_COMPANY_FACTS_ZIP_URL + "#CIK{cik}.json",
            "passthrough_security_id": UNRESOLVED_COMPANYFACTS_CIK_PREFIX + "{cik}",
            "fact_columns": [list(column) for column in FACT_COLUMNS],
        },
        "planner": {
            "target_uncompressed_bytes": target_uncompressed_bytes, "row_group_rows": ROW_GROUP_ROWS,
            "parquet": PARQUET_OPTIONS, "batch_count": len(batches),
        },
        "batches": [_batch_json(batch) for batch in batches],
        "members": [{"member": info.filename, "crc32": info.CRC, "compressed_bytes": info.compress_size,
                     "uncompressed_bytes": info.file_size} for info in infos],
    }
    out_dir = stage_directory(out_root, digest)
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / PLAN_FILE
    if path.is_file():
        if path.read_text(encoding="utf-8") != _canonical_json(plan):
            raise ValueError(f"{path} exists with a different plan (allowlist, planner or archive); refusing to mix "
                             "outputs -- move the stage directory aside to re-plan")
        return path
    _write_json_atomic(path, plan)
    return path


def _batch_json(batch: CompanyFactsBatch) -> dict[str, Any]:
    return {"batch_id": batch.batch_id, "first_cik": batch.first_cik, "last_cik": batch.last_cik,
            "members": list(batch.members), "uncompressed_bytes": batch.uncompressed_bytes}


def load_companyfacts_plan(out_dir: Path) -> tuple[dict[str, Any], list[CompanyFactsBatch]]:
    plan = json.loads((Path(out_dir) / PLAN_FILE).read_text(encoding="utf-8"))
    if plan.get("rule_version") != RULE_VERSION:
        raise ValueError(f"plan rule_version {plan.get('rule_version')!r} != {RULE_VERSION!r}")
    batches = [CompanyFactsBatch(batch_id=b["batch_id"], first_cik=b["first_cik"], last_cik=b["last_cik"],
                                 members=tuple(b["members"]), uncompressed_bytes=b["uncompressed_bytes"])
               for b in plan["batches"]]
    return plan, batches


def plan_sha256(out_dir: Path) -> str:
    return hashlib.sha256((Path(out_dir) / PLAN_FILE).read_bytes()).hexdigest()


def batch_paths(out_dir: Path, batch_id: int) -> tuple[Path, Path]:
    stem = f"batch-{batch_id:04d}"
    return Path(out_dir) / f"{stem}.parquet", Path(out_dir) / f"{stem}.json"


def _file_sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def batch_receipt(out_dir: Path, batch: CompanyFactsBatch, *, plan: dict[str, Any] | None = None,
                  plan_digest: str | None = None) -> dict[str, Any] | None:
    """The batch's manifest row if the batch is complete for this plan, else ``None``.

    Complete means: the row exists, names this plan (sha256, hence archive sha256, allowlist and
    planner) and this batch's members, and the Parquet file exists with the recorded size and sha256.
    """
    parquet, row_path = batch_paths(out_dir, batch.batch_id)
    if not row_path.is_file() or not parquet.is_file():
        return None
    try:
        row = json.loads(row_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if plan is None:
        plan, _ = load_companyfacts_plan(out_dir)
    if (not isinstance(row, dict) or row.get("rule_version") != RULE_VERSION
            or row.get("plan_sha256") != (plan_digest or plan_sha256(out_dir))
            or row.get("archive_sha256") != plan["archive"]["sha256"]
            or row.get("batch_id") != batch.batch_id
            or [m.get("member") for m in row.get("members", [])] != list(batch.members)
            or parquet.stat().st_size != row.get("parquet_bytes")
            or _file_sha256(parquet) != row.get("parquet_sha256")):
        return None
    return row


def pending_companyfacts_batches(out_dir: Path) -> list[CompanyFactsBatch]:
    plan, batches = load_companyfacts_plan(out_dir)
    digest = plan_sha256(out_dir)
    return [batch for batch in batches if batch_receipt(out_dir, batch, plan=plan, plan_digest=digest) is None]


# ---------------------------------------------------------------------------
# Per-member normalization (normalize_companyfacts, pandas-free)
# ---------------------------------------------------------------------------

def _date(value: Any) -> dt.date | None:
    """``fundamentals._date``, verbatim."""
    if not value:
        return None
    return dt.date.fromisoformat(str(value))


def _validate_archive_payload_cik(payload: dict[str, Any], cik: str) -> str:
    """``fundamentals._validate_archive_payload_cik``, verbatim."""
    if "cik" not in payload:
        return "validated_archive_member_missing_payload_cik"
    raw_cik = payload["cik"]
    if (isinstance(raw_cik, bool) or not isinstance(raw_cik, (str, int))
            or re.fullmatch(r"[0-9]{1,10}", str(raw_cik)) is None):
        raise ValueError("companyfacts payload CIK is invalid")
    if f"{int(raw_cik):010d}" != cik:
        raise ValueError("payload CIK does not match archive member CIK")
    return "payload_cik_matches_archive_member"


def _observed_payload_cik(payload: dict[str, Any]) -> int | None:
    """The payload's own CIK when it is a well-formed number (``members.parquet`` ``payload_cik``)."""
    raw_cik = payload.get("cik")
    if type(raw_cik) in (str, int) and re.fullmatch(r"[0-9]{1,10}", str(raw_cik)) is not None:
        return int(raw_cik)
    return None


def _text(value: Any, field: str) -> str | None:
    if value is None or type(value) is str:
        return value
    raise CompanyFactsParityError(f"{field} is {type(value).__name__}, not a string or null")


def _number(value: Any) -> float | None:
    """``pd.to_numeric(errors='coerce')`` then the DuckDB pandas scan into DOUBLE, for JSON numbers/null."""
    kind = type(value)
    if kind is float:
        return None if math.isnan(value) else value
    if kind is int:
        if not _INT64[0] <= value <= _INT64[1]:
            raise CompanyFactsParityError(f"val {value} is outside BIGINT")
        return float(value)  # correctly rounded, as pandas' object->float64 and DuckDB's BIGINT->DOUBLE
    if value is None:
        return None
    raise CompanyFactsParityError(f"val is {kind.__name__}, not a JSON number or null")


def _fiscal_year(value: Any) -> int | None:
    if value is None:
        return None
    if type(value) is int and _INT32[0] <= value <= _INT32[1]:
        return value
    raise CompanyFactsParityError(f"fy {value!r} is not an INTEGER or null")


def _member_columns(facts: dict[str, Any], concepts: frozenset[str]) -> dict[str, list[Any]]:
    """The fact rows of ``normalize_companyfacts`` as column lists, in the loader's row order."""
    taxonomy_col: list[str] = []
    concept_col: list[str] = []
    label_col: list[str | None] = []
    description_col: list[str | None] = []
    unit_col: list[str] = []
    start_col: list[dt.date | None] = []
    end_col: list[dt.date] = []
    filed_col: list[dt.date] = []
    fy_col: list[int | None] = []
    fp_col: list[str | None] = []
    form_col: list[str | None] = []
    accn_col: list[str | None] = []
    frame_col: list[str | None] = []
    value_col: list[float | None] = []
    parsed: dict[str, dt.date] = {}

    def as_date(value: Any) -> dt.date | None:
        if type(value) is str and value:
            found = parsed.get(value)
            if found is None:
                found = parsed[value] = dt.date.fromisoformat(value)
            return found
        return _date(value)

    for taxonomy, taxonomy_facts in facts.items():
        if taxonomy not in SUPPORTED_FACT_TAXONOMIES:
            continue
        if not isinstance(taxonomy_facts, dict):
            raise ValueError(f"{taxonomy} facts must be an object")
        for concept, concept_payload in taxonomy_facts.items():
            if concepts and concept not in concepts:
                continue
            if not isinstance(concept_payload, dict) or not isinstance(concept_payload.get("units"), dict):
                raise ValueError(f"{taxonomy}:{concept} requires a units object")
            label = concept_payload.get("label")
            description = concept_payload.get("description")
            concept_start = len(end_col)
            for unit, unit_rows in concept_payload["units"].items():
                if not isinstance(unit_rows, list):
                    raise ValueError(f"{taxonomy}:{concept} unit {unit!r} requires a fact list")
                unit_start = len(end_col)
                for item in unit_rows:
                    if not isinstance(item, dict):
                        raise ValueError(f"{taxonomy}:{concept} unit {unit!r} contains a non-object fact")
                    end_date = as_date(item.get("end"))
                    filed_date = as_date(item.get("filed"))
                    if end_date is None or filed_date is None:
                        continue
                    if end_date > filed_date:
                        continue
                    start_col.append(as_date(item.get("start")))
                    end_col.append(end_date)
                    filed_col.append(filed_date)
                    fy_col.append(_fiscal_year(item.get("fy")))
                    fp_col.append(_text(item.get("fp"), "fp"))
                    form_col.append(_text(item.get("form"), "form"))
                    accn_col.append(_text(item.get("accn"), "accn"))
                    frame_col.append(_text(item.get("frame"), "frame"))
                    value_col.append(_number(item.get("val")))
                unit_rows_kept = len(end_col) - unit_start
                if unit_rows_kept:
                    unit_col.extend([unit] * unit_rows_kept)
            concept_rows = len(end_col) - concept_start
            if concept_rows:
                taxonomy_col.extend([taxonomy] * concept_rows)
                concept_col.extend([concept] * concept_rows)
                label_col.extend([_text(label, "label")] * concept_rows)
                description_col.extend([_text(description, "description")] * concept_rows)
    return {"taxonomy": taxonomy_col, "concept": concept_col, "label": label_col,
            "description": description_col, "unit": unit_col, "period_start": start_col,
            "period_end": end_col, "filed_date": filed_col, "fiscal_year": fy_col, "fiscal_period": fp_col,
            "form": form_col, "accession_number": accn_col, "frame": frame_col, "value": value_col}


def _empty_reason(facts: dict[str, Any], concepts: frozenset[str]) -> str:
    """The loader's reason for a member with zero fact rows (``SecCompanyFactsDataset.load``)."""
    supported = {key: value for key, value in facts.items() if key in SUPPORTED_FACT_TAXONOMIES}
    if not any(supported.values()):
        return "unsupported_or_empty_taxonomy"
    if concepts and not any(set(group) & concepts for group in supported.values()):
        return "allowlist_empty"
    return "no_valid_fact_rows"


def _arrow_type(name: str) -> Any:
    import pyarrow as pa

    return {"string": pa.string(), "date32": pa.date32(), "int32": pa.int32(), "int64": pa.int64(),
            "float64": pa.float64(), "timestamp_us": pa.timestamp("us")}[name]


def fact_schema() -> Any:
    import pyarrow as pa

    return pa.schema([(name, _arrow_type(kind)) for name, kind in FACT_COLUMNS])


def member_schema() -> Any:
    import pyarrow as pa

    return pa.schema([(name, _arrow_type(kind)) for name, kind in MEMBER_COLUMNS])


def _record_batch(columns: dict[str, list[Any]], cik: str, schema: Any) -> Any:
    import pyarrow as pa
    import pyarrow.compute as pc

    rows = len(columns["period_end"])
    filed = pa.array(columns["filed_date"], pa.date32())
    available_at = pc.add(filed.cast(pa.timestamp("us")), pa.scalar(AVAILABLE_AT_OFFSET, pa.duration("us")))
    constant = {
        "source": SOURCE_NAME, "security_id": f"{UNRESOLVED_COMPANYFACTS_CIK_PREFIX}{cik}", "cik": cik,
        "source_url": f"{SEC_COMPANY_FACTS_ZIP_URL}#CIK{cik}.json",
    }
    arrays = []
    for field in schema:
        if field.name in constant:
            arrays.append(pa.array([constant[field.name]] * rows, field.type))
        elif field.name == "filed_date":
            arrays.append(filed)
        elif field.name == "available_at":
            arrays.append(available_at)
        else:
            arrays.append(pa.array(columns[field.name], field.type))
    return pa.RecordBatch.from_arrays(arrays, schema=schema)


# ---------------------------------------------------------------------------
# Batch extraction
# ---------------------------------------------------------------------------

class _RowGroupWriter:
    """Writes exactly ``ROW_GROUP_ROWS``-row row groups (the last one shorter), one chunk each."""

    def __init__(self, path: Path, schema: Any) -> None:
        import pyarrow.parquet as pq

        self._schema = schema
        self._writer = pq.ParquetWriter(str(path), schema, **PARQUET_OPTIONS)
        self._pending: list[Any] = []
        self._pending_rows = 0
        self.rows = 0
        self.row_groups = 0

    def add(self, batch: Any) -> None:
        if batch.num_rows:
            self._pending.append(batch)
            self._pending_rows += batch.num_rows
            if self._pending_rows >= ROW_GROUP_ROWS:
                self._flush(final=False)

    def _flush(self, *, final: bool) -> None:
        import pyarrow as pa

        table = pa.Table.from_batches(self._pending, schema=self._schema)
        self._pending, self._pending_rows = [], 0
        full = table.num_rows if final else table.num_rows - table.num_rows % ROW_GROUP_ROWS
        offset = 0
        while offset < full:
            group = table.slice(offset, min(ROW_GROUP_ROWS, full - offset)).combine_chunks()
            self._writer.write_table(group, row_group_size=ROW_GROUP_ROWS)
            offset += group.num_rows
            self.rows += group.num_rows
            self.row_groups += 1
        if offset < table.num_rows:
            rest = table.slice(offset).combine_chunks()
            self._pending = rest.to_batches()
            self._pending_rows = rest.num_rows

    def close(self) -> None:
        if self._pending_rows:
            self._flush(final=True)
        self._writer.close()

    def abort(self) -> None:
        """Release the file without flushing; the ``.tmp`` is discarded by the next attempt."""
        self._pending, self._pending_rows = [], 0
        self._writer.close()


def _check_central_directory(archive: zipfile.ZipFile, plan: dict[str, Any], batch: CompanyFactsBatch,
                             zip_path: Path) -> dict[str, zipfile.ZipInfo]:
    if Path(zip_path).stat().st_size != plan["archive"]["bytes"]:
        raise ValueError("companyfacts archive size differs from the plan; not the pinned archive")
    planned = {m["member"]: m for m in plan["members"]}
    infos: dict[str, zipfile.ZipInfo] = {}
    for member in batch.members:
        info = archive.getinfo(member)
        expected = planned[member]
        if (info.CRC, info.compress_size, info.file_size) != (
                expected["crc32"], expected["compressed_bytes"], expected["uncompressed_bytes"]):
            raise ValueError(f"{member}: central directory differs from the plan; not the pinned archive")
        infos[member] = info
    return infos


def _extract_member(raw: bytes, cik: str, concepts: frozenset[str], schema: Any
                    ) -> tuple[dict[str, Any], Any]:
    """One member's disposition record fields and its fact RecordBatch (``None`` when no rows)."""
    if raw == b"{}":
        return {"disposition": "unavailable", "reason": PLACEHOLDER_REASON, "rows": 0, "payload_cik": None}, None
    record: dict[str, Any] = {"payload_cik": None}
    try:
        payload: object = json.loads(raw)
        if not isinstance(payload, dict):
            raise ValueError("companyfacts payload requires an object")
        facts = payload.get("facts")
        if not isinstance(facts, dict):
            raise ValueError("companyfacts payload requires a facts object")
        record["payload_cik"] = _observed_payload_cik(payload)
        _validate_archive_payload_cik(payload, cik)
        columns = _member_columns(facts, concepts)
        rows = len(columns["period_end"])
        if rows == 0:
            record.update(disposition="empty", reason=_empty_reason(facts, concepts), rows=0)
            return record, None
        batch = _record_batch(columns, cik, schema)
        del columns, facts, payload
        record.update(disposition="loaded", reason=None, rows=rows)
        return record, batch
    except MemoryError:
        raise
    except Exception as exc:  # the loader's skippable source/member/normalization failure
        record.update(disposition="error", reason=f"{type(exc).__name__}: {str(exc)[:200]}", rows=0)
        return record, None


def extract_companyfacts_batch(zip_path: Path, batch: CompanyFactsBatch, out_dir: Path) -> dict[str, int]:
    """Extract one planned batch into ``batch-NNNN.parquet`` + its manifest row; skip if already complete."""
    out_dir = Path(out_dir)
    plan, batches = load_companyfacts_plan(out_dir)
    if batch.batch_id >= len(batches) or batches[batch.batch_id] != batch:
        raise ValueError(f"batch {batch.batch_id} is not this plan's batch")
    row = batch_receipt(out_dir, batch, plan=plan)
    if row is None:
        row = _extract(zip_path, batch, out_dir, plan)
    counts = row["counts"]
    return {"batch_id": batch.batch_id, "members": len(batch.members), "rows": row["rows"],
            "row_groups": row["row_groups"], "parquet_bytes": row["parquet_bytes"],
            "uncompressed_bytes": batch.uncompressed_bytes, **{k: counts[k] for k in DISPOSITIONS}}


def _extract(zip_path: Path, batch: CompanyFactsBatch, out_dir: Path, plan: dict[str, Any]) -> dict[str, Any]:
    parquet, row_path = batch_paths(out_dir, batch.batch_id)
    tmp = parquet.with_name(parquet.name + ".tmp")
    for stale in (tmp, row_path.with_name(row_path.name + ".tmp")):
        stale.unlink(missing_ok=True)
    concepts = frozenset(plan["allowlist"]["concepts"])
    schema = fact_schema()
    members: list[dict[str, Any]] = []
    with zipfile.ZipFile(zip_path) as archive:
        infos = _check_central_directory(archive, plan, batch, zip_path)
        writer = _RowGroupWriter(tmp, schema)
        try:
            for member in batch.members:
                info = infos[member]
                cik = _cik_of(member)
                raw = archive.read(info)  # zipfile verifies the member CRC-32 on a full read
                record, member_batch = _extract_member(raw, cik, concepts, schema)
                del raw
                if member_batch is not None:
                    writer.add(member_batch)
                    del member_batch
                members.append({"cik": cik, "member": member, "crc32": info.CRC,
                                "compressed_bytes": info.compress_size, "uncompressed_bytes": info.file_size,
                                **record})
            writer.close()
        except BaseException:
            writer.abort()
            raise
    if writer.rows != sum(m["rows"] for m in members):
        raise AssertionError("written rows differ from member rows")
    os.replace(tmp, parquet)
    counts = {kind: sum(1 for m in members if m["disposition"] == kind) for kind in DISPOSITIONS}
    row = {
        "rule_version": RULE_VERSION, "plan_sha256": plan_sha256(out_dir),
        "archive_sha256": plan["archive"]["sha256"], "batch_id": batch.batch_id,
        "first_cik": batch.first_cik, "last_cik": batch.last_cik, "file": parquet.name,
        "rows": writer.rows, "row_groups": writer.row_groups, "parquet_bytes": parquet.stat().st_size,
        "parquet_sha256": _file_sha256(parquet), "uncompressed_bytes": batch.uncompressed_bytes,
        "counts": counts, "members": members,
    }
    _write_json_atomic(row_path, row)
    return row


# ---------------------------------------------------------------------------
# Assembly: manifest.json + members.parquet
# ---------------------------------------------------------------------------

def assemble_companyfacts_stage(zip_path: Path, out_dir: Path, *, verify_archive: bool = True) -> dict[str, Any]:
    """Assemble ``manifest.json`` and ``members.parquet`` from the complete batch rows.

    Refuses unless every planned batch is complete, every planned member has exactly one
    disposition and the row totals reconcile; re-hashes the archive (``verify_archive``) so the
    manifest's archive sha256 is the bytes the batches were read from.
    """
    import pyarrow as pa
    import pyarrow.parquet as pq

    out_dir = Path(out_dir)
    plan, batches = load_companyfacts_plan(out_dir)
    if verify_archive and archive_sha256(zip_path) != plan["archive"]["sha256"]:
        raise ValueError("companyfacts archive sha256 differs from the plan")
    digest = plan_sha256(out_dir)
    rows = [batch_receipt(out_dir, batch, plan=plan, plan_digest=digest) for batch in batches]
    missing = [batch.batch_id for batch, row in zip(batches, rows, strict=True) if row is None]
    if missing:
        raise ValueError(f"{len(missing)} batches incomplete: {missing[:20]}")
    members = [member for row in rows if row is not None for member in row["members"]]
    names = [m["member"] for m in members]
    planned = [m["member"] for m in plan["members"]]
    if names != planned or len(set(names)) != len(names):
        raise ValueError("assembled members differ from the planned members (order, duplicates or coverage)")
    table = pa.table({name: pa.array([m[name] for m in members], _arrow_type(kind))
                      for name, kind in MEMBER_COLUMNS}, schema=member_schema())
    members_path = out_dir / MEMBERS_FILE
    tmp = members_path.with_name(members_path.name + ".tmp")
    pq.write_table(table, str(tmp), row_group_size=ROW_GROUP_ROWS, **PARQUET_OPTIONS)
    os.replace(tmp, members_path)
    counts = {kind: sum(1 for m in members if m["disposition"] == kind) for kind in DISPOSITIONS}
    reasons: dict[str, dict[str, int]] = {}
    for m in members:
        if m["disposition"] != "loaded":
            key = m["reason"] if m["disposition"] != "error" else m["reason"].split(":", 1)[0]
            bucket = reasons.setdefault(m["disposition"], {})
            bucket[key] = bucket.get(key, 0) + 1
    manifest = {
        "rule_version": RULE_VERSION, "plan_sha256": plan_sha256(out_dir), "archive": plan["archive"],
        "allowlist": {k: plan["allowlist"][k] for k in ("source", "count", "allowlist_sha256")},
        "normalization": plan["normalization"], "planner": plan["planner"],
        "totals": {
            "batches": len(batches), "members": len(members), **counts, "reasons": reasons,
            "rows": sum(row["rows"] for row in rows if row is not None),
            "parquet_bytes": sum(row["parquet_bytes"] for row in rows if row is not None),
            "uncompressed_bytes": sum(batch.uncompressed_bytes for batch in batches),
        },
        "batches": [{key: row[key] for key in ("batch_id", "file", "first_cik", "last_cik", "rows", "row_groups",
                                               "parquet_bytes", "parquet_sha256", "uncompressed_bytes", "counts")}
                    | {"members": len(row["members"])} for row in rows if row is not None],
        "members_file": {"file": MEMBERS_FILE, "rows": len(members), "sha256": _file_sha256(members_path)},
    }
    _write_json_atomic(out_dir / MANIFEST_FILE, manifest)
    return manifest
