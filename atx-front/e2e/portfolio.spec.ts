import { createHash } from "node:crypto";
import { test, expect } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test("portfolio components retain exact text hashes and native validation cannot overwrite newer edits", async ({
  page,
}) => {
  await page.goto("/?panel=portfolio");
  await page.getByRole("button", { name: "Maximize active panel" }).click();
  await page.getByRole("button", { name: "Start portfolio draft" }).click();
  await expect(
    page.getByLabel("Portfolio expression 1", { exact: true }),
  ).toHaveValue("-rank(delta(close,5)/delay(close,5))");
  await page
    .getByLabel("Portfolio expression 1", { exact: true })
    .fill(" rank(close) ");
  await page.getByLabel("Portfolio weight 1", { exact: true }).fill("-2");
  await page
    .getByRole("button", { name: "Validate and pin components" })
    .click();
  await expect(
    page.getByText(
      "Native expressions validated and exact UTF-8 SHA-256 pins stored in the draft.",
      { exact: true },
    ),
  ).toBeVisible();
  const exact = createHash("sha256").update(" rank(close) ").digest("hex");
  await expect(page.locator(".portfolio-components")).toContainText(
    `SHA ${exact.slice(0, 14)}`,
  );
  await expect(page.locator(".portfolio-components")).toContainText(
    "normalized -80.00%",
  );
  await page
    .getByLabel("Portfolio expression 1", { exact: true })
    .fill("rank(ts_mean(close,20))");
  await page
    .getByRole("button", { name: "Validate and pin components" })
    .click();
  await page
    .getByLabel("Portfolio expression 1", { exact: true })
    .fill("rank(close)");
  await expect(page.getByRole("alert")).toContainText(
    "draft changed during validation",
  );
  await expect(
    page.getByLabel("Portfolio expression 1", { exact: true }),
  ).toHaveValue("rank(close)");
  await expect(page.locator(".portfolio-components")).toContainText(
    "Expression edited: pin required",
  );
  await page
    .getByRole("button", { name: "Validate and pin components" })
    .click();
  await expect(
    page.getByText(
      "Native expressions validated and exact UTF-8 SHA-256 pins stored in the draft.",
      { exact: true },
    ),
  ).toBeVisible();
  await page.getByRole("button", { name: "Add expression component" }).click();
  await page
    .getByLabel("Portfolio expression 3", { exact: true })
    .fill("rank(open)");
  await page
    .getByRole("button", { name: "Validate and pin components" })
    .click();
  await expect(page.getByRole("alert")).toContainText(
    "Field open is unavailable",
  );
  await expect(
    page.getByLabel("Portfolio expression 3", { exact: true }),
  ).toHaveValue("rank(open)");
});
