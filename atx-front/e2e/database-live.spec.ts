import { test, expect } from "@playwright/test";

test("live DuckDB catalog, exact query cells and docked preview come from native providers", async ({
  page,
}) => {
  test.skip(
    process.env.ATX_E2E_DATA_LIVE !== "1",
    "Requires native database providers and configured atx-db",
  );
  test.setTimeout(60_000);
  const browserErrors: string[] = [];
  page.on("pageerror", (error) => browserErrors.push(error.message));
  await page.goto("/");
  await page.getByRole("button", { name: "Database", exact: true }).click();
  const sourcesResponse = await page.request.get("/api/data/sources");
  expect(sourcesResponse.ok()).toBe(true);
  const { sources } = await sourcesResponse.json();
  const source = sources.find(
    (candidate: { id: string; provider: string }) =>
      candidate.id === "atx-db" && candidate.provider === "duckdb",
  );
  expect(source).toBeTruthy();
  await page.getByLabel("Database source").selectOption(source.id);
  const catalogResponse = await page.request.get(
    `/api/data/catalog?source_id=${encodeURIComponent(source.id)}`,
  );
  expect(catalogResponse.ok()).toBe(true);
  const catalog = await catalogResponse.json();
  expect(catalog.tables.length).toBeGreaterThan(0);
  const table = page.getByRole("table", { name: "Database result" });
  await expect(table).toBeVisible();
  await expect(table.getByRole("columnheader").first()).toBeVisible();
  await expect(page.getByLabel("Database SQL query")).toHaveValue(
    /^SELECT \* FROM /,
  );
  const sql = page.getByLabel("Database SQL query");
  await sql.fill(
    "SELECT 9223372036854775807::BIGINT AS exact_integer, 12345678901234567890.123456789012::DECIMAL(38,12) AS exact_decimal, NULL::VARCHAR AS missing, 'NULL' AS literal_null",
  );
  await sql.press("Control+Enter");
  await expect(page.locator(".database-result-identity")).toContainText(
    "Query /",
  );
  await expect(
    table.getByRole("cell", { name: "9223372036854775807", exact: true }),
  ).toBeVisible();
  await expect(
    table.getByRole("cell", {
      name: "12345678901234567890.123456789012",
      exact: true,
    }),
  ).toBeVisible();
  await expect(table.locator(".database-null i")).toHaveText("NULL");
  await expect(
    table.getByRole("cell", { name: "NULL", exact: true }),
  ).toHaveCount(2);
  await sql.fill("SELECT * FROM nonexistent_atx_browser_test_table");
  await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
  await expect(
    page.locator(".database-panel").getByRole("alert"),
  ).toBeVisible();
  await expect(table).toHaveCount(0);
  await page
    .getByRole("button", { name: "Table preview", exact: true })
    .click();
  await expect(table).toBeVisible();
  const before = await page.locator(".dv-groupview").count();
  const database = page
    .locator(".dv-tab")
    .filter({ hasText: /^Database viewer$/ });
  const equity = page.locator(".dv-groupview").filter({
    has: page.locator(".dv-tab").filter({ hasText: /^Equity curve$/ }),
  });
  const bounds = await equity.boundingBox();
  await database.dragTo(equity, {
    targetPosition: { x: bounds!.width / 2, y: bounds!.height - 4 },
  });
  await expect(page.locator(".dv-groupview")).toHaveCount(before + 1);
  await page.reload();
  await expect(page.locator(".dv-groupview")).toHaveCount(before + 1);
  await expect(table).toBeVisible();
  await page.getByRole("button", { name: "Reset layout", exact: true }).click();
  await page.getByRole("button", { name: "Database", exact: true }).click();
  await expect(table).toBeVisible();
  await page.screenshot({
    path: "artifacts/workstation-database-live.png",
    fullPage: true,
  });
  expect(browserErrors).toEqual([]);
});
