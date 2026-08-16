import test from 'node:test'
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { readFileSync } from 'node:fs'

const read = path => readFileSync(path, 'utf8')
const workflow = read('.claude/workflows/vol-oracle-iter.js')
const BOOTSTRAP_GATE_COMMANDS = {
  disk: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate disk',
  mode_a_targeted_tests: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_targeted_tests',
  mode_a_smoke: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_smoke',
}
const MODE_A_TARGETS = ['mode_a_price_mae', 'mode_a_vol_mae', 'mode_a_delta_rel', 'mode_a_gamma_rel', 'mode_a_theta_rel', 'mode_a_vega_rel', 'mode_a_rho_rel', 'mode_a_phi_rel', 'mode_a_volga_rel', 'mode_a_vanna_rel', 'mode_a_delta_decay_rel']

function extractFunction(source, name) {
  const start = source.indexOf(`function ${name}(`)
  assert.ok(start >= 0, `missing function ${name}`)
  const brace = source.indexOf('{', start)
  let depth = 0
  for (let index = brace; index < source.length; index += 1) {
    if (source[index] === '{') depth += 1
    if (source[index] === '}' && --depth === 0) return source.slice(start, index + 1)
  }
  throw new Error(`unterminated function ${name}`)
}

const functionNames = [
  'expectedBootstrapMetricIds', 'iterationCommandError', 'validSuccessEvidence', 'diagnosticsUseForbiddenCommand',
  'bootstrapCommandError', 'validBootstrapSuccessEvidence', 'validBootstrapDiagnostics',
  'validLeaseReceipt', 'validGateReceipt', 'validAdoptionReceipt', 'validChangedPathReceipt', 'validPrecheckGateReceipt',
  'bootstrapPathError', 'bootstrapLeaseIdentityError', 'unwrapBootstrapReport', 'bootstrapReportError',
]
const declarations = functionNames.map(name => extractFunction(workflow, name)).join('\n')
const validators = Function(`
  const ADOPTION_COMMAND = 'powershell scripts\\\\oracle-adopt-existing-data.ps1';
  const MODE_A_RECEIPT_ONLY_PATHS = ['atx-vol/bench/oracle/bootstrap/mode-a.json'];
  const BOOTSTRAP_GATE_COMMANDS = ${JSON.stringify(BOOTSTRAP_GATE_COMMANDS)};
  const TARGETED_BOOTSTRAP_GATE_IDS = ['mode_a_targeted_tests', 'mode_a_smoke'];
  const TARGET_REGISTRY = ${JSON.stringify(MODE_A_TARGETS.map(metric_id => ({ metric_id, mode: 'A' })))};
  ${declarations}; return { ${functionNames.join(',')} }
`)()

const bootstrapSchemaSource = workflow.slice(workflow.indexOf('const EVIDENCE_ITEM ='), workflow.indexOf('const REVIEW ='))
const bootstrapReportSchema = Function(`
  const BOOTSTRAP_GATE_COMMANDS = ${JSON.stringify(BOOTSTRAP_GATE_COMMANDS)};
  const READY_MEASURE_GATES = {};
  const RATCHET_GATE_IDS = [];
  const ADOPTION_COMMAND = ${JSON.stringify('powershell scripts\\oracle-adopt-existing-data.ps1')};
  ${bootstrapSchemaSource}; return BOOTSTRAP_REPORT_TOOL_SCHEMA
`)()

function schemaErrors(schema, value, path = '$') {
  if (schema.anyOf) {
    const branches = schema.anyOf.map(item => schemaErrors(item, value, path))
    return branches.some(errors => errors.length === 0) ? [] : [`${path} must match one branch: ${JSON.stringify(branches)}`]
  }
  const errors = []
  if (schema.type === 'object') {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return [`${path} must be object`]
    for (const key of schema.required || []) if (!Object.prototype.hasOwnProperty.call(value, key)) errors.push(`${path}.${key} required`)
    if (schema.additionalProperties === false) for (const key of Object.keys(value)) if (!Object.prototype.hasOwnProperty.call(schema.properties || {}, key)) errors.push(`${path}.${key} unexpected`)
    for (const [key, child] of Object.entries(schema.properties || {})) if (Object.prototype.hasOwnProperty.call(value, key)) errors.push(...schemaErrors(child, value[key], `${path}.${key}`))
  } else if (schema.type === 'array') {
    if (!Array.isArray(value)) return [`${path} must be array`]
    if (schema.minItems !== undefined && value.length < schema.minItems) errors.push(`${path} has too few items`)
    value.forEach((item, index) => errors.push(...schemaErrors(schema.items, item, `${path}[${index}]`)))
  } else if (schema.type === 'string') {
    if (typeof value !== 'string') return [`${path} must be string`]
    if (schema.minLength !== undefined && value.length < schema.minLength) errors.push(`${path} is too short`)
    if (schema.pattern && !new RegExp(schema.pattern).test(value)) errors.push(`${path} does not match pattern`)
  } else if (schema.type === 'integer' && !Number.isInteger(value)) errors.push(`${path} must be integer`)
  if (schema.enum && !schema.enum.includes(value)) errors.push(`${path} is outside enum`)
  if (Object.prototype.hasOwnProperty.call(schema, 'const') && schema.const !== value) errors.push(`${path} differs from const`)
  return errors
}

const sha = char => char.repeat(40)
const digest = char => char.repeat(64)
function gateReceipt(gateId, observations = 20) {
  const common = { schema_version: 1, status: 'PASS', observations, command_id: gateId, raw_output_sha256: digest('f') }
  const semantic = { ...common, tested_sha: sha('a'), tested_tree: sha('b') }
  const result = gateId === 'disk' ? common : gateId.endsWith('_tests')
    ? { ...semantic, gate_kind: 'ctest', tests_executed: 2, tests_passed: 2, rows_processed: 0, metric_ids: [], audit_summary: 'tests_executed=2 tests_passed=2' }
    : { ...semantic, gate_kind: 'oracle_bench', tests_executed: 0, tests_passed: 0, rows_processed: 3, metric_ids: MODE_A_TARGETS, audit_summary: `status=PASS rows_processed=3 metric_ids=${[...MODE_A_TARGETS].sort().join(',')}` }
  return { gate_id: gateId, command: BOOTSTRAP_GATE_COMMANDS[gateId], exit_code: 0, output: JSON.stringify(result), result }
}
function precheck(gateId, status = 'PASS') {
  if (status === 'FAIL') return { gate_id: gateId, command: BOOTSTRAP_GATE_COMMANDS[gateId], status, exit_code: 1, output: 'targeted failure' }
  const gate = gateReceipt(gateId)
  return { gate_id: gateId, command: gate.command, status, exit_code: 0, output: gate.output }
}
function changedPathReceipt(base, tested, paths) {
  return { base_sha: base, tested_sha: tested, command: `git diff --name-only ${base}...${tested}`, exit_code: 0, output: paths.join('\n'), paths }
}
function adoptedReceipt(base) {
  const result = { schema_version: 1, status: 'ADOPTED', command_id: 'oracle_existing_store_adoption', base_sha: base, manifest_sha256: digest('a'), holdout_membership_sha256: digest('b'), total_rows: 9, bucket_count: 3, parquet_files: 3, cohort_underlier_count: 3 }
  return { command: 'powershell scripts\\oracle-adopt-existing-data.ps1', exit_code: 0, output: JSON.stringify(result), result }
}
function ingestRequiredReceipt() {
  const result = { schema_version: 1, status: 'INGEST_REQUIRED', command_id: 'oracle_existing_store_adoption', reason: 'store_validation' }
  return { command: 'powershell scripts\\oracle-adopt-existing-data.ps1', exit_code: 0, output: JSON.stringify(result), result }
}
function completeReport(pathReport, state) {
  const leaseName = 'pool-1'
  const runId = 'run-1'
  const branch = `lane/oracle-${state}`
  const heartbeatId = 'heartbeat-1'
  const keeperPid = 1234
  const keeperStarted = '2026-08-15T12:00:00Z'
  const keeperReady = '2026-08-15T12:00:01Z'
  const worktree = 'C:\\atx-wt\\pool-1'
  return {
    ...pathReport,
    outcome: 'DONE', state, branch, lease_name: leaseName, lease_run_id: runId, heartbeat_id: heartbeatId,
    keeper_pid: keeperPid, keeper_process_started_utc: keeperStarted, worktree,
    deviations: pathReport.deviations ?? '',
    acquisition_receipt: {
      action: 'acquire', lease_name: leaseName, run_id: runId, branch, base_sha: pathReport.base_sha, worktree,
      heartbeat_id: heartbeatId, keeper_pid: keeperPid, keeper_process_started_utc: keeperStarted,
      keeper_ready_utc: keeperReady, exit_code: 0, output: `${leaseName} ${runId} ${keeperPid} ${heartbeatId} ${keeperReady}`,
    },
  }
}
function expectedFor(report) {
  return { state: report.state, branch: report.branch, base_sha: report.base_sha, run_id: report.lease_run_id, heartbeat_id: report.heartbeat_id }
}

test('Stage 1 is adoption-first and Stage 2 is receipt-first with targeted gates', () => {
  const stage1 = workflow.match(/missing_data:\s*\{[^\n]+/u)?.[0] || ''
  const stage2 = workflow.match(/missing_mode_a:\s*\{[^\n]+/u)?.[0] || ''
  assert.match(stage1, /oracle-adopt-existing-data\.ps1/u)
  assert.match(stage1, /ADOPTED/u)
  assert.match(stage1, /INGEST_REQUIRED/u)
  assert.match(stage1, /gate_ids: \['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'\]/u)
  assert.doesNotMatch(stage1, /gate_ids: \[[^\]]*'disk'/u)
  assert.match(stage2, /targeted Mode A gates first/u)
  assert.match(stage2, /write only bootstrap\/mode-a\.json/u)
  assert.match(stage2, /Implement\/fix Mode A only when an exact targeted gate proves it necessary/u)
})

test('adoption contract is footer validation plus one aggregate-only underlier projection', () => {
  const adoption = read('scripts/oracle-adopt-existing-data.ps1')
  const metadata = read('atx-vol/scripts/oracle_store_metadata.py')
  const preflight = read('scripts/oracle-bootstrap-preflight.ps1')
  assert.match(adoption, /Get-CommittedJson/u)
  assert.match(adoption, /Test-Disjoint/u)
  assert.match(adoption, /holdout_membership_sha256/u)
  assert.doesNotMatch(adoption, /atx-vol-oracle-bench/u)
  assert.match(metadata, /parquet_file\.metadata/u)
  assert.match(metadata, /pc\.any\(pc\.equal\(/u)
  assert.doesNotMatch(metadata, /read_table|read_pandas|to_pandas|to_pylist|to_pydict|to_numpy|to_batches|iter_batches|scan_parquet|ParquetDataset|pc\.(?:unique|filter|take|value_counts)|\.(?:flatten|combine_chunks|dictionary_decode)\b/u)
  assert.match(preflight, /'aggregate_store'/u)
})

test('Python AST allowlist rejects alternate constructors, aliases, dynamic, and extra-column reads', () => {
  const result = spawnSync('python', [
    'scripts/tests/oracle_store_projection_guard.py',
    'atx-vol/scripts/oracle_store_metadata.py',
  ], { encoding: 'utf8' })
  assert.equal(result.status, 0, result.stderr || result.stdout)
  assert.deepEqual(JSON.parse(result.stdout), {
    schema_version: 1,
    status: 'PASS',
    production_constructors_allowed: 1,
    production_calls_allowed: 1,
    direct_api_attacks_rejected: 4,
    shape_attacks_rejected: 3,
    bypass_attacks_rejected: 6,
  })
})

test('Stage 1 typed branching rejects missing disk, generic evidence, and contradictory paths', () => {
  const base = sha('b'); const tested = sha('c')
  const adopted = adoptedReceipt(base)
  const adoptionPaths = ['atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256']
  const report = { base_sha: base, sha: tested, holdout_digest_receipt: adopted.result.holdout_membership_sha256, bootstrap_path: 'data_adoption', adoption_receipt: adopted, evidence: [{ command: adopted.command, exit_code: 0, output: adopted.output }], changed_path_receipt: changedPathReceipt(base, tested, adoptionPaths) }
  assert.equal(validators.bootstrapPathError(report, { state: 'missing_data', base_sha: base }), null)

  const required = ingestRequiredReceipt()
  const ingest = { ...report, bootstrap_path: 'data_ingest', adoption_receipt: required, evidence: [{ command: required.command, exit_code: 0, output: required.output }, { command: 'python atx-vol/scripts/oracle_ingest.py --zip licensed.zip', exit_code: 0, output: 'aggregate manifest written' }] }
  assert.match(validators.bootstrapPathError(ingest, { state: 'missing_data', base_sha: base }), /disk receipt/u)
  ingest.disk_receipt = gateReceipt('disk', 20)
  ingest.evidence.push({ command: BOOTSTRAP_GATE_COMMANDS.disk, exit_code: 0, output: ingest.disk_receipt.output })
  assert.equal(validators.bootstrapPathError(ingest, { state: 'missing_data', base_sha: base }), null)
  ingest.evidence[0] = { command: 'generic adoption', exit_code: 0, output: 'PASS' }
  assert.match(validators.bootstrapPathError(ingest, { state: 'missing_data', base_sha: base }), /missing\/untyped/u)

  const contradictory = structuredClone(report)
  contradictory.disk_receipt = gateReceipt('disk', 20)
  contradictory.evidence.push({ command: BOOTSTRAP_GATE_COMMANDS.disk, exit_code: 0, output: contradictory.disk_receipt.output })
  assert.match(validators.bootstrapPathError(contradictory, { state: 'missing_data', base_sha: base }), /contradicted/u)
})

test('Stage 2 precheck receipts mechanically select receipt-only or implementation paths', () => {
  const base = sha('d'); const tested = sha('e')
  const receiptPath = ['atx-vol/bench/oracle/bootstrap/mode-a.json']
  const passing = { base_sha: base, sha: tested, bootstrap_path: 'mode_a_receipt_only', evidence: [], precheck_gate_receipts: [precheck('mode_a_targeted_tests'), precheck('mode_a_smoke')], changed_path_receipt: changedPathReceipt(base, tested, receiptPath) }
  assert.equal(validators.bootstrapPathError(passing, { state: 'missing_mode_a', base_sha: base }), null)
  const wrongGateIdentity = structuredClone(passing)
  const parsedIdentity = JSON.parse(wrongGateIdentity.precheck_gate_receipts[0].output)
  parsedIdentity.tested_sha = 'not-a-sha'
  wrongGateIdentity.precheck_gate_receipts[0].output = JSON.stringify(parsedIdentity)
  assert.match(validators.bootstrapPathError(wrongGateIdentity, { state: 'missing_mode_a', base_sha: base }), /precheck receipt invalid/iu)
  const incompleteMetrics = structuredClone(passing)
  const parsedMetrics = JSON.parse(incompleteMetrics.precheck_gate_receipts[1].output)
  parsedMetrics.metric_ids = parsedMetrics.metric_ids.slice(0, -1)
  parsedMetrics.audit_summary = `status=PASS rows_processed=${parsedMetrics.rows_processed} metric_ids=${[...parsedMetrics.metric_ids].sort().join(',')}`
  incompleteMetrics.precheck_gate_receipts[1].output = JSON.stringify(parsedMetrics)
  assert.match(validators.bootstrapPathError(incompleteMetrics, { state: 'missing_mode_a', base_sha: base }), /precheck receipt invalid/iu)
  passing.changed_path_receipt = changedPathReceipt(base, tested, [...receiptPath, 'atx-vol/tools/oracle_bench_main.cpp'].sort())
  assert.match(validators.bootstrapPathError(passing, { state: 'missing_mode_a', base_sha: base }), /receipt-only/u)

  const implementationPaths = [...receiptPath, 'atx-vol/tools/oracle_bench_main.cpp'].sort()
  const implementation = { ...passing, bootstrap_path: 'mode_a_implementation', precheck_gate_receipts: [precheck('mode_a_targeted_tests', 'FAIL'), precheck('mode_a_smoke')], changed_path_receipt: changedPathReceipt(base, tested, implementationPaths) }
  assert.equal(validators.bootstrapPathError(implementation, { state: 'missing_mode_a', base_sha: base }), null)
  implementation.changed_path_receipt = changedPathReceipt(base, tested, receiptPath)
  assert.match(validators.bootstrapPathError(implementation, { state: 'missing_mode_a', base_sha: base }), /requires implementation/u)
  implementation.changed_path_receipt = { ...changedPathReceipt(base, tested, implementationPaths), output: 'forged' }
  assert.match(validators.bootstrapPathError(implementation, { state: 'missing_mode_a', base_sha: base }), /changed-path receipt invalid/u)
})

test('top-level bootstrap report validator enforces typed Stage 1 and Stage 2 paths', () => {
  const base = sha('1'); const tested = sha('2')
  const required = ingestRequiredReceipt()
  const receiptPaths = ['atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256']
  const stage1 = completeReport({
    base_sha: base, sha: tested, holdout_digest_receipt: digest('3'), bootstrap_path: 'data_ingest',
    adoption_receipt: required,
    evidence: [
      { command: required.command, exit_code: 0, output: required.output },
      { command: 'python atx-vol/scripts/oracle_ingest.py --zip licensed.zip', exit_code: 0, output: 'aggregate manifest written' },
    ],
    changed_path_receipt: changedPathReceipt(base, tested, receiptPaths),
  }, 'missing_data')
  assert.match(validators.bootstrapReportError(stage1, expectedFor(stage1)), /disk receipt invalid/u)
  stage1.disk_receipt = gateReceipt('disk', 15)
  stage1.evidence.push({ command: BOOTSTRAP_GATE_COMMANDS.disk, exit_code: 0, output: stage1.disk_receipt.output })
  assert.equal(validators.bootstrapReportError(stage1, expectedFor(stage1)), null)
  stage1.evidence[0] = { command: 'generic adoption', exit_code: 0, output: 'PASS' }
  assert.match(validators.bootstrapReportError(stage1, expectedFor(stage1)), /missing\/untyped/u)

  const stage2 = completeReport({
    base_sha: base, sha: tested, holdout_digest_receipt: digest('4'), bootstrap_path: 'mode_a_receipt_only',
    evidence: [{ command: BOOTSTRAP_GATE_COMMANDS.mode_a_smoke, exit_code: 0, output: 'targeted PASS' }],
    precheck_gate_receipts: [precheck('mode_a_targeted_tests'), precheck('mode_a_smoke')],
    changed_path_receipt: changedPathReceipt(base, tested, ['atx-vol/bench/oracle/bootstrap/mode-a.json']),
  }, 'missing_mode_a')
  assert.equal(validators.bootstrapReportError(stage2, expectedFor(stage2)), null)
  stage2.changed_path_receipt = changedPathReceipt(base, tested, ['atx-vol/bench/oracle/bootstrap/mode-a.json', 'atx-vol/tools/oracle_bench_main.cpp'].sort())
  assert.match(validators.bootstrapReportError(stage2, expectedFor(stage2)), /receipt-only/u)
})

test('Stage 1 report contract rejects observed malformed builder fields and accepts the exact report', () => {
  const base = sha('5'); const tested = sha('6')
  const adopted = adoptedReceipt(base)
  const report = completeReport({
    base_sha: base, sha: tested, holdout_digest_receipt: adopted.result.holdout_membership_sha256,
    bootstrap_path: 'data_adoption', adoption_receipt: adopted,
    evidence: [{ command: adopted.command, exit_code: 0, output: adopted.output }],
    changed_path_receipt: changedPathReceipt(base, tested, [
      'atx-vol/bench/oracle/bootstrap/data.json',
      'atx-vol/bench/oracle/cohorts/holdout.sha256',
    ]),
  }, 'missing_data')
  assert.equal(validators.bootstrapReportError(report, expectedFor(report)), null)

  const proseDigest = structuredClone(report)
  proseDigest.holdout_digest_receipt += ' (raw holdout membership digest)'
  assert.match(validators.bootstrapReportError(proseDigest, expectedFor(proseDigest)), /evidence\/SHA\/receipt invalid/u)

  const chainedEvidence = structuredClone(report)
  chainedEvidence.evidence[0].command += '; git status --short'
  assert.match(validators.bootstrapReportError(chainedEvidence, expectedFor(chainedEvidence)), /evidence\/SHA\/receipt invalid/u)

  const annotatedAdoption = structuredClone(report)
  annotatedAdoption.evidence[0].command += ' (cwd C:\\atx-wt\\pool-13)'
  assert.match(validators.bootstrapReportError(annotatedAdoption, expectedFor(annotatedAdoption)), /evidence\/SHA\/receipt invalid/u)
})

test('Bootstrap StructuredOutput envelope admits DONE/BLOCKED while runtime rejects semantic forgeries', () => {
  const base = sha('7'); const tested = sha('8'); const adopted = adoptedReceipt(base)
  const done = completeReport({
    base_sha: base, sha: tested, holdout_digest_receipt: adopted.result.holdout_membership_sha256,
    bootstrap_path: 'data_adoption', adoption_receipt: adopted,
    evidence: [{ command: adopted.command, exit_code: 0, output: adopted.output }],
    changed_path_receipt: changedPathReceipt(base, tested, [
      'atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256',
    ]),
    deviations: '',
  }, 'missing_data')
  assert.deepEqual(schemaErrors(bootstrapReportSchema, { report: done }), [])
  assert.notDeepEqual(schemaErrors(bootstrapReportSchema, done), [])
  assert.notDeepEqual(schemaErrors(bootstrapReportSchema, { report: done, extra: true }), [])
  assert.notDeepEqual(schemaErrors(bootstrapReportSchema, { report: null }), [])
  assert.equal(validators.unwrapBootstrapReport({ report: done }).report, done)
  assert.match(validators.unwrapBootstrapReport(done).error, /envelope invalid/u)
  assert.match(validators.unwrapBootstrapReport({ report: done, extra: true }).error, /envelope invalid/u)
  for (const mutate of [
    value => { value.holdout_digest_receipt += ' generated by adoption' },
    value => { value.evidence[0].command += '; git status --short' },
    value => { value.evidence[0].command += ' (cwd C:\\atx-wt\\pool-13)' },
    value => { value.evidence[0].command += '\nwhoami' },
    value => { value.evidence[0].output = '' },
    value => { value.evidence[0].exit_code = 1 },
    value => { value.evidence = [] },
    value => { value.branch = '' },
    value => { value.worktree = 'C:\\atx-wt\\wrong' },
    value => { value.acquisition_receipt.exit_code = 1 },
    value => { value.sha = 'A'.repeat(40) },
    value => { value.holdout_digest_receipt = 'D'.repeat(64) },
  ]) {
    const malformed = structuredClone(done); mutate(malformed)
    assert.notEqual(validators.bootstrapReportError(malformed, expectedFor(malformed)), null)
  }

  const blocked = {
    ...done, outcome: 'BLOCKED', sha: '', holdout_digest_receipt: '', evidence: [],
    blockers: ['licensed ZIP unavailable'],
    diagnostics: [{ command: 'powershell Test-Path licensed-oracle.zip', exit_code: 1, output: 'missing' }],
  }
  delete blocked.bootstrap_path
  delete blocked.adoption_receipt
  delete blocked.changed_path_receipt
  assert.deepEqual(schemaErrors(bootstrapReportSchema, { report: blocked }), [])
  assert.match(validators.bootstrapReportError(blocked, expectedFor(blocked)), /bootstrap blocked: licensed ZIP unavailable/u)
  for (const suffix of ['; git status --short', ' (cwd C:\\atx-wt\\pool-13)']) {
    const malformedBlocked = structuredClone(blocked)
    malformedBlocked.diagnostics[0].command += suffix
    assert.deepEqual(schemaErrors(bootstrapReportSchema, { report: malformedBlocked }), [])
    assert.match(validators.bootstrapReportError(malformedBlocked, expectedFor(malformedBlocked)), /diagnostics invalid/u)
  }
  for (const mutate of [
    value => { value.blockers = [] },
    value => { value.blockers[0] = '' },
    value => { value.diagnostics = [] },
    value => { value.diagnostics[0].output = '' },
    value => { value.diagnostics[0].exit_code = 1.5 },
    value => { value.diagnostics[0].command += '\u0000' },
    value => { value.evidence = [{ command: 'verify target', exit_code: 1, output: 'failed' }] },
  ]) {
    const malformedBlocked = structuredClone(blocked); mutate(malformedBlocked)
    assert.notEqual(validators.bootstrapReportError(malformedBlocked, expectedFor(malformedBlocked)), null)
  }

  const laterStage = completeReport({
    base_sha: base, sha: tested, holdout_digest_receipt: digest('9'),
    evidence: [{ command: 'verify conventions', exit_code: 0, output: 'PASS' }],
  }, 'missing_conventions')
  assert.equal(validators.bootstrapReportError(laterStage, expectedFor(laterStage)), null)

  assert.match(workflow, /const BOOTSTRAP_REPORT_TOOL_SCHEMA = \{/u)
  assert.match(workflow, /properties: \{ report: bootstrapWireSchema\(BOOTSTRAP_REPORT_VARIANTS\) \}/u)
  assert.match(workflow, /DONE requires a committed lowercase SHA, nonempty success evidence/u)
  assert.match(workflow, /If work is BLOCKED after acquiring the lease/u)
  assert.match(workflow, /adoption_receipt\.command and its one matching evidence\[\]\.command must both equal exactly/u)
})
