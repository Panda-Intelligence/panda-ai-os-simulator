import test from "node:test";
import assert from "node:assert/strict";

const theme = await import("../src/theme.ts");

test("theme preference parser falls back to Follow System", () => {
  assert.equal(theme.parseThemePreference(null), "system");
  assert.equal(theme.parseThemePreference("unexpected"), "system");
  assert.equal(theme.parseThemePreference("light"), "light");
  assert.equal(theme.parseThemePreference("dark"), "dark");
});

test("explicit preferences ignore the operating system", () => {
  assert.equal(theme.resolveThemePreference("light", true), "light");
  assert.equal(theme.resolveThemePreference("dark", false), "dark");
});

test("system preference resolves to the media query result", () => {
  assert.equal(theme.resolveThemePreference("system", true), "dark");
  assert.equal(theme.resolveThemePreference("system", false), "light");
});
