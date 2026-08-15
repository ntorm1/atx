import test from 'node:test'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { readFileSync, readdirSync } from 'node:fs'
import { join } from 'node:path'

const workflowDir = '.claude/workflows'
const workflows = readdirSync(workflowDir)
  .filter(name => name.endsWith('.js'))
  .map(name => join(workflowDir, name).replaceAll('\\', '/'))

function forbiddenControlBytes(bytes) {
  return [...bytes].filter(byte => byte < 0x20 && byte !== 0x09 && byte !== 0x0a)
}

test('workflow sources are LF-safe in the index and worktree and parse as Node', () => {
  assert.ok(workflows.length > 0)
  for (const path of workflows) {
    const indexed = execFileSync('git', ['show', `:${path}`])
    const worktree = readFileSync(path)
    assert.deepEqual(forbiddenControlBytes(indexed), [], `${path} index contains forbidden C0 bytes`)
    assert.deepEqual(forbiddenControlBytes(worktree), [], `${path} worktree contains forbidden C0 bytes`)
    execFileSync(process.execPath, ['--check', path], { stdio: 'pipe' })
  }
})
