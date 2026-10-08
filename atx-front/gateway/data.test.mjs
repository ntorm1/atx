import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";

const result = {
  source_id: "atx-db",
  columns: [
    { name: "id", type: "BIGINT" },
    { name: "note", type: "VARCHAR" },
  ],
  rows: [
    {
      cells: [
        { text: "9223372036854775807", is_null: false, truncated: false },
        { text: "", is_null: true, truncated: false },
      ],
    },
  ],
  has_more: true,
  truncated: false,
  elapsed_ms: "8",
  warnings: [],
};
const nativeWith = (unary) => ({
  unary,
  watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
});
test("provider namespaces may be empty while table identifiers remain required", async (t) => {
  let seen;
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      seen = { method, body };
      return result;
    }),
  });
  t.after(() => app.close());
  const response = await app.inject(
    "/api/data/preview?source_id=archive&catalog=&schema=&table=prices",
  );
  assert.equal(response.statusCode, 200);
  assert.deepEqual(seen, {
    method: "previewData",
    body: {
      source_id: "archive",
      catalog: "",
      schema: "",
      table: "prices",
      limit: 200,
      offset: 0,
    },
  });
  assert.equal(
    (
      await app.inject(
        "/api/data/preview?source_id=archive&catalog=&schema=&table=",
      )
    ).statusCode,
    400,
  );
});
test("database busy/deadline/capacity failures provide safe actionable errors", async (t) => {
  let code = 14;
  const app = await createApp({
    native: nativeWith(async () => {
      throw Object.assign(
        new Error("secret connection C:/private/warehouse.db"),
        { code, details: "secret connection C:/private/warehouse.db" },
      );
    }),
  });
  t.after(() => app.close());
  for (const [nextCode, status, explanation] of [
    [14, 503, /writer has finished/],
    [4, 504, /execution deadline/],
    [8, 429, /capacity is in use/],
  ]) {
    code = nextCode;
    const response = await app.inject({
      method: "POST",
      url: "/api/data/query",
      payload: { source_id: "atx-db", sql: "SELECT 1" },
    });
    assert.equal(response.statusCode, status);
    assert.match(response.json().error, explanation);
    assert.doesNotMatch(response.body, /secret|private|warehouse/);
  }
});
test("database adapter preserves provider DTOs, exact integers and SQL NULL", async (t) => {
  const calls = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      calls.push({ method, body });
      return result;
    }),
  });
  t.after(() => app.close());
  const response = await app.inject(
    "/api/data/preview?source_id=atx-db&catalog=atx&schema=main&table=prices",
  );
  assert.equal(response.statusCode, 200);
  assert.deepEqual(response.json(), result);
  assert.deepEqual(calls, [
    {
      method: "previewData",
      body: {
        source_id: "atx-db",
        catalog: "atx",
        schema: "main",
        table: "prices",
        limit: 200,
        offset: 0,
      },
    },
  ]);
  const query = await app.inject({
    method: "POST",
    url: "/api/data/query",
    payload: {
      source_id: "atx-db",
      sql: "SELECT 9223372036854775807 AS id, NULL AS note",
      max_rows: 50,
    },
  });
  assert.equal(query.statusCode, 200);
  assert.deepEqual(calls.at(-1), {
    method: "queryData",
    body: {
      source_id: "atx-db",
      sql: "SELECT 9223372036854775807 AS id, NULL AS note",
      max_rows: 50,
    },
  });
});
test("database browse routes use curated IDs and native catalog metadata", async (t) => {
  const calls = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      calls.push({ method, body });
      return {};
    }),
  });
  t.after(() => app.close());
  assert.equal((await app.inject("/api/data/sources")).statusCode, 200);
  assert.equal(
    (await app.inject("/api/data/catalog?source_id=atx-db")).statusCode,
    200,
  );
  assert.deepEqual(calls, [
    { method: "listDataSources", body: {} },
    { method: "getDataCatalog", body: { source_id: "atx-db" } },
  ]);
});
test("database envelope bounds and connection/path injection fail before native access", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return result;
    }),
  });
  t.after(() => app.close());
  for (const url of [
    "/api/data/catalog?source_id=C%3A%2Fsecret.db",
    "/api/data/catalog?source_id=atx-db&path=C%3A%2Fsecret.db",
    "/api/data/preview?source_id=atx-db&catalog=atx&schema=main&table=prices&limit=501",
    "/api/data/preview?source_id=atx-db&catalog=atx&schema=main&table=prices&offset=1000001",
  ])
    assert.equal((await app.inject(url)).statusCode, 400);
  for (const payload of [
    { source_id: "atx-db", sql: "SELECT 1", max_rows: 501 },
    { source_id: "atx-db", sql: "SELECT 1", connection_string: "secret" },
    { source_id: "atx-db", sql: "SELECT 1", path: "secret.db" },
    { source_id: "atx-db", sql: " " },
    { source_id: "atx-db", sql: `SELECT '${"😀".repeat(17000)}'` },
  ])
    assert.equal(
      (await app.inject({ method: "POST", url: "/api/data/query", payload }))
        .statusCode,
      400,
    );
  assert.equal(calls, 0);
});
test("database endpoints inherit auth/origin policy and surface native query errors", async (t) => {
  let calls = 0;
  const app = await createApp({
    webToken: "browser-key",
    native: nativeWith(async () => {
      calls++;
      throw Object.assign(new Error("read-only SELECT required"), {
        code: 3,
        details: "read-only SELECT required",
      });
    }),
  });
  t.after(() => app.close());
  assert.equal((await app.inject("/api/data/sources")).statusCode, 401);
  const payload = { source_id: "atx-db", sql: "DELETE FROM prices" };
  assert.equal(
    (
      await app.inject({
        method: "POST",
        url: "/api/data/query",
        payload,
        headers: {
          authorization: "Bearer browser-key",
          origin: "https://attacker.test",
        },
      })
    ).statusCode,
    403,
  );
  assert.equal(calls, 0);
  const response = await app.inject({
    method: "POST",
    url: "/api/data/query",
    payload,
    headers: { authorization: "Bearer browser-key" },
  });
  assert.equal(response.statusCode, 400);
  assert.equal(response.json().error, "read-only SELECT required");
  assert.equal(calls, 1);
});
