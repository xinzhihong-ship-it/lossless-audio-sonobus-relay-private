import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";
import { groupFromPublicVideoPath, HttpMediaMtxAdmin, ingestVideoPath, videoRoom } from "./mediaMtx.js";

test("video paths use one safe stable room per SonoBus group", () => {
  assert.equal(videoRoom("studio"), "SB_studio");
  assert.equal(videoRoom("SB_studio"), "SB_studio");
  assert.equal(ingestVideoPath("studio"), "ingest/SB_studio");
  assert.equal(groupFromPublicVideoPath("SB_studio"), "studio");
});

test("video paths reject characters that change URL path semantics", () => {
  for (const value of ["", "SB_", "a/b", "a?b", "a#b", "a\\b", "a\nb"]) {
    assert.equal(videoRoom(value), undefined);
  }
  assert.equal(groupFromPublicVideoPath("ingest/SB_studio"), undefined);
});

test("MediaMTX Control API client authenticates every request", async () => {
  const authorizations: Array<string | undefined> = [];
  const server = http.createServer((req, res) => {
    authorizations.push(req.headers.authorization);
    if (req.method === "GET") {
      res.setHeader("content-type", "application/json");
      res.end(JSON.stringify({ items: [{ name: "SB_studio", ready: true }] }));
    } else {
      res.statusCode = 204;
      res.end();
    }
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  assert(address && typeof address === "object");
  const client = new HttpMediaMtxAdmin(`http://127.0.0.1:${address.port}`, "media-api", "secret");
  try {
    assert.equal((await client.paths())[0]?.name, "SB_studio");
    await client.setInternalUsers?.([]);
    assert.deepEqual(authorizations, [
      `Basic ${Buffer.from("media-api:secret").toString("base64")}`,
      `Basic ${Buffer.from("media-api:secret").toString("base64")}`
    ]);
  } finally {
    await new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  }
});
