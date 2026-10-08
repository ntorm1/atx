import { test, expect, type Page } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test.beforeEach(async ({ request }) => {
  await request.post("http://127.0.0.1:3002/fixture/library");
});
async function panel(page: Page, title: string) {
  await page.getByRole("button", { name: "Windows", exact: false }).click();
  await page.getByRole("button", { name: title, exact: true }).click();
}
async function createAlpha(page: Page, name: string) {
  await panel(page, "Alpha library");
  await page
    .getByRole("button", { name: "Save draft as alpha", exact: true })
    .click();
  await page.getByLabel("Alpha name", { exact: true }).fill(name);
  await page
    .getByLabel("Alpha tags", { exact: true })
    .fill("value, mean-reversion");
  await page
    .getByLabel("Alpha hypothesis", { exact: true })
    .fill("Trailing reversal hypothesis");
  await page.getByRole("button", { name: "Create alpha", exact: true }).click();
  await expect(page.getByLabel("Alpha revision", { exact: true })).toHaveValue(
    "1",
  );
}
test("TRAIN dataset controls pin actual catalog identity and validate the native field contract", async ({
  page,
}) => {
  await page.goto("/");
  await page
    .getByRole("combobox", { name: "Strategy", exact: true })
    .selectOption("real-data-long-short-v1");
  await expect(page.getByLabel("Research dataset")).toHaveValue(
    "fixture-train",
  );
  await expect(page.getByLabel("Dataset manifest SHA-256")).toHaveValue(
    "c".repeat(64),
  );
  await expect(page.getByLabel("Dataset manifest SHA-256")).toHaveAttribute(
    "readonly",
    "",
  );
  await page.getByLabel("Instruments", { exact: true }).fill("64");
  await page
    .getByLabel("Execution delay (sessions)", { exact: true })
    .fill("2");
  const editor = page.getByLabel("Alpha DSL editor");
  await editor.fill("rank(open)");
  await page
    .getByRole("button", { name: "Validate expression", exact: true })
    .click();
  await expect(
    page.locator(".expression-editor").getByRole("alert"),
  ).toContainText("unavailable");
  await editor.fill("rank(close)");
  await page
    .getByRole("button", { name: "Validate expression", exact: true })
    .click();
  await expect(page.locator(".expression-validation")).toContainText(
    "Valid · lookback5".replace("lookback5", "lookback 5"),
  );
  await expect(page.locator(".expression-validation")).toContainText(
    "Syntax and causality only",
  );
  const submitting = page.waitForRequest(
    (request) =>
      request.method() === "POST" && request.url().endsWith("/api/jobs"),
  );
  await page
    .getByRole("button", { name: "Run research", exact: false })
    .click();
  const payload = (await submitting).postDataJSON();
  expect(payload.request).toMatchObject({
    mode: "real_dsl",
    dataset_id: "fixture-train",
    dataset_sha256: "c".repeat(64),
    strategy_dsl: "rank(close)",
    instrument_limit: 64,
    execution_delay_sessions: 2,
    neutralization: "dollar",
  });
});
test("operator examples update the draft and stale validation responses cannot certify new text", async ({
  page,
}) => {
  await page.goto("/");
  const editor = page.getByLabel("Alpha DSL editor");
  await editor.fill("ts_mean(close,20)");
  await page
    .getByRole("button", { name: "Validate expression", exact: true })
    .click();
  await editor.fill("rank(close)");
  await page.waitForTimeout(700);
  await expect(page.locator(".expression-validation")).toHaveCount(0);
  await panel(page, "Alpha operators");
  await page.getByLabel("Filter alpha operators").fill("delta");
  await expect(
    page.getByRole("table", { name: "Alpha operators" }).locator("tbody tr"),
  ).toHaveCount(1);
  await page.getByRole("button", { name: "Use example", exact: true }).click();
  await expect(
    page.getByLabel("Alpha expression", { exact: true }),
  ).toHaveValue("delta(close,5)");
});
test("alpha library stores immutable revisions, tags, archive state and native run lineage", async ({
  page,
  request,
}) => {
  await page.goto("/");
  await createAlpha(page, "Reversal alpha");
  await page
    .getByLabel("Alpha hypothesis", { exact: true })
    .fill("A revised hypothesis");
  await page
    .getByRole("button", { name: "Save new revision", exact: true })
    .click();
  await expect(page.getByLabel("Alpha revision", { exact: true })).toHaveValue(
    "2",
  );
  const list = await (
    await request.get("http://127.0.0.1:3002/api/alphas")
  ).json();
  const id = list.alphas[0].id;
  const old = await (
    await request.get(`http://127.0.0.1:3002/api/alphas/${id}?revision=1`)
  ).json();
  expect(old.hypothesis).toBe("Trailing reversal hypothesis");
  await page.getByLabel("Alpha revision", { exact: true }).selectOption("1");
  await expect(
    page.getByLabel("Alpha hypothesis", { exact: true }),
  ).toHaveValue("Trailing reversal hypothesis");
  const submitting = page.waitForRequest(
    (request) =>
      request.method() === "POST" && request.url().endsWith("/api/jobs"),
  );
  await page
    .getByRole("button", { name: "Run saved revision", exact: true })
    .click();
  const body = (await submitting).postDataJSON();
  expect(body.alpha_id).toBe(id);
  expect(body.alpha_revision).toBe("1");
  expect(body.request).toBeUndefined();
  await panel(page, "Alpha library");
  await page.getByLabel("Alpha revision", { exact: true }).selectOption("2");
  await page
    .getByRole("button", { name: "Archive alpha", exact: true })
    .click();
  await expect(
    page.getByRole("button", { name: "Restore alpha", exact: true }),
  ).toBeEnabled();
  await expect(
    page
      .getByRole("table", { name: "Saved alpha library" })
      .locator("tbody tr"),
  ).toHaveCount(0);
  await page
    .getByRole("button", { name: "Restore alpha", exact: true })
    .click();
  await expect(
    page
      .getByRole("table", { name: "Saved alpha library" })
      .locator("tbody tr"),
  ).toHaveCount(1);
});
test("alpha revision conflicts retain metadata edits and comparison uses backend alignment", async ({
  page,
  request,
}) => {
  await page.goto("/");
  await createAlpha(page, "Shared alpha");
  const list = await (
    await request.get("http://127.0.0.1:3002/api/alphas")
  ).json();
  const id = list.alphas[0].id;
  const record = await (
    await request.get(`http://127.0.0.1:3002/api/alphas/${id}`)
  ).json();
  await request.put(`http://127.0.0.1:3002/api/alphas/${id}`, {
    data: {
      name: record.name,
      description: record.description,
      tags: record.tags,
      hypothesis: "External revision",
      request: record.request,
      expected_revision: record.revision,
    },
  });
  await page
    .getByLabel("Alpha hypothesis", { exact: true })
    .fill("Keep these local edits");
  await page
    .getByRole("button", { name: "Save new revision", exact: true })
    .click();
  await expect(page.locator(".alpha-library").getByRole("alert")).toContainText(
    "revision changed",
  );
  await expect(
    page.getByLabel("Alpha hypothesis", { exact: true }),
  ).toHaveValue("Keep these local edits");
  await page
    .getByRole("button", { name: "Save conflict copy", exact: true })
    .click();
  await expect(page.getByLabel("Alpha revision", { exact: true })).toHaveValue(
    "1",
  );
  await panel(page, "Run comparison");
  await page
    .getByLabel(`Compare run ${"a".repeat(32)}`, { exact: true })
    .check();
  await page
    .getByLabel(`Compare run ${"b".repeat(32)}`, { exact: true })
    .check();
  await page.getByRole("button", { name: "Compare runs", exact: true }).click();
  await expect(
    page.getByRole("img", { name: "Aligned comparison curves rebased to100" }),
  ).toBeVisible();
  await expect(
    page
      .getByRole("table", { name: "Aligned run metrics" })
      .locator("tbody tr"),
  ).toHaveCount(2);
  await expect(
    page.getByRole("table", { name: "Return correlation matrix" }),
  ).toBeVisible();
  await expect(page.locator(".comparison-results")).toContainText(
    "10 matched periods",
  );
  await page.getByLabel("Comparison start date").fill("2020-01-05");
  await expect(
    page.getByRole("img", { name: "Aligned comparison curves rebased to100" }),
  ).toHaveCount(0);
});
