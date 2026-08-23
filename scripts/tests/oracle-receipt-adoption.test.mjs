// Contracts for the bootstrap receipt surface of vol-oracle-iter: the Stage 1
// (missing_data) and Stage 2 (missing_mode_a) typed reports, the sealed broker
// recovery result, and the StructuredOutput wire schema.
//
// HISTORY: Stage 1 originally ran the PowerShell adoption transaction
// (scripts/oracle-adopt-existing-data.ps1: bootstrap_path data_adoption /
// data_ingest plus a disk receipt). That contract is RETIRED — the workflow
// moved to the broker's atomic recover_stage1 (bootstrap_path data_recovery
// against the pinned immutable STAGE1_RECOVERY source), and the adoption
// command now survives only as a forbidden-evidence marker the validators use
// to fail closed. This suite asserts TODAY'S contract.
//
// TESTABILITY BOUNDARY: broker tools (recover_stage1, recovery_result,
// gate_run) cannot execute inside a plain `node --test` process, so this suite
// pins what is statically checkable — the pure validators evaluated out of the
// workflow's declaration region, the sealed-recovery result shape, the frozen
// lane contracts, the wire schema, and the dispatch prompt strings. The
// executing side lives in scripts/tests/oracle-lane-broker.test.mjs.
//
// Every numeric fixture here is DERIVED from the workflow's own constants
// (ORACLE_BENCH_TEST_COUNT, TARGET_REGISTRY, STAGE1_RECOVERY, ...). The one
// deliberately-bare review literal for ORACLE_BENCH_TEST_COUNT lives in
// oracle-ready-contracts.test.mjs and is intentionally NOT duplicated here.
import test from 'node:test'
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import vm from 'node:vm'
import { brokerGateOutputSha256 as productionGateOutputSha256, buildBrokerGateReceipt } from '../oracle-lane-broker.mjs'

const read = path => readFileSync(path, 'utf8')
const workflow = read('.claude/workflows/vol-oracle-iter.js')

// Same declaration-region loader oracle-ready-contracts.test.mjs uses: the
// slice is pure declarations, so every constant keeps its real value and every
// validator keeps its real closure (including sameJsonPayload, which the old
// per-function string extraction lost).
function loadScope(source, startAnchor, endAnchor, names) {
  const start = source.indexOf(startAnchor)
  const end = source.indexOf(endAnchor)
  assert.ok(start >= 0 && end > start, `could not slice ${startAnchor} .. ${endAnchor}`)
  const body = source.slice(start, end)
  return vm.runInNewContext(`(() => {\n${body}\nreturn { ${names.join(', ')} }\n})()`, { args: { task: 'load declarations only' } })
}

const O = loadScope(workflow, 'function oracleRunId(', "phase('Capability')", [
  'BOOTSTRAP_GATE_COMMANDS', 'TARGETED_BOOTSTRAP_GATE_IDS', 'TARGET_REGISTRY', 'ORACLE_BENCH_TEST_COUNT',
  'ADOPTION_COMMAND', 'MODE_A_RECEIPT_ONLY_PATHS', 'STAGE1_RECOVERY', 'BOOTSTRAP_LANES', 'BOOTSTRAP_REPORT_TOOL_SCHEMA',
  'sameJsonPayload', 'brokerGateOutputSha256', 'brokerGateReceiptId',
  'bootstrapPathError', 'bootstrapReportError', 'unwrapBootstrapReport',
  'sealedRecoveryQueryError', 'reportFromSealedRecovery',
])

const STAGE1_GATE_IDS = O.BOOTSTRAP_LANES.missing_data.gate_ids
const STAGE1_PATHS = Object.keys(O.STAGE1_RECOVERY.blobs).sort()
const MODE_A_TARGETS = O.TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => item.metric_id)

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
const LANE = {
  lease_name: 'pool-1', run_id: 'run-1', heartbeat_id: 'heartbeat-1', keeper_pid: 1234,
  keeper_process_started_utc: '2026-08-15T12:00:00Z', keeper_ready_utc: '2026-08-15T12:00:01Z',
  worktree: 'C:\\atx-wt\\pool-1',
}
function rootGuard() {
  return { main_sha: sha('9'), canonical_sha: sha('8'), index_sha256: digest('1'), tracked_sha256: digest('2'), untracked_sha256: digest('3'), raw_sha256: digest('4') }
}
function brokerEvidence(logicalOperation, worktree, command, output, exitCode) {
  return {
    logical_operation: logicalOperation, physical_cwd: worktree, command, exit_code: exitCode, output,
    raw_output_sha256: productionGateOutputSha256(output), root_guard_before: rootGuard(), root_guard_after: rootGuard(),
  }
}
const gateBrokerEvidence = (gateId, worktree, command, output, exitCode) => brokerEvidence(`gate:${gateId}`, worktree, command, output, exitCode)
function gateReceipt(gateId, { observations = 20, testedSha = sha('a'), testedTree = sha('b'), worktree = LANE.worktree, operationId = 'bootstrap_mode_a', testsExecuted = O.ORACLE_BENCH_TEST_COUNT, metricIds = MODE_A_TARGETS } = {}) {
  const common = { schema_version: 1, status: 'PASS', observations, command_id: gateId, raw_output_sha256: digest('f') }
  const result = !O.TARGETED_BOOTSTRAP_GATE_IDS.includes(gateId) ? common : gateId.endsWith('_tests')
    ? { ...common, tested_sha: testedSha, tested_tree: testedTree, gate_kind: 'ctest', tests_executed: testsExecuted, tests_passed: testsExecuted, rows_processed: 0, metric_ids: [], audit_summary: `tests_executed=${testsExecuted} tests_passed=${testsExecuted}` }
    : { ...common, tested_sha: testedSha, tested_tree: testedTree, gate_kind: 'oracle_bench', tests_executed: 0, tests_passed: 0, rows_processed: 3, metric_ids: metricIds, audit_summary: `status=PASS rows_processed=3 metric_ids=${[...metricIds].sort().join(',')}` }
  const command = O.BOOTSTRAP_GATE_COMMANDS[gateId]
  const output = JSON.stringify(result)
  const broker_evidence = gateBrokerEvidence(gateId, worktree, command, output, 0)
  return { ...buildBrokerGateReceipt(operationId, { gate_id: gateId, tested_sha: testedSha, tested_tree: testedTree, command, exit_code: 0, output, broker_evidence }), result }
}
function precheck(gateId, status = 'PASS', testedSha = sha('a'), testedTree = sha('b'), worktree = LANE.worktree, operationId = 'bootstrap_mode_a') {
  if (status === 'FAIL') {
    const command = O.BOOTSTRAP_GATE_COMMANDS[gateId]
    const output = 'targeted failure'
    const broker_evidence = gateBrokerEvidence(gateId, worktree, command, output, 1)
    return { ...buildBrokerGateReceipt(operationId, { gate_id: gateId, tested_sha: testedSha, tested_tree: testedTree, command, exit_code: 1, output, broker_evidence }), status }
  }
  return { ...gateReceipt(gateId, { testedSha, testedTree, worktree, operationId }), status }
}
function changedPathReceipt(base, tested, paths) {
  return { base_sha: base, tested_sha: tested, command: `git diff --name-only ${base}...${tested}`, exit_code: 0, output: paths.join('\n'), paths }
}
// Shape of the RETIRED adoption receipt: kept only to prove the validators now
// REJECT a report that carries one.
function adoptedReceipt(base) {
  const result = { schema_version: 1, status: 'ADOPTED', command_id: 'oracle_existing_store_adoption', base_sha: base, manifest_sha256: digest('a'), holdout_membership_sha256: digest('b'), total_rows: 9, bucket_count: 3, parquet_files: 3, cohort_underlier_count: 3 }
  return { command: O.ADOPTION_COMMAND, exit_code: 0, output: JSON.stringify(result), result }
}
function acquisitionReceipt(branch, baseSha) {
  return {
    action: 'acquire', lease_name: LANE.lease_name, run_id: LANE.run_id, branch, base_sha: baseSha, worktree: LANE.worktree,
    heartbeat_id: LANE.heartbeat_id, keeper_pid: LANE.keeper_pid, keeper_process_started_utc: LANE.keeper_process_started_utc,
    keeper_ready_utc: LANE.keeper_ready_utc, exit_code: 0,
    output: `LEASED pool=${LANE.lease_name} branch=${branch} run_id=${LANE.run_id} owner_kind=heartbeat heartbeat_id=${LANE.heartbeat_id} keeper_pid=${LANE.keeper_pid} keeper_ready_utc=${LANE.keeper_ready_utc}`,
  }
}
function completeReport(pathReport, state) {
  const branch = `lane/oracle-${state}`
  return {
    tree: sha('0'),
    broker_evidence: [brokerEvidence('lane_commit', LANE.worktree, 'git commit', 'committed', 0)],
    ...pathReport,
    outcome: 'DONE', state, branch, lease_name: LANE.lease_name, lease_run_id: LANE.run_id, heartbeat_id: LANE.heartbeat_id,
    keeper_pid: LANE.keeper_pid, keeper_process_started_utc: LANE.keeper_process_started_utc, worktree: LANE.worktree,
    deviations: pathReport.deviations ?? '',
    acquisition_receipt: acquisitionReceipt(branch, pathReport.base_sha),
  }
}
function expectedFor(report) {
  return {
    state: report.state, branch: report.branch, base_sha: report.base_sha,
    base_tree: report.precheck_gate_receipts?.[0]?.tested_tree || sha('c'),
    operation_id: report.state === 'missing_mode_a' ? 'bootstrap_mode_a' : undefined,
    run_id: report.lease_run_id, heartbeat_id: report.heartbeat_id,
  }
}

// ── Stage 1 recovery fixtures ───────────────────────────────────────────────

function recoverySourceReceipt() {
  return {
    source_commit: O.STAGE1_RECOVERY.source_commit, source_parent: O.STAGE1_RECOVERY.source_parent,
    source_tree: O.STAGE1_RECOVERY.source_tree, adoption_rerun: false,
    blobs: STAGE1_PATHS.map(path => ({ path, source_blob: O.STAGE1_RECOVERY.blobs[path], replay_blob: O.STAGE1_RECOVERY.blobs[path] })),
  }
}
function stage1GateResult(gateId) {
  return { schema_version: 1, status: 'PASS', observations: 20, command_id: gateId, raw_output_sha256: digest('f') }
}
function recoveryReport(base, tested) {
  return {
    base_sha: base, sha: tested, holdout_digest_receipt: O.STAGE1_RECOVERY.holdout_digest,
    bootstrap_path: 'data_recovery', recovery_id: digest('c'), recovery_replayed: false,
    recovery_source_receipt: recoverySourceReceipt(),
    evidence: STAGE1_GATE_IDS.map(gateId => ({ command: O.BOOTSTRAP_GATE_COMMANDS[gateId], exit_code: 0, output: JSON.stringify(stage1GateResult(gateId)) })),
    changed_path_receipt: changedPathReceipt(base, tested, STAGE1_PATHS),
  }
}
function sealedGateReceipt(gateId, testedSha, testedTree) {
  const result = stage1GateResult(gateId)
  const command = O.BOOTSTRAP_GATE_COMMANDS[gateId]
  const output = JSON.stringify(result)
  const broker_evidence = gateBrokerEvidence(gateId, LANE.worktree, command, output, 0)
  return buildBrokerGateReceipt('bootstrap_data', { gate_id: gateId, tested_sha: testedSha, tested_tree: testedTree, command, exit_code: 0, output, broker_evidence })
}
function sealedRecoveryQuery(base) {
  const recoveredSha = sha('a')
  const recoveredTree = sha('b')
  const recoveryId = digest('c')
  const result = {
    recovery_id: recoveryId, replayed: true, recovery: recoverySourceReceipt(),
    replay_paths: [...STAGE1_PATHS], gate_receipts: STAGE1_GATE_IDS.map(gateId => sealedGateReceipt(gateId, recoveredSha, recoveredTree)),
    sha: recoveredSha, tree: recoveredTree, files_changed: [...STAGE1_PATHS],
    changed_path_receipt: changedPathReceipt(base, recoveredSha, STAGE1_PATHS),
    broker_evidence: [brokerEvidence('recover_stage1_replay', LANE.worktree, `recover-stage1:sealed-replay:${recoveryId}`, recoveryId, 0)],
  }
  return { found: true, recovery_id: recoveryId, result, broker_evidence: brokerEvidence('recovery_result', LANE.worktree, 'recovery-result', 'sealed result returned', 0) }
}

// ── tests ───────────────────────────────────────────────────────────────────

test('Stage 1 lane contract is broker recover_stage1 only and Stage 2 stays receipt-first', () => {
  const stage1 = O.BOOTSTRAP_LANES.missing_data
  const stage2 = O.BOOTSTRAP_LANES.missing_mode_a
  assert.deepEqual([...stage1.gate_ids], ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'])
  assert.ok(!stage1.gate_ids.includes('disk'), 'the disk gate belongs to the retired adoption path')
  assert.match(stage1.contract, /Use only broker recover_stage1/u)
  assert.ok(stage1.contract.includes(O.STAGE1_RECOVERY.source_commit), 'lane contract must pin the immutable Stage 1 source commit')
  assert.match(stage1.contract, /Do not run adoption, disk, ingest, builds/u)
  assert.doesNotMatch(stage1.contract, /oracle-adopt-existing-data\.ps1/u)
  assert.match(stage2.contract, /targeted Mode A gates first/u)
  assert.match(stage2.contract, /write only bootstrap\/mode-a\.json/u)
  assert.match(stage2.contract, /Implement\/fix Mode A only when an exact targeted gate proves it necessary/u)
  // The pinned immutable source is fully typed: three commit-ish SHAs, the raw
  // holdout membership digest, and exactly the two replay blobs.
  for (const value of [O.STAGE1_RECOVERY.source_commit, O.STAGE1_RECOVERY.source_parent, O.STAGE1_RECOVERY.source_tree]) assert.match(value, /^[0-9a-f]{40}$/u)
  assert.match(O.STAGE1_RECOVERY.holdout_digest, /^[0-9a-f]{64}$/u)
  assert.deepEqual(STAGE1_PATHS, ['atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256'])
  for (const blob of Object.values(O.STAGE1_RECOVERY.blobs)) assert.match(blob, /^[0-9a-f]{40}$/u)
  // The retired adoption command survives only as the forbidden-evidence
  // marker; if this constant drifts the fail-closed evidence filter goes blind.
  assert.equal(O.ADOPTION_COMMAND, 'powershell scripts\\oracle-adopt-existing-data.ps1')
})

test('the live store-metadata projection is aggregate-only and preflight owns the aggregate_store gate', () => {
  const metadata = read('atx-vol/scripts/oracle_store_metadata.py')
  const preflight = read('scripts/oracle-bootstrap-preflight.ps1')
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

test('sameJsonPayload binds key order and canonicalises number rendering', () => {
  assert.equal(typeof O.sameJsonPayload, 'function')
  assert.equal(O.sameJsonPayload('{"a":1,"b":2}', { a: 1, b: 2 }), true)
  // Key ORDER is the property the check exists to bind.
  assert.equal(O.sameJsonPayload('{"b":2,"a":1}', { a: 1, b: 2 }), false)
  // Number rendering is canonicalised through JS formatting on both sides:
  // PowerShell re-emits the literal source digits and C++ prints %.17g, so a
  // byte comparison of the renderings would reject most MAE-shaped doubles.
  assert.equal(O.sameJsonPayload('{"x":1.50}', { x: 1.5 }), true)
  assert.equal(O.sameJsonPayload('{"x":  1.5 }', { x: 1.5 }), true)
  assert.equal(O.sameJsonPayload('not json', { x: 1 }), false)
  assert.equal(O.sameJsonPayload(undefined, {}), false)
})

test('Stage 1 typed branching accepts only the pinned broker recovery and rejects the retired adoption paths', () => {
  const base = sha('b'); const tested = sha('c')
  const expected = { state: 'missing_data', base_sha: base }
  const valid = recoveryReport(base, tested)
  assert.equal(O.bootstrapPathError(valid, expected), null)

  for (const [mutate, message] of [
    // The three retired adoption-era shapes are rejected outright.
    [value => { value.bootstrap_path = 'data_ingest' }, /must use recovery without adoption\/disk/u],
    [value => { value.adoption_receipt = adoptedReceipt(value.base_sha) }, /must use recovery without adoption\/disk/u],
    [value => { value.disk_receipt = gateReceipt('disk') }, /must use recovery without adoption\/disk/u],
    // Sealed-recovery identity and the pinned immutable source.
    [value => { value.recovery_id = 'abc' }, /sealed recovery identity invalid/u],
    [value => { value.recovery_replayed = 'yes' }, /sealed recovery identity invalid/u],
    [value => { value.recovery_source_receipt.source_commit = sha('9') }, /recovery source identity invalid/u],
    [value => { value.recovery_source_receipt.adoption_rerun = true }, /recovery source identity invalid/u],
    [value => { value.changed_path_receipt = changedPathReceipt(value.base_sha, value.sha, [...STAGE1_PATHS, 'atx-vol/tools/oracle_bench_main.cpp'].sort()) }, /recovery path set invalid/u],
    [value => { value.recovery_source_receipt.blobs[0].replay_blob = sha('9') }, /recovery blob invalid/u],
    [value => { value.holdout_digest_receipt = digest('d') }, /recovery digest mismatch/u],
    // Evidence closure: no adoption/disk/ingest/build commands, and exactly one
    // passing entry per fixed Stage 1 gate.
    [value => { value.evidence.push({ command: O.ADOPTION_COMMAND, exit_code: 0, output: 'ADOPTED' }) }, /includes adoption\/disk\/ingest\/build evidence/u],
    [value => { value.evidence.push({ command: O.BOOTSTRAP_GATE_COMMANDS.disk, exit_code: 0, output: 'PASS' }) }, /includes adoption\/disk\/ingest\/build evidence/u],
    [value => { value.evidence.push({ command: 'python atx-vol/scripts/oracle_ingest.py --zip licensed.zip', exit_code: 0, output: 'written' }) }, /includes adoption\/disk\/ingest\/build evidence/u],
    [value => { value.evidence = value.evidence.slice(1) }, /recovery evidence missing/u],
    [value => { value.evidence.push({ ...value.evidence[0] }) }, /recovery evidence missing/u],
  ]) {
    const forged = structuredClone(valid)
    mutate(forged)
    assert.match(O.bootstrapPathError(forged, expected), message)
  }
})

test('sealed Stage 1 recovery results replay into reports the top-level validator accepts', () => {
  const base = sha('4')
  const branch = 'lane/oracle-missing_data'
  const expected = { state: 'missing_data', branch, base_sha: base, run_id: LANE.run_id, heartbeat_id: LANE.heartbeat_id }
  const acquire = {
    worktree: LANE.worktree, lease_name: LANE.lease_name, keeper_pid: LANE.keeper_pid,
    keeper_process_started_utc: LANE.keeper_process_started_utc, acquisition_receipt: acquisitionReceipt(branch, base),
  }
  const query = sealedRecoveryQuery(base)
  assert.equal(O.sealedRecoveryQueryError(query, acquire, expected), null)
  const report = O.reportFromSealedRecovery(query, acquire, expected)
  assert.equal(report.bootstrap_path, 'data_recovery')
  assert.equal(report.recovery_replayed, true)
  assert.equal(report.holdout_digest_receipt, O.STAGE1_RECOVERY.holdout_digest)
  assert.deepEqual(schemaErrors(O.BOOTSTRAP_REPORT_TOOL_SCHEMA, { report }), [])
  assert.equal(O.bootstrapReportError(report, expected), null)

  for (const [mutate, message] of [
    [value => { value.found = false }, /query missing\/invalid/u],
    [value => { value.result.replayed = false }, /replay identity invalid/u],
    [value => { value.result.recovery.adoption_rerun = true }, /source invalid/u],
    [value => { value.result.replay_paths = [...value.result.replay_paths, 'atx-vol/tools/extra.cpp'] }, /path closure invalid/u],
    [value => { value.result.recovery.blobs[0].source_blob = sha('9') }, /blob invalid/u],
    [value => { value.result.gate_receipts = value.result.gate_receipts.slice(1) }, /gate set invalid/u],
    [value => { value.result.gate_receipts[0].tested_sha = sha('9') }, /gate invalid/u],
    [value => { value.result.gate_receipts[0].output = 'not json' }, /gate output invalid/u],
  ]) {
    const forged = structuredClone(query)
    mutate(forged)
    assert.match(O.sealedRecoveryQueryError(forged, acquire, expected), message)
  }
})

test('Stage 2 precheck receipts mechanically select receipt-only or implementation paths', () => {
  const base = sha('d'); const baseTree = sha('c'); const tested = sha('e')
  const expected = { state: 'missing_mode_a', operation_id: 'bootstrap_mode_a', base_sha: base, base_tree: baseTree }
  const receiptPath = [...O.MODE_A_RECEIPT_ONLY_PATHS]
  const passing = { base_sha: base, sha: tested, bootstrap_path: 'mode_a_receipt_only', worktree: LANE.worktree, evidence: [], precheck_gate_receipts: [precheck('mode_a_targeted_tests', 'PASS', base, baseTree), precheck('mode_a_smoke', 'PASS', base, baseTree)], changed_path_receipt: changedPathReceipt(base, tested, receiptPath) }
  const actualReceipt = passing.precheck_gate_receipts[0]
  for (const value of ['', 'abc', 'oracle-\u0394-\ud83d\ude80']) assert.equal(O.brokerGateOutputSha256(value), productionGateOutputSha256(value))
  assert.equal(O.brokerGateOutputSha256(actualReceipt.output), productionGateOutputSha256(actualReceipt.output))
  assert.equal(O.brokerGateReceiptId(expected.operation_id, actualReceipt), actualReceipt.receipt_id)
  assert.equal(O.bootstrapPathError(passing, expected), null)
  const structured = completeReport({
    ...passing, holdout_digest_receipt: digest('a'),
    evidence: [{ command: O.BOOTSTRAP_GATE_COMMANDS.mode_a_smoke, exit_code: 0, output: 'targeted PASS' }],
  }, 'missing_mode_a')
  assert.deepEqual(schemaErrors(O.BOOTSTRAP_REPORT_TOOL_SCHEMA, { report: structured }), [])
  const strippedStructured = structuredClone(structured)
  delete strippedStructured.precheck_gate_receipts[0].receipt_id
  assert.notDeepEqual(schemaErrors(O.BOOTSTRAP_REPORT_TOOL_SCHEMA, { report: strippedStructured }), [])

  for (const mutate of [
    value => { value.precheck_gate_receipts[0].tested_sha = sha('f') },
    value => { value.precheck_gate_receipts[0].tested_tree = sha('f') },
    value => {
      const receipt = value.precheck_gate_receipts[0]
      receipt.result.tested_sha = sha('f')
      receipt.output = JSON.stringify(receipt.result)
      receipt.broker_evidence.output = receipt.output
    },
    value => { delete value.precheck_gate_receipts[0].receipt_id },
    value => { value.precheck_gate_receipts[0].receipt_id = digest('a') },
    value => { value.precheck_gate_receipts[1].receipt_id = value.precheck_gate_receipts[0].receipt_id },
    value => { value.precheck_gate_receipts[0].broker_evidence.command = 'forged command' },
    value => { value.precheck_gate_receipts[0].broker_evidence.output = 'forged output' },
    value => { delete value.precheck_gate_receipts[0].broker_evidence.raw_output_sha256 },
    value => { value.precheck_gate_receipts[0].broker_evidence.raw_output_sha256 = digest('a') },
    value => { value.precheck_gate_receipts[0].broker_evidence.physical_cwd = 'C:\\atx-wt\\pool-99' },
    value => {
      const receipt = value.precheck_gate_receipts[0]
      receipt.result.raw_output_sha256 = digest('a')
      receipt.output = JSON.stringify(receipt.result)
      receipt.broker_evidence.output = receipt.output
    },
    // Internally consistent forgeries (hashes and receipt_id recomputed) whose
    // SEMANTICS are stale: a suite count one below the workflow's pinned
    // ORACLE_BENCH_TEST_COUNT, and a metric registry missing one Mode A id.
    // Both counts are derived, never re-pinned - the bare review literal lives
    // in oracle-ready-contracts.test.mjs.
    value => { value.precheck_gate_receipts[0] = { ...gateReceipt('mode_a_targeted_tests', { testedSha: base, testedTree: baseTree, testsExecuted: O.ORACLE_BENCH_TEST_COUNT - 1 }), status: 'PASS' } },
    value => { value.precheck_gate_receipts[1] = { ...gateReceipt('mode_a_smoke', { testedSha: base, testedTree: baseTree, metricIds: MODE_A_TARGETS.slice(0, -1) }), status: 'PASS' } },
  ]) {
    const forged = structuredClone(passing)
    mutate(forged)
    assert.match(O.bootstrapPathError(forged, expected), /Stage2 precheck receipt/u)
  }

  passing.changed_path_receipt = changedPathReceipt(base, tested, [...receiptPath, 'atx-vol/tools/oracle_bench_main.cpp'].sort())
  assert.match(O.bootstrapPathError(passing, expected), /receipt-only/u)

  const implementationPaths = [...receiptPath, 'atx-vol/tools/oracle_bench_main.cpp'].sort()
  const implementation = { ...passing, bootstrap_path: 'mode_a_implementation', precheck_gate_receipts: [precheck('mode_a_targeted_tests', 'FAIL', base, baseTree), precheck('mode_a_smoke', 'PASS', base, baseTree)], changed_path_receipt: changedPathReceipt(base, tested, implementationPaths) }
  assert.equal(O.bootstrapPathError(implementation, expected), null)
  implementation.changed_path_receipt = changedPathReceipt(base, tested, receiptPath)
  assert.match(O.bootstrapPathError(implementation, expected), /requires implementation/u)
  implementation.changed_path_receipt = { ...changedPathReceipt(base, tested, implementationPaths), output: 'forged' }
  assert.match(O.bootstrapPathError(implementation, expected), /changed-path receipt invalid/u)
})

test('top-level bootstrap report validator enforces typed Stage 1 and Stage 2 paths', () => {
  const base = sha('1'); const tested = sha('2')
  const stage1 = completeReport(recoveryReport(base, tested), 'missing_data')
  assert.equal(O.bootstrapReportError(stage1, expectedFor(stage1)), null)

  // The retired adoption-era shapes are rejected at the top level too.
  const ingest = structuredClone(stage1)
  ingest.bootstrap_path = 'data_ingest'
  assert.match(O.bootstrapReportError(ingest, expectedFor(ingest)), /must use recovery without adoption\/disk/u)
  const adopted = structuredClone(stage1)
  adopted.adoption_receipt = adoptedReceipt(base)
  assert.match(O.bootstrapReportError(adopted, expectedFor(adopted)), /must use recovery without adoption\/disk/u)
  const generic = structuredClone(stage1)
  generic.evidence[0] = { command: 'generic recovery claim', exit_code: 0, output: 'PASS' }
  assert.match(O.bootstrapReportError(generic, expectedFor(generic)), /recovery evidence missing/u)

  const stage2Tree = sha('3')
  const stage2 = completeReport({
    base_sha: base, sha: tested, holdout_digest_receipt: digest('4'), bootstrap_path: 'mode_a_receipt_only',
    evidence: [{ command: O.BOOTSTRAP_GATE_COMMANDS.mode_a_smoke, exit_code: 0, output: 'targeted PASS' }],
    precheck_gate_receipts: [precheck('mode_a_targeted_tests', 'PASS', base, stage2Tree), precheck('mode_a_smoke', 'PASS', base, stage2Tree)],
    changed_path_receipt: changedPathReceipt(base, tested, [...O.MODE_A_RECEIPT_ONLY_PATHS]),
  }, 'missing_mode_a')
  assert.equal(O.bootstrapReportError(stage2, expectedFor(stage2)), null)
  stage2.changed_path_receipt = changedPathReceipt(base, tested, [...O.MODE_A_RECEIPT_ONLY_PATHS, 'atx-vol/tools/oracle_bench_main.cpp'].sort())
  assert.match(O.bootstrapReportError(stage2, expectedFor(stage2)), /receipt-only/u)
})

test('Stage 1 report contract rejects observed malformed builder fields and accepts the exact report', () => {
  const base = sha('5'); const tested = sha('6')
  const report = completeReport(recoveryReport(base, tested), 'missing_data')
  assert.equal(O.bootstrapReportError(report, expectedFor(report)), null)

  const proseDigest = structuredClone(report)
  proseDigest.holdout_digest_receipt += ' (raw holdout membership digest)'
  assert.match(O.bootstrapReportError(proseDigest, expectedFor(proseDigest)), /evidence\/SHA\/receipt invalid/u)

  const chainedEvidence = structuredClone(report)
  chainedEvidence.evidence[0].command += '; git status --short'
  assert.match(O.bootstrapReportError(chainedEvidence, expectedFor(chainedEvidence)), /evidence\/SHA\/receipt invalid/u)

  const annotatedCommand = structuredClone(report)
  annotatedCommand.evidence[0].command += ' (cwd C:\\atx-wt\\pool-13)'
  assert.match(O.bootstrapReportError(annotatedCommand, expectedFor(annotatedCommand)), /evidence\/SHA\/receipt invalid/u)
})

test('Bootstrap StructuredOutput envelope admits DONE/BLOCKED while runtime rejects semantic forgeries', () => {
  const base = sha('7'); const tested = sha('8')
  const done = completeReport({ ...recoveryReport(base, tested), deviations: '' }, 'missing_data')
  const schema = O.BOOTSTRAP_REPORT_TOOL_SCHEMA
  assert.deepEqual(schemaErrors(schema, { report: done }), [])
  assert.notDeepEqual(schemaErrors(schema, done), [])
  assert.notDeepEqual(schemaErrors(schema, { report: done, extra: true }), [])
  assert.notDeepEqual(schemaErrors(schema, { report: null }), [])
  assert.equal(O.unwrapBootstrapReport({ report: done }).report, done)
  assert.match(O.unwrapBootstrapReport(done).error, /envelope invalid/u)
  assert.match(O.unwrapBootstrapReport({ report: done, extra: true }).error, /envelope invalid/u)
  for (const mutate of [
    value => { value.holdout_digest_receipt += ' generated by recovery' },
    value => { value.holdout_digest_receipt = digest('d') },
    value => { value.holdout_digest_receipt = 'D'.repeat(64) },
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
    value => { value.adoption_receipt = adoptedReceipt(value.base_sha) },
    value => { value.recovery_source_receipt.source_tree = sha('9') },
    value => { value.recovery_id = 'not-a-recovery-id' },
  ]) {
    const malformed = structuredClone(done); mutate(malformed)
    assert.notEqual(O.bootstrapReportError(malformed, expectedFor(malformed)), null)
  }

  const blocked = {
    ...done, outcome: 'BLOCKED', sha: '', tree: '', holdout_digest_receipt: '', evidence: [],
    blockers: ['broker recover_stage1 sealed result unavailable'],
    diagnostics: [{ command: O.BOOTSTRAP_GATE_COMMANDS.aggregate_store, exit_code: 1, output: 'aggregate store missing' }],
  }
  delete blocked.bootstrap_path
  delete blocked.recovery_id
  delete blocked.recovery_replayed
  delete blocked.recovery_source_receipt
  delete blocked.changed_path_receipt
  assert.deepEqual(schemaErrors(schema, { report: blocked }), [])
  assert.match(O.bootstrapReportError(blocked, expectedFor(blocked)), /bootstrap blocked: broker recover_stage1 sealed result unavailable/u)
  for (const suffix of ['; git status --short', ' (cwd C:\\atx-wt\\pool-13)']) {
    const malformedBlocked = structuredClone(blocked)
    malformedBlocked.diagnostics[0].command += suffix
    // bootstrapWireSchema strips pattern/minItems on purpose, so the wire
    // envelope ADMITS a chained/annotated diagnostic command; the runtime
    // validator is the layer that rejects it. Both directions are pinned.
    assert.deepEqual(schemaErrors(schema, { report: malformedBlocked }), [])
    assert.match(O.bootstrapReportError(malformedBlocked, expectedFor(malformedBlocked)), /diagnostics invalid/u)
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
    assert.notEqual(O.bootstrapReportError(malformedBlocked, expectedFor(malformedBlocked)), null)
  }

  const laterStage = completeReport({
    base_sha: base, sha: tested, holdout_digest_receipt: digest('9'),
    evidence: [{ command: 'verify conventions', exit_code: 0, output: 'PASS' }],
  }, 'missing_conventions')
  assert.equal(O.bootstrapReportError(laterStage, expectedFor(laterStage)), null)

  // The wire schema and the Stage 1 dispatch strings the loop actually runs.
  assert.match(workflow, /const BOOTSTRAP_REPORT_TOOL_SCHEMA = \{/u)
  assert.match(workflow, /properties: \{ report: bootstrapWireSchema\(BOOTSTRAP_REPORT_VARIANTS\) \}/u)
  assert.match(workflow, /Stage 1 must call only recover_stage1, set bootstrap_path=data_recovery/u)
  assert.match(workflow, /DONE requires the broker-returned lowercase SHA and tree plus raw lowercase holdout digest/u)
  assert.match(workflow, /Call broker recovery_result exactly once/u)
  assert.match(workflow, /capability\.state === 'missing_data' \? 'vol-stage1-recovery' : 'vol-builder'/u)
  assert.match(workflow, /recover_stage1 already owns all four fixed gates atomically/u)
})
