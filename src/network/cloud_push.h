#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "../config.h"
#include "cloud_cfg.h"

class NVSManager;
class PowerCalculator;
class LimitManager;

// Remote monitoring + remote control over Firebase RTDB, STA-only.
//
// UPLINK: pushes a small snapshot (merge-write /devices/<MAC>/latest)
// every PUSH_INTERVAL_MS through the Firebase SDK.
//
// DOWNLINK: polls /devices/<MAC>/cmd every POLL_INTERVAL_MS for
// a {id, frame, ts} written by the Gmail admin, executes a NEW id once via
// processCommand(..., skipAuth=true), and acks it at
// /devices/<MAC>/ack. Trust comes from the RTDB rules (only the admin
// Gmail can write cmd) + TLS + the board's own ID token - the cloud never
// carries a PIN, so the PIN gate is skipped for cloud frames only. Local
// WebSocket callers keep the PIN (processCommand default).
//
// Herd auth: the board signs into Firebase Authentication with an
// email + password THROUGH the SDK (auth.user.*), which owns the ID token
// and its hourly refresh internally. No token buffer lives in this code,
// so there is nothing to log, dump or render: not in the snapshot, not in
// logs, not on the dashboard. Unlike the secret era, an ID token RESPECTS
// the database rules - the deployed .validate shape-check is enforced on
// the board's own writes, not just on readers.
//
// The deliberate differences from the removed cloud era
// (doc/opencode_agent/lessons.md, "Archived - cloud era") still hold:
// STA-only (the fallback AP has no internet), MAC identity (no settable
// board id, nothing to mistype), herd account (no service-account private
// key on the device - the August SDK used one; this one does not). The old
// "never reads" half is superseded: the downlink reads only the cmd node
// on its own FirebaseData session, and a missing node is a quiet no-op,
// not a teardown.
//
// Self-healing: a 30 KB heap floor and a 60-straight-fail counter reboot
// the board instead of going silent (the hand-rolled 1 s era proved a
// quiet board stays quiet until a human brings USB).
//
// Cost, stated honestly: token refresh happens inside the SDK and each
// push/poll is a short round trip. Broadcasts pause during that window;
// sensing on Core 1 never notices. Enabling cloud also keeps eco from
// sleeping (see wantsRadio) - a radio that naps cannot push.
class CloudPush {
public:
  void begin(NVSManager *nvsRef);
  // Call from networkTask (~20 ms tick). Takes dataMutex briefly to snapshot
  // the readings, then does TLS with the mutex RELEASED - a multi-second
  // handshake must never hold the lock the sensor task needs.
  void loop(SystemData *sysData, SemaphoreHandle_t *mutex,
            PowerCalculator *powerCalc, LimitManager *limitMgr);

  // One downlink poll: GET the cmd node, execute a NEW id once via
  // processCommand(..., skipAuth=true), PATCH the ack. Safe to call from
  // loop() only; does TLS with dataMutex RELEASED and commits the new id
  // under dataMutex (50 ms take, same pattern as snapshot). Quiet no-op when
  // WiFi is down, login fails, or no new command is waiting.
  void pollCmd(SystemData *sysData, SemaphoreHandle_t *mutex,
               PowerCalculator *powerCalc, LimitManager *limitMgr,
               NVSManager *nvsRef);

  bool enabled() const { return enabled_; }
  // True while cloud needs the radio awake. updateEcoMode() counts this as
  // watched: cloud on means eco off, stated in the UI so it is not mysterious.
  bool wantsRadio() const;
  bool lastPushOk() const { return lastOk_; }
  // Seconds since the last 200 OK, or UINT32_MAX when nothing ever landed.
  uint32_t secondsSincePush() const;
  // Seconds since the last downlink poll attempt, or UINT32_MAX before first.
  uint32_t secondsSincePoll() const;
  // 12-char MAC id, or "" when the radio has no MAC to read.
  const char *deviceId() const { return deviceId_; }
  // Signed-in account (identifier, not secret - shown in the dashboard).
  const char *account() const { return email_; }

  // Read-only diagnostic for `cloud diag` (serial + web console): account,
  // session age, token LENGTH and token audience claim, plus the last
  // executed command id (first 20 chars) and the poll age. The token and the
  // password are never printed - length + aud is everything needed to tell
  // "mangled on the board" from "rejected by the project". Takes dataMutex
  // briefly: the WS console runs on a different task than loop().
  void diag(String &out, SemaphoreHandle_t *mutex);

  // 1 s pushes stay cheap ONLY because the transport below holds one
  // persistent keep-alive TLS session: a fresh 1-2 s handshake per push
  // outlasted the interval and starved networkTask. The downlink rides the
  // same connection on its own 2 s timer (commands land in ~2 s).
  static const uint32_t PUSH_INTERVAL_MS = 1000;
  static const uint32_t POLL_INTERVAL_MS = 2000;

 private:
  NVSManager *nvs = nullptr;
  bool enabled_ = false;
  char host_[CLOUD_MAX_HOST_LEN + 1] = {0};
  char email_[CLOUD_MAX_EMAIL_LEN + 1] = {0};
  char pass_[CLOUD_MAX_PASS_LEN + 1] = {0};
  char deviceId_[CLOUD_DEVICE_ID_LEN + 1] = {0};

  // No token buffers live here by design: the SDK owns the ID token and its
  // refresh internally. Email/password stay (NVS-loaded, used once at SDK
  // start); the password is never logged, never rendered.
  bool lastOk_ = false;
  uint32_t lastAttemptMs_ = 0;
  uint32_t lastOkMs_ = 0;

  // Downlink dedup: last executed command id (mirrored to NVS "cloud_cmd").
  char lastCmdId_[48] = {0};
  uint32_t lastPollMs_ = 0;
  // Self-healing: straight failed pushes with the link up (not-authed or
  // post fail). 60 in a row reboots; success resets. RAM-only, like the
  // session - a reboot starts it at zero, which is correct.
  uint32_t consecFails_ = 0;

  bool snapshot(SystemData *sysData, SemaphoreHandle_t *mutex, String &body);
  // Merge-writes the snapshot via the SDK; returns HTTP-like status
  // (200 = written, 0 = transport/auth failure). Push-only: the reply is
  // never parsed beyond success.
  bool post(const String &body);
  int postStatus(const String &body);
  // Starts the SDK (once per boot, after WiFi) and reports Firebase.ready().
  // Returns false with the reason logged (never the password).
  bool ensureLogin();
};
