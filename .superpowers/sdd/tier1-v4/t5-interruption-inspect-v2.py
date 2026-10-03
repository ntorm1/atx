"""Preserve v1 evidence and repeat byte inspection against reviewed round5 code."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('t5_prior_inspection', HERE / 't5-interruption-inspect.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.OUT = HERE / 'logs/t5-interruption-inspection-v2.json'
with contextlib.redirect_stdout(io.StringIO()):
    module.main()
result = json.loads(module.OUT.read_text())
for path in (module.ROOT / 'atx-db/src/atx_db/alpha_panel/fund_prepare_cache.py', Path(__file__)):
    result['code_sha256'][str(path.relative_to(module.ROOT))] = module.sha(path)
for relative, digest in result['code_sha256'].items():
    if module.sha(module.ROOT / relative) != digest:
        raise RuntimeError('Code changed during inspection: ' + relative)
result['scoped_approved_commit'] = '9df139aa'
result['independent_review'] = 'review-T5-prebuild-r5-findings.md'
result['previous_evidence_preserved'] = 'logs/t5-interruption-inspection-v1.json'
result['safe_recovery'] = ('Preserve interrupted _work at the checked sibling; run fresh root-owned preparation. '
                           'Only authenticated round5 units can support later bounded resume; actual gates remain unmeasured.')
module.OUT.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({key: result[key] for key in ('source_manifest_sha256', 'extract_plan_sha256',
      'extract_members', 'extract_data_bytes', 'extract_identity', 'stage_root_files', 'acceptance',
      'scoped_approved_commit', 'safe_recovery')}))
