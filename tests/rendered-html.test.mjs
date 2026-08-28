import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("http://localhost/", { headers: { accept: "text/html" } }),
    {
      ASSETS: { fetch: async () => new Response("Not found", { status: 404 }) },
    },
    { waitUntil() {}, passThroughOnException() {} },
  );
}

test("server-renders the Pecky PWA shell and production metadata", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<html[^>]+lang="zh-CN"/i);
  assert.match(html, /Pecky 啄米/);
  assert.match(html, /正在看看米罐/);
  assert.match(html, /manifest\.webmanifest/);
  assert.match(html, /theme-color/);
  assert.doesNotMatch(html, /Your site is taking shape|Building your site|codex-preview/);
  assert.doesNotMatch(html, /react-loading-skeleton/);
});

test("ships the one-way opening flow, local persistence, adapters, and PWA files", async () => {
  const [app, model, storage, sources, manifest, serviceWorker, packageJson] = await Promise.all([
    readFile(new URL("../app/components/PeckyApp.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/lib/model.ts", import.meta.url), "utf8"),
    readFile(new URL("../app/lib/storage.ts", import.meta.url), "utf8"),
    readFile(new URL("../app/lib/sources.ts", import.meta.url), "utf8"),
    readFile(new URL("../public/manifest.webmanifest", import.meta.url), "utf8"),
    readFile(new URL("../public/sw.js", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(app, /OpeningExperience/);
  assert.match(app, /openingPhase/);
  assert.match(app, /模拟并播放开屏/);
  assert.match(model, /presentationStartedAt/);
  assert.match(model, /purchaseGoal/);
  assert.match(storage, /indexedDB\.open/);
  assert.match(storage, /transaction\.oncomplete/);
  assert.match(sources, /class BlePeckyDataSource/);
  assert.match(sources, /subscribe\(/);
  assert.equal(JSON.parse(manifest).display, "standalone");
  assert.match(serviceWorker, /CACHE_NAME/);
  assert.doesNotMatch(packageJson, /react-loading-skeleton/);

  await Promise.all([
    access(new URL("../public/assets/media/pecky-opening.mp4", import.meta.url)),
    access(new URL("../public/assets/media/pecky-orbit.mp4", import.meta.url)),
    access(new URL("../public/assets/jar-still.webp", import.meta.url)),
    access(new URL("../public/assets/icons/pecky-512.png", import.meta.url)),
    access(new URL("../public/assets/rewards/stocks.webp", import.meta.url)),
    access(new URL("../public/assets/rewards/jewelry.webp", import.meta.url)),
  ]);
});
