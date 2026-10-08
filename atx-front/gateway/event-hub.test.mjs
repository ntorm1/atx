import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { EventHub } from "./event-hub.mjs";
function native() {
  const calls = [];
  return {
    calls,
    watch(query) {
      const stream = Object.assign(new EventEmitter(), {
        cancelled: false,
        cancel() {
          this.cancelled = true;
        },
        pause() {
          throw new Error("Upstream must never pause");
        },
      });
      calls.push({ query, stream });
      return stream;
    },
  };
}
const event = (n, channel = "jobs", job_id = "a", gap = false) => ({
  sequence: String(n),
  channel,
  job_id,
  type: gap ? "gap" : "job.progress",
  payload_json: '{"progress":0.5}',
  created_at_ms: "1700000000000",
  gap,
});
function sink({ fail = false } = {}) {
  return {
    chunks: [],
    reasons: [],
    send(chunk) {
      this.chunks.push(chunk);
      return !fail;
    },
    close(reason) {
      this.reasons.push(reason);
    },
  };
}
test("hundreds of independent browsers share one native subscription", (t) => {
  const client = native(),
    hub = new EventHub(client);
  t.after(() => hub.close());
  const browsers = Array.from({ length: 200 }, () => sink());
  browsers.forEach((s) => hub.subscribe({}, s));
  assert.equal(client.calls.length, 1);
  assert.deepEqual(client.calls[0].query, {
    channel: "all",
    job_id: "",
    after_sequence: "0",
  });
  client.calls[0].stream.emit("data", event(1));
  browsers.forEach((s) => assert.match(s.chunks.at(-1), /id: 1/));
});
test("local replay filters jobs/channels and signals stale/future cursor gaps", (t) => {
  const client = native(),
    hub = new EventHub(client, { maxEvents: 3 });
  t.after(() => hub.close());
  hub.subscribe({}, sink());
  for (let n = 1; n <= 5; n++)
    client.calls[0].stream.emit(
      "data",
      event(n, n === 4 ? "system" : "jobs", n === 5 ? "b" : "a"),
    );
  const filtered = sink();
  hub.subscribe(
    { after_sequence: "1", channel: "jobs", job_id: "a" },
    filtered,
  );
  assert.match(filtered.chunks[0], /"gap":true/);
  assert.match(filtered.chunks[1], /id: 3/);
  assert.equal(filtered.chunks.length, 2);
  const future = sink();
  hub.subscribe({ after_sequence: "999" }, future);
  assert.match(future.chunks[0], /id: 5/);
  assert.match(future.chunks[0], /"gap":true/);
});
test("one disconnected or slow browser never pauses or cancels other consumers", (t) => {
  const client = native(),
    hub = new EventHub(client);
  t.after(() => hub.close());
  const removed = sink(),
    slow = sink({ fail: true }),
    good = sink();
  const remove = hub.subscribe({}, removed);
  hub.subscribe({}, slow);
  hub.subscribe({}, good);
  remove();
  client.calls[0].stream.emit("data", event(1));
  client.calls[0].stream.emit("data", event(2));
  assert.equal(removed.chunks.length, 0);
  assert.equal(slow.chunks.length, 1);
  assert.match(slow.reasons[0], /Slow consumer/);
  assert.equal(good.chunks.length, 2);
  assert.equal(client.calls[0].stream.cancelled, false);
  assert.equal(hub.size, 1);
});
test("native outage reconnects with last sequence and epoch gap forces snapshot", async (t) => {
  const client = native(),
    hub = new EventHub(client, { retryMs: 5, maxRetryMs: 20 });
  t.after(() => hub.close());
  const first = sink();
  hub.subscribe({}, first);
  client.calls[0].stream.emit("data", event(9));
  client.calls[0].stream.emit("error", new Error("offline"));
  assert.match(first.reasons[0], /snapshot refresh/);
  await new Promise((resolve) => setTimeout(resolve, 15));
  assert.equal(client.calls.length, 2);
  assert.equal(client.calls[1].query.after_sequence, "9");
  const second = sink();
  hub.subscribe({ after_sequence: "9" }, second);
  client.calls[1].stream.emit("data", event(4097, "system", "", true));
  assert.match(second.chunks.at(-1), /"gap":true/);
  assert.equal(hub.ring.length, 1);
  hub.close();
  assert.equal(client.calls[1].stream.cancelled, true);
  assert.equal(hub.bytes, 0);
});
test("hub replay byte and subscriber budgets hold under sustained delivery", (t) => {
  const client = native(),
    hub = new EventHub(client, { maxBytes: 600, maxSubscribers: 2 });
  t.after(() => hub.close());
  hub.subscribe({}, sink());
  hub.subscribe({}, sink());
  assert.throws(() => hub.subscribe({}, sink()), /capacity/);
  for (let n = 1; n <= 50; n++) client.calls[0].stream.emit("data", event(n));
  assert.ok(hub.bytes <= 600);
  assert.ok(hub.ring.length <= 3);
  assert.equal(client.calls.length, 1);
});
test("idle hub keeps native delivery and replays events accumulated without browsers", (t) => {
  const client = native(),
    hub = new EventHub(client);
  t.after(() => hub.close());
  const first = sink();
  const disconnect = hub.subscribe({}, first);
  client.calls[0].stream.emit("data", event(1));
  disconnect();
  assert.equal(hub.size, 0);
  for (let n = 2; n <= 5; n++) client.calls[0].stream.emit("data", event(n));
  const returning = sink();
  hub.subscribe({ after_sequence: "3" }, returning);
  assert.equal(client.calls.length, 1);
  assert.equal(returning.chunks.length, 2);
  assert.match(returning.chunks[0], /id: 4/);
  assert.match(returning.chunks[1], /id: 5/);
});
test("future cursor against a fresh hub receives a snapshot gap before native boot replay", (t) => {
  const client = native(),
    hub = new EventHub(client);
  t.after(() => hub.close());
  const browser = sink();
  hub.subscribe({ after_sequence: "999999999" }, browser);
  assert.match(browser.chunks[0], /"gap":true/);
  client.calls[0].stream.emit("data", event(4097, "system", "", true));
  client.calls[0].stream.emit("data", event(4098));
  assert.match(browser.chunks[1], /id: 4097/);
  assert.match(browser.chunks[2], /id: 4098/);
});
test("malformed native event closes subscribers and schedules bounded recovery", (t) => {
  const client = native(),
    hub = new EventHub(client);
  t.after(() => hub.close());
  const browser = sink();
  hub.subscribe({}, browser);
  client.calls[0].stream.emit("data", { ...event(1), payload_json: "{broken" });
  assert.equal(browser.chunks.length, 0);
  assert.match(browser.reasons[0], /snapshot refresh/);
  assert.equal(client.calls[0].stream.cancelled, true);
});
