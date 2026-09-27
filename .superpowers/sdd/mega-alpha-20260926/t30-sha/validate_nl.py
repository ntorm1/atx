import json, sys, glob, subprocess
sys.path.insert(0, sys.argv[1])
import nljson
root = sys.argv[2]
files = subprocess.run(['git', '-C', root, 'ls-files'], capture_output=True, text=True).stdout.split('\n')
files = [f for f in files if f.endswith('recipe.json') or f.endswith('summary.json')]
ok = bad = 0
for f in files:
    raw = open(root + '/' + f, 'rb').read().decode('utf-8')
    if '\r\n' in raw: raw = raw.replace('\r\n', '\n')
    try:
        v = json.loads(raw)
    except Exception as e:
        print('parse fail', f, e); continue
    out = nljson.dump(v) + '\n'
    if out == raw: ok += 1
    else:
        bad += 1
        # first difference
        i = next((j for j in range(min(len(out), len(raw))) if out[j] != raw[j]), min(len(out), len(raw)))
        print('DIFF', f, repr(raw[max(0,i-60):i+40]), '||', repr(out[max(0,i-60):i+40]))
print('ok', ok, 'bad', bad)
