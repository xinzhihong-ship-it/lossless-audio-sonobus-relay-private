import assert from "node:assert/strict";
import test from "node:test";
import { VideoPresenceRegistry } from "./videoPresence.js";

test("video presence stays visible while offline and expires after retention", () => {
  const registry = new VideoPresenceRegistry(1000, 3000);
  const first = registry.heartbeat({ group: "studio", user: "alice", camera: "FaceTime" });

  assert.equal(first.videoRoom, "SB_studio");
  assert.equal(first.online, true);

  const offline = registry.connections(Date.parse(first.lastSeenAt) + 1001);
  assert.equal(offline[0]?.online, false);
  assert.equal(offline[0]?.user, "alice");

  assert.deepEqual(registry.connections(Date.parse(first.lastSeenAt) + 4000), []);
});

test("video heartbeat keeps the same session and updates camera metadata", () => {
  const registry = new VideoPresenceRegistry();
  const first = registry.heartbeat({ group: "studio", user: "alice", camera: "Camera A" });
  const second = registry.heartbeat({ sessionId: first.sessionId, group: "studio", user: "alice", camera: "Camera B" });

  assert.equal(second.sessionId, first.sessionId);
  assert.equal(second.camera, "Camera B");
  assert.equal(registry.connections().length, 1);
});
