import Fastify from "fastify";
import { EventHub } from "./event-hub.mjs";
import { registerDataRoutes } from "./data.mjs";
import { registerWorkspaceRoutes } from "./workspaces.mjs";
import { registerAnalysisRoutes } from "./analysis.mjs";
import { registerLibraryRoutes } from "./library.mjs";
import { registerExperimentRoutes } from "./experiments.mjs";
import { registerGovernanceRoutes } from "./governance.mjs";
import { registerWorkflowRoutes } from "./workflows.mjs";
import rateLimit from "@fastify/rate-limit";
import serveStatic from "@fastify/static";
import { timingSafeEqual } from "node:crypto";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

const httpStatus = {
  1: 499,
  3: 400,
  4: 504,
  5: 404,
  6: 409,
  7: 403,
  8: 429,
  9: 409,
  10: 409,
  11: 400,
  12: 501,
  14: 503,
  16: 401,
};
const parseDocument = (value) => {
  if (!value) return null;
  try {
    return JSON.parse(value);
  } catch {
    throw Object.assign(new Error("Invalid document from native server"), {
      code: 13,
    });
  }
};
export const decodeJob = (job) => {
  const { request_json, result_json, ...rest } = job;
  return {
    ...rest,
    request: parseDocument(request_json),
    result: parseDocument(result_json),
  };
};
const secureEqual = (a, b) => {
  const x = Buffer.from(a);
  const y = Buffer.from(b);
  return x.length === y.length && timingSafeEqual(x, y);
};

export async function createApp({
  native,
  webToken = "",
  teamAuth = false,
  logger = false,
  staticRoot = fileURLToPath(new URL("../dist", import.meta.url)),
  maxStreams = 512,
  requestsPerMinute = 240,
  allowedHostnames = ["localhost", "127.0.0.1", "[::1]"],
} = {}) {
  if (!native) throw new Error("Native gRPC client is required");
  if (teamAuth && webToken)
    throw new Error(
      "Cannot combine team authorization and a shared browser token",
    );
  const app = Fastify({
    logger,
    bodyLimit: 1024 * 1024,
    requestTimeout: 20_000,
    trustProxy: false,
    ajv: { customOptions: { removeAdditional: false } },
  });
  const hub = new EventHub(native, { maxSubscribers: maxStreams });
  const hosts = new Set(allowedHostnames.map((host) => host.toLowerCase()));
  await app.register(rateLimit, {
    max: requestsPerMinute,
    timeWindow: "1 minute",
  });
  app.addHook("onRequest", async (req, reply) => {
    reply
      .header("X-Content-Type-Options", "nosniff")
      .header("Referrer-Policy", "no-referrer");
    reply.header(
      "Content-Security-Policy",
      "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'",
    );
    let hostname;
    try {
      hostname = new URL(`http://${req.headers.host}`).hostname.toLowerCase();
    } catch {
      /* Missing or malformed Host is denied. */
    }
    if (!hostname || !hosts.has(hostname))
      return reply
        .code(403)
        .send({ error: "This hostname is not allowed for the workstation" });
    if (
      req.routeOptions.url?.startsWith("/api/") ||
      req.url.startsWith("/api/")
    ) {
      reply.header("Cache-Control", "no-store");
      if (teamAuth) {
        const authorization = req.headers.authorization;
        const raw = req.raw.rawHeaders;
        let count = 0;
        for (let index = 0; index < raw.length; index += 2)
          if (raw[index].toLowerCase() === "authorization") count++;
        if (
          count !== 1 ||
          typeof authorization !== "string" ||
          !/^Bearer [A-Za-z0-9_-]{32,256}$/.test(authorization)
        )
          return reply
            .code(401)
            .send({ error: "A team access token is required", code: 16 });
      }
      if (["POST", "PUT", "PATCH", "DELETE"].includes(req.method)) {
        const origin = req.headers.origin;
        // Browser requests must originate from this gateway. Non-browser gRPC
        // tooling can omit Origin; production non-loopback binds require auth.
        if (req.headers["sec-fetch-site"] === "cross-site")
          return reply
            .code(403)
            .send({ error: "Cross-site requests are not permitted" });
        if (origin) {
          let allowed = false;
          try {
            const parsed = new URL(origin);
            allowed =
              ["http:", "https:"].includes(parsed.protocol) &&
              parsed.host === req.headers.host;
          } catch {
            /* Invalid Origin is denied. */
          }
          if (!allowed)
            return reply.code(403).send({
              error: "Request origin does not match this workstation",
            });
        }
      }
      if (
        webToken &&
        !secureEqual(req.headers.authorization ?? "", `Bearer ${webToken}`)
      )
        return reply.code(401).send({
          error: "A workstation access token is required",
          code: "UNAUTHENTICATED",
        });
    }
  });
  app.setErrorHandler((error, req, reply) => {
    const status =
      typeof error.code === "number"
        ? (httpStatus[error.code] ?? 502)
        : (error.statusCode ?? 500);
    if (status >= 500) req.log.error({ err: error }, "Gateway request failed");
    const databaseRequest = req.routeOptions.url?.startsWith("/api/data/");
    const databaseError = databaseRequest
      ? {
          4: "Database query exceeded its execution deadline; narrow the query and retry.",
          8: "Database query capacity is in use; retry shortly.",
          14: "Database service is busy or unavailable; retry after any database writer has finished.",
        }[error.code]
      : undefined;
    reply.code(status).send({
      error:
        databaseError ??
        (status >= 500
          ? "Native service unavailable or request failed"
          : (error.details ?? error.message)),
      code: error.code ?? "REQUEST_ERROR",
    });
  });
  const unary = async (req, method, body) => {
    const controller = new AbortController();
    const abort = () => controller.abort();
    const socket = req.raw.socket;
    req.raw.on("aborted", abort);
    socket.on("close", abort);
    try {
      return await native.unary(
        method,
        body,
        controller.signal,
        teamAuth ? req.headers.authorization : undefined,
      );
    } finally {
      req.raw.off("aborted", abort);
      socket.off("close", abort);
    }
  };
  app.get("/api/health", async (req) => unary(req, "getHealth", {}));
  app.get("/api/session", async (req) => unary(req, "getSession", {}));
  app.get("/api/team", async (req) => unary(req, "listTeam", {}));
  app.get("/healthz", async (_req, reply) => {
    const health = await native.unary("getHealth", {});
    reply.header("Cache-Control", "no-store");
    if (health.status !== "SERVING") reply.code(503);
    return { status: health.status };
  });
  registerDataRoutes(app, unary);
  registerWorkspaceRoutes(app, unary);
  registerAnalysisRoutes(app, unary);
  registerLibraryRoutes(app, unary);
  registerExperimentRoutes(app, unary);
  registerGovernanceRoutes(app, unary);
  registerWorkflowRoutes(app, unary);
  app.get("/api/strategies", async (req) =>
    parseDocument((await unary(req, "listStrategies", {})).strategies_json),
  );
  app.get(
    "/api/jobs",
    {
      schema: {
        querystring: {
          type: "object",
          additionalProperties: false,
          properties: {
            limit: { type: "integer", minimum: 1, maximum: 100, default: 100 },
            offset: {
              type: "integer",
              minimum: 0,
              maximum: 1_000_000,
              default: 0,
            },
            status: {
              type: "string",
              enum: [
                "",
                "queued",
                "running",
                "succeeded",
                "failed",
                "cancelled",
              ],
            },
          },
        },
      },
    },
    async (req) => {
      const result = await unary(req, "listJobs", {
        ...req.query,
        status: req.query.status ?? "",
      });
      return { jobs: result.jobs.map(decodeJob), total: result.total };
    },
  );
  const jobParams = {
    type: "object",
    required: ["id"],
    properties: {
      id: {
        type: "string",
        minLength: 1,
        maxLength: 128,
        pattern: "^[a-zA-Z0-9_-]+$",
      },
    },
  };
  app.get("/api/jobs/:id", { schema: { params: jobParams } }, async (req) =>
    decodeJob(await unary(req, "getJob", req.params)),
  );
  app.post(
    "/api/jobs/:id/cancel",
    { schema: { params: jobParams } },
    async (req) => decodeJob(await unary(req, "cancelJob", req.params)),
  );
  app.post(
    "/api/jobs",
    {
      preValidation: async (req) => {
        if (
          req.body?.alpha_revision !== undefined &&
          typeof req.body.alpha_revision !== "string"
        )
          throw Object.assign(
            new Error("alpha_revision must be an exact decimal string"),
            { code: 3 },
          );
      },
      schema: {
        body: {
          type: "object",
          required: ["idempotency_key"],
          additionalProperties: false,
          properties: {
            request: { type: "object" },
            alpha_id: { type: "string", pattern: "^[a-zA-Z0-9_-]{1,128}$" },
            alpha_revision: { type: "string", pattern: "^[1-9][0-9]{0,18}$" },
            label: { type: "string", maxLength: 256 },
            idempotency_key: { type: "string", minLength: 1, maxLength: 128 },
            scheduled_at_ms: {
              type: "integer",
              minimum: 0,
              maximum: 8_640_000_000_000_000,
            },
          },
        },
      },
    },
    async (req, reply) => {
      const {
        request,
        idempotency_key,
        scheduled_at_ms = 0,
        alpha_id,
        alpha_revision,
        label,
      } = req.body;
      if (
        (!request && !alpha_id) ||
        Boolean(alpha_id) !== Boolean(alpha_revision) ||
        (alpha_revision && BigInt(alpha_revision) > 9223372036854775807n)
      )
        return reply.code(400).send({
          error:
            "Supply a request or a complete immutable alpha ID/revision pair",
        });
      if (label && Buffer.byteLength(label) > 256)
        return reply
          .code(400)
          .send({ error: "Job label exceeds256 UTF-8 bytes" });
      const job = await unary(req, "submitJob", {
        request_json: request === undefined ? "" : JSON.stringify(request),
        idempotency_key,
        scheduled_at_ms,
        ...(alpha_id ? { alpha_id, alpha_revision } : {}),
        ...(label !== undefined ? { label } : {}),
      });
      reply.code(201);
      return decodeJob(job);
    },
  );
  app.get(
    "/api/events",
    {
      schema: {
        querystring: {
          type: "object",
          additionalProperties: false,
          properties: {
            channel: {
              type: "string",
              enum: ["all", "jobs", "system"],
              default: "all",
            },
            after_sequence: {
              type: "string",
              pattern: "^\\d{1,20}$",
              default: "0",
            },
            job_id: { type: "string", maxLength: 128 },
          },
        },
      },
    },
    async (req, reply) => {
      let streamLifetime = 30 * 60_000;
      if (teamAuth) {
        const session = await unary(req, "getSession", {});
        const principal = session?.principal;
        if (
          session?.team_mode !== true ||
          typeof principal?.id !== "string" ||
          !principal.id ||
          !["viewer", "researcher", "reviewer", "admin"].includes(
            principal.role,
          ) ||
          typeof principal.expires_at_ms !== "string" ||
          !/^(?:0|[1-9][0-9]{0,18})$/.test(principal.expires_at_ms)
        )
          throw Object.assign(new Error("Invalid native team session"), {
            code: 13,
          });
        const expiry = BigInt(principal.expires_at_ms);
        const remaining = expiry - BigInt(Date.now());
        if (expiry !== 0n && remaining <= 0n)
          return reply
            .code(401)
            .send({ error: "Team access token expired", code: 16 });
        streamLifetime = 15 * 60_000;
        if (expiry !== 0n && remaining < BigInt(streamLifetime))
          streamLifetime = Number(remaining);
      }
      if (BigInt(req.query.after_sequence) > 18446744073709551615n)
        return reply
          .code(400)
          .send({ error: "Event sequence exceeds uint64 range" });
      if (hub.size >= maxStreams)
        return reply
          .code(429)
          .send({ error: "Event connection capacity reached" });
      const res = reply.raw;
      reply.hijack();
      res.writeHead(200, {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-store",
        Connection: "keep-alive",
        "X-Accel-Buffering": "no",
        "X-Content-Type-Options": "nosniff",
      });
      res.write(": connected\n\n");
      let done = false;
      let unsubscribe = () => {};
      let heartbeat, lifetime;
      const cleanup = (reason) => {
        if (done) return;
        done = true;
        clearInterval(heartbeat);
        clearTimeout(lifetime);
        unsubscribe();
        if (reason && res.writableLength < 256 * 1024 - 2048)
          res.write(
            `event: fault\ndata: ${JSON.stringify({ error: reason, snapshot_required: true })}\n\n`,
          );
        if (reason?.startsWith("Slow consumer")) res.destroy();
        else res.end();
      };
      const send = (chunk) => {
        if (
          done ||
          res.destroyed ||
          res.writableLength + Buffer.byteLength(chunk) > 256 * 1024
        )
          return false;
        res.write(chunk);
        return res.writableLength <= 256 * 1024;
      };
      res.on("close", () => cleanup());
      unsubscribe = hub.subscribe(req.query, { send, close: cleanup });
      if (!done) {
        heartbeat = setInterval(() => {
          if (!send(": heartbeat\n\n"))
            cleanup("Slow consumer; snapshot refresh required");
        }, 15_000);
        lifetime = setTimeout(() => cleanup(), streamLifetime);
      }
    },
  );
  if (existsSync(staticRoot)) {
    await app.register(serveStatic, { root: staticRoot, index: "index.html" });
    app.setNotFoundHandler((req, reply) =>
      req.url.startsWith("/api/")
        ? reply.code(404).send({ error: "Unknown API route" })
        : reply.sendFile("index.html"),
    );
  }
  app.addHook("preClose", async () => {
    hub.close();
  });
  app.addHook("onClose", async () => {
    native.close?.();
  });
  return app;
}
