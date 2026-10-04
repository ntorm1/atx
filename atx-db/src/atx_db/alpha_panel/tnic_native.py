"""Bounded native legacy TNIC reproduction, distinct from official TNIC.

Prior-year DF25, prior-year SIC3 histogram calibration and MIN_WORDS50 preserve
the original strict gate. Both source documents, final annual selection and
every calibration/SIC source contribute to the published availability clock.
"""
from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np

from . import common as C, filing_text as FT, tnic as T
from . import filing_text_native as N


def _pairs(con,year):
    focal=con.cursor().execute("SELECT cik,accession,available_at,vocabulary FROM nt_encoded WHERE y=? AND len(vocabulary)>=? ORDER BY cik",[year,T.MIN_WORDS])
    while row:=focal.fetchone():
        cik,accession,clock,vocab=row
        peer=con.cursor().execute("SELECT cik,accession,available_at,vocabulary FROM nt_encoded WHERE y=? AND cik>? AND len(vocabulary)>=? ORDER BY cik",[year,cik,T.MIN_WORDS])
        words=set(vocab)
        while other:=peer.fetchone():
            peer_cik,peer_accession,peer_clock,peer_vocab=other
            if not clock or not peer_clock or peer_accession==accession:
                continue
            score=len(words & set(peer_vocab))/math.sqrt(len(words)*len(peer_vocab))
            yield cik,peer_cik,accession,peer_accession,score,max(clock,peer_clock)


def _sic_sources(root,plan,con):
    con.execute("CREATE TABLE nt_sic(cik BIGINT,sic VARCHAR,available_at TIMESTAMPTZ)")
    for item in plan.get("sic_sources",[]):
        if item["path"] not in plan["files"] or item["receipt"] not in plan["files"]:
            raise ValueError("unbound SIC original/vintage receipt")
        raw=json.loads(N.bounded_bytes(root,item["path"],16*1024*1024).decode('utf-8'))
        receipt=json.loads(N.bounded_bytes(root,item["receipt"],1_000_000).decode('utf-8'))
        identity=plan["files"][item["path"]]
        if receipt.get("http_status")!=200 or receipt.get("error") or any(receipt.get(k)!=identity[k] for k in ("sha256","bytes")):
            raise ValueError("SIC original source receipt mismatch")
        if receipt.get("url")!=item["url"] or item['url']!=f"https://data.sec.gov/submissions/CIK{int(raw['cik']):010d}.json":
            raise ValueError("SIC source is not original SEC submissions")
        con.execute("INSERT INTO nt_sic VALUES (?,?,?)",[int(raw["cik"]),str(raw.get("sic") or ""),N.acceptance(receipt.get("fetched_at"))])
    if con.execute("SELECT count(*) FROM (SELECT cik,available_at,count(*) n FROM nt_sic GROUP BY 1,2 HAVING n<>1)").fetchone()[0]:
        raise ValueError("ambiguous native SIC vintages")


def build_native(con,output,root,plan):
    if str(Path(T.__file__).resolve()) not in plan['code']:
        raise ValueError("legacy reproduced TNIC code pin required")
    _sic_sources(root,plan,con)
    con.execute("CREATE TABLE nt_full_cohort AS SELECT t.*,year(t.filing_date) y FROM nt_text t WHERE t.form IN ("+FT._quote_list(FT.FORMS_10K)+")")
    con.execute("CREATE TABLE nt_raw_tokens(y INTEGER,cik BIGINT,accession VARCHAR,word VARCHAR)")
    cursor=con.cursor().execute("SELECT y,cik,accession,business_vocabulary FROM nt_full_cohort ORDER BY y,cik,accession")
    while row:=cursor.fetchone():
        y,cik,accession,words=row
        if len(words)>500_000 or sum(map(len,words))>FT.MAX_DOC_BYTES:
            raise ValueError('bounded native TNIC vocabulary exceeded')
        vocabulary=T.vocabulary_words(' '.join(words))
        if len(vocabulary)<T.MIN_WORDS:
            continue  # Original tokenize_pass excludes short raw documents from DF.
        for offset in range(0,len(vocabulary),64):
            con.executemany("INSERT INTO nt_raw_tokens VALUES (?,?,?,?)",[(y,cik,accession,word) for word in vocabulary[offset:offset+64]])
    # Original tokenize_pass applies MIN_WORDS before _load_year picks latest.
    # A latest short document remains inventory but cannot displace an earlier
    # eligible original document in fitting. Unknown source blocks cohort proof.
    con.execute("CREATE TABLE nt_peer_candidates AS SELECT c.* FROM nt_full_cohort c QUALIFY row_number() OVER (PARTITION BY c.cik,c.y ORDER BY EXISTS(SELECT 1 FROM nt_raw_tokens t WHERE t.y=c.y AND t.cik=c.cik AND t.accession=c.accession) DESC,c.filing_date DESC,c.accession DESC)=1")
    con.execute('CREATE TABLE nt_tokens AS SELECT t.* FROM nt_raw_tokens t JOIN nt_peer_candidates c USING(y,cik,accession)')
    con.execute("CREATE TABLE nt_common AS SELECT y,word FROM nt_tokens GROUP BY y,word HAVING count(DISTINCT accession) > .25*(SELECT count(DISTINCT accession) FROM nt_tokens a WHERE a.y=nt_tokens.y)")
    con.execute("CREATE TABLE nt_kept AS SELECT t.* FROM nt_tokens t WHERE NOT EXISTS (SELECT 1 FROM nt_common p WHERE p.y=CASE WHEN t.y=2018 THEN 2018 ELSE t.y-1 END AND p.word=t.word)")
    con.execute("CREATE TABLE nt_encoded AS SELECT c.y,c.cik,c.accession,c.available_at,coalesce(list(t.word ORDER BY t.word) FILTER(WHERE t.word IS NOT NULL),[]::VARCHAR[]) vocabulary FROM nt_peer_candidates c LEFT JOIN nt_kept t ON t.y=c.y AND t.cik=c.cik GROUP BY ALL")
    con.execute("CREATE TABLE nt_pairs(year INTEGER,cik BIGINT,peer_cik BIGINT,accession VARCHAR,peer_accession VARCHAR,score DOUBLE,available_at TIMESTAMPTZ,threshold DOUBLE,peer_basis VARCHAR,corpus_sha256 VARCHAR)")
    con.execute("CREATE TABLE nt_years(year INTEGER,calibrated_on INTEGER,threshold DOUBLE,available_at TIMESTAMPTZ,corpus_sha256 VARCHAR,calibration_reason VARCHAR,docs BIGINT)")
    con.execute('CREATE TABLE nt_best(year INTEGER,cik BIGINT,best_score DOUBLE)')
    previous_hist=previous_share=previous_clock=previous_digest=None
    prior_raw_clock=prior_raw_digest=None
    for year in range(2018,2027):
        hist=np.zeros(T.HIST_BINS,dtype=np.int64)
        corpus=[]
        cursor=con.cursor().execute("SELECT c.cik,c.accession,c.available_at,coalesce(e.vocabulary,[]::VARCHAR[]),s.sic,s.available_at,c.business_vocabulary FROM nt_full_cohort c LEFT JOIN nt_encoded e ON c.y=e.y AND c.cik=e.cik AND c.accession=e.accession LEFT JOIN LATERAL (SELECT * FROM nt_sic s WHERE s.cik=c.cik AND s.available_at < make_date(c.y+1,1,1) ORDER BY available_at DESC LIMIT 1) s ON true WHERE c.y=? ORDER BY c.cik,c.accession",[year])
        sic=[]
        clocks=[]
        closure=con.execute('SELECT complete,available_at,source_sha256 FROM nt_source_closure WHERE y=?',[year]).fetchone()
        missing=not closure or not closure[0] or not closure[1] or bool(con.execute("SELECT count(*) FROM nt_full_cohort WHERE y=? AND (reason IS NOT NULL OR business_reason IS NOT NULL OR available_at IS NULL)",[year]).fetchone()[0])
        source_missing=missing
        raw_clocks=[]
        if closure and closure[1]:
            clocks.append(closure[1])
            raw_clocks.append(closure[1])
        while row:=cursor.fetchone():
            cik,accession,clock,vocabulary,industry,sic_clock,raw_vocabulary=row
            corpus.append(N.digest([cik,accession,clock,vocabulary,raw_vocabulary,industry,sic_clock]))
            if clock:
                raw_clocks.append(clock)
            eligible=len(vocabulary)>=T.MIN_WORDS
            if eligible:
                sic.append(industry)
            valid_sic=industry and len(industry)>=3 and industry[:3].isdigit() and industry!='0000'
            if not clock or (eligible and (not sic_clock or not valid_sic)):
                missing=True
            else:
                clocks.append(clock)
                if eligible:
                    clocks.append(sic_clock)
        share,_=T.sic3_pair_share(sic)
        # Latest selection is known only at the receipt-authenticated full
        # original census snapshot, whose clock participates here. Immutable
        # generations preserve older snapshot vintages; no early backfill.
        corpus_clock=max(clocks) if clocks and not missing else None
        corpus_digest=N.digest([closure,corpus])
        tau=T.threshold_from_hist(previous_hist,previous_share) if previous_hist is not None and previous_share is not None and previous_clock else None
        filter_clock=corpus_clock if year==2018 else prior_raw_clock
        encoded_clock=max(corpus_clock,filter_clock) if corpus_clock and filter_clock else None
        encoded_digest=N.digest([corpus_digest,corpus_digest if year==2018 else prior_raw_digest])
        available=max(encoded_clock,previous_clock) if encoded_clock and previous_clock else None
        reason=None if tau is not None and available else 'prior_year_corpus_or_SIC_clock_unqualified'
        combined=N.digest([previous_digest,encoded_digest])
        top={}
        for a,b,aa,bb,score,pair_clock in _pairs(con,year):
            hist[min(T.HIST_BINS-1,int(score*T.HIST_BINS))]+=1
            if score>0:
                for x,z,xa,za in ((a,b,aa,bb),(b,a,bb,aa)):
                    candidates=top.setdefault(x,[])
                    candidates.append((score,z,xa,za))
                    candidates.sort(key=lambda r:(-r[0],r[1]))
                    del candidates[T.FALLBACK_K:]
                if len(top)>100_000:
                    raise ValueError('bounded nearest-peer metadata exceeds100000issuers')
            if year>=2019 and tau is not None and available and score>tau:
                for x,z,xa,za in ((a,b,aa,bb),(b,a,bb,aa)):
                    con.execute("INSERT INTO nt_pairs VALUES (?,?,?,?,?,?,?,?,?,?)",[year,x,z,xa,za,score,max(available,pair_clock),tau,'reproduced_tnic3_df25_sic3',combined])
        for cik,candidates in sorted(top.items()):
            con.execute('INSERT INTO nt_best VALUES (?,?,?)',[year,cik,candidates[0][0]])
        if year>=2019 and tau is not None and available:
            strict={r[0] for r in con.execute('SELECT DISTINCT cik FROM nt_pairs WHERE year=? AND peer_basis=?',[year,'reproduced_tnic3_df25_sic3']).fetchall()}
            for cik,candidates in sorted(top.items()):
                if cik not in strict:
                    for score,peer,accession,peer_accession in candidates:
                        con.execute('INSERT INTO nt_pairs VALUES (?,?,?,?,?,?,?,?,?,?)',[year,cik,peer,accession,peer_accession,score,available,tau,T.FALLBACK_BASIS,combined])
        con.execute("INSERT INTO nt_years VALUES (?,?,?,?,?,?,?)",[year,year-1,tau,available,combined,reason,len(corpus)])
        previous_hist,previous_share,previous_clock,previous_digest=hist,share,encoded_clock,encoded_digest
        prior_raw_clock=max(raw_clocks) if raw_clocks and not source_missing else None
        prior_raw_digest=corpus_digest
    con.execute("CREATE TABLE nt_peer_firms AS SELECT l.y AS year,l.cik,c.accession,c.filing_date,c.available_at filing_available_at,CASE WHEN c.reason IS NULL AND c.business_reason IS NULL THEN len(e.vocabulary) ELSE NULL END vocabulary_words,y.threshold,CASE WHEN y.calibration_reason IS NULL THEN count(p.peer_cik) FILTER(WHERE p.peer_basis='reproduced_tnic3_df25_sic3') ELSE NULL END n_peers,CASE WHEN y.calibration_reason IS NULL THEN count(p.peer_cik) FILTER(WHERE p.peer_basis='reproduced_tnic3_df25_sic3' AND p.available_at<=c.available_at) ELSE NULL END n_peers_own_clock,CASE WHEN y.calibration_reason IS NULL THEN count(p.peer_cik) FILTER(WHERE p.peer_basis='nearest5_fallback') ELSE NULL END n_fallback_peers,CASE WHEN y.calibration_reason IS NULL THEN coalesce(sum(p.score) FILTER(WHERE p.peer_basis='reproduced_tnic3_df25_sic3'),0) ELSE NULL END total_similarity,CASE WHEN y.calibration_reason IS NULL THEN coalesce(b.best_score,0) ELSE NULL END best_score,max(y.available_at) available_at,y.calibration_reason reason,'reproduced_tnic3_df25_sic3' peer_basis,'historical_SIC_source_and_full_original_census_snapshot' vintage_risk FROM (SELECT DISTINCT year(f.filing_date) y,f.cik FROM nt_inventory f JOIN nt_links l ON l.cik=f.cik AND l.y=year(f.filing_date) WHERE f.form IN ("+FT._quote_list(FT.FORMS_10K)+")) l LEFT JOIN nt_pairs p ON p.year=l.y AND p.cik=l.cik JOIN nt_years y ON y.year=l.y JOIN nt_peer_candidates c ON c.y=l.y AND c.cik=l.cik JOIN nt_encoded e ON e.y=l.y AND e.cik=l.cik LEFT JOIN nt_best b ON b.year=l.y AND b.cik=l.cik GROUP BY l.y,l.cik,c.accession,c.filing_date,c.available_at,e.vocabulary,c.reason,c.business_reason,y.threshold,y.calibration_reason,b.best_score")
    for name,table,order in (('pairs','nt_pairs','year,cik,peer_cik'),('firms','nt_peer_firms','year,cik'),('years','nt_years','year')):
        C.copy_to_parquet(con,f"SELECT * FROM {table} ORDER BY {order}",output/f"classification_tnic/{name}.parquet",row_group_size=4096)
    return {"name":"reproduced_tnic3_df25_sic3","authoritative_external_tnic":False,
            "scope":"original_10k_filers;prior_year_DF25;prior_year_SIC3_calibration;MIN_WORDS50;strict_score_gt_threshold",
            "clock":"max_both_filings_prior_and_current_complete_original_census_corpus_SIC_observed_vintage",
            "qualification":"legacy_reproduction_separate_from_official_TNIC;fallback_excluded_from_original_gate"}
