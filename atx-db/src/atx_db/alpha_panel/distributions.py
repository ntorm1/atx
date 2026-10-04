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

RULE = "native-distributions-v3"
SCHEMA = "atx.alpha-panel.distributions/v3"
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
        if re.search(r"\b(?:not|no|never)\b.{0,40}\b(?:declare|pay|approve)|\b(?:variable|policy|historical|previously|undetermined)\b|\b(?:to\s+be|not\s+yet)\s+determined\b", sentence, re.I):
            continue
        amounts = list(re.finditer(r"(?:US\$|USD\s*|U\.S\.\s+dollars?\s*)\s*(\d+(?:\.\d+)?)", sentence, re.I))
        classes = list(re.finditer(r"\b(?:Class\s+[A-Z]\s+)?common\s+(?:stock|shares)\b", sentence, re.I))
        if not amounts or len(classes) != 1:
            continue
        def per_share(match: re.Match) -> bool:
            return bool(re.match(r"\s*(?:per\s+(?:common\s+)?share|a\s+share)\b", sentence[match.end():], re.I))
        klass = classes[0]
        if len(amounts) == 1 and per_share(amounts[0]):
            amount = amounts[0]
        elif len(amounts) == 2 and per_share(amounts[1]) and re.search(r"\b(?:increase\w*|raise\w*)\b", sentence, re.I) and re.search(r"\bfrom\s*$", sentence[:amounts[0].start()], re.I) and re.fullmatch(r"\s*(?:(?:per\s+(?:common\s+)?share|a\s+share)\s*)?to\s*", sentence[amounts[0].end():amounts[1].start()], re.I):
            amount = amounts[1]
        else:
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


def visible(row: dict[str, Any], session: dt.date, *, sessions: list[dt.date], monthly: bool = False) -> bool:
    if sessions != sorted(set(sessions)) or session not in sessions:
        raise ValueError("sorted unique admitted exchange sessions required")
    index = sessions.index(session)
    if monthly:
        if index + 1 == len(sessions) or sessions[index + 1].month == session.month:
            raise ValueError("month-end session requires following-session calendar proof")
        day = session
    else:
        if index == 0:
            raise ValueError("prior exchange session missing")
        day = sessions[index - 1]
    cutoff = dt.datetime.combine(day, dt.time(22), UTC)
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


def replay_rows(admission_path: Path, expected_sha256: str) -> tuple[list[dict[str, Any]], dict[str, str], dict[str, Any]]:
    from . import distributions_native as N
    from . import distributions_ancillary as A
    originals, pinned, admitted = N.replay_inputs(admission_path, expected_sha256)
    rows = []
    diagnostics = []
    for doc, blob, text in originals:
        digest = hashlib.sha256(blob).hexdigest()
        source = Path(doc["source_path"])
        # Catalog clock/class assertions are not semantic source certification.
        # Preserve a conservative fetched-at floor until original acceptance evidence is certified.
        available = clock(doc["fetched_at"])
        if not doc.get("accession") or not doc.get("source_url") or not doc.get("link_basis"):
            raise ValueError("accession, source URL and explicit link basis required")
        events = [xbrl_declared(f) for f in json.loads(blob)["facts"] if f["concept"] == "CommonStockDividendsPerShareDeclared"] if doc.get("format") == "xbrl_json" else extract_declarations(text) + extract_spin_offs(text)
        diagnostics.append({"accession": doc["accession"], "source_sha256": digest, "parsed_events": len(events),
            "status": "parsed_source_semantics_unqualified" if events else "no_supported_declaration",
            "class_clock": "UNQUALIFIED"})
        for ordinal, event in enumerate(events):
            if event["declare_date"] and event["declare_date"] > available.date():
                raise ValueError("future declaration cannot be available earlier")
            event["indicated_annual_dividend_rate"] = None
            qualification = A.qualify(event, text, doc, admitted, pinned) if event["kind"] == "cash_dividend" else {"reason": "not_fixed_cash_declaration"}
            qualified = qualification.get("qualified", {})
            if qualified:
                event = qualified
            row_available = qualified.get("available_at", available)
            rows.append({**event, "event_id": f'{doc["accession"]}:{digest}:{ordinal}',
                "security_id": qualified.get("security_id", doc.get("security_id")), "cik": doc["cik"], "accession": doc["accession"],
                "source_url": doc["source_url"], "source_doc": str(source), "source_sha256": digest,
                "stored_sha256": doc["stored_sha256"], "normalized_sha256": hashlib.sha256(text.encode()).hexdigest(),
                "link_basis": "native_class_dated_symbol" if qualified else "unqualified", "link_tier": "native_verified" if qualified else "unqualified", "available_at": row_available,
                "dividend_announcement_available_at": qualified.get("dividend_announcement_available_at", available),
                "security_class_currency_basis": qualified.get("security_class_currency_basis"),
                "source_qualification": qualification["reason"],
                "quote_currency_qualified": False, "rate_carry_status": qualified.get("rate_carry_status", "not_established"),
                "source_valid_until": qualified.get("source_valid_until"),
                "vintage_risk": True, "extractor_version": RULE})
    N.validate_keys(rows, ["event_id"])
    for diagnostic in diagnostics:
        emitted = [row for row in rows if row["accession"] == diagnostic["accession"] and row["source_sha256"] == diagnostic["source_sha256"]]
        diagnostic["qualified_fixed_regular_events"] = sum(row["source_qualification"] == "native_fixed_regular_event" for row in emitted)
        diagnostic["qualification_reasons"] = sorted({row["source_qualification"] for row in emitted})
        diagnostic["class_clock"] = "CONDITIONALLY_VERIFIED" if diagnostic["qualified_fixed_regular_events"] else "UNQUALIFIED"
        if diagnostic["qualified_fixed_regular_events"]:
            diagnostic["status"] = "parsed_conditional_native_semantics"
    N.recheck(pinned)
    admitted["diagnostics"] = diagnostics
    return rows, pinned, admitted


def output_schema():
    import pyarrow as pa
    strings = ["event_id", "accession", "source_url", "source_doc", "source_sha256", "link_basis", "link_tier", "kind",
               "currency", "security_class", "declared_cadence", "source_concept", "evidence", "extractor_version",
               "security_class_currency_basis", "period_start", "period_end", "distributed_security",
               "stored_sha256", "normalized_sha256", "source_qualification", "rate_carry_status", "class_context"]
    return pa.schema([(k, pa.string()) for k in strings] + [(k, pa.int64()) for k in ("security_id", "cik")] +
                       [(k, pa.date32()) for k in ("declare_date", "record_date", "pay_date")] +
                       [(k, pa.float64()) for k in ("amount", "declared_regular_rate", "indicated_annual_dividend_rate", "spin_off_ratio")] +
                       [(k, pa.bool_()) for k in ("special_flag", "vintage_risk", "quote_currency_qualified")] +
                       [(k, pa.timestamp("us", tz="UTC")) for k in ("available_at", "dividend_announcement_available_at", "source_valid_until")])


def validate_product(target: Path, admission_path: Path, expected_sha256: str) -> dict[str, Any]:
    """Public read-only admission: exact schema, unique keys and every native-replayed row."""
    import pyarrow as pa
    import pyarrow.parquet as pq
    from . import distributions_native as N
    before = C.sha256_file(target)
    expected, pins, admitted = replay_rows(admission_path, expected_sha256)
    actual = pq.read_table(target)
    N.validate_keys(actual.to_pylist(), ["event_id"])
    if actual.schema != output_schema() or not actual.equals(pa.Table.from_pylist(expected, schema=output_schema())):
        raise ValueError("distribution schema/full native replay mismatch")
    N.recheck(pins)
    if before != C.sha256_file(target):
        raise ValueError("product changed during admission")
    return {"schema": SCHEMA, "rows": len(expected), "native_catalog_rows": admitted["population"]["catalog_rows"],
            "native_replay": "PASS", "conditionally_qualified_events": sum(row["source_qualification"] == "native_fixed_regular_event" for row in expected),
            "quote_currency_qualified": False, "actual_task_gates": "FAIL_UNMEASURED", "sha256": before}


def build(documents: Path, input_manifests: list[Path], *, admission_path: Path | None = None,
          expected_sha256: str | None = None) -> dict[str, Any]:
    """Replay retained native documents; no network. Pin input bytes before and after.

    Inputs are UTF-8 text or a JSON document containing a ``facts`` list. Linkage is
    supplied explicitly per document and preserved without claiming class parity.
    """
    import pyarrow as pa
    import pyarrow.parquet as pq
    if admission_path is None or expected_sha256 is None:
        raise ValueError("independent complete native admission required")
    rows, pinned, admitted = replay_rows(admission_path, expected_sha256)
    if documents.resolve() != Path(admitted["inputs"]["catalog"]["path"]).resolve():
        raise ValueError("catalog not independently admitted")
    if any(str(p.resolve()) not in pinned for p in input_manifests):
        raise ValueError("manifest not independently admitted")
    # Explicit schema makes an empty publication useful and keeps unknowns typed.
    schema = output_schema()
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
    replay_proof = validate_product(target, admission_path, expected_sha256)
    manifest = {"schema": SCHEMA, "rows": len(rows), "product_id": identity, "gate_status": gates(),
                "input_manifests_sha256": {str(p): pinned[str(p.resolve())] for p in input_manifests},
                "native_inputs_sha256": pinned, "files": {"distributions.parquet": C.sha256_file(target)},
                "admission_sha256": expected_sha256, "admission": replay_proof,
                "population": admitted["population"], "population_diagnostics": admitted["diagnostics"],
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
    ap.add_argument("--admission", type=Path, required=True)
    ap.add_argument("--admission-sha256", required=True)
    ap.add_argument("--validate-product", type=Path)
    args = ap.parse_args()
    result = validate_product(args.validate_product, args.admission, args.admission_sha256) if args.validate_product else build(args.documents, args.input_manifest, admission_path=args.admission, expected_sha256=args.admission_sha256)
    print(json.dumps(result, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
