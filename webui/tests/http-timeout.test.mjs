import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import ts from "typescript";

const root = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(root, "../src/api/http.ts"), "utf8");
const compiled = ts.transpileModule(source, {
  compilerOptions: {
    module: ts.ModuleKind.ES2022,
    target: ts.ScriptTarget.ES2020,
  },
}).outputText;
const moduleUrl = `data:text/javascript;base64,${Buffer.from(compiled).toString("base64")}`;

test("wifi fetches time out instead of hanging refresh forever", async () => {
  const originalFetch = globalThis.fetch;
  const originalWindow = globalThis.window;
  globalThis.window = {
    setTimeout,
    clearTimeout,
  };
  globalThis.fetch = (_url, init) =>
    new Promise((_resolve, reject) => {
      init.signal.addEventListener("abort", () => {
        reject(new DOMException("aborted", "AbortError"));
      });
    });
  try {
    const { fetchJson } = await import(`${moduleUrl}#timeout`);
    await assert.rejects(() => fetchJson("/api/status"), /request timeout: \/api\/status/);
  } finally {
    globalThis.fetch = originalFetch;
    globalThis.window = originalWindow;
  }
});

test("json body keeps content-type while GET remains a simple request", async () => {
  const originalFetch = globalThis.fetch;
  const originalWindow = globalThis.window;
  const calls = [];
  globalThis.window = {
    setTimeout,
    clearTimeout,
  };
  globalThis.fetch = async (url, init = {}) => {
    calls.push({ url, init });
    return {
      ok: true,
      json: async () => ({ ok: true }),
    };
  };
  try {
    const { fetchJson } = await import(`${moduleUrl}#headers`);
    await fetchJson("/api/status");
    await fetchJson("/api/send", { method: "POST", body: "{}" });
    assert.equal(calls[0].init.headers, undefined);
    assert.deepEqual(calls[1].init.headers, { "content-type": "application/json" });
  } finally {
    globalThis.fetch = originalFetch;
    globalThis.window = originalWindow;
  }
});
