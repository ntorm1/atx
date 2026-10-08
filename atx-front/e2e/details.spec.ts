import { test, expect } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test("summary selection loads rich details and refresh preserves terminal evidence", async ({
  page,
}) => {
  await page.goto("/");
  await expect(
    page.getByRole("img", { name: "Net asset value curve" }),
  ).toBeVisible();
  const total = page.locator("tr").filter({
    has: page.getByRole("cell", { name: "Total return", exact: true }),
  });
  await expect(total).toContainText("12.50%");
  await page.locator("tr.clickable").nth(1).click();
  await expect(total).toContainText("35.00%");
  const before = await page.request.get("/api/health");
  await page
    .locator(".menubar")
    .getByRole("button", { name: "↻ Refresh", exact: true })
    .click();
  await expect(total).toContainText("35.00%");
  const after = await page.request.get("/api/health");
  expect((await after.json()).cache_hits).toBe(
    (await before.json()).cache_hits,
  );
  await expect(
    page.getByText("SYNTHETIC DEMO — simulated research results", {
      exact: true,
    }),
  ).toBeVisible();
});
test("degraded native health stays visible without discarding completed evidence", async ({
  page,
}) => {
  await page.goto("/");
  await expect(
    page.getByRole("img", { name: "Net asset value curve" }),
  ).toBeVisible();
  try {
    await page.request.post("/fixture/health", {
      data: { status: "DEGRADED" },
    });
    await page
      .locator(".menubar")
      .getByRole("button", { name: "↻ Refresh", exact: true })
      .click();
    await expect(page.getByText("Degraded", { exact: true })).toBeVisible();
    await expect(page.getByRole("alert")).toContainText(
      "queued work may not start",
    );
    await expect(
      page.getByRole("img", { name: "Net asset value curve" }),
    ).toBeVisible();
  } finally {
    await page.request.post("/fixture/health", { data: { status: "SERVING" } });
  }
});
