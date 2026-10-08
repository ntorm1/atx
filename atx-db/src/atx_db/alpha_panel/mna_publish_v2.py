"""Immutable M&A publication with independent native replay admission.

Full candidate inventory (including unqualified legacy continuations) is pinned
by an external reviewed contract. Value-only policy computes no return. A frozen
pre2023 return policy additionally requires independently pinned original
security/class/price/split/FX/settlement replay; missing proof remains NULL.
Original document/source/value coverage can be published while gates fail.
"""
from __future__ import annotations

import dataclasses
import datetime as dt
import hashlib
import json
import os
from decimal import Decimal
from pathlib import Path
from typing import Mapping
from urllib.parse import urlsplit

from . import common as C
from . import mna_financial_v2 as F
from . import mna_native_v2 as N
from . import mna_exact_v2 as E
from . import terminal_mna_v2 as T

SCHEMA = "atx.alpha-panel.mna-terminal/v2"


def _json(value) -> bytes:
    def default(item):
        if dataclasses.is_dataclass(item):
            return dataclasses.asdict(item)
        if isinstance(item, Decimal):
            return format(item.normalize(), "f")
        if isinstance(item, (dt.date, dt.datetime)):
            return str(item)
        raise TypeError(type(item).__name__)
    return json.dumps(value, default=default, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def publisher_code_pins() -> dict[str, str]:
    return {**E.exact_code_pins(), "mna_publish_v2": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "common": hashlib.sha256(Path(C.__file__).read_bytes()).hexdigest()}


def _contract(contract: bytes, expected_sha256: str) -> dict:
    if hashlib.sha256(contract).hexdigest() != expected_sha256:
        raise ValueError("independently pinned immutable admission contract mismatch")
    value = json.loads(contract)
    if set(value) != {"schema", "admission_id", "expected_code", "candidate_inventory", "source_pins",
                      "input_manifests_sha256", "candidate_source_links", "gate_handcheck",
                      "candidate_original_sha256", "source_receipts", "candidate_clock_rule", "exact_admissions", "auxiliary_clock_inventory"}:
        raise ValueError("complete exact producer admission contract schema required")
    if value["schema"] != SCHEMA or not value["admission_id"] or value["expected_code"] != publisher_code_pins():
        raise ValueError("producer schema/admission/code mismatch")
    if not value["input_manifests_sha256"]:
        raise ValueError("complete producer input-manifest pins required")
    for sha in value["input_manifests_sha256"].values():
        if not isinstance(sha, str) or len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
            raise ValueError("invalid input-manifest pin")
    # Handcheck booleans or counts cannot be trusted without an independently
    # admitted original review corpus. Its absence is explicit and always FAIL.
    review = value["gate_handcheck"]
    if review != "unmeasured" and (not isinstance(review, dict) or set(review) != {"sha256", "candidate_keys"}):
        raise ValueError("complete independently pinned native handcheck contract required")
    if value["candidate_clock_rule"] != "legacy-stage-documented-UTC":
        raise ValueError("independently pinned legacy candidate timestamp normalization required")
    return value


def _review_counts(review_pin, original: bytes | None, keys: set[str], links: dict, by_source: dict) -> tuple[int, int]:
    if review_pin == "unmeasured":
        if original is not None:
            raise ValueError("unadmitted manual review object supplied")
        return 0, 0
    if original is None or hashlib.sha256(original).hexdigest() != review_pin["sha256"]:
        raise ValueError("independent original manual review identity mismatch")
    review = json.loads(original)
    if set(review) != {"schema", "reviewer", "records"} or review["schema"] != "atx.mna.native-handcheck/v1" or not review["reviewer"]:
        raise ValueError("complete original independent manual review schema required")
    records = review["records"]
    frozen = review_pin["candidate_keys"]
    if len(frozen) != len(set(frozen)) or set(frozen) - keys or len(records) != len(frozen):
        raise ValueError("complete frozen original candidate review inventory required")
    fields = {"announce_date", "termination_date", "completion_date", "settlement_date", "target_name", "target_share_class", "acquirer_name",
              "acquirer_share_class", "consideration_type", "cash_per_share", "cash_currency", "stock_ratio", "status"}
    seen, correct = set(), 0
    for record in records:
        if set(record) != {"candidate_key", "source_id", "source_sha256", "expected_values"} or set(record["expected_values"]) != fields:
            raise ValueError("complete native review fields required; booleans/counts are not evidence")
        key = record["candidate_key"]
        if key not in frozen or key in seen:
            raise ValueError("unique exact frozen candidate review keys required")
        seen.add(key)
        source = by_source.get(record["source_id"])
        if source is None or record["source_id"] not in links.get(key, []) or source.source_sha256 != record["source_sha256"]:
            # Missing source/replay remains failed in the independent200 sample.
            continue
        values = {name: getattr(source, name) for name in fields}
        correct += _json(values) == _json(record["expected_values"])
    return len(frozen), correct


def build_rows(contract: bytes, expected_contract_sha256: str, objects: Mapping[str, bytes],
               pins: tuple[N.NativePin, ...], *, candidate_original: bytes,
               input_manifests: Mapping[str, bytes], receipts: Mapping[str, bytes], bulk_clocks=None,
               exact_inputs: Mapping[str, dict] | None = None,
               handcheck_original: bytes | None = None, clock_receipts=None) -> tuple[list[dict], list[dict], dict]:
    """Bind admission before native compute; retain complete candidate/source sets."""
    admission = _contract(contract, expected_contract_sha256)
    before = publisher_code_pins()
    if set(input_manifests) != set(admission["input_manifests_sha256"]) or any(
        hashlib.sha256(blob).hexdigest() != admission["input_manifests_sha256"][name]
        for name, blob in input_manifests.items()
    ):
        raise ValueError("complete original input-manifest replay identity required")
    if hashlib.sha256(candidate_original).hexdigest() != admission["candidate_original_sha256"]:
        raise ValueError("original complete candidate source identity mismatch")
    import pyarrow as pa
    import pyarrow.parquet as pq
    original = pq.read_table(pa.BufferReader(candidate_original)).to_pylist()
    candidate_rows = []
    for row in original:
        if row["cause"] == "mna" and row["last_session"] >= dt.date(2020, 1, 1):
            clock = row["available_at"]
            # Legacy TIMESTAMP schema is explicitly UTC-normalized by the
            # reviewed contract; do not normalize new native financial clocks.
            if clock.tzinfo is None:
                clock = clock.replace(tzinfo=T.UTC)
            candidate_rows.append(dataclasses.asdict(T.Candidate(row["security_id"], row["last_session"],
                                                                  T.utc(clock), row["cause"], row["continued"])))
    if _json(candidate_rows) != _json(admission["candidate_inventory"]):
        raise ValueError("full original2020+ M&A population replay mismatch")
    if json.loads(_json(pins)) != admission["source_pins"]:
        raise ValueError("complete independently pinned original source inventory mismatch")
    if set(receipts) != set(objects) or set(admission["source_receipts"]) != set(objects):
        raise ValueError("complete independently pinned original receipt inventory required")
    for pin in pins:
        receipt = receipts[pin.source_id]
        expected = admission["source_receipts"][pin.source_id]
        if set(expected) != {"sha256", "url"} or hashlib.sha256(receipt).hexdigest() != expected["sha256"]:
            raise ValueError("independent original receipt identity mismatch")
        record = json.loads(receipt)
        url = urlsplit(record.get("url", ""))
        prefix = f"/Archives/edgar/data/{pin.cik}/{pin.accession.replace('-', '')}/"
        if (record.get("url") != expected["url"] or url.scheme != "https" or url.hostname != "www.sec.gov"
            or not url.path.startswith(prefix) or not url.path[len(prefix):] or record.get("status") != 200
            or record.get("sha256") != pin.sha256 or record.get("bytes") != len(objects[pin.source_id])
            or record.get("error") is not None
            or T.utc(dt.datetime.fromisoformat(record["fetched_at"].replace("Z", "+00:00"))) != T.utc(pin.fetched_at)):
            raise ValueError("native original receipt/document clock/identity mismatch")
    financial = F.replay_financial(objects, pins, admission_id=admission["admission_id"],
                                   expected_code=F.financial_code_pins(), bulk_clocks=bulk_clocks,
                                   expected_auxiliary=admission["auxiliary_clock_inventory"], clock_receipts=clock_receipts)
    source_rows = [dataclasses.asdict(row) for row in financial]
    F.validate_financial_rows(source_rows, objects, pins, admission_id=admission["admission_id"],
                              expected_code=F.financial_code_pins(), bulk_clocks=bulk_clocks,
                                   expected_auxiliary=admission["auxiliary_clock_inventory"], clock_receipts=clock_receipts)
    by_source = {row.source_id: row for row in financial}
    pin_by_source = {pin.source_id: pin for pin in pins}
    exact_inputs = exact_inputs or {}
    if set(exact_inputs) != set(admission["exact_admissions"]):
        raise ValueError("complete exact-native economic/price input inventory required")
    inventory = admission["candidate_inventory"]
    names = {field.name for field in dataclasses.fields(T.Candidate)}
    outcomes, keys, consideration_count, qualified_consideration_count = [], set(), 0, 0
    links = admission["candidate_source_links"]
    for raw in inventory:
        if set(raw) != names:
            raise ValueError("complete candidate schema required")
        candidate = T.Candidate(**{**raw, "last_session": dt.date.fromisoformat(raw["last_session"]),
                                   "available_at": T.utc(dt.datetime.fromisoformat(raw["available_at"]))})
        key = f"{candidate.security_id}/{candidate.last_session}/{candidate.available_at.isoformat()}"
        if key in keys or candidate.security_id <= 0 or candidate.last_session < dt.date(2020, 1, 1) or candidate.cause != "mna":
            raise ValueError("unique complete2020+ M&A candidate keys required")
        keys.add(key)
        source_ids = links.get(key, [])
        if len(source_ids) != len(set(source_ids)) or any(source_id not in by_source for source_id in source_ids):
            raise ValueError("candidate link outside native admitted inventory")
        reasons = ["native_security_class_price_basis_admission_missing"]
        if not source_ids:
            reasons.append("missing_native_deal_source")
        if len(source_ids) > 1:
            reasons.append("ambiguous_native_deal_sources")
        if candidate.continued:
            reasons.append("legacy_continuation_unqualified")
        selected = by_source[source_ids[0]] if len(source_ids) == 1 else None
        if selected:
            reasons.extend(selected.reasons)
            consideration_count += selected.consideration_type is not None
        if candidate.last_session >= T.HOLDOUT or (selected and selected.settlement_date and selected.settlement_date >= T.HOLDOUT):
            reasons.append("return_holdout")
        clocks = [candidate.available_at] + [by_source[source_id].available_at for source_id in source_ids]
        outcome = {"security_id": candidate.security_id, "last_session": candidate.last_session,
                         "candidate_available_at": candidate.available_at,
                         "available_at": max(clocks), "legacy_continued": candidate.continued,
                         "deal_id": selected.accession if selected else None,
                         "currency": selected.cash_currency if selected else None,
                         "settlement_date": selected.settlement_date if selected else None,
                         "source_ids": tuple(source_ids), "proceeds_per_share": None, "dlret": None,
                         "dlret_policy": admission["exact_admissions"].get(key, {}).get("return_policy", "value-only"),
                         "is_forward_label": False,
                         "dlret_exact": False, "dlret_imputed": False, "reason": tuple(sorted(set(reasons)))}
        if key in exact_inputs:
            if selected is None:
                raise ValueError("exact-native admission requires one independently linked native source")
            try:
                selected_clock = (bulk_clocks or {}).get(selected.source_id)
                selected_receipts = {}
                if selected_clock:
                    auxiliary_ids = {selected_clock[1].source_id, selected_clock[2][1].source_id}
                    selected_receipts = {source_id: clock_receipts[source_id] for source_id in auxiliary_ids}
                qualified = E.replay_exact(candidate, objects[selected.source_id], pin_by_source[selected.source_id],
                                            expected=admission["exact_admissions"][key],
                                            bulk_clock=selected_clock, clock_receipts=selected_receipts, **exact_inputs[key])
                outcome.update(available_at=qualified.available_at, source_ids=qualified.source_ids,
                               deal_id=qualified.deal_id, currency=qualified.currency, settlement_date=qualified.settlement_date,
                               proceeds_per_share=float(qualified.proceeds_per_share) if qualified.proceeds_per_share is not None else None,
                               dlret=float(qualified.dlret) if qualified.dlret is not None else None,
                               dlret_exact=qualified.dlret_exact, reason=qualified.reason)
                qualified_consideration_count += qualified.proceeds_per_share is not None
            except ValueError as exc:
                outcome["reason"] = tuple(sorted(set(reasons + ["native_exact_admission_failed:" + str(exc)])))
        outcomes.append(outcome)
    if set(links) - keys or set(exact_inputs) - keys:
        raise ValueError("links contain candidates outside complete inventory")
    if publisher_code_pins() != before:
        raise ValueError("producer/readers changed during computation")
    checked, correct = _review_counts(admission["gate_handcheck"], handcheck_original, keys, links, by_source)
    gates = T.gates(candidates=len(outcomes), exact=sum(row["dlret_exact"] for row in outcomes), deals=len(outcomes),
                    consideration=qualified_consideration_count, checked=checked, correct=correct)
    gates["consideration"]["source_only_parsed"] = consideration_count
    gates["consideration"]["qualified_consideration"] = qualified_consideration_count
    return source_rows, outcomes, gates


def publish_value_only(output_root: Path, contract: bytes, expected_contract_sha256: str,
                       objects: Mapping[str, bytes], pins: tuple[N.NativePin, ...], *, candidate_original: bytes,
                       input_manifests: Mapping[str, bytes], receipts: Mapping[str, bytes], bulk_clocks=None,
                       exact_inputs: Mapping[str, dict] | None = None, handcheck_original: bytes | None = None, clock_receipts=None) -> Path:
    """Immutable build, publish-last manifest, no existing vintage overwrite."""
    import pyarrow as pa
    source_rows, outcomes, gates = build_rows(contract, expected_contract_sha256, objects, pins,
                                             candidate_original=candidate_original, input_manifests=input_manifests,
                                             receipts=receipts, bulk_clocks=bulk_clocks, exact_inputs=exact_inputs,
                                             handcheck_original=handcheck_original, clock_receipts=clock_receipts)
    admission = _contract(contract, expected_contract_sha256)
    build_id = expected_contract_sha256
    output_root.mkdir(parents=True, exist_ok=True)
    final = output_root / build_id
    if final.exists():
        validate_publication(final, contract, expected_contract_sha256, objects, pins,
                             candidate_original=candidate_original, input_manifests=input_manifests, receipts=receipts,
                             bulk_clocks=bulk_clocks, exact_inputs=exact_inputs, handcheck_original=handcheck_original, clock_receipts=clock_receipts)
        return final
    pending = output_root / (build_id + ".partial")
    if pending.exists():
        # Recover only a fully written and independently replay-valid pending
        # build. Incomplete evidence is retained; never overwrite or guess.
        validate_publication(pending, contract, expected_contract_sha256, objects, pins,
                             candidate_original=candidate_original, input_manifests=input_manifests,
                             receipts=receipts, bulk_clocks=bulk_clocks, exact_inputs=exact_inputs, handcheck_original=handcheck_original, clock_receipts=clock_receipts)
        os.rename(pending, final)
        return final
    pending.mkdir(exist_ok=False)
    con = C.connect(memory="128MB", threads=1, temp_dir=pending / "spill")
    try:
        # Explicit schemas preserve null types and arrays even for empty tables.
        schemas = publication_schemas()
        source_schema, terminal_schema = schemas["mna"], schemas["events"]
        for name, rows, schema in (("mna", source_rows, source_schema), ("events", outcomes, terminal_schema)):
            con.register("native_rows", pa.Table.from_pylist(rows, schema=schema))
            C.copy_to_parquet(con, "SELECT * FROM native_rows", pending / f"{name}.parquet", row_group_size=32768)
            con.unregister("native_rows")
    finally:
        con.close()
    # Validate entire output against original native objects before publication.
    manifest = {"schema": SCHEMA, "status": "complete_gates_reported", "build_id": build_id,
                "admission_contract_sha256": expected_contract_sha256, "code": publisher_code_pins(),
                "input_manifests_sha256": admission["input_manifests_sha256"], "gate_status": gates,
                "rows": {"mna": len(source_rows), "events": len(outcomes)},
                "keys": {"mna": ["accession", "source_id", "available_at"],
                         "events": ["security_id", "last_session", "candidate_available_at"]},
                "table_schemas": {"mna": F.SCHEMA, "events": T.SCHEMA},
                "files": {name: hashlib.sha256((pending / name).read_bytes()).hexdigest() for name in ("mna.parquet", "events.parquet")},
                "return_computation": "frozen-native-policy" if admission["exact_admissions"] else "none",
                "exact_admissions": admission["exact_admissions"],
                "native_financial_price_admission": "per-key-native-replay" if admission["exact_admissions"] else "incomplete"}
    C.write_json_atomic(pending / "manifest.json", manifest)
    validate_publication(pending, contract, expected_contract_sha256, objects, pins,
                         candidate_original=candidate_original, input_manifests=input_manifests, receipts=receipts,
                         bulk_clocks=bulk_clocks, exact_inputs=exact_inputs, handcheck_original=handcheck_original, clock_receipts=clock_receipts)
    os.rename(pending, final)
    return final


def validate_publication(directory: Path, contract: bytes, expected_contract_sha256: str,
                         objects: Mapping[str, bytes], pins: tuple[N.NativePin, ...], *, candidate_original: bytes,
                         input_manifests: Mapping[str, bytes], receipts: Mapping[str, bytes], bulk_clocks=None,
                         exact_inputs: Mapping[str, dict] | None = None, handcheck_original: bytes | None = None, clock_receipts=None) -> None:
    """Public read-only full-file/native replay; hashes alone cannot certify rows."""
    import pyarrow.parquet as pq
    code_before = publisher_code_pins()
    manifest_bytes = (directory / "manifest.json").read_bytes()
    identities_before = {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                         for name in ("mna.parquet", "events.parquet")}
    expected_source, expected_terminal, gates = build_rows(contract, expected_contract_sha256, objects, pins,
                                                          candidate_original=candidate_original,
                                                          input_manifests=input_manifests, receipts=receipts,
                                                          bulk_clocks=bulk_clocks, exact_inputs=exact_inputs,
                                                          handcheck_original=handcheck_original, clock_receipts=clock_receipts)
    manifest = json.loads(manifest_bytes)
    admission = _contract(contract, expected_contract_sha256)
    if set(manifest) != {"schema", "status", "build_id", "admission_contract_sha256", "code",
                         "input_manifests_sha256", "gate_status", "rows", "keys", "table_schemas", "files",
                         "return_computation", "exact_admissions", "native_financial_price_admission"}:
        raise ValueError("complete exact public manifest schema required")
    if (manifest.get("schema") != SCHEMA or manifest.get("admission_contract_sha256") != expected_contract_sha256
        or manifest.get("status") != "complete_gates_reported" or manifest.get("build_id") != expected_contract_sha256
        or set(manifest.get("files", {})) != {"mna.parquet", "events.parquet"}
        or set(manifest.get("rows", {})) != {"mna", "events"}
        or set(manifest.get("keys", {})) != {"mna", "events"}
        or manifest.get("table_schemas") != {"mna": F.SCHEMA, "events": T.SCHEMA}
        or manifest.get("code") != publisher_code_pins() or manifest.get("gate_status") != gates
        or manifest.get("input_manifests_sha256") != admission["input_manifests_sha256"]
        or manifest.get("exact_admissions") != admission["exact_admissions"]
        or manifest.get("return_computation") != ("frozen-native-policy" if admission["exact_admissions"] else "none")):
        raise ValueError("complete public manifest/admission mismatch")
    for name, expected in (("mna", expected_source), ("events", expected_terminal)):
        path = directory / f"{name}.parquet"
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest["files"].get(path.name):
            raise ValueError("public output identity mismatch")
        expected_schema = publication_schemas()[name]
        physical = pq.read_schema(path)
        if not physical.equals(expected_schema, check_metadata=False):
            raise ValueError("exact physical Arrow/Parquet schema required")
        table = pq.read_table(path)
        if not table.schema.equals(expected_schema, check_metadata=False):
            raise ValueError("parsed table physical schema changed during admission")
        actual = table.to_pylist()
        expected_keys = ["accession", "source_id", "available_at"] if name == "mna" else ["security_id", "last_session", "candidate_available_at"]
        if manifest["keys"].get(name) != expected_keys:
            raise ValueError("complete immutable output key contract required")
        row_keys = [tuple(row[column] for column in expected_keys) for row in actual]
        if any(any(item is None for item in key) for key in row_keys) or len(set(row_keys)) != len(row_keys):
            raise ValueError("nonnull unique public output keys required")
        # Compare complete columns and typed values, including arrays/NULLs,
        # using canonical serialization; no selected passing projection.
        if _json(actual) != _json(expected) or manifest["rows"].get(name) != len(expected):
            raise ValueError("full public schema/key/native output replay mismatch")
    if (publisher_code_pins() != code_before or (directory / "manifest.json").read_bytes() != manifest_bytes
        or any(hashlib.sha256((directory / name).read_bytes()).hexdigest() != sha for name, sha in identities_before.items())):
        raise ValueError("public product/manifest/runtime identity changed during admission")


def publication_schemas():
    import pyarrow as pa
    source_schema = pa.schema([(name, typ) for name, typ in (
        ("accession", pa.string()), ("source_id", pa.string()), ("source_sha256", pa.string()),
        ("available_at", pa.timestamp("us", tz="UTC")), ("completion_date", pa.date32()),
        ("announce_date", pa.date32()),
        ("termination_date", pa.date32()),
        ("settlement_date", pa.date32()), ("target_name", pa.string()), ("target_share_class", pa.string()),
        ("acquirer_name", pa.string()), ("acquirer_share_class", pa.string()), ("consideration_type", pa.string()),
        ("cash_per_share", pa.decimal128(30, 12)), ("cash_currency", pa.string()), ("stock_ratio", pa.decimal128(30, 12)),
        ("status", pa.string()), ("consideration_clauses", pa.list_(pa.string())), ("reasons", pa.list_(pa.string())))])
    terminal_schema = pa.schema([(name, typ) for name, typ in (
        ("security_id", pa.int64()), ("last_session", pa.date32()),
        ("candidate_available_at", pa.timestamp("us", tz="UTC")), ("available_at", pa.timestamp("us", tz="UTC")),
        ("legacy_continued", pa.bool_()), ("source_ids", pa.list_(pa.string())),
        ("deal_id", pa.string()), ("currency", pa.string()), ("settlement_date", pa.date32()),
        ("proceeds_per_share", pa.float64()), ("dlret", pa.float64()),
        ("dlret_exact", pa.bool_()), ("dlret_imputed", pa.bool_()), ("reason", pa.list_(pa.string())))])
    terminal_schema = terminal_schema.append(pa.field("dlret_policy", pa.string())).append(pa.field("is_forward_label", pa.bool_()))
    return {"mna": source_schema, "events": terminal_schema}
