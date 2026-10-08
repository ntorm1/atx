import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";

const id = "a".repeat(32);
const alpha = "b".repeat(32);
const job = "c".repeat(32);
const nativeWith = (unary) => ({
  unary,
  watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
});
const record = {
  id,
  alpha_id: alpha,
  alpha_revision: "9007199254740993",
  revision: "1",
  state: "submitted",
  requester_actor_id: "alice",
  author_actor_id: "alice",
  evidence_json: '{"jobs":[{"id":"evidence","synthetic":true}]}',
  current: true,
};

test("review submission freezes exact revisions and exposes native evidence without actor claims", async (t) => {
  const calls = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      calls.push({ method, body });
      return record;
    }),
  });
  t.after(() => app.close());
  const response = await app.inject({
    method: "POST",
    url: "/api/reviews",
    payload: {
      alpha_id: alpha,
      alpha_revision: "9007199254740993",
      job_ids: [job],
      note: "Research review",
      idempotency_key: "same-review",
    },
  });
  assert.equal(response.statusCode, 201);
  assert.equal(calls[0].method, "submitReview");
  assert.equal(calls[0].body.alpha_revision, "9007199254740993");
  assert.equal(calls[0].body.actor_id, undefined);
  assert.equal(response.json().evidence.jobs[0].synthetic, true);
  assert.equal(response.json().evidence_json, undefined);
});

test("review decisions preserve compare-and-swap and native permission failures", async (t) => {
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      assert.equal(method, "decideReview");
      assert.equal(body.expected_revision, "2");
      assert.equal(body.state, "approved");
      throw Object.assign(new Error("Independent reviewer required"), {
        code: 7,
      });
    }),
  });
  t.after(() => app.close());
  const response = await app.inject({
    method: "POST",
    url: `/api/reviews/${id}/decision`,
    payload: {
      expected_revision: "2",
      state: "approved",
      rationale: "Checked source evidence",
    },
  });
  assert.equal(response.statusCode, 403);
});

test("review boundaries reject spoofed actors, rounded revisions and oversized evidence lists", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return record;
    }),
  });
  t.after(() => app.close());
  const input = { alpha_id: alpha, alpha_revision: "1", job_ids: [job] };
  for (const payload of [
    { ...input, actor_id: "admin" },
    { ...input, alpha_revision: 1 },
    { ...input, alpha_revision: "9223372036854775808" },
    { ...input, job_ids: [job, job] },
    {
      ...input,
      job_ids: Array.from({ length: 13 }, (_, n) =>
        n.toString(16).padStart(32, "0"),
      ),
    },
    { ...input, note: "é".repeat(2049) },
  ]) {
    const response = await app.inject({
      method: "POST",
      url: "/api/reviews",
      payload,
    });
    assert.equal(
      response.statusCode,
      400,
      JSON.stringify(payload).slice(0, 200),
    );
  }
  const empty = await app.inject({
    method: "POST",
    url: `/api/reviews/${id}/decision`,
    payload: { expected_revision: "1", state: "approved", rationale: "   " },
  });
  assert.equal(empty.statusCode, 400);
  assert.equal(calls, 0);
});

test("audit and review browsing decode bounded metadata and omit heavy list evidence", async (t) => {
  const app = await createApp({
    native: nativeWith(async (method) => {
      if (method === "listAudit")
        return {
          records: [
            {
              sequence: "9007199254740993",
              actor_id: "alice",
              metadata_json: '{"state":"submitted"}',
            },
          ],
          total: "1",
        };
      if (method === "listReviewDecisions")
        return { decisions: [], total: "0" };
      if (method === "getReview") return record;
      return { reviews: [record], total: "1" };
    }),
  });
  t.after(() => app.close());
  const audit = await app.inject(
    `/api/audit?entity_type=review&entity_id=${id}&actor_id=alice`,
  );
  assert.equal(audit.statusCode, 200);
  assert.equal(audit.json().records[0].sequence, "9007199254740993");
  assert.deepEqual(audit.json().records[0].metadata, { state: "submitted" });
  const page = await app.inject("/api/reviews");
  assert.equal(page.statusCode, 200);
  assert.equal(page.json().reviews[0].evidence, undefined);
  assert.equal(page.json().reviews[0].evidence_json, undefined);
  assert.equal((await app.inject(`/api/reviews/${id}`)).json().current, true);
  assert.equal(
    (await app.inject(`/api/reviews/${id}/decisions`)).statusCode,
    200,
  );
});
