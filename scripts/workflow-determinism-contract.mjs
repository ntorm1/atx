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

function loadFunction(path, name) {
  const source = sources.get(path)
  const start = source.indexOf(`function ${name}(`)
  assert.notEqual(start, -1, `${name} is missing from ${path}`)
  const open = source.indexOf('{', start)
  let depth = 0
  for (let index = open; index < source.length; index += 1) {
    if (source[index] === '{') depth += 1
    if (source[index] === '}') depth -= 1
    if (depth === 0) return vm.runInNewContext(`(${source.slice(start, index + 1)})`)
  }
  assert.fail(`${name} is unterminated in ${path}`)
}

const oracleRunId = loadFunction('.claude/workflows/vol-oracle-iter.js', 'oracleRunId')
const shaA = 'a'.repeat(40)
const shaB = 'b'.repeat(40)
const bootstrapId = oracleRunId(shaA, 'missing_data', '')
assert.equal(bootstrapId, oracleRunId(shaA, 'missing_data', ''), 'oracle bootstrap ID changed for identical state')
assert.notEqual(bootstrapId, oracleRunId(shaA, 'missing_mode_a', ''), 'oracle bootstrap stages collided')
assert.notEqual(oracleRunId(shaA, 'ready', 'iter-1'), oracleRunId(shaA, 'ready', 'iter-2'), 'oracle iterations collided')
assert.notEqual(bootstrapId, oracleRunId(shaB, 'missing_data', ''), 'oracle ID ignored frozen base SHA')

const sprintSource = sources.get('.claude/workflows/vol-sprint.js').replace('export const meta', 'const meta')
const compileSprint = new Function('args', `return (async () => {\n${sprintSource}\n})()`)
const first = await compileSprint({ task: 'same task', base: shaA, run_key: 'same caller' })
const second = await compileSprint({ task: 'same task', base: shaA, run_key: 'same caller' })
assert.deepEqual(first, second, 'fail-closed sprint result changed for identical input')
assert.equal(first.failure, 'ORACLE_BROKER_MIGRATION_REQUIRED')
assert.equal(first.passed, false)

console.log('workflow determinism contract: PASS')
