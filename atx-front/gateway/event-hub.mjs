const UINT64_MAX = 18446744073709551615n;
const sequence = (value) => {
  const text = String(value);
  if (!/^\d{1,20}$/.test(text) || BigInt(text) > UINT64_MAX)
    throw new Error("Invalid event sequence");
  return BigInt(text);
};
const encode = (event) =>
  `id: ${event.sequence}\nevent: workspace\ndata: ${JSON.stringify(event)}\n\n`;

// Delivery state only: durable business state stays in the native service. One
// upstream serves every browser; a slow browser can never pause native delivery.
export class EventHub {
  constructor(
    native,
    {
      maxSubscribers = 512,
      maxEvents = 2048,
      maxBytes = 4 * 1024 * 1024,
      maxEventBytes = 256 * 1024,
      retryMs = 1000,
      maxRetryMs = 30000,
    } = {},
  ) {
    this.native = native;
    this.maxSubscribers = maxSubscribers;
    this.maxEvents = maxEvents;
    this.maxBytes = maxBytes;
    this.maxEventBytes = maxEventBytes;
    this.retryMs = retryMs;
    this.maxRetryMs = maxRetryMs;
    this.delay = retryMs;
    this.clients = new Set();
    this.ring = [];
    this.bytes = 0;
    this.cursor = 0n;
    this.stream = null;
    this.timer = null;
    this.closed = false;
    this.started = false;
  }
  get size() {
    return this.clients.size;
  }
  gap(cursor = this.cursor) {
    return {
      sequence: cursor.toString(),
      channel: "system",
      type: "gap",
      job_id: "",
      payload: { reason: "Snapshot refresh required" },
      created_at_ms: String(Date.now()),
      gap: true,
    };
  }
  deliver(client, event, chunk = encode(event)) {
    if (!this.clients.has(client)) return;
    const cursor = sequence(event.sequence);
    if (!event.gap && cursor <= client.cursor) return;
    client.cursor = cursor;
    if (
      !event.gap &&
      ((client.channel !== "all" && client.channel !== event.channel) ||
        (client.jobId && client.jobId !== event.job_id))
    )
      return;
    if (!client.sink.send(chunk)) {
      this.clients.delete(client);
      client.sink.close("Slow consumer; snapshot refresh required");
    }
  }
  subscribe(query, sink) {
    if (this.closed || this.size >= this.maxSubscribers)
      throw Object.assign(new Error("Event connection capacity reached"), {
        statusCode: 429,
      });
    const client = {
      cursor: sequence(query.after_sequence ?? "0"),
      channel: query.channel ?? "all",
      jobId: query.job_id ?? "",
      sink,
    };
    this.clients.add(client);
    const oldest = this.ring[0]?.cursor ?? this.cursor + 1n;
    if (client.cursor > this.cursor) this.deliver(client, this.gap());
    else if (client.cursor < oldest - 1n)
      this.deliver(client, this.gap(oldest - 1n));
    for (const record of this.ring)
      if (record.cursor > client.cursor)
        this.deliver(client, record.event, record.chunk);
    if (!this.started) {
      this.started = true;
      this.connect();
    }
    return () => this.clients.delete(client);
  }
  accept(raw) {
    const cursor = sequence(raw.sequence);
    if (
      typeof raw.channel !== "string" ||
      !["system", "jobs"].includes(raw.channel) ||
      typeof raw.type !== "string" ||
      typeof raw.job_id !== "string" ||
      typeof raw.gap !== "boolean"
    )
      throw new Error("Invalid event envelope");
    const payload = raw.payload_json ? JSON.parse(raw.payload_json) : null;
    const event = {
      ...raw,
      sequence: cursor.toString(),
      payload,
      payload_json: undefined,
    };
    const chunk = encode(event),
      payloadBytes = Buffer.byteLength(chunk),
      bytes =
        payloadBytes +
        256 +
        2 *
          (event.sequence.length + event.channel.length + event.job_id.length);
    if (payloadBytes > this.maxEventBytes || bytes > this.maxBytes)
      throw new Error("Event exceeds delivery budget");
    if (!raw.gap && cursor <= this.cursor) return;
    if (raw.gap) {
      this.ring = [];
      this.bytes = 0;
    }
    this.cursor = cursor;
    while (
      this.ring.length &&
      (this.ring.length >= this.maxEvents || this.bytes + bytes > this.maxBytes)
    )
      this.bytes -= this.ring.shift().bytes;
    // Keep one serialized payload, plus only the metadata needed for filtering.
    // Conservative bookkeeping overhead also participates in the byte budget.
    const metadata = {
      sequence: event.sequence,
      channel: event.channel,
      job_id: event.job_id,
      gap: event.gap,
    };
    this.ring.push({ cursor, event: metadata, chunk, bytes });
    this.bytes += bytes;
    for (const client of [...this.clients]) this.deliver(client, event, chunk);
    this.delay = this.retryMs;
  }
  connect() {
    if (this.closed || this.stream) return;
    let stream;
    try {
      stream = this.native.watch({
        channel: "all",
        job_id: "",
        after_sequence: this.cursor.toString(),
      });
    } catch {
      this.disconnected();
      return;
    }
    this.stream = stream;
    stream.on("data", (raw) => {
      if (this.stream !== stream || this.closed) return;
      try {
        this.accept(raw);
      } catch {
        this.disconnected(stream);
      }
    });
    stream.on("error", () => this.disconnected(stream));
    stream.on("end", () => this.disconnected(stream));
  }
  disconnected(stream) {
    if (this.closed || (stream && this.stream !== stream)) return;
    const active = this.stream;
    this.stream = null;
    active?.cancel();
    for (const client of [...this.clients]) {
      this.clients.delete(client);
      client.sink.close(
        "Native channel interrupted; snapshot refresh required",
      );
    }
    if (!this.timer) {
      const delay = this.delay;
      this.delay = Math.min(this.delay * 2, this.maxRetryMs);
      this.timer = setTimeout(() => {
        this.timer = null;
        this.connect();
      }, delay);
      this.timer.unref?.();
    }
  }
  close() {
    if (this.closed) return;
    this.closed = true;
    clearTimeout(this.timer);
    this.timer = null;
    const stream = this.stream;
    this.stream = null;
    stream?.cancel();
    for (const client of [...this.clients]) {
      this.clients.delete(client);
      client.sink.close();
    }
    this.ring = [];
    this.bytes = 0;
  }
}
