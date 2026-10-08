"""Actual native kernels over retained synthetic originals, no warehouse data."""
import dataclasses
import datetime as dt
import json
import pytest
import pyarrow as pa
import pyarrow.parquet as pq
from atx_db.alpha_panel import golden_returns as W, golden_panel as P, golden_sources as S, golden_labels as L
from atx_db.alpha_panel.char_monthly_registry import FREEZE_SHA256
from atx_db.alpha_panel.char_replication import CONFIG_SHA


def fixture(root,dates=None,ticker='ABC',issuer='Issuer'):
    root.mkdir()
    dates=dates or [dt.date(2021,1,4)+dt.timedelta(days=i) for i in range(7)]
    def record(path):return {"path":path.relative_to(root).as_posix(),"identity":S.pin(path),"expected_sha256":S.pin(path)["sha256"]}
    vendor=root/"vendor.parquet"
    pq.write_table(pa.Table.from_pylist([{"securityID":1,"tradingDate":d,"ticker_tk":ticker,"close":10.+i} for i,d in enumerate(dates)],schema=pa.schema([("securityID",pa.int64()),("tradingDate",pa.date32()),("ticker_tk",pa.string()),("close",pa.float32())])),vendor)
    actions=root/"original-actions.parquet"
    pq.write_table(pa.Table.from_pylist([{"security_id":1,"session_date":d,"split_multiplier":1.,"cash_dividend":0.,"terminal_treatment":"none"} for d in dates]),actions)
    statements=[f"{issuer} common stock trades as {ticker}.","Prices are USD per share.","close is the unadjusted raw traded close.","conversion terms are unchanged with no split or FX conversion.","Effective from 2020-01-01 through 2022-12-31."]
    economic=root/"economic-original.txt";economic.write_text(" ".join(statements))
    economic_pin={"source_id":"economic-original","sha256":S.pin(economic)["sha256"],"issuer_name":issuer,"ticker":ticker,"share_class":"common stock","currency":"USD","unit":"share","basis":"raw-USD-common-share","valid_from":"2020-01-01","valid_to":"2022-12-31","available_at":"2020-01-01T00:00:00+00:00","class_quote":statements[0],"currency_unit_quote":statements[1],"raw_basis_quote":statements[2],"conversion_basis_quote":statements[3],"validity_quote":statements[4],"clock_basis":"retrieval_floor","fetched_at":"2020-01-01T00:00:00+00:00"}
    def item(date):
        return {"session_date":str(date),"vendor_original":record(vendor),"economic_original":record(economic),"economic_pin":economic_pin,
            "vendor_pin":{"source_id":"original-vendor","sha256":S.pin(vendor)["sha256"],"expected_schema":str(pq.ParquetFile(vendor).schema_arrow),"security_id":1,"session_date":str(date),"ticker":ticker,"available_at":str(dt.datetime.combine(date,dt.time(22),dt.UTC)),"economic_source_id":"economic-original"},
            "corporate_action_source":record(actions)}
    proof={"version":"golden_complete_window_v1","registry_sha256":FREEZE_SHA256,"feature_date":str(dates[0]),"security_id":1,"horizon":5,"entry_date":str(dates[1]),"exit_date":str(dates[6]),"start_mark":item(dates[1]),"observations":[item(d) for d in dates[2:]]}
    proof_path=root/"window-proof.json";P.C.write_json_atomic(proof_path,proof)
    inventory=root/"windows.jsonl";inventory.write_text(json.dumps({"feature_date":str(dates[0]),"security_id":1,"horizon":5,"proof_binding":record(proof_path)})+"\n")
    product=lambda path:{"files":{path.relative_to(root).as_posix():S.pin(path)},"source":"synthetic retained original","clock":"native original observation clock","admission":"independent offline fixture review"}
    source=root/"source-contract.json";P.C.write_json_atomic(source,{"version":"char_replication_label_source_v1","registry_sha256":FREEZE_SHA256,"config_sha256":CONFIG_SHA,"convention":"unadjusted_close_split_cash_per_prior_share_v1","currency":"USD","prices":product(vendor),"actions":product(actions)})
    dependencies={str(p.resolve()):S.pin(p) for p in (vendor,actions,economic,proof_path,inventory,source)}
    dependencies.update(W.native_code())
    spec={"version":W.VERSION,"independent_review":"synthetic fixture only","lake_root":str(root),"native_code":W.native_code(),"dependency_pins":dependencies,
          "source_contract":record(source),"task8_config_sha256":CONFIG_SHA,"window_inventories":[record(inventory)]}
    return dates,spec,proof_path


def test_full_native_original_price_action_window_and_missing_proof(tmp_path):
    dates,spec,proof=fixture(tmp_path/"source")
    con=P.connect(tmp_path/"db")
    try:
        native=W.NativeWindows(tmp_path/"source",spec,con)
        result=native.window(dates,0,1,5)
        assert result["reason"]=="qualified_complete_window"
        assert result["value"]==pytest.approx(16/11-1)
        assert native.window(dates,0,2,5)["reason"]=="missing_independent_complete_window_proof"
        assert native.window(dates,0,1,21)["reason"]=="incomplete_calendar_window"
        proof.write_text("changed")
        with pytest.raises(ValueError,match="stale"):native.recheck()
    finally:con.close()


def test_native_window_timing_proof_complete_pins_and_typed_identity(tmp_path):
    dates,spec,proof_path=fixture(tmp_path/"source")
    con=P.connect(tmp_path/"db")
    try:
        bad={**spec,"dependency_pins":{k:v for k,v in spec["dependency_pins"].items() if not k.endswith("original-actions.parquet")}}
        with pytest.raises(ValueError,match="abbreviated"):W.NativeWindows(tmp_path/"source",bad,con)
        proof=P.C.read_json(proof_path);proof["observations"].pop()
        P.C.write_json_atomic(proof_path,proof)
        # Re-pinning the producer row cannot alter the independent original spec.
        with pytest.raises(ValueError,match="stale"):W.NativeWindows(tmp_path/"source",spec,con)
        with pytest.raises(ValueError,match="typed"):
            W.typed(W.MP.VendorPricePin,{"security_id":1})
    finally:con.close()


def test_native_label_writer_complete_denominator_and_full_return_replay(tmp_path):
    dates,spec,proof=fixture(tmp_path/"source")
    con=P.connect(tmp_path/"db")
    try:
        con.execute("CREATE TABLE gc(session_date DATE)")
        con.executemany("INSERT INTO gc VALUES (?)",[(d,) for d in dates])
        con.execute("CREATE TABLE gu(session_date DATE,security_id BIGINT,univ_all_common BOOLEAN,univ_r3000p BOOLEAN)")
        con.executemany("INSERT INTO gu VALUES (?,1,true,false)",[(d,) for d in dates])
        native=W.NativeWindows(tmp_path/"source",spec,con)
        first,second=tmp_path/"labels-first.parquet",tmp_path/"labels-second.parquet"
        L.write_native_year(con,L.diagnostic_query(2021),first,native)
        L.write_native_year(con,L.diagnostic_query(2021),second,native)
        result=con.execute(f"SELECT count(*),count(value),sum(label_qualified::INTEGER) FROM {P.sql([first])}").fetchone()
        assert result==(21,1,1) # every7feature-session x3horizon retained
        difference=con.execute(f"SELECT count(*) FROM ((SELECT * FROM {P.sql([first])} EXCEPT ALL SELECT * FROM {P.sql([second])}) UNION ALL (SELECT * FROM {P.sql([second])} EXCEPT ALL SELECT * FROM {P.sql([first])}))").fetchone()[0]
        assert difference==0
        assert con.execute(f"SELECT value FROM {P.sql([first])} WHERE value IS NOT NULL").fetchone()[0]==pytest.approx(16/11-1)
    finally:con.close()


def test_source_shaped_native_cash_conversion_reaches_physical_label_population(tmp_path):
    import runpy
    from pathlib import Path
    terminal_fixture=runpy.run_path(str(Path(__file__).with_name('test_mna_exact_v2.py')))['fixture']
    from atx_db.alpha_panel import mna_exact_v2 as MX
    dates=[dt.date(2020,1,1)+dt.timedelta(days=index) for index in range(7)]
    root=tmp_path/'source';dates,spec,proof_path=fixture(root,dates,ticker='ACME',issuer='Acme')
    def record(path):return {'path':path.relative_to(root).as_posix(),'identity':S.pin(path),'expected_sha256':S.pin(path)['sha256']}
    vendor=root/'vendor.parquet';table=pq.read_table(vendor)
    pq.write_table(table.set_column(table.schema.get_field_index('close'),'close',pa.array([10.]*len(dates),type=pa.float32())),vendor)
    action=root/'original-actions.parquet'
    pq.write_table(pa.Table.from_pylist([{'security_id':1,'session_date':date,'split_multiplier':1.,'cash_dividend':0.,
        'terminal_treatment':'cash_conversion' if date==dates[3] else 'none'} for date in dates]),action)
    args,_,expected=terminal_fixture()
    candidate,document,docpin,terminal_vendor,target_pin,economic,economic_pin=args
    original=root/'terminal-original.txt';original.write_bytes(document)
    terminal_path=root/'terminal-vendor.parquet';terminal_path.write_bytes(terminal_vendor)
    economic_path=root/'terminal-economic.txt';economic_path.write_bytes(economic)
    proof=P.C.read_json(proof_path)
    for item in [proof['start_mark'],*proof['observations']]:
        item['vendor_original']=record(vendor);item['vendor_pin']['sha256']=S.pin(vendor)['sha256']
        item['corporate_action_source']=record(action)
    proof['observations'][1]['terminal_native']={
        'document_original':record(original),'target_economic_original':record(economic_path),'vendor_original':record(terminal_path),
        'candidate':MX._canonical(candidate),'document_pin':MX._canonical(docpin),'target_pin':MX._canonical(target_pin),
        'target_economic_pin':MX._canonical(economic_pin),'expected_exact':expected}
    for index in range(2,5):
        date=dates[index+2];cash=root/f'cash-{date}.json'
        observation={'session_date':str(date),'security_id':1,'currency':'USD','cash_factor':1.01,
            'available_at':dt.datetime.combine(date,dt.time(20),dt.UTC).isoformat()}
        P.C.write_json_atomic(cash,observation)
        admission=root/f'cash-proof-{date}.json'
        P.C.write_json_atomic(admission,{'version':'golden_residual_cash_observation_v1','registry_sha256':FREEZE_SHA256,'original':record(cash),'observation':observation})
        proof['observations'][index]['residual_cash']=record(admission)
    P.C.write_json_atomic(proof_path,proof)
    inventory=root/'windows.jsonl';inventory.write_text(json.dumps({'feature_date':str(dates[0]),'security_id':1,'horizon':5,'proof_binding':record(proof_path)})+'\n')
    source=root/'source-contract.json';contract=P.C.read_json(source)
    contract['prices']['files']={record(path)['path']:S.pin(path) for path in (vendor,terminal_path)}
    contract['actions']['files']={record(action)['path']:S.pin(action)};P.C.write_json_atomic(source,contract)
    spec['source_contract']=record(source);spec['window_inventories']=[record(inventory)]
    spec['dependency_pins']={str(path.resolve()):S.pin(path) for path in root.iterdir() if path.is_file()}
    spec['dependency_pins'].update(W.native_code())
    con=P.connect(tmp_path/'db')
    try:
        con.execute('CREATE TABLE gc(session_date DATE)');con.executemany('INSERT INTO gc VALUES(?)',[(date,) for date in dates])
        con.execute('CREATE TABLE gu(session_date DATE,security_id BIGINT,univ_all_common BOOLEAN,univ_r3000p BOOLEAN)')
        con.executemany('INSERT INTO gu VALUES(?,1,true,true)',[(date,) for date in dates])
        windows=W.NativeWindows(root,spec,con)
        path=tmp_path/'labels.parquet';L.write_native_year(con,L.diagnostic_query(2020),path,windows)
        assert con.execute('SELECT count(*) FROM '+P.sql([path])).fetchone()==(21,)
        value,reason=con.execute('SELECT value,reason FROM '+P.sql([path])+' WHERE feature_date=? AND horizon=5',[dates[0]]).fetchone()
        assert reason=='qualified_complete_window' and value==pytest.approx(1.2*1.01**3-1)
        assert windows.window(dates,0,1,5)['delisting_adjusted'] is True
        (root/'cash-2020-01-05.json').write_text('changed original')
        with pytest.raises(ValueError,match='stale'):windows.recheck()
    finally:con.close()
