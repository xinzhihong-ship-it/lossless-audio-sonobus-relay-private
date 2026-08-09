import assert from "node:assert/strict";
import { createHash, createHmac, randomBytes } from "node:crypto";
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
  const clientId = "client-a";
  const secret = Buffer.alloc(32, 0x22);
  const key = deriveEnrollmentKey(secret, "studio", "alice", clientId);
  const connectionServer = {
    async enrollmentSecret() { return secret.toString("hex"); },
    async connections() { return []; },
    async kick() { return { kicked: 0 }; },
    async ban() { return { banned: 0, expiresAt: null }; },
    async listBans() { return []; },
    async unban() { return { removed: 0 }; }
  };
  await service.requestEnrollment(makeEnrollment(key, "studio", "alice", clientId), "203.0.113.9", connectionServer);
  const [pending] = await service.listPendingEnrollments();
  assert.ok(pending);
  const pairing = await service.approveEnrollment(pending.requestId, "  room  secret  ");
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
    bitrate: 8_000_000,
    captureWidth: 2560,
    captureHeight: 1440,
    captureFps: 60,
  })).toString("base64url");

  try {
    const firstInput = makePoll(key, pairing.pairingId, clientId, 1, status);
    const first = decodeResponse(key, firstInput.nonce!, await service.poll(firstInput));
    assert.equal(first.enabled, false);

    await service.setDesired({
      group: "studio", user: "alice", enabled: true, cameraDeviceId: "camera-a",
      maxHeight: 720, maxFps: 30, maxBitrate: 3_000_000
    });
    const secondInput = makePoll(key, pairing.pairingId, clientId, 2, status);
    const desired = decodeResponse(key, secondInput.nonce!, await service.poll(secondInput));
    assert.equal(desired.enabled, true);
    assert.equal(desired.cameraDeviceId, "camera-a");
    assert.equal(desired.maxHeight, 720);
    assert.equal(desired.maxFps, 30);
    assert.equal(desired.maxBitrate, 3_000_000);
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
    assert.equal(view?.captureHeight, 1440);
    assert.equal(view?.maxHeight, 720);
    assert.equal(authWrites, 3, "steady-state polls must not rewrite global MediaMTX auth");
    await assert.rejects(() => service.poll(secondInput), /replayed/);
  } finally {
    await service.close();
  }
});

test("authenticated SonoBus client enrolls without a visible pairing code", async () => {
  const store = new MemoryStore();
  const service = new VideoControlService(store, "test-encryption-secret");
  const secret = Buffer.alloc(32, 0x11);
  const group = "studio";
  const user = "alice";
  const clientId = "client-auto";
  const key = deriveEnrollmentKey(secret, group, user, clientId);
  const connectionServer = {
    async enrollmentSecret() { return secret.toString("hex"); },
    async connections() { return []; },
    async kick() { return { kicked: 0 }; },
    async ban() { return { banned: 0, expiresAt: null }; },
    async listBans() { return []; },
    async unban() { return { removed: 0 }; }
  };

  try {
    const firstInput = makeEnrollment(key, group, user, clientId);
    const first = await service.requestEnrollment(firstInput, "203.0.113.9", connectionServer);
    assert.equal(first.signature, hmac(key, `enrollment-response\n${firstInput.nonce}\n${first.payload}`));
    assert.equal(JSON.parse(Buffer.from(first.payload, "base64url").toString("utf8")).state, "pending");
    const [pending] = await service.listPendingEnrollments();
    assert.equal(pending?.group, group);
    assert.equal(pending?.address, "203.0.113.9");
    await assert.rejects(() => service.requestEnrollment(firstInput, "203.0.113.9", connectionServer), /replayed/);

    const approved = await service.approveEnrollment(pending!.requestId, "room-secret");
    const secondInput = makeEnrollment(key, group, user, clientId);
    const second = await service.requestEnrollment(secondInput, "203.0.113.9", connectionServer);
    const approvedPayload = JSON.parse(Buffer.from(second.payload, "base64url").toString("utf8"));
    assert.deepEqual(approvedPayload, { type: "enrollment", state: "approved", pairingId: approved.pairingId });

    const status = Buffer.from(JSON.stringify({ type: "status", capturing: false, cameras: [] })).toString("base64url");
    const poll = makePoll(key, approved.pairingId, clientId, 1, status);
    await service.poll(poll);
    assert.equal((await service.list())[0]?.online, true);
    assert.deepEqual(await service.listPendingEnrollments(), []);
  } finally {
    await service.close();
  }
});

function makeEnrollment(key: Buffer, group: string, user: string, clientId: string) {
  const timestamp = Date.now();
  const nonce = randomBytes(18).toString("base64url");
  return {
    group,
    user,
    clientId,
    timestamp,
    nonce,
    signature: hmac(key, `enroll\n${group}\n${user}\n${clientId}\n${timestamp}\n${nonce}`)
  };
}

function deriveEnrollmentKey(secret: Buffer, group: string, user: string, clientId: string): Buffer {
  return createHash("sha256")
    .update("sonobus-video-enrollment-v1\0")
    .update(secret)
    .update(`\0${group}\0${user}\0${clientId}`)
    .digest();
}


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
