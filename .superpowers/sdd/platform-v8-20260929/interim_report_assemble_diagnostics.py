"""Assemble B0c's diagnostics G-1..G-3 into the one ``diagnostics-v8.json`` the report block reads (Ruling PM6-11,
interim render 3). COPY ONLY: E-18 ran the ten diagnostics as eight split runs (cells batch 1b); this script reads the
eight committed split files, checks each against the SHA-256 recorded in integration-log.md (batch 1b) and checks that
their header (schema, declaration, window_id, tool) is identical, then writes that header once and every
``diagnostics.<id>`` section unchanged, in G-1a..G-3d order, plus an ``assembled_from`` record of the eight sources
(path, SHA-256, ids). No value is typed, computed or altered; after writing, the output is re-read and every section is
checked equal to its source. Usage: interim_report_assemble_diagnostics.py OUT.json"""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path('C:/atx-wt/pool-2')
SRC_DIR = '.superpowers/sdd/platform-v8-20260929/diagnostics-v8'  # committed copies, bytes equal to build-equity/b0c-diagnostics
SOURCES = {  # file -> SHA-256 as cells batch 1b recorded it
    'diagnostics-v8-g1a-g3d.json': 'de1990b8992f9da20344c6bb6c72670de3398cc7f3aba4bf1b8e958280c25cda',
    'diagnostics-v8-g1b-g1c.json': 'a985adf86049b41622cf53e7b7c539d64369efb1fb1bf6e9ed661c5d64b0b548',
    'diagnostics-v8-g2a.json': 'e2a9874bd1fd14fa9c0af4f05837102569b6db8dacc0b045f7d36261fe18598d',
    'diagnostics-v8-g2b.json': '86bf47af96153a124b61e5cd7d3353a38daa5a63ddb3b294b15ee1b2f1c3f866',
    'diagnostics-v8-g2c.json': 'cbed04d7219dca7dfe82c5721b3d88a9d4cd23d9c9d311795e1cb658bbe4e969',
    'diagnostics-v8-g3a.json': 'b44ea5d08efeb8cbf93df8dd75ddb260f100d499ca15f7b89f6c22204473efe4',
    'diagnostics-v8-g3b.json': '19e5e5da96b834b1afd29c40a25de016bef5f8731ebf77f9c68074b7cbebe423',
    'diagnostics-v8-g3c.json': '9f00b5f3afffd2cfdef0deef9fb40cc25f07f7eddbd145be348ec4148a359ee4'}
ORDER = ('G-1a', 'G-1b', 'G-1c', 'G-2a', 'G-2b', 'G-2c', 'G-3a', 'G-3b', 'G-3c', 'G-3d')
HEADER = ('schema', 'declaration', 'window_id', 'tool')


def main(out_rel: str) -> int:
    header, sections, record = None, {}, []
    for name, want in SOURCES.items():
        rel = f'{SRC_DIR}/{name}'
        data = (ROOT / rel).read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        assert sha == want, f'{rel}: sha256 {sha} != batch 1b {want}'
        doc = json.loads(data.decode('utf-8'))
        assert set(doc) == set(HEADER) | {'diagnostics'}, (rel, sorted(doc))
        h = {k: doc[k] for k in HEADER}
        assert header is None or h == header, f'{rel}: header differs from the first split file'
        header = h
        ids = list(doc['diagnostics'])
        for gid in ids:
            assert gid not in sections, f'{gid} in two split files'
            sections[gid] = doc['diagnostics'][gid]
        record.append({'path': rel, 'sha256': sha, 'ids': ids})
    assert sorted(sections) == sorted(ORDER), sorted(sections)
    out = dict(header)
    out['diagnostics'] = {gid: sections[gid] for gid in ORDER}
    out['assembled_from'] = record
    out['assembly'] = {'script': '.superpowers/sdd/platform-v8-20260929/interim_report_assemble_diagnostics.py',
                       'rule': 'copy only (Ruling PM6-11): the header of the eight split files (identical) and each '
                               'diagnostics.<id> section copied unchanged; nothing typed, computed or altered',
                       'cell': 'B0c'}
    p = ROOT / out_rel
    p.parent.mkdir(parents=True, exist_ok=False)
    p.write_text(json.dumps(out, indent=1) + '\n', encoding='utf-8', newline='\n')
    back = json.loads(p.read_text(encoding='utf-8'))
    assert {k: back[k] for k in HEADER} == header and back['diagnostics'] == out['diagnostics']
    for r in record:  # every section equal to its source document's, value for value
        src = json.loads((ROOT / r['path']).read_text(encoding='utf-8'))
        for gid in r['ids']:
            assert back['diagnostics'][gid] == src['diagnostics'][gid], gid
    print(f'{out_rel} {p.stat().st_size} bytes sha256 {hashlib.sha256(p.read_bytes()).hexdigest()}')
    for r in record:
        print(f"  from {r['path']} {r['sha256']} {','.join(r['ids'])}")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
