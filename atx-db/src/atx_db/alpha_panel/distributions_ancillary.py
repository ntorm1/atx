"""Native ancillary object admission; source semantics derive from parsed bodies."""
import datetime as dt
import json
from pathlib import Path
from . import common as C, distributions_native as N, distributions_qualification as Q


def records(admitted: dict, role: str) -> dict:
    path = Path(admitted["inputs"][role]["path"])
    value = json.loads(path.read_text(encoding="utf-8"))
    if value == []:
        return {}
    expected_schema = f"atx.native-{role}-evidence/v1"
    if not isinstance(value, dict) or set(value) != {"schema", "records"} or value["schema"] != expected_schema:
        raise ValueError("exact ancillary evidence schema required")
    result = {}
    for record in value["records"]:
        key = (record["declaration_accession"], record["declaration_sha256"])
        expected = {"declaration_accession", "declaration_sha256", "clock_document"} if role == "clock" else {
            "declaration_accession", "declaration_sha256", "class_document", "class_clock_document"}
        if set(record) != expected or key in result:
            raise ValueError("unknown/duplicate ancillary evidence fields/key")
        result[key] = record
    return result


def qualify(event: dict, native_text: str, doc: dict, admitted: dict, pins: dict) -> dict:
    key = (doc["accession"], doc["source_sha256"])
    clocks, classes = records(admitted, "clock"), records(admitted, "class")
    if key not in clocks or key not in classes:
        return {"reason": "missing_native_clock_or_class_chain"}
    if admitted["schema"] != "atx.native-admission/v2":
        return {"reason": "missing_independently_admitted_price_symbol_history"}
    clock_blob, _ = N.native_document(clocks[key]["clock_document"], pins)
    class_doc = classes[key]["class_document"]
    class_blob, _ = N.native_document(class_doc, pins)
    class_clock_blob, _ = N.native_document(classes[key]["class_clock_document"], pins)
    if clocks[key]["clock_document"]["accession"] != doc["accession"] or classes[key]["class_clock_document"]["accession"] != class_doc["accession"]:
        raise ValueError("ancillary original accession chain conflict")
    try:
        publication = Q.accepted_clock(clock_blob, doc["accession"])
        native_classes = Q.native_classes(class_blob)
    except ValueError as error:
        return {"reason": "native_chain_unqualified:" + str(error)}
    symbols = sorted({row["symbol"] for row in native_classes})
    prices = Path(admitted["inputs"]["prices"]["path"])
    links = Path(admitted["inputs"]["identity"]["path"]).parent / "link_table.parquet"
    con = C.connect(memory="128MB", threads=1)
    try:
        con.execute("CREATE TABLE symbols(ticker VARCHAR)")
        if symbols:
            con.executemany("INSERT INTO symbols VALUES (?)", [(symbol,) for symbol in symbols])
        # Same bounded seven-calendar-day as-of symbol rule as dated identity.
        result = con.execute(f"""WITH p AS (
            SELECT security_id,ticker,session_date FROM read_parquet('{prices.as_posix()}')
            WHERE session_date<=? AND session_date>=? AND ticker IN (SELECT ticker FROM symbols)),
            last AS (SELECT ticker,max(session_date) AS day FROM p GROUP BY ticker)
            SELECT DISTINCT p.security_id,p.ticker,l.cik,l.valid_from,l.valid_to,l.available_at
            FROM p JOIN last ON p.ticker=last.ticker AND p.session_date=last.day
            JOIN read_parquet('{links.as_posix()}') l ON l.security_id=p.security_id
            WHERE ? BETWEEN l.valid_from AND l.valid_to AND l.available_at IS NOT NULL
            AND l.link_tier IN ('strict','dated')""", [publication.date(), publication.date()-dt.timedelta(days=7), publication.date()]).fetchall()
    finally:
        con.close()
    lines = [{"security_id": sid, "ticker": ticker, "cik": cik, "valid_from": start, "valid_to": end,
              "available_at": clock.replace(tzinfo=dt.timezone.utc) if clock.tzinfo is None else clock}
             for sid,ticker,cik,start,end,clock in result]
    try:
        qualified = Q.qualify({**event, "cik": doc["cik"]}, native_text,
            declaration_accession=doc["accession"], declaration_clock=clock_blob,
            class_document=class_blob, class_accession=class_doc["accession"], class_clock=class_clock_blob,
            line_rows=lines)
    except ValueError as error:
        return {"reason": "native_chain_unqualified:" + str(error)}
    N.recheck(pins)
    return {"qualified": qualified, "reason": "native_fixed_regular_event"}
