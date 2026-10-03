"""Final publication checks, hand-original binding and documentation provenance."""
import runpy,re
from pathlib import Path
from atx_db.alpha_panel import common as C
art=Path('../.superpowers/sdd/tier1-v4')
runpy.run_path(str(art/'t2r4_verify.py'),run_name='__main__')
# Originals already rechecked immediately before this build; bind the new sample without repeating parsing.
binding=C.read_json(art/'t2r4-hand-binding.json')
assert not binding['changed_parser_assertions'],'hand original parser classification changed'
assert binding['sample_file_sha256']==C.sha256_file(C.stage_dir('validation')/'universe_hand_check_sample.json')
assert binding['hand_file_sha256']==C.sha256_file(C.PACKAGE_ROOT/'docs/datasets/universe_hand_check.json')
before=C.read_json(art/'t2r2-after-verification.json');after=C.read_json(art/'t2r4-after-verification.json')
comparison={'all_universe_output_sha_size_unchanged':before['publications']['universe']['actual_files']==after['publications']['universe']['actual_files'],
            'universe_keys_unchanged':before['universe_keys']==after['universe_keys'],
            'hand_sample_file_unchanged':before['hand_sample_sha256']==after['hand_sample_sha256'],
            'hand_document_unchanged':before['hand_judgments_sha256']==after['hand_judgments_sha256'],
            'gates_before':{key:row['pass'] for key,row in before['validation']['gates'].items()},
            'gates_after':{key:row['pass'] for key,row in after['validation']['gates'].items()},
            'final_manifest_sha256':after['manifest_hashes']['universe']}
C.write_json_atomic(art/'t2r4-before-after.json',comparison)
assert all(comparison[key] for key in ('all_universe_output_sha_size_unchanged','universe_keys_unchanged','hand_sample_file_unchanged','hand_document_unchanged'))
assert comparison['gates_before']==comparison['gates_after']
manifest=C.read_json(C.stage_dir('universe')/'manifest.json')
path=C.PACKAGE_ROOT/'docs/datasets/universe.md';text=path.read_text(encoding='utf-8')
body='| input | SHA-256 |\n|---|---|\n'+'\n'.join(f'| {key} | {value} |' for key,value in manifest['input_manifests_sha256'].items())
text=re.sub(r'(## Input manifest provenance\n).*?(?=\n## |\Z)',lambda match:match[1]+'\n'+body+'\n',text,flags=re.S)
path.write_text(text,encoding='utf-8')
print('Final byte/key/sample/hand/gate comparison',comparison,flush=True)

coverage=after['validation']['coverage']
assert len(coverage['fields'])==30
assert all(str(year) in coverage for year in range(2020,2026))
lines=['# Task2 fix round4 annual field availability','', 'Measured by the guarded universe build; source is validation/universe.json. These are presence fractions, not return statistics.','']
for universe in ('univ_all_common','univ_r3000p','member_equity'):
    lines += ['## '+universe,'','| field | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |','|---|---|---|---|---|---|---|']
    for field in coverage['fields']:
        assert all(field in coverage[str(year)][universe] for year in range(2020,2026))
        lines.append('| '+field+' | '+' | '.join(str(coverage[str(year)][universe][field]) for year in range(2020,2026))+' |')
    lines += ['| cells | '+' | '.join(str(coverage[str(year)][universe]['cells']) for year in range(2020,2026))+' |','']
(art/'t2r4-annual-coverage.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
C.write_json_atomic(art/'t2r4-coverage-gates.json',{'manifest_sha256':after['manifest_hashes']['universe'],'validation_sha256':C.sha256_file(C.stage_dir('validation')/'universe.json'),'coverage':coverage,'gates':after['validation']['gates']})
