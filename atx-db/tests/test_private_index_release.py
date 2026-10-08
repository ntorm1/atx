import datetime as dt
from pathlib import Path
import pytest
from atx_db.alpha_panel import private_index_release as R
from atx_db.alpha_panel import private_index_pit as P


def test_available_year_selection_never_opens_2024_file(tmp_path,monkeypatch):
    products={name:[] for name in R.PRODUCTS}
    products['exits']=[{'index_id':'R2000P','security_id':1,'recon_date':dt.date(2022,6,24),
        'last_session':dt.date(year,1,2),'available_at':P.mark(dt.date(year,1,3)),
        'exit_basis':'explicit_delisting_date'} for year in (2023,2024)]
    root=tmp_path/'candidate'
    manifest=R.write_candidate(root,products,source_binding={'fixture_only':True},receipt={},gates={'pass':False})
    manifest_pin=R.pin(root/'manifest.json')
    original=R.check
    opened=[]
    def tracked(path,expected):
        opened.append(str(path))
        assert 'available_year=2024' not in str(path)
        return original(path,expected)
    monkeypatch.setattr(R,'check',tracked)
    cutoff=dt.datetime(2024,1,1,tzinfo=dt.timezone.utc)
    with R.consuming_files(root,manifest_pin,'exits',cutoff) as paths:
        assert len(paths)==1 and 'available_year=2023' in str(paths[0])
    assert opened and all('available_year=2024' not in p for p in opened)
    assert manifest['status']=='diagnostic_pending_independent_replay'


def test_consumed_file_mutation_is_rejected_after_read(tmp_path):
    products={name:[] for name in R.PRODUCTS}
    products['exits']=[{'index_id':'R2000P','security_id':1,'recon_date':dt.date(2022,6,24),
        'last_session':dt.date(2023,1,2),'available_at':P.mark(dt.date(2023,1,3)),
        'exit_basis':'explicit_delisting_date'}]
    root=tmp_path/'candidate'
    R.write_candidate(root,products,source_binding={'fixture_only':True},receipt={},gates={'pass':False})
    with pytest.raises(ValueError,match='physical index byte identity'):
        with R.consuming_files(root,R.pin(root/'manifest.json'),'exits',
                dt.datetime(2024,1,1,tzinfo=dt.timezone.utc)) as paths:
            with paths[0].open('ab') as stream:
                stream.write(b'tamper')


def test_every_output_requires_house_clock_and_generation_is_immutable(tmp_path):
    products={name:[] for name in R.PRODUCTS}
    root=tmp_path/'candidate'
    R.write_candidate(root,products,source_binding={},receipt={},gates={})
    with pytest.raises(FileExistsError):
        R.write_candidate(root,products,source_binding={},receipt={},gates={})
    products['membership']=[{'available_at':dt.datetime(2023,1,1)}]
    with pytest.raises(ValueError,match='explicit UTC clock'):
        R.write_candidate(tmp_path/'invalid',products,source_binding={},receipt={},gates={})


def test_full_typed_replay_rejects_adjacent_manifest_row_forgery(tmp_path):
    import duckdb
    import pyarrow as pa
    import pyarrow.parquet as pq
    products={name:[] for name in R.PRODUCTS}
    row={'index_id':'R2000P','security_id':1,'recon_date':dt.date(2022,6,24),
        'last_session':dt.date(2023,1,2),'available_at':P.mark(dt.date(2023,1,3)),
        'exit_basis':'explicit_delisting_date'}
    products['exits']=[row]
    root=tmp_path/'candidate'
    manifest=R.write_candidate(root,products,source_binding={'fixture_only':True},receipt={},gates={'pass':False})
    con=duckdb.connect(str(tmp_path/'replay.duckdb'))
    con.execute("SET memory_limit='128MB'")
    con.execute('SET threads=1')
    con.execute("SET TimeZone='UTC'")
    try:
        result=R._validate_rowsets(con,root,manifest,products)
        assert result['exits']['rows']==1 and result['exits']['typed_duplicate_preserving_replay']
        key=next(iter(manifest['files']))
        pq.write_table(pa.Table.from_pylist([{**row,'security_id':999}],schema=R.schemas()['exits']),root/key)
        manifest['files'][key].update(R.pin(root/key))
        with pytest.raises(ValueError,match='full typed row population replay differs'):
            R._validate_rowsets(con,root,manifest,products)
    finally:
        con.close()
