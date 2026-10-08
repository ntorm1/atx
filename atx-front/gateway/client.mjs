import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";
import { fileURLToPath } from "node:url";

export function createNativeClient({
  address = "127.0.0.1:50051",
  token = "",
  tls = false,
  rootCert,
} = {}) {
  const proto = fileURLToPath(
    new URL("../../atx-server/proto/atx/v1/workspace.proto", import.meta.url),
  );
  const definition = protoLoader.loadSync(proto, {
    keepCase: true,
    longs: String,
    enums: String,
    defaults: true,
    oneofs: true,
  });
  const Service =
    grpc.loadPackageDefinition(definition).atx.v1.WorkspaceService;
  const client = new Service(
    address,
    tls
      ? grpc.credentials.createSsl(rootCert)
      : grpc.credentials.createInsecure(),
    {
      "grpc.max_receive_message_length": 20 * 1024 * 1024,
      "grpc.max_send_message_length": 2 * 1024 * 1024,
      "grpc.enable_retries": 0,
    },
  );
  const metadata = (authorization) => {
    const value = new grpc.Metadata();
    const credential =
      authorization === undefined
        ? token
          ? `Bearer ${token}`
          : ""
        : authorization;
    if (credential) value.set("authorization", credential);
    return value;
  };
  return {
    unary(method, body, signal, authorization) {
      return new Promise((resolve, reject) => {
        const call = client[method](
          body,
          metadata(authorization),
          { deadline: new Date(Date.now() + 15_000) },
          (error, result) => {
            signal?.removeEventListener("abort", abort);
            error ? reject(error) : resolve(result);
          },
        );
        const abort = () => call.cancel();
        if (signal?.aborted) abort();
        else signal?.addEventListener("abort", abort, { once: true });
      });
    },
    watch(body) {
      return client.watchEvents(body, metadata());
    },
    close() {
      client.close();
    },
  };
}
