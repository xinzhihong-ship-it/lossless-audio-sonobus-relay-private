import assert from "node:assert/strict";
import test from "node:test";
import { MemoryStore } from "./store.js";

test("video control keeps one enabled publisher per group", async () => {
  const store = new MemoryStore();
  await store.createVideoPairing({ pairingId: "pair-a", pairingKeyCiphertext: "key-a", group: "studio", user: "alice" });
  await store.createVideoPairing({ pairingId: "pair-b", pairingKeyCiphertext: "key-b", group: "studio", user: "bob" });

  await store.setVideoControl({ group: "studio", user: "alice", enabled: true, cameraDeviceId: "cam-a" });
  await store.setVideoControl({ group: "studio", user: "bob", enabled: true, cameraDeviceId: "cam-b" });

  const controls = await store.listVideoControls();
  assert.equal(controls.find((control) => control.user === "alice")?.enabled, false);
  assert.equal(controls.find((control) => control.user === "bob")?.enabled, true);
  assert.equal(controls.find((control) => control.user === "bob")?.cameraDeviceId, "cam-b");
});

test("new pairing disables persisted camera state", async () => {
  const store = new MemoryStore();
  await store.createVideoPairing({ pairingId: "old", pairingKeyCiphertext: "old-key", group: "studio", user: "alice" });
  await store.setVideoControl({ group: "studio", user: "alice", enabled: true, cameraDeviceId: "cam-a" });
  const repaired = await store.createVideoPairing({ pairingId: "new", pairingKeyCiphertext: "new-key", group: "studio", user: "alice" });

  assert.equal(repaired.enabled, false);
  assert.equal(repaired.cameraDeviceId, undefined);
});
