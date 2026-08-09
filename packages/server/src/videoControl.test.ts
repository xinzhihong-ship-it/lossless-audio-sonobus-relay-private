import assert from "node:assert/strict";
import { createHmac, randomBytes } from "node:crypto";
import test from "node:test";
import { MemoryStore } from "./store.js";
import { VideoControlService, type VideoControlPollInput } from "./videoControl.js";

test("paired client polls persisted admin state and receives scoped MediaMTX digest credentials", async () => {
  const store = new MemoryStore();
  let internalUsers: Array<{ user: string; pass?: string; permissions: Array<{ action: string; path?: string }> }> = [];
  let authWrites = 0;
  const mediaMtx = {
    async paths() { return []; },
    async setInternalUsers(users: typeof internalUsers) { internalUsers = users; authWrites += 1; }
  };
  const service = new VideoControlService(
    store, "test-encryption-secret", 19092, mediaMtx, "media-muxer", "muxer-pass", "media-api", "api-pass"
  );
  const pairing = await service.createPairing("studio", "alice", "  room  secret  ");
  const key = Buffer.from(pairing.pairingCode, "base64url");
  const clientId = "client-a";
  const status = Buffer.from(JSON.stringify({
    type: "status",
    capturing: true,
    cameras: [{ id: "camera-a", name: "FaceTime Camera" }],
    cameraDeviceId: "camera-a",
    cameraName: "FaceTime Camera",
    codec: "H264",
    width: 1920,
    height: 1080,
    fps: 60,
    bitrate: 8_000_000
  })).toString("base64url");

  try {
    const firstInput = makePoll(key, pairing.pairingId, clientId, 1, status);
    const first = decodeResponse(key, firstInput.nonce!, await service.poll(firstInput));
    assert.equal(first.enabled, false);

    await service.setDesired("studio", "alice", true, "camera-a");
    const secondInput = makePoll(key, pairing.pairingId, clientId, 2, status);
    const desired = decodeResponse(key, secondInput.nonce!, await service.poll(secondInput));
    assert.equal(desired.enabled, true);
    assert.equal(desired.cameraDeviceId, "camera-a");
    assert.equal(desired.ingestPath, "ingest/SB_studio");
    assert.equal(desired.publishUser, `camera-${pairing.pairingId}`);
    assert.deepEqual(await service.activeGroups(), [{ group: "studio", groupPassword: "  room  secret  " }]);

    const password = hmac(key, `publish\n${pairing.pairingId}\n${desired.publishNonce}`);
    const publisher = internalUsers.find((user) => user.user === `camera-${pairing.pairingId}`);
    assert.equal(publisher?.pass, password);
    assert.deepEqual(publisher?.permissions, [{ action: "publish", path: "ingest/SB_studio" }]);
    assert.deepEqual(internalUsers.find((user) => user.user === "media-muxer"), {
      user: "media-muxer",
      pass: "muxer-pass",
      ips: [],
      permissions: [
        { action: "read", path: "~^ingest/SB_[^/]+$" },
        { action: "publish", path: "~^SB_[^/]+$" }
      ]
    });
    assert.deepEqual(internalUsers.find((user) => user.user === "media-api"), {
      user: "media-api",
      pass: "api-pass",
      ips: [],
      permissions: [{ action: "api" }]
    });

    const view = (await service.list())[0];
    assert.equal(view?.online, true);
    assert.equal(view?.capturing, true);
    assert.equal(view?.width, 1920);
    assert.equal(view?.fps, 60);
    assert.equal(authWrites, 3, "steady-state polls must not rewrite global MediaMTX auth");
    await assert.rejects(() => service.poll(secondInput), /replayed/);
  } finally {
    await service.close();
  }
});

function makePoll(key: Buffer, pairingId: string, clientId: string, sequence: number, status: string): VideoControlPollInput {
  const timestamp = Date.now();
  const nonce = randomBytes(18).toString("base64url");
  return {
    pairingId,
    clientId,
    timestamp,
    sequence,
    nonce,
    status,
    signature: hmac(key, `poll\n${pairingId}\n${clientId}\n${timestamp}\n${sequence}\n${nonce}\n${status}`)
  };
}

function decodeResponse(key: Buffer, nonce: string, response: { payload: string; signature: string }): Record<string, any> {
  assert.equal(response.signature, hmac(key, `response\n${nonce}\n${response.payload}`));
  return JSON.parse(Buffer.from(response.payload, "base64url").toString("utf8")) as Record<string, any>;
}

function hmac(key: Buffer, value: string): string {
  return createHmac("sha256", key).update(value).digest("base64url");
}
