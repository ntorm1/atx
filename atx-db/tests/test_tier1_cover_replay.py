"""Source-shaped exact cover projection oracles; no actual source access."""
import ast
import datetime as dt
import inspect
import json
from pathlib import Path
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
from atx_db.alpha_panel import cover_page as CP,notes_fetch as N
from atx_db.alpha_panel import private_tier1_cover_replay as P,private_tier1_admission as A


def _sources(root):
    parts=root/'parts';keys=['2019q1','2019q2']
    for key in keys:
        directory=parts/('source='+key);directory.mkdir(parents=True)
        filings=[{'adsh':'0000000001-19-000001','cik':1,'form':'10-K','period':dt.date(2018,12,31),
            'fy':2018,'fp':'FY','filed':dt.date(2019,3,1),'accepted_utc':dt.datetime(2019,3,1,21,tzinfo=dt.timezone.utc),
            'instance':'original-20181231.xml','source':key}]
        if key=='2019q2':filings.append({'adsh':'0000000001-19-000002','cik':1,'form':'8-K',
            'filed':dt.date(2019,4,1),'accepted_utc':None,'source':key})
        schema=N.out_schema('sub',(('accepted_utc',pa.timestamp('us',tz='UTC')),))
        pq.write_table(pa.Table.from_pylist(filings,schema=schema),directory/'sub.parquet')
        text=[]
        for tag,value,dimh,coreg in [('TradingSymbol','ORIG','common',None),
            ('Security12bTitle','Common Stock','common',None),
            ('AuditorName','Original Auditor' if key=='2019q1' else 'Later Ignored Auditor','',None),
            ('TradingSymbol','AFF','affiliate','affiliate'),('EntityCentralIndexKey','2','affiliate','affiliate')]:
            text.append({'adsh':filings[0]['adsh'],'tag':tag,'value':value,'dimh':dimh,'coreg':coreg,
                'ddate':dt.date(2018,12,31),'iprx':1,'source':key})
        pq.write_table(pa.Table.from_pylist(text,schema=N.out_schema('txt')),directory/'txt_dei.parquet')
        numeric=[{'adsh':filings[0]['adsh'],'tag':'EntityCommonStockSharesOutstanding','value':1000.,
            'uom':'shares','dimh':'common','coreg':None,'ddate':dt.date(2018,12,31),'datp':10.,'iprx':1,'source':key}]
        pq.write_table(pa.Table.from_pylist(numeric,schema=N.out_schema('num')),directory/'num_dei.parquet')
        dims=[{'dimhash':'common','segments':'ClassOfStock=CommonClassA;','source':key},
              {'dimhash':'affiliate','segments':'LegalEntityAxis=Affiliate;','source':key}]
        pq.write_table(pa.Table.from_pylist(dims,schema=N.out_schema('dim')),directory/'dim.parquet')
    ledger=root/'receipts.jsonl'
    ledger.write_text('\n'.join(json.dumps(row) for row in [
        {'key':'2019q1','status':'parsed','repost':0},
        {'key':'2019q2','status':'parsed','repost':1},
        {'key':'2019q2','status':'failed','repost':1}])+'\n{torn final record')
    return parts,keys,ledger


def _native_setup(con,parts,keys,ledger):
    con.execute('CREATE TABLE src(source VARCHAR,ord INTEGER)')
    con.executemany('INSERT INTO src VALUES(?,?)',[(key,index) for index,key in enumerate(sorted(keys,key=N.order_key))])
    reposts={key:value.get('repost',0) for key,value in N.S.Ledger(ledger).latest().items() if value.get('status')=='parsed'}
    con.execute('CREATE TABLE rep(source VARCHAR,repost INTEGER)')
    con.executemany('INSERT INTO rep VALUES(?,?)',list(reposts.items()) or [('',0)])
    # Execute the unchanged native literal sub SQL with only its explicit local
    # physical input map supplied. Never call its publisher or change globals.
    tree=ast.parse(inspect.getsource(CP.build))
    for node in ast.walk(tree):
        if isinstance(node,ast.Call) and isinstance(node.func,ast.Attribute) and node.func.attr=='execute' and node.args:
            argument=node.args[0]
            if isinstance(argument,ast.JoinedStr) and any(isinstance(value,ast.Constant) and 'CREATE TABLE sub AS' in str(value.value) for value in argument.values):
                expression=ast.Expression(argument);ast.fix_missing_locations(expression)
                con.execute(eval(compile(expression,'native-cover-setup','eval'),dict(CP.__dict__),
                    {'g':{'sub':(parts/'source=*'/'sub.parquet').as_posix()}}));return
    raise AssertionError('native complete cover sub projection not found')


def test_cover_original_setup_and_all_rows_match_native_oracle(tmp_path):
    parts,keys,ledger=_sources(tmp_path)
    outputs=[]
    for index,setup in enumerate((_native_setup,P._setup)):
        target=tmp_path/(str(index)+'.parquet')
        with duckdb.connect(str(tmp_path/(str(index)+'.duckdb')),config={'memory_limit':'128MB','threads':'1'}) as con:
            con.execute("SET TimeZone='UTC'");setup(con,parts,keys,ledger)
            CP._build_year(con,parts,keys,2019,target)
        outputs.append(target)
    with duckdb.connect(str(tmp_path/'compare.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'");A.compare(con,*outputs)
        sql=A.sql([outputs[1]])
        rows=con.execute('SELECT adsh,security_key,entity_cik,clock_basis,n_data_sets,vintage_risk,auditor_name FROM '+sql).fetchall()
        assert len(rows)==3
        assert sum(row[3]=='filed_plus_46h' for row in rows)==1
        assert all(row[4]==2 and row[6]=='Original Auditor' for row in rows if row[0].endswith('000001'))
        assert not any(row[5] for row in rows)  # last failed receipt is not parsed
        assert any(row[2]==2 for row in rows)
        assert CP.checks(con,outputs[0].as_posix())==CP.checks(con,outputs[1].as_posix())
