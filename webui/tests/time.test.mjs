import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import ts from "typescript";

const root = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(root, "../src/time.ts"), "utf8");
const compiled = ts.transpileModule(source, {
  compilerOptions: {
    module: ts.ModuleKind.ES2022,
    target: ts.ScriptTarget.ES2020,
  },
}).outputText;
const moduleUrl = `data:text/javascript;base64,${Buffer.from(compiled).toString("base64")}`;
const { formatEventTime } = await import(moduleUrl);

test("formats WS63 firmware event uptime seconds", () => {
  assert.equal(formatEventTime("123"), "123s");
});

test("falls back to raw invalid event time", () => {
  assert.equal(formatEventTime("not-a-date"), "not-a-date");
});
