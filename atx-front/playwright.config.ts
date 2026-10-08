import { defineConfig } from "@playwright/test";
export default defineConfig({
  testDir: "./e2e",
  timeout: 30_000,
  fullyParallel: false,
  // Native database capacity and mutable fixture ledgers are deliberately shared.
  workers: 1,
  use: {
    baseURL: process.env.ATX_E2E_BASE_URL ?? "http://127.0.0.1:3001",
    viewport: { width: 1536, height: 1024 },
    headless: true,
    launchOptions: {
      executablePath:
        process.env.ATX_CHROME_PATH ??
        "C:/Program Files/Google/Chrome/Application/chrome.exe",
    },
    screenshot: "only-on-failure",
  },
  webServer: [
    ...(process.env.ATX_E2E_BASE_URL
      ? []
      : [
          {
            command: "node gateway/main.mjs",
            url: "http://127.0.0.1:3001",
            reuseExistingServer: false,
            env: { ATX_WEB_PORT: "3001" },
            timeout: 15_000,
          },
        ]),
    {
      command: "node e2e/mock-server.mjs",
      url: "http://127.0.0.1:3002",
      reuseExistingServer: false,
      timeout: 15000,
    },
  ],
});
