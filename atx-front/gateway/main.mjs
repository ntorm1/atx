import { createNativeClient } from "./client.mjs";
import { createApp } from "./app.mjs";
import { readFileSync } from "node:fs";

function boundedEnvironment(name, fallback, minimum, maximum) {
  const value = Number(process.env[name] ?? fallback);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum)
    throw new Error(`${name} must be an integer in ${minimum}..${maximum}`);
  return value;
}

const host = process.env.ATX_WEB_HOST ?? "127.0.0.1";
const teamAuth = process.env.ATX_WEB_TEAM_AUTH === "1";
if (
  !["127.0.0.1", "::1", "localhost"].includes(host) &&
  !process.env.ATX_WEB_TOKEN &&
  !teamAuth
)
  throw new Error(
    "Browser or team authorization is required for a non-loopback bind",
  );
const native = createNativeClient({
  address: process.env.ATX_GRPC_ADDRESS,
  token: process.env.ATX_SERVER_TOKEN,
  tls: process.env.ATX_GRPC_TLS === "1",
  rootCert:
    process.env.ATX_GRPC_TLS === "1" && process.env.ATX_GRPC_CA
      ? readFileSync(process.env.ATX_GRPC_CA)
      : undefined,
});
const app = await createApp({
  native,
  webToken: process.env.ATX_WEB_TOKEN,
  teamAuth,
  logger: true,
  maxStreams: boundedEnvironment("ATX_WEB_MAX_STREAMS", 512, 1, 4096),
  requestsPerMinute: boundedEnvironment("ATX_WEB_RATE_LIMIT", 240, 1, 1000000),
  allowedHostnames: [
    "localhost",
    "127.0.0.1",
    "[::1]",
    host,
    ...(process.env.ATX_WEB_ALLOWED_HOSTS ?? "").split(",").filter(Boolean),
  ],
});
const stop = async () => {
  await app.close();
  process.exit(0);
};
process.once("SIGINT", stop);
process.once("SIGTERM", stop);
if (process.platform === "win32") process.once("SIGBREAK", stop);
await app.listen({ host, port: Number(process.env.ATX_WEB_PORT ?? 3000) });
