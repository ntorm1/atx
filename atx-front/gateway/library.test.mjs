import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";
const recipe = {
  mode: "synthetic_demo",
  strategy_id: "synthetic-long-short-v1",
  strategy_dsl: "rank(close)",
};
const metadata = {
  id: "alpha-1",
  project_id: "local",
  name: "Alpha",
  description: "",
  tags: ["value"],
  stage: "research",
  revision: "9007199254740993",
  request_sha256: "a".repeat(64),
  source_job_id: "",
  created_at_ms: "1",
  updated_at_ms: "2",
};
const nativeWith = (unary) => ({
  unary,
  watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
});
test("alpha routes preserve immutable recipe and exact revision, and omit heavy summary fields", async (t) => {
  const seen = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      seen.push({ method, body });
      const record = {
        ...metadata,
        hypothesis: "Causal hypothesis",
        request_json: JSON.stringify(recipe),
      };
      if (method === "listAlphas") return { alphas: [record], total: "1" };
      if (method === "listAlphaRevisions")
        return { revisions: [record], total: "1" };
      return record;
    }),
  });
  t.after(() => app.close());
  const list = await app.inject("/api/alphas?include_archived=true");
  assert.equal(list.statusCode, 200);
  assert.deepEqual(list.json().alphas, [metadata]);
  assert.equal(seen[0].body.include_archived, true);
  const created = await app.inject({
    method: "POST",
    url: "/api/alphas",
    payload: { name: "Alpha", request: recipe, tags: ["value"] },
  });
  assert.equal(created.statusCode, 201);
  assert.deepEqual(created.json().request, recipe);
  assert.equal(seen[1].body.request_json, JSON.stringify(recipe));
  assert.equal(seen[1].body.project_id, "local");
  const updated = await app.inject({
    method: "PUT",
    url: "/api/alphas/alpha-1",
    payload: {
      name: "Alpha",
      request: recipe,
      expected_revision: metadata.revision,
    },
  });
  assert.equal(updated.statusCode, 200);
  assert.equal(seen[2].body.expected_revision, metadata.revision);
  const archived = await app.inject({
    method: "POST",
    url: "/api/alphas/alpha-1/archive",
    payload: { expected_revision: metadata.revision, archived: true },
  });
  assert.equal(archived.statusCode, 200);
  assert.equal(seen[3].body.archived, true);
  const history = await app.inject(
    "/api/alphas/alpha-1/revisions?limit=50&offset=100",
  );
  assert.deepEqual(history.json().revisions, [metadata]);
  assert.deepEqual(seen[4].body, { id: "alpha-1", limit: 50, offset: 100 });
});
test("library limits reject credential, Unicode byte, metadata and revision overflows before native work", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return { ...metadata, request_json: JSON.stringify(recipe) };
    }),
  });
  t.after(() => app.close());
  for (const payload of [
    { name: "a".repeat(81), request: recipe },
    { name: "Alpha", request: recipe, description: "é".repeat(257) },
    { name: "Alpha", request: recipe, hypothesis: "é".repeat(2049) },
    { name: "Alpha", request: recipe, tags: ["é".repeat(33)] },
    { name: "Alpha", request: recipe, tags: [" x"] },
    { name: "Alpha", request: { ...recipe, access_token: "secret" } },
    { name: "Alpha", request: { ...recipe, strategy_dsl: "é".repeat(40000) } },
    { name: "Alpha", request: recipe, project_id: "other" },
  ])
    assert.equal(
      (await app.inject({ method: "POST", url: "/api/alphas", payload }))
        .statusCode,
      400,
    );
  for (const expected_revision of [
    "0",
    "9223372036854775808",
    9007199254740992,
  ])
    assert.equal(
      (
        await app.inject({
          method: "PUT",
          url: "/api/alphas/alpha-1",
          payload: { name: "Alpha", request: recipe, expected_revision },
        })
      ).statusCode,
      400,
    );
  for (const url of [
    "/api/alphas?limit=101",
    "/api/alphas?search=" + "é".repeat(65),
    "/api/alphas/alpha-1?revision=9223372036854775808",
  ])
    assert.equal((await app.inject(url)).statusCode, 400);
  assert.equal(calls, 0);
});
test("saved alpha submission resolves an exact native revision and validates pair semantics", async (t) => {
  const seen = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      seen.push({ method, body });
      return {
        id: "a".repeat(32),
        request_json: JSON.stringify(recipe),
        result_json: "",
        status: "queued",
      };
    }),
  });
  t.after(() => app.close());
  const submitted = await app.inject({
    method: "POST",
    url: "/api/jobs",
    payload: {
      alpha_id: "alpha-1",
      alpha_revision: metadata.revision,
      label: "Saved alpha r1",
      idempotency_key: "exact-library-revision",
    },
  });
  assert.equal(submitted.statusCode, 201);
  assert.deepEqual(seen[0], {
    method: "submitJob",
    body: {
      request_json: "",
      alpha_id: "alpha-1",
      alpha_revision: metadata.revision,
      label: "Saved alpha r1",
      idempotency_key: "exact-library-revision",
      scheduled_at_ms: 0,
    },
  });
  for (const payload of [
    { alpha_id: "alpha-1" },
    { alpha_revision: "1" },
    { alpha_id: "alpha-1", alpha_revision: 1 },
    { alpha_id: "alpha-1", alpha_revision: "9223372036854775808" },
    { label: "é".repeat(129), request: recipe },
  ])
    assert.equal(
      (
        await app.inject({
          method: "POST",
          url: "/api/jobs",
          payload: { ...payload, idempotency_key: "bounded" },
        })
      ).statusCode,
      400,
    );
  assert.equal(seen.length, 1);
});
