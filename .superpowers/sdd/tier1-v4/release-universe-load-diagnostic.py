"""Guarded source-read diagnosis only; never publish or alter universe outputs."""
import json
import time
from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import universe as U

name = 'release-universe-load-diagnostic-v1.duckdb'
path = C.build_root() / '_tmp' / name
if path.exists():
    raise RuntimeError('Preserve prior diagnostic database; choose a new version')
con = C.connect(memory='380MB', threads=2, db_file=name)
started = time.monotonic()


class TimedConnection:
    def execute(self, sql, *args, **kwargs):
        label = ' '.join(sql.split()[:6])
        begin = time.monotonic()
        print(json.dumps({'event': 'begin', 'statement': label,
                          'elapsed_s': round(begin - started, 3)}), flush=True)
        result = con.execute(sql, *args, **kwargs)
        print(json.dumps({'event': 'end', 'statement': label,
                          'seconds': round(time.monotonic() - begin, 3)}), flush=True)
        return result


try:
    counts = U.load_issuer_tables(TimedConnection())
    print(json.dumps({'status': 'SOURCE_READ_DIAGNOSTIC_COMPLETE', 'counts': counts,
                      'seconds': round(time.monotonic() - started, 3),
                      'published': False}), flush=True)
finally:
    con.close()
