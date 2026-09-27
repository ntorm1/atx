"""Permanent line identities and clocked company links (0331, part 1).

The retained-input rehearsal is independent of the production warehouse. It
keeps the original archive receipt clocks and labels reconstruction explicitly.
Positive vendor ids are the permanent security ids in allocation version 1;
the natural line key is always TBLTICKERHISTORY-<vendor id>, never a ticker.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path

import duckdb

PREPARATION_VERSION = "identity_retained_inputs_v1"
ALLOCATION_VERSION = "positive_vendor_id_v1_cik_company_v1"


def file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _literal(value: object) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def _write_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _connect(path: Path) -> duckdb.DuckDBPyConnection:
    spill = path.with_name(path.name + ".spill")
    spill.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect(str(path), config={"memory_limit": "256MB", "threads": 1,
        "preserve_insertion_order": False, "temp_directory": str(spill)})
    con.execute("SET TimeZone='UTC'")
    return con


def _retained_inputs(receipt: Path):
    from .identity_reconstruction import ReconstructionInputs, RetainedInput

    values = json.loads(receipt.read_text(encoding="utf-8"))["inputs"]
    inputs = {}
    for name in ("ticker_history", "companyfacts", "submissions", "company_tickers"):
        item = values[name]
        path = Path(item["path"])
        actual = file_sha256(path)
        if actual != item["sha256"]:
            raise ValueError(f"{name}: retained bytes differ from receipt {receipt}")
        observed = dt.datetime.fromisoformat(item["received_at"])
        if observed.tzinfo:
            observed = observed.astimezone(dt.UTC).replace(tzinfo=None)
        inputs[name] = RetainedInput(name, path, actual, observed, item["receipt_basis"])
    return ReconstructionInputs(**inputs)


def prepare_rehearsal_inputs(work: Path, receipt: Path, step: str) -> dict[str, object]:
    """Resumable bounded preparation: vendor -> facts -> reconstruction.

    Only the selected step's tables are replaced after an interrupted attempt.
    Inputs/code are pinned before any write; completed evidence files are hashed.
    No production connection, network request or whole-stage transaction occurs.
    """
    from . import identity_reconstruction as ir
    import pyarrow as pa
    import pyarrow.parquet as pq

    inputs = _retained_inputs(receipt)
    pin = {"version": PREPARATION_VERSION, "reconstruction_method": ir.METHOD,
           "reconstruction_code_sha256": file_sha256(Path(ir.__file__)), "params_digest": ir.ReconstructionParams().digest(),
           "inputs": {item.name: item.as_detail() for item in inputs.files()}}
    work.mkdir(parents=True, exist_ok=True)
    manifest_path = work / "preparation.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {"pin": pin, "steps": {}}
    if manifest["pin"] != pin:
        raise ValueError("retained preparation input/code pins changed: use a new work directory")
    if step in manifest["steps"]:
        result = manifest["steps"][step]
        for output in result.get("files", []):
            if file_sha256(work / output["file"]) != output["sha256"]:
                raise ValueError(f"prepared output changed: {output['file']}")
        return {**result, "reused": True}
    _write_json(manifest_path, manifest)
    con = _connect(work / "retained.duckdb")
    result: dict[str, object] = {"step": step, "reused": False}
    try:
        if step == "vendor":
            for table in ("ri_vendor_lines", "ri_vendor_symbols", "ri_share_runs", "ri_share_obs"):
                con.execute(f"DROP TABLE IF EXISTS {table}")
            ir.stage_vendor_ticker_history(con, inputs.ticker_history.path, chunks=64)
            result["vendor_lines"] = con.execute("SELECT count(*) FROM ri_vendor_lines").fetchone()[0]
            con.execute("CREATE OR REPLACE TABLE ri_operating_lines AS SELECT DISTINCT securityID::BIGINT AS vendor_id "
                        "FROM read_parquet(?) WHERE securityID > 0 AND earnFlag = '0'", [str(inputs.ticker_history.path)])
            result["operating_proxy_lines"] = con.execute("SELECT count(*) FROM ri_operating_lines").fetchone()[0]
        elif step == "facts":
            if "vendor" not in manifest["steps"]:
                raise ValueError("complete vendor preparation first")
            con.execute("DROP TABLE IF EXISTS ri_share_facts")
            result["share_facts"] = ir.stage_share_facts(con, ir.iter_companyfacts_share_facts(inputs.companyfacts.path), batch=10_000)
        elif step == "reconstruct":
            if "facts" not in manifest["steps"]:
                raise ValueError("complete fact preparation first")
            params = ir.ReconstructionParams()
            lines = ir.read_vendor_lines(con)
            ciks = set()
            for start in range(0, len(lines), 500):
                ciks.update(str(row[1]) for row in ir.match_share_counts(con, params, vendor_ids=[line.vendor_id for line in lines[start:start+500]]))
            filings = ir.read_lifecycle_filings(inputs.submissions.path, sorted(ciks))
            result.update(candidate_ciks=len(ciks), lifecycle_filings=len(filings), vendor_lines=len(lines), evidence_rows=0, files=[])
            artifacts = {item.name + "_sha256": item.sha256 for item in inputs.files()}
            output_dir = work / "evidence"
            output_dir.mkdir(exist_ok=True)
            for ordinal, batch in enumerate(ir.reconstruct_in_batches(con, lines, filings=filings,
                    tickers=ir.read_sec_ticker_snapshot(inputs.company_tickers.path),
                    ticker_observed_at=inputs.company_tickers.received_at, params=params, batch_size=250)):
                rows = batch.evidence_rows(observed_at=inputs.companyfacts.received_at,
                    source_loaded_at=inputs.companyfacts.received_at, run_id="identity-rehearsal-preparation",
                    artifact_sha256=inputs.companyfacts.sha256, artifacts=artifacts, revision_key=inputs.revision_key())
                path = output_dir / f"part-{ordinal:04d}.parquet"
                temporary = path.with_suffix(".parquet.tmp")
                table = pa.Table.from_pylist(rows, schema=ir._evidence_schema())
                pq.write_table(table, temporary, compression="zstd")
                os.replace(temporary, path)
                result["files"].append({"file": path.relative_to(work).as_posix(), "rows": len(rows),
                                       "sha256": file_sha256(path), "bytes": path.stat().st_size})
                result["evidence_rows"] += len(rows)
                print(json.dumps({"step": step, "batch": ordinal, "rows": len(rows)}), flush=True)
        else:
            raise ValueError(f"unknown preparation step {step}")
        con.execute("CHECKPOINT")
    finally:
        con.close()
    manifest["steps"][step] = result
    _write_json(manifest_path, manifest)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare")
    prepare.add_argument("--work", type=Path, required=True)
    prepare.add_argument("--receipt", type=Path, required=True)
    prepare.add_argument("--step", choices=("vendor", "facts", "reconstruct"), required=True)
    args = parser.parse_args()
    print(json.dumps(prepare_rehearsal_inputs(args.work, args.receipt, args.step), default=str), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
