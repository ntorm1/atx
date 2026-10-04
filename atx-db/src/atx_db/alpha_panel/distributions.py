"""Source-native cash declarations, distinct from realized payments and vendor factors.

The acquisition-free adapter takes JSONL document records with explicit security linkage,
UTC acceptance, and a retained native document. No issuer-to-class expansion is performed.
Only a sentence explicitly declaring a regular cash dividend and its cadence supports an
indicated rate. Duration XBRL facts remain historical declared amounts without annualization.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import re
from pathlib import Path
from typing import Any

from . import common as C

RULE = "native-distributions-v1"
SCHEMA = "atx.alpha-panel.distributions/v1"
UTC = dt.timezone.utc
CADENCE = {"monthly": 12, "quarterly": 4, "semiannual": 2, "semi-annual": 2, "annual": 1}
MONTHS = {m.lower(): i for i, m in enumerate(
    ("January", "February", "March", "April", "May", "June", "July", "August", "September",
     "October", "November", "December"), 1)}
DATE = r"(?:January|February|March|April|May|June|July|August|September|October|November|December)\s+\d{1,2},?\s+20\d{2}"


def clock(value: str) -> dt.datetime:
    result = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if result.tzinfo is None:
        raise ValueError("acceptance must have an explicit UTC offset")
    return result.astimezone(UTC)


def _date(value: str) -> dt.date:
    month, day, year = value.replace(",", "").split()
    return dt.date(int(year), MONTHS[month.lower()], int(day))


def extract_declarations(text: str) -> list[dict[str, Any]]:
    """Conservative sentence-local extraction; unknown fields remain NULL.

    This deliberately requires explicit USD, a per-share amount, an authorizing act,
    and common-stock class. A bare dollar sign does not establish currency.
    """
    out = []
    for sentence in re.split(r"(?<=[.!?])\s+(?=[A-Z])|\n+", text):
        if not re.search(r"\b(?:declared|declares|approved)\b", sentence, re.I):
            continue
        if not re.search(r"\bcash\s+dividend\b", sentence, re.I):
            continue
        amount = re.search(r"(?:US\$|USD\s*|U\.S\.\s+dollars?\s*)\s*(\d+(?:\.\d+)?)\s*(?:per\s+(?:common\s+)?share|a\s+share)", sentence, re.I)
        klass = re.search(r"\b(?:Class\s+[A-Z]\s+)?common\s+(?:stock|shares)\b", sentence, re.I)
        if not amount or not klass:
            continue
        special = True if re.search(r"\b(?:special|one-time|extraordinary)\b", sentence, re.I) else (
            False if re.search(r"\bregular\b", sentence, re.I) else None)
        cadence_match = re.search(r"\b(monthly|quarterly|semiannual|semi-annual|annual)\b", sentence, re.I)
        cadence = cadence_match.group(1).lower() if cadence_match else None
        dates = {}
        for name, cue in (("declare_date", r"(?:declared|declares|approved)\s+on"), ("record_date", r"(?:record\s+(?:on|as\s+of)|record\s+date(?:\s+of)?)"),
                          ("pay_date", r"(?:payable\s+(?:on)?|paid\s+on)")):
            match = re.search(cue + r"\s*(" + DATE + r")", sentence, re.I)
            dates[name] = _date(match.group(1)) if match else None
        value = float(amount.group(1))
        if not math.isfinite(value) or value < 0:
            continue
        out.append({**dates, "amount": value, "currency": "USD", "security_class": klass.group(0),
                    "special_flag": special, "declared_regular_rate": value if special is False else None,
                    "declared_cadence": cadence, "spin_off_ratio": None, "kind": "cash_dividend",
                    "indicated_annual_dividend_rate": value * CADENCE[cadence] if special is False and cadence else None,
                    "source_concept": None, "evidence": sentence.strip()})
    return out


def xbrl_declared(fact: dict[str, Any]) -> dict[str, Any]:
    """Retain exact native per-share declared fact; a duration total is not a rate."""
    if fact["concept"] != "CommonStockDividendsPerShareDeclared":
        raise ValueError("unsupported declared-dividend concept")
    value = float(fact["value"])
    if not math.isfinite(value) or value < 0:
        raise ValueError("invalid declared amount")
    unit = fact["unit"]
    if not re.fullmatch(r"[A-Z]{3}/shares", unit):
        raise ValueError("per-share money unit required")
    return {"kind": "historical_declared_xbrl", "amount": value, "currency": unit[:3],
            "security_class": fact.get("security_class"), "declare_date": None, "record_date": None,
            "pay_date": None, "special_flag": None, "declared_regular_rate": None,
            "declared_cadence": None, "indicated_annual_dividend_rate": None, "spin_off_ratio": None,
            "period_start": fact["start"], "period_end": fact["end"], "source_concept": fact["concept"],
            "evidence": json.dumps(fact, sort_keys=True)}


def extract_spin_offs(text: str) -> list[dict[str, Any]]:
    """Explicit distributed shares per parent share; do not infer cash or cadence."""
    result = []
    rx = re.compile(r"(?:distribute|distribution\s+of)\s+(\d+(?:\.\d+)?)\s+shares?\s+of\s+([A-Za-z][\w .-]{0,60}?)\s+for\s+(?:each|every)\s+(\d+(?:\.\d+)?)\s+shares?\s+of\s+((?:Class\s+[A-Z]\s+)?common\s+(?:stock|shares))", re.I)
    for match in rx.finditer(text):
        numerator, denominator = float(match[1]), float(match[3])
        if denominator <= 0 or not all(math.isfinite(v) for v in (numerator, denominator)):
            continue
        result.append({"kind": "spin_off", "amount": None, "currency": None, "security_class": match[4],
                       "declare_date": None, "record_date": None, "pay_date": None, "special_flag": None,
                       "declared_regular_rate": None, "declared_cadence": None, "indicated_annual_dividend_rate": None,
                       "spin_off_ratio": numerator / denominator, "distributed_security": match[2],
                       "source_concept": None, "evidence": match[0]})
    return result


def visible(row: dict[str, Any], session: dt.date, *, monthly: bool = False) -> bool:
    cutoff = dt.datetime.combine(session if monthly else session - dt.timedelta(days=1), dt.time(22), UTC)
    return row["available_at"] < cutoff


def gates(cash_rows: list[dict[str, Any]] | None = None,
          handchecks: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    """Full member-line cash denominator; absent or incomplete evidence fails closed."""
    rows = cash_rows or []
    declared = sum(bool(r.get("declare_date")) for r in rows)
    agrees = sum(r.get("source_amount") is not None and r.get("vendor_cash") is not None
                 and r["vendor_cash"] > 0 and abs(r["source_amount"] / r["vendor_cash"] - 1) <= .01 + 1e-12 for r in rows)
    checked = handchecks or []
    keys = {(r["accession"], r["event_id"]) for r in checked}
    matched = sum(r.get("matches") is True and bool(r.get("native_source_sha256")) and bool(r.get("evidence")) for r in checked)
    def result(n: int, d: int, threshold: float, complete: bool = True) -> dict[str, Any]:
        return {"status": "PASS" if d and complete and n / d >= threshold else "FAIL",
                "numerator": n, "denominator": d, "fraction": n / d if d else None, "threshold": threshold}
    return {"declare_date": result(declared, len(rows), .90), "vendor_cash_within_1pct": result(agrees, len(rows), .95),
            "buyback_handcheck": result(matched, len(checked), .95, len(keys) == len(checked) == 200)}


def build(documents: Path, input_manifests: list[Path]) -> dict[str, Any]:
    """Replay retained native documents; no network. Pin input bytes before and after.

    Inputs are UTF-8 text or a JSON document containing a ``facts`` list. Linkage is
    supplied explicitly per document and preserved without claiming class parity.
    """
    import pyarrow as pa
    import pyarrow.parquet as pq
    pinned = {str(p.resolve()): C.sha256_file(p) for p in [documents, *input_manifests]}
    rows = []
    with documents.open(encoding="utf-8") as stream:
        for line in stream:
            if len(line) > 65536 or len(rows) >= 100000:
                raise ValueError("bounded adapter limit exceeded; partition input catalog")
            doc = json.loads(line)
            source = Path(doc["source_path"])
            if source.stat().st_size > 2 * 1024 * 1024:
                raise ValueError("document exceeds 2MiB bounded replay limit")
            digest = C.sha256_file(source)
            if digest != doc["source_sha256"]:
                raise ValueError("native document hash mismatch")
            pinned[str(source.resolve())] = digest
            available = clock(doc["available_at"])
            if not doc.get("accession") or not doc.get("source_url") or not doc.get("link_basis"):
                raise ValueError("accession, source URL and explicit link basis required")
            text = source.read_text(encoding="utf-8")
            events = [xbrl_declared(f) for f in json.loads(text)["facts"] if f["concept"] == "CommonStockDividendsPerShareDeclared"] if doc.get("format") == "xbrl_json" else extract_declarations(text) + extract_spin_offs(text)
            for ordinal, event in enumerate(events):
                if event["declare_date"] and event["declare_date"] > available.date():
                    raise ValueError("future declaration cannot be available earlier")
                class_basis = bool(doc.get("security_id") is not None and doc.get("security_class") == event["security_class"]
                                   and doc.get("currency") == event["currency"])
                if not class_basis:
                    event["indicated_annual_dividend_rate"] = None
                rows.append({**event, "event_id": f'{doc["accession"]}:{digest}:{ordinal}',
                             "security_id": doc.get("security_id"), "cik": doc["cik"], "accession": doc["accession"],
                             "source_url": doc["source_url"], "source_doc": str(source), "source_sha256": digest,
                             "link_basis": doc["link_basis"], "link_tier": doc.get("link_tier"),
                             "available_at": available, "dividend_announcement_available_at": available,
                             "security_class_currency_basis": f'{event["security_class"]}/{event["currency"]}' if class_basis else None,
                             "vintage_risk": doc.get("vintage_risk", True), "extractor_version": RULE})
    # Explicit schema makes an empty publication useful and keeps unknowns typed.
    strings = ["event_id", "accession", "source_url", "source_doc", "source_sha256", "link_basis", "link_tier", "kind",
               "currency", "security_class", "declared_cadence", "source_concept", "evidence", "extractor_version",
               "security_class_currency_basis", "period_start", "period_end", "distributed_security"]
    schema = pa.schema([(k, pa.string()) for k in strings] + [(k, pa.int64()) for k in ("security_id", "cik")] +
                       [(k, pa.date32()) for k in ("declare_date", "record_date", "pay_date")] +
                       [(k, pa.float64()) for k in ("amount", "declared_regular_rate", "indicated_annual_dividend_rate", "spin_off_ratio")] +
                       [(k, pa.bool_()) for k in ("special_flag", "vintage_risk")] +
                       [(k, pa.timestamp("us", tz="UTC")) for k in ("available_at", "dividend_announcement_available_at")])
    code = C.code_identity("distributions", "common")
    identity = hashlib.sha256(json.dumps({"inputs": pinned, "code": {k: v for k, v in code.items() if k != "git_head"}}, sort_keys=True).encode()).hexdigest()
    out = C.stage_dir("corporate_actions") / "distributions_products" / identity
    if out.exists():
        raise FileExistsError("immutable product already exists")
    out.mkdir(parents=True)
    for name, digest in pinned.items():
        if C.sha256_file(Path(name)) != digest:
            raise ValueError("input changed during replay")
    target = out / "distributions.parquet"
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), target.with_suffix(".partial"), compression="zstd")
    target.with_suffix(".partial").replace(target)
    manifest = {"schema": SCHEMA, "rows": len(rows), "product_id": identity, "gate_status": gates(),
                "input_manifests_sha256": {str(p): pinned[str(p.resolve())] for p in input_manifests},
                "native_inputs_sha256": pinned, "files": {"distributions.parquet": C.sha256_file(target)},
                "code": C.code_identity("distributions", "common"), "status": "complete"}
    C.write_json_atomic(out / "manifest.json", manifest)
    # Compatibility path is a replaceable view of a retained immutable product.
    import shutil
    alias = C.stage_dir("corporate_actions") / "distributions.parquet"
    shutil.copyfile(target, alias.with_suffix(".partial"))
    alias.with_suffix(".partial").replace(alias)
    # Explicit pointer, no mutable replacement of a historical vintage product.
    C.write_json_atomic(C.stage_dir("corporate_actions") / "distributions.latest.json", {"manifest": str(out / "manifest.json"), "sha256": C.sha256_file(out / "manifest.json")})
    return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--documents", type=Path, required=True)
    ap.add_argument("--input-manifest", type=Path, action="append", required=True)
    args = ap.parse_args()
    print(json.dumps(build(args.documents, args.input_manifest), default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
