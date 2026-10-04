"""Original-byte SEC annual filing admission; never performs network IO.

The independently reviewed plan pins the complete native member/identity
populations and original submissions census. Normalized landing rows cannot
replace that census. A receipt is transport evidence, not a completeness seal.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import platform
import re
from pathlib import Path
from zoneinfo import ZoneInfo

from . import common as C
from . import filing_text as FT


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    default=str, allow_nan=False).encode()).hexdigest()


def runtime_identity():
    import duckdb
    import numpy
    import pyarrow
    import tzdata
    return {'python':platform.python_version(),'duckdb':duckdb.__version__,
            'numpy':numpy.__version__,'pyarrow':pyarrow.__version__,'tzdata':tzdata.__version__}


def pin(path):
    path = Path(path)
    before = path.stat()
    result = {"sha256": C.sha256_file(path), "bytes": before.st_size}
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("source changed while hashing")
    return result


def member(root, relative):
    path = (Path(root) / relative).resolve()
    if Path(relative).is_absolute() or ".." in Path(relative).parts or not path.is_relative_to(Path(root).resolve()):
        raise ValueError("invalid source member")
    return path


def sql_files(root, files):
    if not files:
        raise ValueError("complete source file inventory required")
    return "read_parquet([" + ",".join("'" + member(root, f).as_posix().replace("'", "''") + "'" for f in files) + "])"


def check_plan(root, plan, expected_plan_sha256):
    if digest(plan) != expected_plan_sha256:
        raise ValueError("independently reviewed plan mismatch")
    if plan.get("schema") != "atx.text-native-source-plan/v1":
        raise ValueError("unsupported source plan")
    if plan.get("member_frequency") != "monthly":
        raise ValueError("monthly full member population required")
    if plan.get('runtime')!=runtime_identity():
        raise ValueError('independently frozen native runtime mismatch')
    review = plan.get("source_review", {})
    for field in ("complete_submissions_census", "complete_native_members", "accepted_identity",
                  "original_sec_records", "free_commercial_use"):
        if review.get(field) is not True:
            raise ValueError("unqualified native source prerequisite: " + field)
    # The reviewed evidence is independently pinned and retained, not just flags.
    for field in ("submissions", "members", "identity"):
        if not review.get(field + "_evidence"):
            raise ValueError("missing independent native evidence: " + field)
        if review[field + "_evidence"] not in plan["files"]:
            raise ValueError("native evidence absent from complete pins")
    for name, expected in plan["files"].items():
        if pin(member(root, name)) != expected:
            raise ValueError("changed independently pinned source: " + name)
    if not plan.get("input_manifests_sha256"):
        raise ValueError("native upstream manifest identities required")
    for name, sha in plan["input_manifests_sha256"].items():
        if name not in plan["files"] or plan["files"][name]["sha256"] != sha:
            raise ValueError("upstream manifest absent from complete independent pins")
    for name, expected in plan["code"].items():
        if not Path(name).is_absolute() or pin(name) != expected:
            raise ValueError("changed independently pinned code: " + name)
    required = {str(Path(m.__file__).resolve()) for m in (C, FT)} | {str(Path(__file__).resolve())}
    if not required <= set(plan["code"]):
        raise ValueError("complete parser/common/source code pins required")
    referenced = set(plan["members"] + plan["identity"] + [s["path"] for s in plan["submissions"]])
    referenced |= {s['receipt'] for s in plan['submissions']}
    for index in plan.get('sec_indexes',[]):
        referenced |= {index['path'],index['receipt']}
    for proof in plan.get('acceptance_proofs',[]):
        referenced |= {proof['index_path'],proof['receipt']}
    referenced |= {r["path"] for r in plan["originals"]}
    referenced |= {r["receipt"] for r in plan["originals"]}
    for r in plan['originals']:
        if r.get('exhibits'):
            referenced |= {r['index_path'],r['index_receipt']}
            for exhibit in r['exhibits']:
                referenced |= {exhibit['path'],exhibit['receipt']}
    if not referenced <= set(plan["files"]):
        raise ValueError("source inventory is not completely pinned")


def acceptance(value):
    """Only original SEC acceptance timestamps with an explicit offset qualify.

    Unknown/ambiguous clocks remain missing. No filing-date midnight substitute.
    """
    if not value:
        return None
    try:
        clock = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        if clock.tzinfo is None:
            return None
        return clock.astimezone(dt.timezone.utc)
    except (ValueError, TypeError):
        return None


def acceptance_proofs(root,plan):
    """Original EDGAR index Accepted clocks, not potentially mislabeled Z.

    Native sec_filings.py documents that source member files mix true UTC
    with ET wall times labeled Z. EDGAR index Accepted is New York time.
    Missing or DST-ambiguous index evidence cannot become an early clock.
    """
    result={}
    for proof in plan.get('acceptance_proofs',[]):
        key=(int(proof['cik']),proof['accession'])
        if key in result:
            raise ValueError('duplicate original acceptance index proof')
        owner=int(proof.get('archives_cik',proof['cik']))
        url=f"{FT.ARCHIVES}/{owner}/{proof['accession'].replace('-','')}/{proof['accession']}-index.htm"
        receipt=json.loads(member(root,proof['receipt']).read_text())
        identity=plan['files'][proof['index_path']]
        if receipt.get('url')!=url or receipt.get('http_status')!=200 or receipt.get('error') or any(receipt.get(k)!=identity[k] for k in ('sha256','bytes')):
            raise ValueError('original accepted-clock index receipt mismatch')
        if member(root,proof['index_path']).stat().st_size>5_000_000:
            raise ValueError('bounded original acceptance index exceeded')
        text=FT.html_to_text(member(root,proof['index_path']).read_bytes())
        times=re.findall(r'\bAccepted\s+(\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2})\b',text)
        if len(times)!=1:
            result[key]=None
            continue
        local=dt.datetime.fromisoformat(times[0])
        eastern=ZoneInfo('America/New_York')
        a=local.replace(tzinfo=eastern,fold=0)
        b=local.replace(tzinfo=eastern,fold=1)
        clock=a.astimezone(dt.timezone.utc)
        result[key]=None if a.utcoffset()!=b.utcoffset() or clock.astimezone(eastern).replace(tzinfo=None)!=local else clock
    return result


def derive_population(root, plan, con):
    """Disk-backed native populations, preserving every full member cell."""
    con.execute(f"CREATE TABLE nt_members AS SELECT session_date, security_id, cik, member_equity FROM {sql_files(root, plan['members'])} WHERE member_equity")
    con.execute('CREATE TABLE nt_universe_cells(universe VARCHAR,session_date DATE,security_id BIGINT,cik BIGINT)')
    for universe,column in plan.get('universe_flags',{}).items():
        if universe not in ('univ_all_common','univ_r3000p') or column!=universe:
            raise ValueError('unsupported independently pinned native universe')
        con.execute(f"INSERT INTO nt_universe_cells SELECT '{universe}',session_date,security_id,cik FROM {sql_files(root,plan['members'])} WHERE {column}")
    measured={name:{str(y):int(n) for y,n in con.execute('SELECT year(session_date),count(*) FROM nt_universe_cells WHERE universe=? GROUP BY 1 ORDER BY 1',[name]).fetchall()} for name in plan.get('universe_flags',{})}
    if measured!=plan.get('expected_universe_cells',{}):
        raise ValueError('full independent universe yearly population mismatch')
    if con.execute('SELECT count(*) FROM (SELECT universe,session_date,security_id,count(*) n FROM nt_universe_cells GROUP BY 1,2,3 HAVING n<>1)').fetchone()[0]:
        raise ValueError('orphan duplicate native universe cell')
    con.execute(f"CREATE TABLE nt_identity AS SELECT security_id,cik,valid_from,valid_to,link_tier FROM {sql_files(root, plan['identity'])}")
    if con.execute("SELECT count(*) FROM (SELECT session_date,security_id,count(*) n FROM nt_members GROUP BY ALL HAVING n<>1)").fetchone()[0]:
        raise ValueError("duplicate native member cells")
    con.execute("CREATE TABLE nt_links AS SELECT DISTINCT year(session_date) y,cik FROM nt_members WHERE cik IS NOT NULL UNION SELECT DISTINCT year(m.session_date) y,l.cik FROM nt_members m JOIN nt_identity l ON m.security_id=l.security_id AND m.session_date BETWEEN l.valid_from AND l.valid_to WHERE l.cik IS NOT NULL")
    con.execute("CREATE TABLE nt_filings(cik BIGINT, accession VARCHAR,form VARCHAR,filing_date DATE,report_date DATE,available_at TIMESTAMPTZ,primary_document VARCHAR,source_path VARCHAR,source_index BIGINT,reason VARCHAR,acceptance_clock VARCHAR,vintage_risk VARCHAR)")
    proved=acceptance_proofs(root,plan)
    for source in plan["submissions"]:
        if member(root,source['path']).stat().st_size>16*1024*1024:
            raise ValueError('bounded original submissions unit exceeds16MiB')
        raw = json.loads(member(root, source["path"]).read_text(encoding="utf-8"))
        receipt=json.loads(member(root,source['receipt']).read_text())
        identity=plan['files'][source['path']]
        if receipt.get('http_status')!=200 or receipt.get('error') or receipt.get('url')!=source['url'] or any(receipt.get(k)!=identity[k] for k in ('sha256','bytes')):
            raise ValueError('original submissions receipt mismatch')
        prefix=f"https://data.sec.gov/submissions/CIK{int(source['cik']):010d}"
        if not source['url'].startswith(prefix) or not source['url'].endswith('.json'):
            raise ValueError('original submissions SEC issuer URL mismatch')
        cik = int(raw.get("cik", source["cik"]))
        if cik != int(source["cik"]):
            raise ValueError("original submissions issuer mismatch")
        rows = raw.get("filings", {}).get("recent", raw)
        size = len(rows["accessionNumber"])
        fields = ("form", "filingDate", "reportDate", "primaryDocument")
        if any(len(rows[k]) != size for k in fields):
            raise ValueError("incomplete original submissions arrays")
        clocks = rows.get("acceptanceDateTime", [None] * size)
        if len(clocks) != size:
            raise ValueError("incomplete original acceptance array")
        for index in range(size):
            form = rows["form"][index]
            # Preserve amendments as explicit excluded inventory; they never
            # replace original annual periods or silently become comparisons.
            if form.removesuffix("/A") not in FT.FORMS_10K + FT.FORMS_20F:
                continue
            filed = dt.date.fromisoformat(rows["filingDate"][index])
            if not dt.date(2018, 1, 1) <= filed <= dt.date(2026, 12, 31):
                continue
            report = rows["reportDate"][index]
            clock = proved.get((cik,rows['accessionNumber'][index]))
            raw_clock=acceptance(clocks[index])
            raw_et=None
            if raw_clock:
                raw_et=raw_clock.replace(tzinfo=None).replace(tzinfo=ZoneInfo('America/New_York')).astimezone(dt.timezone.utc)
            vintage='submission_clock_disagrees_with_original_index' if clock and raw_clock and clock not in (raw_clock,raw_et) else None
            reason = "amendment_excluded" if form.endswith("/A") else ("unknown_acceptance_clock" if clock is None else None)
            con.execute("INSERT INTO nt_filings VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", [cik,rows["accessionNumber"][index],form,filed,dt.date.fromisoformat(report) if report else None,clock,rows["primaryDocument"][index],source["path"],index,reason,'native_original_sec_index_et' if clock else None,vintage])
    # Overlap between recent and historical submissions is permitted only for
    # exactly equal native values; disagreements never select a longest row.
    if con.execute("SELECT count(*) FROM (SELECT cik,accession,count(DISTINCT (form,filing_date,report_date,available_at,primary_document,reason)) n FROM nt_filings GROUP BY ALL HAVING n<>1)").fetchone()[0]:
        raise ValueError("conflicting original accession records")
    # Fitting must never select issuers using future member inclusion. Derive
    # the entire independent original SEC cohort; member links only determine
    # publication coverage denominators and consumer membership.
    con.execute("CREATE TABLE nt_inventory AS SELECT * FROM nt_filings QUALIFY row_number() OVER(PARTITION BY cik,accession ORDER BY source_path,source_index)=1")
    derive_corpus_closure(root,plan,con)
    if con.execute('SELECT count(*) FROM (SELECT DISTINCT cik FROM nt_links EXCEPT SELECT DISTINCT cik FROM nt_filings)').fetchone()[0]:
        # A linked nonfiler is a valid explicit exclusion only if its original
        # submissions census was supplied and proved to contain no annuals.
        supplied={int(s['cik']) for s in plan['submissions']}
        linked={int(r[0]) for r in con.execute('SELECT DISTINCT cik FROM nt_links').fetchall()}
        if not linked<=supplied:
            raise ValueError('full linked-issuer original submissions census incomplete')
    counts = {str(y): int(n) for y,n in con.execute("SELECT year(session_date),count(*) FROM nt_members GROUP BY 1 ORDER BY 1").fetchall()}
    filings = {str(y): int(n) for y,n in con.execute("SELECT year(filing_date),count(*) FROM nt_inventory GROUP BY 1 ORDER BY 1").fetchall()}
    if counts != plan["expected_member_cells"] or filings != plan["expected_filing_records"]:
        raise ValueError("full original native population count mismatch")
    return {"member_cells": counts, "filing_records": filings,'other_universe_cells':measured}


def derive_corpus_closure(root,plan,con):
    """Replay original SEC full-index census against every accession record.

    A reviewed flag/count alone cannot establish full fitting-corpus closure.
    Missing source index quarters keep fitting unavailable, without changing
    the full linked filing/member publication denominators.
    """
    con.execute('CREATE TABLE nt_source_index(y INTEGER,q INTEGER,cik BIGINT,accession VARCHAR,form VARCHAR,filing_date DATE)')
    con.execute('CREATE TABLE nt_source_closure(y INTEGER,complete BOOLEAN,available_at TIMESTAMPTZ,source_sha256 VARCHAR)')
    by_year={}
    for index in plan.get('sec_indexes',[]):
        year,quarter=int(index['year']),int(index['quarter'])
        if year not in range(2018,2027) or quarter not in range(1,5):
            raise ValueError('invalid original SEC index cohort')
        expected_url=f'https://www.sec.gov/Archives/edgar/full-index/{year}/QTR{quarter}/master.idx'
        receipt=json.loads(member(root,index['receipt']).read_text())
        identity=plan['files'][index['path']]
        if receipt.get('url')!=expected_url or receipt.get('http_status')!=200 or receipt.get('error') or any(receipt.get(k)!=identity[k] for k in ('sha256','bytes')):
            raise ValueError('original complete SEC census receipt mismatch')
        if quarter in by_year.setdefault(year,{}):
            raise ValueError('duplicate original SEC index quarter')
        by_year[year][quarter]=(acceptance(receipt.get('fetched_at')),identity)
        with member(root,index['path']).open(encoding='latin-1') as handle:
            for line in handle:
                cells=line.rstrip('\r\n').split('|')
                if len(cells)!=5 or not cells[0].isdigit():
                    continue
                cik,company,form,filed,filename=cells
                if form.removesuffix('/A') not in FT.FORMS_10K+FT.FORMS_20F:
                    continue
                accession=Path(filename).stem
                if not re.fullmatch(r'\d{10}-\d{2}-\d{6}',accession):
                    raise ValueError('invalid original SEC census accession')
                date=dt.date.fromisoformat(filed)
                if date.year!=year or (date.month-1)//3+1!=quarter:
                    raise ValueError('SEC original census date/cohort mismatch')
                con.execute('INSERT INTO nt_source_index VALUES (?,?,?,?,?,?)',[year,quarter,int(cik),accession,form,date])
    for year in range(2018,2027):
        quarters=by_year.get(year,{})
        complete=set(quarters)=={1,2,3,4} and all(v[0] for v in quarters.values())
        if complete:
            # Co-registrants may share one physical index accession while
            # having separate issuer-original submission records. Preserve
            # their original CIK metadata but require the index owner record.
            mismatches=con.execute('SELECT count(*) FROM ((SELECT DISTINCT accession,form,filing_date FROM nt_source_index WHERE y=? EXCEPT SELECT DISTINCT accession,form,filing_date FROM nt_inventory WHERE year(filing_date)=?) UNION ALL (SELECT DISTINCT accession,form,filing_date FROM nt_inventory WHERE year(filing_date)=? EXCEPT SELECT DISTINCT accession,form,filing_date FROM nt_source_index WHERE y=?))',[year]*4).fetchone()[0]
            mismatches+=con.execute('SELECT count(*) FROM nt_source_index i WHERE i.y=? AND NOT EXISTS (SELECT 1 FROM nt_inventory f WHERE f.cik=i.cik AND f.accession=i.accession AND f.form=i.form AND f.filing_date=i.filing_date)',[year]).fetchone()[0]
            if mismatches:
                raise ValueError('complete original SEC corpus differs from native accession census')
        clock=max(v[0] for v in quarters.values()) if complete else None
        con.execute('INSERT INTO nt_source_closure VALUES (?,?,?,?)',[year,complete,clock,digest({str(q):v for q,v in quarters.items()})])


def original_map(root, plan):
    out = {}
    for original in plan["originals"]:
        key = (int(original["cik"]), original["accession"])
        if key in out:
            raise ValueError("duplicate original document binding")
        receipt = json.loads(member(root, original["receipt"]).read_text(encoding="utf-8"))
        expected = plan["files"][original["path"]]
        if receipt.get("http_status") != 200 or receipt.get("error") or receipt.get("sha256") != expected["sha256"] or receipt.get("bytes") != expected["bytes"]:
            raise ValueError("original bytes disagree with SEC receipt")
        if receipt.get("url") != original["url"] or receipt.get("accession") != key[1]:
            raise ValueError("SEC original receipt identity mismatch")
        out[key] = original
    return out


def authenticated_exhibits(root,plan,original):
    """EX-13 bytes only when typed on this accession's original SEC index."""
    if not original.get('exhibits'):
        return
    index_url=f"{FT.ARCHIVES}/{int(original.get('archives_cik',original['cik']))}/{original['accession'].replace('-','')}/{original['accession']}-index.htm"
    index_receipt=json.loads(member(root,original['index_receipt']).read_text())
    def check(receipt,path,url):
        identity=plan['files'][path]
        if receipt.get('http_status')!=200 or receipt.get('error') or receipt.get('url')!=url or any(receipt.get(k)!=identity[k] for k in ('sha256','bytes')):
            raise ValueError('original exhibit/index receipt mismatch')
    check(index_receipt,original['index_path'],index_url)
    allowed=FT.exhibit_links(member(root,original['index_path']).read_bytes())
    if len(original['exhibits'])>4:
        raise ValueError('bounded EX-13 inventory exceeds four original documents')
    for exhibit in original['exhibits']:
        if exhibit['url'] not in allowed or not exhibit['url'].startswith(FT.ARCHIVES+'/'):
            raise ValueError('exhibit not typed EX-13 on original accession index')
        receipt=json.loads(member(root,exhibit['receipt']).read_text())
        check(receipt,exhibit['path'],exhibit['url'])
        path=member(root,exhibit['path'])
        if path.stat().st_size>FT.MAX_DOC_BYTES:
            raise ValueError('bounded original exhibit bytes exceeded')
        yield exhibit,path
