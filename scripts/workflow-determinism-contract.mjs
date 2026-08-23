import assert from 'node:assert/strict'
import fs from 'node:fs'
import vm from 'node:vm'

const workflowPaths = [
  '.claude/workflows/vol-oracle-iter.js',
  '.claude/workflows/vol-sprint.js',
]
const sources = new Map(workflowPaths.map(path => [path, fs.readFileSync(path, 'utf8')]))
const forbiddenRuntimeApi = /(?:\bDate\s*\.\s*now\s*\(|\bnew\s+Date\s*\(|\bMath\s*\.\s*random\s*\(|\brandomUUID\s*\()/

for (const [path, source] of sources) {
  assert.equal(forbiddenRuntimeApi.test(source), false, `${path} uses a nondeterministic runtime API`)
  assert.equal(source.includes('\r'), false, `${path} must retain LF line endings`)
  assert.equal(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(source), false, `${path} contains a forbidden control byte`)
}

function functionSource(path, name) {
  const source = sources.get(path)
  const start = source.indexOf(`function ${name}(`)
  assert.notEqual(start, -1, `${name} is missing from ${path}`)
  const open = source.indexOf('{', start)
  let depth = 0
  for (let index = open; index < source.length; index += 1) {
    if (source[index] === '{') depth += 1
    if (source[index] === '}') depth -= 1
    if (depth === 0) return source.slice(start, index + 1)
  }
  return assert.fail(`${name} is unterminated in ${path}`)
}

// Loads one or more mutually referring pure functions into a single fresh
// context, so a run-identity helper still sees the hash it is built on.
function loadFunctions(path, names) {
  const body = names.map(name => functionSource(path, name)).join('\n')
  return vm.runInNewContext(`(() => {\n${body}\nreturn { ${names.join(', ')} }\n})()`)
}

function loadFunction(path, name) {
  return loadFunctions(path, [name])[name]
}

const oracleRunId = loadFunction('.claude/workflows/vol-oracle-iter.js', 'oracleRunId')
const shaA = 'a'.repeat(40)
const shaB = 'b'.repeat(40)
const bootstrapId = oracleRunId(shaA, 'missing_data', '')
assert.equal(bootstrapId, oracleRunId(shaA, 'missing_data', ''), 'oracle bootstrap ID changed for identical state')
assert.notEqual(bootstrapId, oracleRunId(shaA, 'missing_mode_a', ''), 'oracle bootstrap stages collided')
assert.notEqual(oracleRunId(shaA, 'ready', 'iter-1'), oracleRunId(shaA, 'ready', 'iter-2'), 'oracle iterations collided')
assert.notEqual(bootstrapId, oracleRunId(shaB, 'missing_data', ''), 'oracle ID ignored frozen base SHA')

// vol-sprint now dispatches, so its run identity is what has to be reproducible
// across a resumed run rather than its whole return value. The identity is a
// pure function of the frozen base SHA, the task text and the caller key.
const { sprintRunId, deterministicToken } = loadFunctions('.claude/workflows/vol-sprint.js', ['deterministicToken', 'sprintRunId'])
assert.equal(deterministicToken('x'), deterministicToken('x'), 'sprint token changed for identical input')
assert.notEqual(deterministicToken('x'), deterministicToken('y'), 'sprint token collided')
const sprintId = sprintRunId(shaA, 'same task', 'same caller')
assert.equal(sprintId, sprintRunId(shaA, 'same task', 'same caller'), 'sprint run ID changed for identical input')
assert.notEqual(sprintId, sprintRunId(shaB, 'same task', 'same caller'), 'sprint run ID ignored the frozen base SHA')
assert.notEqual(sprintId, sprintRunId(shaA, 'other task', 'same caller'), 'sprint run ID ignored the task')
assert.notEqual(sprintId, sprintRunId(shaA, 'same task', 'other caller'), 'sprint run ID ignored the caller identity')
assert.match(sprintId, new RegExp(`^vol-sprint-${shaA}-[0-9a-f]{16}$`), 'sprint run ID is not registry-conforming')

// The one sprint path that still returns without dispatching is the holdout
// taint refusal, and it must be byte-identical on every replay. Compiling the
// workflow with agent/phase/workflow stubs that throw proves nothing is
// dispatched before that refusal.
const sprintSource = sources.get('.claude/workflows/vol-sprint.js').replace('export const meta', 'const meta')
const compileSprint = new Function('args', 'agent', 'phase', 'workflow', 'pipeline', `return (async () => {\n${sprintSource}\n})()`)
const refuse = () => { throw new Error('vol-sprint dispatched before the holdout taint refusal') }
const taintedArgs = { task: 'improve the holdout cohort score', base: shaA, run_key: 'same caller' }
const first = await compileSprint(taintedArgs, refuse, refuse, refuse, refuse)
const second = await compileSprint(taintedArgs, refuse, refuse, refuse, refuse)
assert.deepEqual(first, second, 'holdout-taint refusal changed for identical input')
assert.equal(first.passed, false)
assert.match(first.failure, /names the holdout cohort/)
assert.deepEqual(first.gate_evidence, [])
assert.equal(first.integration_sha, null)

console.log('workflow determinism contract: PASS')
