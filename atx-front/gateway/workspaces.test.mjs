import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";
import { workspaceDocument } from "./workspaces.mjs";

const document = {
  schema_version: 1,
  layout: { grid: {}, panels: {} },
  panel_state: {
    draft_request: { mode: "synthetic_demo" },
    database: { sql: "SELECT 'é'" },
  },
};
const nativeWith = (unary) => ({
  unary,
  watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
});
const metadata = {
  id: "work-1",
  name: "Research",
  description: "",
  revision: "9007199254740993",
  created_at_ms: "1",
  updated_at_ms: "2",
};
test("workspace routes translate object documents and preserve exact CAS revisions", async (t) => {
  const seen = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      seen.push({ method, body });
      if (method === "listWorkspaces")
        return { workspaces: [{ ...metadata, document_json: "" }], total: "1" };
      if (method === "deleteWorkspace") return {};
      return { ...metadata, document_json: JSON.stringify(document) };
    }),
  });
  t.after(() => app.close());
  const list = await app.inject("/api/workspaces");
  assert.deepEqual(list.json(), { workspaces: [metadata], total: "1" });
  assert.deepEqual(seen[0], {
    method: "listWorkspaces",
    body: { limit: 100, offset: 0 },
  });
  const created = await app.inject({
    method: "POST",
    url: "/api/workspaces",
    payload: { name: "Research", document },
  });
  assert.equal(created.statusCode, 201);
  assert.deepEqual(created.json(), { ...metadata, document });
  assert.equal(seen[1].body.document_json, JSON.stringify(document));
  assert.equal(seen[1].body.description, "");
  const updated = await app.inject({
    method: "PUT",
    url: "/api/workspaces/work-1",
    payload: {
      name: "Research",
      document,
      expected_revision: metadata.revision,
    },
  });
  assert.equal(updated.statusCode, 200);
  assert.equal(seen[2].body.expected_revision, "9007199254740993");
  const deleted = await app.inject({
    method: "DELETE",
    url: "/api/workspaces/work-1",
    payload: { expected_revision: metadata.revision },
  });
  assert.equal(deleted.statusCode, 204);
  assert.equal(deleted.body, "");
  assert.deepEqual(seen[3], {
    method: "deleteWorkspace",
    body: { id: "work-1", expected_revision: metadata.revision },
  });
});
test("workspace bounds reject byte overflow, invalid revisions, depth, credentials and unknown fields", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return { ...metadata, document_json: JSON.stringify(document) };
    }),
  });
  t.after(() => app.close());
  const create = (payload) =>
    app.inject({ method: "POST", url: "/api/workspaces", payload });
  for (const payload of [
    { name: "a".repeat(81), document },
    { name: "Research", description: "é".repeat(257), document },
    {
      name: "Research",
      document: { ...document, panel_state: { sql: "é".repeat(50000) } },
    },
    {
      name: "Research",
      document: { ...document, panel_state: { access_token: "secret" } },
    },
    { name: "Research", document, unexpected: true },
  ])
    assert.equal((await create(payload)).statusCode, 400);
  let nested = {};
  for (let i = 0; i < 35; i++) nested = { child: nested };
  assert.equal(
    (
      await create({
        name: "Research",
        document: { ...document, panel_state: nested },
      })
    ).statusCode,
    400,
  );
  for (const expected_revision of [
    "0",
    "18446744073709551616",
    9007199254740992,
  ])
    assert.equal(
      (
        await app.inject({
          method: "PUT",
          url: "/api/workspaces/work-1",
          payload: { name: "Research", document, expected_revision },
        })
      ).statusCode,
      400,
    );
  for (const url of [
    "/api/workspaces?limit=101",
    "/api/workspaces?offset=1000001",
    "/api/workspaces/bad.id",
  ])
    assert.equal((await app.inject(url)).statusCode, 400);
  assert.equal(calls, 0);
  assert.doesNotThrow(() =>
    workspaceDocument({ ...document, panel_state: { sql: "SELECT 'token'" } }),
  );
});
test("CAS conflicts preserve 409 semantics and malformed native documents stay private", async (t) => {
  let corrupt = false;
  const app = await createApp({
    native: nativeWith(async () => {
      if (corrupt)
        return {
          ...metadata,
          document_json: "secret malformed native response",
        };
      throw Object.assign(new Error("Workspace revision changed"), {
        code: 10,
        details: "Workspace revision changed",
      });
    }),
  });
  t.after(() => app.close());
  const conflict = await app.inject({
    method: "PUT",
    url: "/api/workspaces/work-1",
    payload: { name: "Research", document, expected_revision: "1" },
  });
  assert.equal(conflict.statusCode, 409);
  corrupt = true;
  const bad = await app.inject("/api/workspaces/work-1");
  assert.equal(bad.statusCode, 502);
  assert.doesNotMatch(bad.body, /secret|malformed/);
});
