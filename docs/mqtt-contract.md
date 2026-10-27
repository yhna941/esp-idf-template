# MQTT contract proposal

Status: telemetry and availability publishing are implemented by the optional
Wi-Fi/MQTT adapter. The portable command ingress is implemented and does not
subscribe to the command topics below. Command execution on MQTT remains design
only. See [Hardware and MQTT](hardware-mqtt.md) and
[ADR 0005](decisions/0005-command-ingress.md).

## Topics

```text
iot/v1/{site_id}/{device_id}/telemetry
iot/v1/{site_id}/{device_id}/state
iot/v1/{site_id}/{device_id}/availability
iot/v1/{site_id}/{device_id}/commands
iot/v1/{site_id}/{device_id}/command-results
```

Use per-device credentials and ACLs, TLS server verification, bounded reconnect
backoff with jitter, and a last will for availability. Broker-reported presence
is not a safety input. Telemetry may be lossy; safety faults need a separate
observable record. Define broker limits and offline storage limits explicitly.

## Portable ingress seam

`application::CommandIngress::submit` is the adapter boundary. The default
firmware constructs it and never calls it. `ESPKIT_REMOTE_COMMANDS` defaults to
off; `CommandSource::remote` is then `rejected_unauthorized`. Turning the option
on does not open a socket or subscribe to `commands`.

A future MQTT adapter should:

- reassemble a bounded payload before parsing
- reject the command when trusted time is unavailable
- map the JSON fields below onto `domain::Command` using the device monotonic clock
- call `submit` from one task, not from code that writes the output
- publish `command-results` from the result FIFO when the broker is reachable,
  without retaining one-shot actuator commands

`command_id` maps to `Command::id` (the portable id is numeric; the adapter owns
any string encoding). `type` is `set_output` or `stop`. `enabled` and
`max_duration_ms` apply to `set_output`. An enable is `rejected_invalid` unless
`max_duration_ms` is strictly below the device `max_run_ms`; the ingress does not
clamp it. The sample payload uses 5000 ms, which the default policy rejects.
`expires_at` must already be a monotonic deadline no further away than
`command_defaults::max_ttl_ms`. Identity is a non-zero `uint32_t` chosen by the
adapter after authentication. Sequence is a per-source counter that must increase
for each new accepted command. Duplicate memory and sequence cursors reset on reboot.

Admission statuses are `accepted`, `rejected_invalid`, `rejected_expired`,
`rejected_duplicate`, `rejected_stale_sequence`, `rejected_unauthorized`, and
`rejected_full`. After the control loop runs, a queued command is `applied`,
`not_applied`, `expired_before_apply`, `dropped_by_stop`, or `superseded`.
`accepted` does not mean the output turned on, and it does not mean a stop left
the output off. A stop is `applied` only after the control step observes
`AppliedOutput::off`. `unknown` and `on` are `not_applied`. See
[ADR 0008](decisions/0008-output-quiesce.md). `TimedOutput` can still refuse.

## Commands

```json
{
  "schema_version": 1,
  "command_id": "8f36a7b1",
  "type": "set_output",
  "enabled": true,
  "issued_at": "2026-10-25T03:00:00Z",
  "expires_at": "2026-10-25T03:00:15Z",
  "max_duration_ms": 5000
}
```

Never retain one-shot actuator commands. Define maximum topic, payload, and queue
sizes. Reassemble fragmented MQTT payloads into bounded owned storage before
parsing. Reject malformed, expired, unsupported, or unauthorized requests.
Reject time-dependent enable commands before trusted time is available.
Deduplicate IDs with a defined lifetime and reboot policy. MQTT QoS does not
replace application-level idempotency for physical operations.

Acknowledge acceptance separately from completion. Queue saturation produces an
explicit rejection when transport is available; it must not silently accept work.
A reconnect must not replay expired commands. A device may shorten requested
runtime, and all remote requests remain subject to local interlocks.
