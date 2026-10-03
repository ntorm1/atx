import runpy
for name in ('t2_docs.py','t2_market_docs.py','t2_report.py'):
    runpy.run_path('../.superpowers/sdd/tier1-v4/'+name, run_name='__main__')
