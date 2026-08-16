import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const workflow = readFileSync('.claude/workflows/vol-oracle-iter.js', 'utf8')
const schemaSource = workflow.slice(workflow.indexOf('const EVIDENCE_ITEM ='), workflow.indexOf('const REVIEW ='))
assert.ok(schemaSource.length > 0, 'bootstrap schema declarations are missing')

const BOOTSTRAP_GATE_COMMANDS = {
  disk: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate disk',
  aggregate_store: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate aggregate_store',
  ingest_manifest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate ingest_manifest',
  cohort_manifests: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate cohort_manifests',
  holdout_digest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate holdout_digest',
  mode_a_targeted_tests: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_targeted_tests',
  mode_a_smoke: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_smoke',
}
const wireSchema = Function(`
  const BOOTSTRAP_GATE_COMMANDS = ${JSON.stringify(BOOTSTRAP_GATE_COMMANDS)};
  const READY_MEASURE_GATES = {};
  const RATCHET_GATE_IDS = [];
  const ADOPTION_COMMAND = 'powershell scripts\\\\oracle-adopt-existing-data.ps1';
  ${schemaSource};
  return BOOTSTRAP_REPORT_TOOL_SCHEMA;
`)()

const ALLOWED = new Set([
  '$schema', 'type', 'description', 'title', 'properties', 'required',
  'additionalProperties', 'items', 'enum', 'const', 'anyOf',
])

function mirrorClaudeStrictSchema(schema, path = '$', anyOfNode = false) {
  assert.ok(schema && typeof schema === 'object' && !Array.isArray(schema), `${path} must be a schema object`)
  for (const key of Object.keys(schema)) assert.ok(ALLOWED.has(key), `${path}.${key} is unsupported by Claude 2.1.233`)
  if (anyOfNode) assert.equal(Object.prototype.hasOwnProperty.call(schema, 'type'), false, `${path} anyOf node must not have type`)
  const result = {}
  for (const [key, value] of Object.entries(schema)) {
    if (key === 'properties') {
      assert.ok(value && typeof value === 'object' && !Array.isArray(value), `${path}.properties must be an object`)
      result.properties = Object.fromEntries(Object.entries(value).map(([name, child]) =>
        [name, mirrorClaudeStrictSchema(child, `${path}.properties.${name}`, Object.prototype.hasOwnProperty.call(child, 'anyOf'))]))
    } else if (key === 'items') result.items = mirrorClaudeStrictSchema(value, `${path}.items`, Object.prototype.hasOwnProperty.call(value, 'anyOf'))
    else if (key === 'anyOf') {
      assert.ok(Array.isArray(value) && value.length > 0, `${path}.anyOf must be nonempty`)
      result.anyOf = value.map((child, index) => mirrorClaudeStrictSchema(child, `${path}.anyOf[${index}]`, false))
    } else result[key] = Array.isArray(value) ? [...value] : value
  }
  if (schema.type === 'object') {
    assert.equal(schema.additionalProperties, false, `${path} object must reject additional properties`)
    assert.ok(schema.properties && typeof schema.properties === 'object', `${path} object properties missing`)
    assert.ok(Array.isArray(schema.required), `${path} object required list missing`)
  }
  if (schema.type === 'array') assert.ok(schema.items, `${path} array items missing`)
  return result
}

function mirrorStructuredOutputTool(schema) {
  assert.equal(schema?.type, 'object', 'tools.custom.input_schema.type must be object')
  return {
    type: 'custom', name: 'StructuredOutput', description: 'Return the required structured result.',
    input_schema: mirrorClaudeStrictSchema(schema),
  }
}

const mirrored = mirrorStructuredOutputTool(wireSchema)
assert.deepEqual(mirrored.input_schema, wireSchema)
assert.deepEqual(Object.keys(wireSchema).sort(), ['additionalProperties', 'properties', 'required', 'type'])
assert.deepEqual(Object.keys(wireSchema.properties), ['report'])
assert.deepEqual(wireSchema.required, ['report'])
assert.equal(wireSchema.additionalProperties, false)
assert.equal(Object.prototype.hasOwnProperty.call(wireSchema.properties.report, 'type'), false)
assert.equal(wireSchema.properties.report.anyOf.length, 2)
assert.deepEqual(wireSchema.properties.report.anyOf.map(branch => branch.properties.outcome.enum[0]), ['DONE', 'BLOCKED'])

for (const unsafe of [
  { oneOf: [{ type: 'string' }] },
  { type: 'string', pattern: '^x$' },
  { type: 'string', minLength: 1 },
  { type: 'array', minItems: 1, items: { type: 'string' } },
  { type: 'object', properties: {}, required: [], additionalProperties: true },
]) assert.throws(() => mirrorClaudeStrictSchema(unsafe), /unsupported|additional properties/u)
assert.throws(() => mirrorStructuredOutputTool({ anyOf: [{ type: 'string' }] }), /input_schema\.type/u)
assert.throws(() => mirrorClaudeStrictSchema({ type: 'string', anyOf: [{ type: 'string' }] }, '$', true), /must not have type/u)

function schemaErrors(schema, value, path = '$') {
  if (schema.anyOf) {
    const branchErrors = schema.anyOf.map(branch => schemaErrors(branch, value, path))
    return branchErrors.some(errors => errors.length === 0) ? [] : [`${path} does not match anyOf`]
  }
  const errors = []
  if (schema.type === 'object') {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return [`${path} must be object`]
    for (const key of schema.required || []) if (!Object.prototype.hasOwnProperty.call(value, key)) errors.push(`${path}.${key} required`)
    if (schema.additionalProperties === false) for (const key of Object.keys(value)) if (!Object.prototype.hasOwnProperty.call(schema.properties, key)) errors.push(`${path}.${key} unexpected`)
    for (const [key, child] of Object.entries(schema.properties || {})) if (Object.prototype.hasOwnProperty.call(value, key)) errors.push(...schemaErrors(child, value[key], `${path}.${key}`))
  } else if (schema.type === 'array') {
    if (!Array.isArray(value)) return [`${path} must be array`]
    value.forEach((item, index) => errors.push(...schemaErrors(schema.items, item, `${path}[${index}]`)))
  } else if (schema.type === 'string' && typeof value !== 'string') errors.push(`${path} must be string`)
  else if (schema.type === 'integer' && !Number.isInteger(value)) errors.push(`${path} must be integer`)
  if (schema.enum && !schema.enum.includes(value)) errors.push(`${path} outside enum`)
  if (Object.prototype.hasOwnProperty.call(schema, 'const') && schema.const !== value) errors.push(`${path} differs from const`)
  return errors
}

const base = 'b'.repeat(40)
const lease = {
  action: 'acquire', lease_name: 'pool-14', run_id: 'run-1', branch: 'lane/oracle-bootstrap-data-run-1',
  base_sha: base, worktree: 'C:\\atx-wt\\pool-14', heartbeat_id: 'heartbeat-1', keeper_pid: 1234,
  keeper_process_started_utc: '2026-08-15T12:00:00Z', keeper_ready_utc: '2026-08-15T12:00:01Z',
  exit_code: 0, output: 'LEASED pool-14 run-1',
}
const guard = {
  main_sha: base, canonical_sha: null,
  index_sha256: '1'.repeat(64), tracked_sha256: '2'.repeat(64),
  untracked_sha256: '3'.repeat(64), raw_sha256: '4'.repeat(64),
}
const brokerEvidence = {
  logical_operation: 'recover_stage1_source_replay', physical_cwd: lease.worktree,
  command: 'recover-stage1:validated-source', exit_code: 0, output: 'PASS',
  raw_output_sha256: '5'.repeat(64), root_guard_before: guard, root_guard_after: guard,
}
const common = {
  state: 'missing_data', branch: lease.branch, base_sha: base, tree: 'c'.repeat(40), worktree: lease.worktree,
  lease_name: lease.lease_name, lease_run_id: lease.run_id, heartbeat_id: lease.heartbeat_id,
  keeper_pid: lease.keeper_pid, keeper_process_started_utc: lease.keeper_process_started_utc,
  acquisition_receipt: lease, broker_evidence: [brokerEvidence], deviations: '',
}
const done = {
  ...common, outcome: 'DONE', sha: 'a'.repeat(40), holdout_digest_receipt: 'd'.repeat(64),
  evidence: [{ command: 'verify target', exit_code: 0, output: 'PASS' }],
}
const blocked = {
  ...common, outcome: 'BLOCKED', sha: '', tree: '', holdout_digest_receipt: '', evidence: [],
  blockers: ['licensed data unavailable'],
  diagnostics: [{ command: 'powershell Test-Path licensed.zip', exit_code: 1, output: 'missing' }],
}
assert.deepEqual(schemaErrors(wireSchema, { report: done }), [])
assert.deepEqual(schemaErrors(wireSchema, { report: blocked }), [])
assert.notDeepEqual(schemaErrors(wireSchema, done), [])
assert.notDeepEqual(schemaErrors(wireSchema, { report: done, extra: true }), [])
assert.notDeepEqual(schemaErrors(wireSchema, { report: null }), [])

assert.equal((workflow.match(/schema: BOOTSTRAP_REPORT_TOOL_SCHEMA/g) || []).length, 2, 'build and Fix must share the strict envelope')
assert.equal((workflow.match(/unwrapped = unwrapBootstrapReport\(envelope\)/g) || []).length, 2, 'build and Fix must unwrap immediately')

console.log(JSON.stringify({
  status: 'PASS', schema_keys: [...ALLOWED], branches: ['DONE', 'BLOCKED'],
  envelopes_checked: 5, unsafe_shapes_rejected: 7, agent_calls_wrapped: 2,
}))
