# Deadbolt RCA diagnostics

This firmware exposes a read-only diagnostic snapshot on the existing local HTTP
server:

```text
GET http://<deadbolt-ip>/diag
```

`/diagnostics` is retained as an alias. The existing Matter clusters and the
`health` attribute bitmap are unchanged.
The endpoint is intended for a controller on the same trusted network. All
counters and command history are bounded RAM state and reset when the device
reboots. Times are monotonic milliseconds since boot, not wall-clock time.

## Captured signals

- firmware uptime and ESP reset reason
- diagnostic schema ID and the running ELF's full SHA-256 build ID
- current and minimum-ever free heap
- live Wi-Fi driver connectivity, RSSI and channel
- Wi-Fi connection, disconnect and reconnect counters, the last ESP-IDF
  disconnect reason code/RSSI, and last event times
- Matter server-ready time
- Matter command callback and health-request counters
- the last eight commands with these stages:
  `received`, `queued`, `started`, `relay_commanded`, `completed`,
  `rejected_busy`, or `queue_unavailable`
- command origin (`matter`, `timer`, or `internal`), result and monotonic
  timestamps for each boundary

`relay_commanded` only proves that firmware issued a GPIO command to the relay.
It does **not** prove relay contact movement, motor movement, or physical bolt
engagement. The response therefore always declares
`physicalBoltVerificationAvailable: false`.

## Incident capture procedure

The Edge controller should retain a snapshot immediately before a command and
another snapshot after success or timeout. Compare counter deltas and match the
command by monotonic time and `sequence`.

| Observation after a timed-out command | Proven boundary |
| --- | --- |
| Diagnostic HTTP and device IP are unreachable | Failure is at or before device Wi-Fi/IP reachability; firmware cannot narrow it further while offline |
| Uptime decreased or reset reason changed | Device rebooted; reset reason separates brownout, watchdog, panic, software, and power-on resets |
| Wi-Fi disconnect/reconnect counter changed | The device observed a Wi-Fi link interruption during the interval |
| HTTP works but `matter.applicationCommandCallbackCount` did not increase and no record exists | The command never reached the application callback; investigate discovery, CASE/session, or Matter transport with the Edge packet trace |
| Record ends at `received` or `queued` | Matter callback ran, but the local worker did not start the command |
| Record ends at `started` or `rejected_busy` | Local command-handler contention or stall |
| Record ends at `relay_commanded` | GPIO command was issued, but the handler did not finish its reporting path |
| Record is `completed/success`, but the bolt is physically open | Firmware and GPIO-command paths completed; physical actuation still requires a bolt-position sensor to verify |

`queueOverwriteObservedCount` is only a best-effort contention signal. The
producer performs a non-blocking `xQueuePeek` immediately before the unchanged
`xQueueOverwrite`, so the worker may consume the pending item between those two
calls. It must not be used alone to label a specific command as superseded.

This data deliberately does not claim whether mDNS, CASE Sigma exchanges, or
the Invoke response failed when the Matter callback was never entered. Those
sub-stages must be established from the controller-side CHIP logs or a packet
capture.

## Release gates

- The pre-release canary deliberately keeps `FIRMWARE_VERSION=v1.1.5` and
  reports `diagnosticsRevision=edge76-rca1`. Verify that revision through
  `/diag`, and retain the full `firmwareBuildId` ELF SHA-256. The release
  version alone does not prove that diagnostics are installed. The project
  fixes `CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64`; reject a canary whose reported
  build ID is not exactly 64 lowercase hexadecimal characters.
- Do not bump the canary version before a matching release: the current AutoOTA
  logic treats any unequal tag as an update and could downgrade the canary to
  latest `v1.1.5`. Do not publish the new latest tag before canary approval
  either, because every existing device can discover it. Enforce a release
  freeze that keeps latest at `v1.1.5` for the whole canary, or first add a
  compile-time AutoOTA disable/staged channel. `/diag` explicitly reports the
  current `autoOtaEnabled` state and channel.
- Install the exact clean canary build with local USB `idf.py flash`. Do not use
  the Matter OTA trigger, the auto-OTA latest URL, or a `flash_remote.sh` input
  downloaded from a release. Confirm `diagnosticsRevision` and save the exact
  `firmwareBuildId` before starting incident tests.
- After canary approval, bump `FIRMWARE_VERSION`, the release tag, and the build
  `diagnosticsRevision` together under the planned fleet rollout, then retain
  and verify the newly generated `firmwareBuildId`.
- Build with the project's exact ESP-IDF/ESP-Matter toolchain, then flash one
  canary device only. Verify fail-secure GPIO state at boot, `/diag` JSON, Wi-Fi
  disconnect/reconnect enrichment, and command-stage progression before fleet
  rollout.
- A physical bolt-position sensor is still required before any software result
  can be treated as proof of mechanical engagement.

`wifi.disconnectCount` counts disconnected **episodes**, not callback calls.
The raw ESP-IDF disconnect event and Matter connectivity-loss event are
coalesced until the next IP reconnect; `lastDisconnectUptimeMs` is the first
signal in that episode, while a later raw event may enrich its reason and RSSI.

## Example response

```json
{
  "schemaVersion": 1,
  "firmwareVersion": "v1.1.5",
  "diagnosticsRevision": "edge76-rca1",
  "firmwareBuildId": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "autoOtaEnabled": true,
  "autoOtaChannel": "github-latest",
  "uptimeMs": 123456,
  "reset": { "code": 1, "reason": "power_on" },
  "heap": { "freeBytes": 100352, "minimumFreeBytes": 94208 },
  "wifi": {
    "eventConnected": true,
    "driverConnected": true,
    "rssiDbm": -49,
    "channel": 6,
    "connectionCount": 1,
    "disconnectCount": 0,
    "reconnectCount": 0,
    "lastDisconnectReason": 0,
    "lastDisconnectRssiDbm": -128,
    "lastDisconnectUptimeMs": 0,
    "lastReconnectUptimeMs": 0
  },
  "matter": {
    "serverReady": true,
    "serverReadyUptimeMs": 2140,
    "applicationCommandCallbackCount": 1,
    "healthRequestCount": 2,
    "lastHealthRequestUptimeMs": 120000
  },
  "command": {
    "receivedCount": 1,
    "completedCount": 1,
    "busyCount": 0,
    "queueOverwriteObservedCount": 0,
    "historyCapacity": 8,
    "physicalBoltVerificationAvailable": false,
    "recent": [
      {
        "sequence": 1,
        "type": "unlock",
        "origin": "matter",
        "target": "unlocked",
        "stage": "completed",
        "result": "success",
        "receivedUptimeMs": 100000,
        "queuedUptimeMs": 100001,
        "startedUptimeMs": 100002,
        "relayCommandedUptimeMs": 100003,
        "completedUptimeMs": 100006,
        "handlerDurationMs": 4
      }
    ]
  }
}
```
