"""Bounded original-byte text publication and complete immutable reader replay.

No network, warehouse discovery or import-time source IO. Original full annual
member/filing populations are independently bound before any transformation.
"""
from __future__ import annotations

import datetime as dt
import json
import os
import tempfile
from pathlib import Path

from . import common as C, filing_text as FT, text_features as TF
from . import filing_text_native as N

SCHEMA = "atx.text-native-generation/v1"
METRICS = tuple(s + "_" + f for s in TF.SECTIONS for f in TF.FEATURE_COLS + TF.SIM_COLS)


def text_sql_schema():
    """Stable native types even when an entire metric/clock column is NULL."""
    fields={name:'VARCHAR' for name in ('accession','form','source_path','reason','comparison_reason','metric_basis','source_sha256','prior_accession','acceptance_clock','vintage_risk')}
    fields.update({name:'BIGINT' for name in ('cik','source_index','prior_gap_days','prior_report_gap_days','doc_bytes','doc_words','n_sections')})
    fields.update({name:'DATE' for name in ('filing_date','report_date','prior_filing_date')})
    fields.update(available_at='TIMESTAMPTZ',doc_fog='DOUBLE',business_vocabulary='VARCHAR[]')
    for section in TF.SECTIONS:
        for field in TF.FEATURE_COLS+TF.SIM_COLS:
            fields[section+'_'+field]='BIGINT' if field in ('chars','words','sentences') else 'DOUBLE'
        for field in ('source','method','scope','reason','comparison_reason','edit_reason'):
            fields[section+'_'+field]='VARCHAR'
    return fields


def _clock(value):
    return value.isoformat() if value else None


def _filing_rows(con):
    cursor = con.cursor().execute("SELECT * FROM nt_inventory ORDER BY cik,report_date,available_at,accession")
    fields = [d[0] for d in cursor.description]
    while batch := cursor.fetchmany(32):
        for values in batch:
            yield dict(zip(fields, values))


def _profile(root, record, originals, plan):
    original = originals.get((record["cik"], record["accession"]))
    if record["reason"] or not record["primary_document"] or not original:
        return {}, {}, record["reason"] or "original_primary_bytes_missing"
    url = FT.document_url(original.get('archives_cik',record["cik"]), record["accession"], record["primary_document"])
    if original["url"] != url:
        raise ValueError("primary document differs from original submissions record")
    path = N.member(root, original["path"])
    if path.stat().st_size > FT.MAX_DOC_BYTES:
        return {}, {}, "oversized_original_document"
    raw = path.read_bytes()
    text = FT.html_to_text(raw)
    if not TF.words(text):
        return {}, {}, "empty_original_document"
    sections, diagnostics = FT.extract_sections(text, record["form"])
    wanted=[name for name,section in sections.items() if 'by_reference' in section.flags]
    if wanted:
        for exhibit,path in N.authenticated_exhibits(root,plan=plan,original=original):
            found=FT.extract_titled(FT.html_to_text(path.read_bytes()),wanted)
            for name,section in found.items():
                if 'by_reference' in sections[name].flags or len(section.text)>len(sections[name].text):
                    sections[name]=section
    profiles = {}
    flags = {}
    for name, section in sections.items():
        flags[name] = section.flags
        if "by_reference" not in section.flags and 'item3_whole' not in section.flags and TF.words(section.text):
            profiles[name] = TF.section_profile(section.text)
    doc = TF.readability(text)
    doc.update(bytes=len(raw), profiles=profiles, flags=flags)
    return doc, sections, None


def _text_rows(root, con, plan):
    originals = N.original_map(root, plan)
    for (cik,accession),original in originals.items():
        owner=original.get('archives_cik',cik)
        if not con.execute('SELECT count(*) FROM nt_filings WHERE cik=? AND accession=?',[owner,accession]).fetchone()[0]:
            raise ValueError('primary archive owner absent from original submissions census')
    # O(annual filings per issuer); a hard bound rejects malformed source census.
    history = []
    issuer = None
    for record in _filing_rows(con):
        if issuer != record["cik"]:
            history, issuer = [], record["cik"]
        if len(history) >= 128:
            raise ValueError("issuer filing unit exceeds bounded native census")
        report=record['report_date']
        if report:
            history=[(r,d) for r,d in history if not r['report_date'] or (report-r['report_date']).days<=550]
        if sum(p['words'] for r,d in history for p in d.get('profiles',{}).values())>500_000:
            raise ValueError('issuer profile unit exceeds bounded 500000-word working set')
        doc, sections, reason = _profile(root, record, originals,plan)
        report = record["report_date"]
        family = FT.form_family(record["form"])
        candidates = [(r,d) for r,d in history if report and r["report_date"] and
                      180 <= (report-r["report_date"]).days <= 550 and
                      r["report_date"].year == report.year-1 and
                      FT.form_family(r["form"]) == family and
                      180 <= (record["filing_date"]-r["filing_date"]).days <= 550 and
                      r["available_at"] and record["available_at"] and
                      r["available_at"] < record["available_at"] and not r["reason"]]
        # Multiple transition periods in the same prior fiscal year are
        # ambiguous; never pick a conveniently downloaded prior accession.
        prior = candidates[0] if len(candidates) == 1 else None
        out = {k:_clock(record[k]) if k in ("filing_date","report_date","available_at") else record[k]
               for k in ("cik","accession","form","filing_date","report_date","available_at","source_path","source_index","acceptance_clock","vintage_risk")}
        out.update(reason=reason, prior_accession=prior[0]["accession"] if prior else None,
                   prior_filing_date=_clock(prior[0]['filing_date']) if prior else None,
                   prior_gap_days=(record['filing_date']-prior[0]['filing_date']).days if prior else None,
                   prior_report_gap_days=(report-prior[0]["report_date"]).days if prior else None,
                   comparison_reason=None if prior else "ambiguous_or_unmatched_prior_annual_period",
                   doc_bytes=doc.get("bytes"), doc_words=doc.get("words"), doc_fog=doc.get("fog"),
                   metric_basis="exact_words_tf_jaccard;heuristic_fog;sentence_sequence_edit_proxy",
                   source_sha256=plan["files"][originals[(issuer,record["accession"])]["path"]]["sha256"] if (issuer,record["accession"]) in originals else None)
        for s in TF.SECTIONS:
            profile = doc.get("profiles",{}).get(s)
            out[s+'_source']='ex13' if s in sections and 'ex13' in sections[s].flags else ('primary' if profile else None)
            out[s+'_method']=sections[s].method if profile else None
            out[s+'_scope']=sections[s].item if profile else None
            prev = prior[1].get("profiles",{}).get(s) if prior else None
            out[s+'_comparison_reason']=None if profile and prev else ('current_section_missing_or_empty' if not profile else ('prior_section_missing_or_empty' if prior else 'ambiguous_or_unmatched_prior_annual_period'))
            out[s+"_reason"] = None if profile else ('20f_item3d_scope_unqualified' if 'item3_whole' in doc.get('flags',{}).get(s,[]) else ("incorporated_by_reference_requires_original_exhibit" if "by_reference" in doc.get("flags",{}).get(s,[]) else "section_missing_or_empty"))
            for f in TF.FEATURE_COLS:
                out[s+"_"+f] = profile.get(f) if profile else None
            for f,v in TF.compare(profile,prev).items():
                out[s+"_"+f] = v
            if (profile and len(profile["sents"]) > TF.MAX_EDIT_SENTENCES) or (prev and len(prev["sents"]) > TF.MAX_EDIT_SENTENCES):
                out[s+"_sim_minedit"] = None
                out[s+"_edit_reason"] = "sentence_proxy_bound_exceeded"
            else:
                out[s+"_edit_reason"] = None
        # The profile is intentionally unhashed: no CRC-32 token collisions.
        out["business_vocabulary"] = sorted(set(TF.words(sections["business"].text))) if "business" in doc.get("profiles",{}) else []
        out['n_sections']=len(doc.get('profiles',{})) if doc else None
        yield out
        history.append((record,doc))


def _write_lines(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".partial")
    with tmp.open("w",encoding="utf-8",newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row,sort_keys=True,separators=(",",":"),allow_nan=False)+"\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp,path)


def _materialize(root, output, plan, expected_plan_sha256, *, unit_size=32,
                 expected_checkpoint_sha256=None, interrupt_after_units=None):
    if not 1 <= unit_size <= 64:
        raise ValueError("bounded unit_size must be 1..64")
    N.check_plan(root,plan,expected_plan_sha256)
    required = {str(Path(m.__file__).resolve()) for m in (TF,)} | {str(Path(__file__).resolve())}
    from . import tnic_native as T
    required.add(str(Path(T.__file__).resolve()))
    if not required <= set(plan["code"]):
        raise ValueError("complete native producer code binding required")
    output.mkdir(parents=True,exist_ok=True)
    checkpoint_path = output / "checkpoint.json"
    checkpoint = {"schema":SCHEMA,"plan_sha256":expected_plan_sha256,"unit_size":unit_size,"units":{}}
    if checkpoint_path.exists():
        if not expected_checkpoint_sha256 or N.pin(checkpoint_path)["sha256"] != expected_checkpoint_sha256:
            raise ValueError("independently authenticated resume checkpoint required")
        saved = json.loads(checkpoint_path.read_text())
        if any(saved[k] != checkpoint[k] for k in ("schema","plan_sha256","unit_size")):
            raise ValueError("stale resumable source/schema/unit binding")
        checkpoint = saved
    con = C.connect(memory="128MB",threads=1,temp_dir=output/"scratch",db_file="native.duckdb")
    try:
        population = N.derive_population(root,plan,con)
        unit, index = [], 0
        def flush(rows):
            nonlocal index
            name = f"units/text-{index:06d}.jsonl"
            dest = output/name
            if name in checkpoint["units"]:
                if N.pin(dest) != checkpoint["units"][name]:
                    raise ValueError("corrupt resumable unit")
                # Authenticate semantics as well as checkpoint bytes.
                expected = "".join(json.dumps(r,sort_keys=True,separators=(",",":"),allow_nan=False)+"\n" for r in rows)
                if dest.read_text(encoding="utf-8") != expected:
                    raise ValueError("resumed unit disagrees with original source replay")
            else:
                _write_lines(dest,rows)
                checkpoint["units"][name] = N.pin(dest)
                C.write_json_atomic(checkpoint_path,checkpoint)
            index += 1
            if interrupt_after_units and index == interrupt_after_units:
                raise InterruptedError("bounded publication interrupted before activation")
        for row in _text_rows(root,con,plan):
            unit.append(row)
            if len(unit) == unit_size:
                flush(unit)
                unit=[]
        if unit:
            flush(unit)
        if not index or len(checkpoint["units"]) != index:
            raise ValueError("empty or incomplete text units")
        units = sorted(checkpoint["units"])
        if set(p.relative_to(output).as_posix() for p in (output/"units").glob("*.jsonl")) != set(units):
            raise ValueError("orphan publication unit")
        paths = "["+",".join("'"+(output/u).as_posix().replace("'","''")+"'" for u in units)+"]"
        typed=','.join(f'CAST({name} AS {kind}) AS {name}' for name,kind in text_sql_schema().items())
        con.execute(f"CREATE TABLE nt_text AS SELECT {typed} FROM read_json_auto({paths},union_by_name=true,maximum_object_size=16777216)")
        if con.execute("SELECT count(*) FROM ((SELECT cik,accession FROM nt_inventory EXCEPT ALL SELECT cik,accession FROM nt_text) UNION ALL (SELECT cik,accession FROM nt_text EXCEPT ALL SELECT cik,accession FROM nt_inventory))").fetchone()[0]:
            raise ValueError("full inventory/publication population mismatch")
        C.copy_to_parquet(con,"SELECT * EXCLUDE(business_vocabulary) FROM nt_text ORDER BY cik,accession",output/"text/features.parquet",row_group_size=4096)
        peers = T.build_native(con,output,root,plan)
        coverage = _coverage(con)
        N.check_plan(root,plan,expected_plan_sha256)
        products = ["text/features.parquet", "classification_tnic/pairs.parquet", "classification_tnic/firms.parquet", "classification_tnic/years.parquet"]
        files = {n:N.pin(output/n) for n in products}
        manifest = {"schema":SCHEMA,"status":"published_conditional_native","code":plan['code'],"source_plan_sha256":expected_plan_sha256,"source_population":population,
                    "coverage":coverage,"peer_method":peers,"files":files,"units":checkpoint["units"],
                    "input_manifests_sha256":plan.get("input_manifests_sha256",{}),
                    "gate_status":"PASS" if all(r["text_gate"] in ("PASS","NOT_APPLICABLE") and r["tnic_gate"]=="PASS" for r in coverage) else "FAIL"}
        C.write_json_atomic(output/"source-plan.json",plan)
        manifest['metadata_files']={name:N.pin(output/name) for name in ('source-plan.json','checkpoint.json')}
        C.write_json_atomic(output/"manifest.json",manifest)
        return manifest
    finally:
        con.close()


def _coverage(con):
    result=[]
    # Every reported year is retained even when zero source coverage exists.
    for year in range(2019,2027):
        denominator=con.execute("SELECT count(DISTINCT f.cik) FROM nt_inventory f JOIN nt_links l ON f.cik=l.cik AND year(f.filing_date)=l.y WHERE year(f.filing_date)=? AND f.form IN ("+FT._quote_list(FT.FORMS_10K)+")",[year]).fetchone()[0]
        numerator=con.execute("SELECT count(*) FROM nt_peer_firms WHERE year=? AND n_peers>0",[year]).fetchone()[0]
        members=con.execute("SELECT count(*) FROM nt_members WHERE year(session_date)=?",[year]).fetchone()[0]
        # Monthly member cells obey strict same-month-end 22:00 cutoff. Member
        # input must be monthly full population; daily mode is a separate plan.
        usable=con.execute("SELECT count(*) FROM nt_members m JOIN LATERAL (SELECT * FROM nt_text t WHERE t.cik=m.cik AND t.available_at < CAST(m.session_date AS TIMESTAMPTZ)+INTERVAL 22 HOUR AND t.form NOT LIKE '%/A' ORDER BY t.available_at DESC,t.accession DESC LIMIT 1) t ON true WHERE year(m.session_date)=? AND t.reason IS NULL AND t.filing_date>=m.session_date-INTERVAL 400 DAY AND t.business_words>0 AND t.risk_words>0 AND t.mdna_words>0 AND t.business_sim_cosine IS NOT NULL AND t.risk_sim_cosine IS NOT NULL AND t.mdna_sim_cosine IS NOT NULL",[year]).fetchone()[0]
        result.append({"year":year,"linked_10k_filers":denominator,"strict_reproduced_peer_filers":numerator,
                       "member_cells":members,"text_cells":usable,"text_share":usable/members if members else None,
                       "tnic_share":numerator/denominator if denominator else None,
                       "text_gate":"NOT_APPLICABLE" if year==2019 else ("PASS" if members and usable/members>=.90 else "FAIL"),
                       "tnic_gate":"PASS" if denominator and numerator/denominator>=.95 else "FAIL",
                       "tnic_reason":None if denominator and numerator/denominator>=.95 else "strict_reproduced_tnic3_coverage_below_95_percent"})
        result[-1]['other_universes']={}
        for (universe,) in con.execute('SELECT DISTINCT universe FROM nt_universe_cells ORDER BY universe').fetchall():
            denominator_other=con.execute('SELECT count(*) FROM nt_universe_cells WHERE universe=? AND year(session_date)=?',[universe,year]).fetchone()[0]
            covered_other=con.execute("SELECT count(*) FROM nt_universe_cells m JOIN LATERAL (SELECT * FROM nt_text t WHERE t.cik=m.cik AND t.available_at < CAST(m.session_date AS TIMESTAMPTZ)+INTERVAL 22 HOUR AND t.form NOT LIKE '%/A' ORDER BY t.available_at DESC,t.accession DESC LIMIT 1) t ON true WHERE m.universe=? AND year(m.session_date)=? AND t.reason IS NULL AND t.filing_date>=m.session_date-INTERVAL 400 DAY AND t.business_words>0 AND t.risk_words>0 AND t.mdna_words>0 AND t.business_sim_cosine IS NOT NULL AND t.risk_sim_cosine IS NOT NULL AND t.mdna_sim_cosine IS NOT NULL",[universe,year]).fetchone()[0]
            result[-1]['other_universes'][universe]={'cells':denominator_other,'text_cells':covered_other,'text_share':covered_other/denominator_other if denominator_other else None}
    return result


def publish_native(source_root,output_root,plan,expected_plan_sha256,**options):
    """Deterministic immutable generation; activation follows full reader replay.

    On interruption staging is retained. Resume requires the independently
    captured checkpoint hash. No existing active generation is overwritten.
    """
    root,output=Path(source_root).resolve(),Path(output_root).resolve()
    generation_id=N.digest({"schema":SCHEMA,"plan":expected_plan_sha256,"unit_size":options.get("unit_size",32)})
    staging=output/"pending"/generation_id
    generation=output/"generations"/generation_id
    output.mkdir(parents=True,exist_ok=True)
    lock=output/'publication.lock'
    try:
        lock_fd=os.open(lock,os.O_CREAT|os.O_EXCL|os.O_WRONLY)
    except FileExistsError as exc:
        raise ValueError('native publication already active; reconcile retained lock before resume') from exc
    try:
        return _publish_locked(root,output,staging,generation,plan,expected_plan_sha256,options)
    finally:
        os.close(lock_fd)
        lock.unlink()


def _publish_locked(root,output,staging,generation,plan,expected_plan_sha256,options):
    if generation.exists():
        expected=options.pop("expected_manifest_sha256",None)
        if not expected:
            raise ValueError("existing generation requires independent manifest identity")
        read_native_generation(root,generation,expected,expected_plan_sha256)
        return generation
    independent_output=options.pop('expected_manifest_sha256',None)
    manifest=_materialize(root,staging,plan,expected_plan_sha256,**options)
    manifest_sha=N.pin(staging/"manifest.json")["sha256"]
    if independent_output and manifest_sha!=independent_output:
        raise ValueError('new native output differs from independently expected manifest')
    read_native_generation(root,staging,manifest_sha,expected_plan_sha256)
    generation.parent.mkdir(parents=True,exist_ok=True)
    os.rename(staging,generation)
    # Atomic pointer is the last operation; failure preserves prior activation.
    C.write_json_atomic(output/"active.json",{"generation":generation.name,"manifest_sha256":manifest_sha})
    return generation


def read_native_generation(source_root,generation,expected_manifest_sha256,expected_plan_sha256):
    """Full population original-byte replay, including exclusions and orphan keys."""
    root,generation=Path(source_root).resolve(),Path(generation).resolve()
    manifest_path=generation/"manifest.json"
    if N.pin(manifest_path)["sha256"] != expected_manifest_sha256:
        raise ValueError("independent native manifest identity mismatch")
    manifest=json.loads(manifest_path.read_text())
    plan=json.loads((generation/"source-plan.json").read_text())
    N.check_plan(root,plan,expected_plan_sha256)
    if manifest["schema"] != SCHEMA or manifest["source_plan_sha256"] != expected_plan_sha256:
        raise ValueError("native source/schema binding mismatch")
    expected_files=set(manifest["files"])|set(manifest["units"])|{"checkpoint.json","manifest.json","source-plan.json"}
    actual={p.relative_to(generation).as_posix() for p in generation.rglob("*") if p.is_file() and "scratch" not in p.relative_to(generation).parts}
    if actual != expected_files:
        raise ValueError("orphan or incomplete immutable publication")
    for name,expected in (manifest["files"]|manifest["units"]|manifest['metadata_files']).items():
        if N.pin(N.member(generation,name)) != expected:
            raise ValueError("native immutable product changed")
    checkpoint=json.loads((generation/"checkpoint.json").read_text())
    if checkpoint["units"] != manifest["units"]:
        raise ValueError("native phase outputs incomplete")
    with tempfile.TemporaryDirectory(prefix="atx-text-replay-") as temporary:
        replay=_materialize(root,Path(temporary),plan,expected_plan_sha256,unit_size=checkpoint["unit_size"])
        if replay != manifest:
            raise ValueError("full original-byte native replay differs")
    N.check_plan(root,plan,expected_plan_sha256)
    if N.pin(manifest_path)["sha256"] != expected_manifest_sha256:
        raise ValueError("native manifest changed during replay")
    return {"generation":str(generation),"manifest_sha256":expected_manifest_sha256,
            "source_plan_sha256":expected_plan_sha256,"status":manifest["gate_status"],
            "products":{name:str(generation/name) for name in manifest["files"]},
            "coverage":manifest["coverage"],"dependencies":plan["files"],"code":plan["code"]}


def iter_native_rows(admission,product,batch_size=32):
    """Bounded public immutable rows; validate admission through full reader first."""
    import pyarrow.parquet as pq
    if not 1<=batch_size<=64 or product not in admission["products"]:
        raise ValueError("invalid bounded native product reader")
    path=Path(admission["products"][product])
    generation=Path(admission["generation"])
    if N.pin(generation/"manifest.json")["sha256"]!=admission["manifest_sha256"]:
        raise ValueError("stale reader admission")
    manifest=json.loads((generation/"manifest.json").read_text())
    if N.pin(path)!=manifest["files"][product]:
        raise ValueError("changed native product before reading")
    for batch in pq.ParquetFile(path).iter_batches(batch_size=batch_size):
        yield from batch.to_pylist()
    if N.pin(path)!=manifest["files"][product]:
        raise ValueError("changed native product while reading")


def main(argv=None):
    """Explicit controller-run native producer/reader; no implicit source scans."""
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase',choices=('publish','read'),required=True)
    parser.add_argument('--source-root',required=True)
    parser.add_argument('--expected-plan-sha256',required=True)
    parser.add_argument('--plan')
    parser.add_argument('--output-root')
    parser.add_argument('--generation')
    parser.add_argument('--expected-manifest-sha256')
    parser.add_argument('--expected-checkpoint-sha256')
    parser.add_argument('--unit-size',type=int,default=32)
    args=parser.parse_args(argv)
    if args.phase=='publish':
        if not args.plan or not args.output_root:
            parser.error('publish requires independently reviewed --plan and --output-root')
        plan=json.loads(Path(args.plan).read_text())
        generation=publish_native(args.source_root,args.output_root,plan,args.expected_plan_sha256,
             unit_size=args.unit_size,expected_checkpoint_sha256=args.expected_checkpoint_sha256,
             **({'expected_manifest_sha256':args.expected_manifest_sha256} if args.expected_manifest_sha256 else {}))
        result={'generation':str(generation),'manifest_sha256':N.pin(generation/'manifest.json')['sha256']}
    else:
        if not args.generation or not args.expected_manifest_sha256:
            parser.error('read requires --generation and independently reviewed --expected-manifest-sha256')
        result=read_native_generation(args.source_root,args.generation,args.expected_manifest_sha256,args.expected_plan_sha256)
    print(json.dumps(result,sort_keys=True,default=str))
    return 0


if __name__=='__main__':
    raise SystemExit(main())
