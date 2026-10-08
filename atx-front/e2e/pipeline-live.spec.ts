import { test, expect, type Page } from "@playwright/test";
test.skip(
  process.env.ATX_E2E_PIPELINE_LIVE !== "1",
  "Requires configured TRAIN dataset and native library/analysis services",
);
async function panel(page: Page, title: string) {
  await page.getByRole("button", { name: "Windows", exact: false }).click();
  await page.getByRole("button", { name: title, exact: true }).click();
}
test("actual TRAIN DSL execution, saved alpha lineage and aligned comparison form one workflow", async ({
  page,
  request,
}) => {
  test.setTimeout(180000);
  const createdAlphas: string[] = [];
  try {
    await page.goto("/");
    await page
      .getByRole("combobox", { name: "Strategy", exact: true })
      .selectOption("real-data-long-short-v1");
    await expect(page.getByLabel("Research dataset")).not.toHaveValue("");
    const pin = await page.getByLabel("Dataset manifest SHA-256").inputValue();
    expect(pin).toMatch(/^[a-f0-9]{64}$/);
    await page.getByLabel("Instruments", { exact: true }).fill("32");
    await page.getByLabel("Start date", { exact: true }).fill("2020-01-01");
    await page.getByLabel("End date", { exact: true }).fill("2020-03-31");
    await page
      .getByLabel("Alpha DSL editor")
      .fill("-rank(delta(close,5)/delay(close,5))");
    await page
      .getByRole("button", { name: "Validate expression", exact: true })
      .click();
    await expect(page.locator(".expression-validation")).toContainText(
      "Valid",
      { timeout: 15000 },
    );
    const submit = page.waitForResponse(
      (response) =>
        response.request().method() === "POST" &&
        response.url().endsWith("/api/jobs"),
    );
    await page
      .getByRole("button", { name: "Run research", exact: false })
      .click();
    const firstResponse = await submit;
    expect(firstResponse.ok()).toBe(true);
    const first = await firstResponse.json();
    const firstRow = page
      .locator("tr.clickable")
      .filter({ has: page.locator(`td[title="${first.id}"]`) });
    await expect(firstRow.getByText("succeeded", { exact: true })).toBeVisible({
      timeout: 120000,
    });
    const detail = await (await request.get(`/api/jobs/${first.id}`)).json();
    expect(detail.result.provenance.synthetic).toBe(false);
    expect(detail.result.provenance.dataset_manifest_sha256).toBe(pin);
    expect(detail.result.provenance.daily_returns_complete).toBe(true);
    expect(detail.result.daily_returns.length).toBeGreaterThan(10);
    expect(detail.result.metrics.final_nav).toBeGreaterThan(0);
    expect(detail.request.mode).toBe("real_dsl");
    await expect(
      page.getByRole("img", { name: "Net asset value curve" }),
    ).toBeVisible();
    await panel(page, "Alpha library");
    await page
      .getByRole("button", { name: "Save selected run recipe", exact: true })
      .click();
    await page
      .getByLabel("Alpha name", { exact: true })
      .fill(`TRAIN browser ${Date.now()}`);
    await page
      .getByLabel("Alpha hypothesis", { exact: true })
      .fill(
        "Trailing reversal on observed TRAIN panels; unqualified research evidence",
      );
    await page
      .getByLabel("Alpha tags", { exact: true })
      .fill("browser-qualification, train");
    await page
      .getByRole("button", { name: "Create alpha", exact: true })
      .click();
    await expect(
      page.getByLabel("Alpha revision", { exact: true }),
    ).toHaveValue("1");
    const list = await (
      await request.get("/api/alphas?tag=browser-qualification&limit=100")
    ).json();
    const saved = list.alphas.find(
      (alpha: { source_job_id: string }) => alpha.source_job_id === first.id,
    );
    expect(saved).toBeTruthy();
    createdAlphas.push(saved.id);
    const rerun = page.waitForResponse(
      (response) =>
        response.request().method() === "POST" &&
        response.url().endsWith("/api/jobs"),
    );
    await page
      .getByRole("button", { name: "Run saved revision", exact: true })
      .click();
    const secondResponse = await rerun;
    expect(secondResponse.ok()).toBe(true);
    const second = await secondResponse.json();
    expect(second.alpha_id).toBe(saved.id);
    expect(second.alpha_revision).toBe("1");
    const secondRow = page
      .locator("tr.clickable")
      .filter({ has: page.locator(`td[title="${second.id}"]`) });
    await expect(secondRow.getByText("succeeded", { exact: true })).toBeVisible(
      { timeout: 120000 },
    );
    const secondDetail = await (
      await request.get(`/api/jobs/${second.id}`)
    ).json();
    expect(secondDetail.request.dataset_sha256).toBe(pin);
    expect(secondDetail.result.provenance.synthetic).toBe(false);
    await page
      .getByRole("combobox", { name: "Strategy", exact: true })
      .scrollIntoViewIfNeeded();
    await page.screenshot({
      path: "artifacts/workstation-train-live.png",
      fullPage: true,
    });
    await panel(page, "Run comparison");
    await page
      .getByRole("button", { name: "Refresh comparable runs", exact: true })
      .click();
    await page.getByLabel(`Compare run ${first.id}`, { exact: true }).check();
    await page.getByLabel(`Compare run ${second.id}`, { exact: true }).check();
    const comparing = page.waitForResponse((response) =>
      response.url().endsWith("/api/analysis/compare"),
    );
    await page
      .getByRole("button", { name: "Compare runs", exact: true })
      .click();
    const compared = await comparing;
    expect(compared.ok()).toBe(true);
    const comparison = await compared.json();
    expect(comparison.alignment).toBe("intersection_of_exact_return_periods");
    expect(comparison.mixed_evidence).toBe(false);
    expect(comparison.runs).toHaveLength(2);
    for (const run of comparison.runs) expect(run.dataset_sha256).toBe(pin);
    expect(comparison.observations).toBeGreaterThan(10);
    expect(comparison.correlation[0][1]).toBeCloseTo(1, 10);
    await expect(
      page.getByRole("img", {
        name: "Aligned comparison curves rebased to100",
      }),
    ).toBeVisible();
    await page
      .getByRole("button", { name: "Maximize active panel", exact: true })
      .click();
    await expect(
      page
        .getByRole("table", { name: "Aligned run metrics" })
        .locator("tbody tr"),
    ).toHaveCount(2);
    await page.screenshot({
      path: "artifacts/workstation-comparison-live.png",
      fullPage: true,
    });
  } finally {
    for (const id of createdAlphas) {
      const response = await request.get(`/api/alphas/${id}`);
      if (response.ok()) {
        const record = await response.json();
        await request.post(`/api/alphas/${id}/archive`, {
          data: { expected_revision: record.revision, archived: true },
        });
      }
    }
  }
});
