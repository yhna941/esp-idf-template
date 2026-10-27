# MQTT contract proposal

Status: telemetry and availability publishing are implemented by the optional
Wi-Fi/MQTT adapter. The portable command ingress and a schema 1 command decoder
are implemented. The firmware does not subscribe to the command topics below.
Command execution on MQTT remains design only. The decoder was exercised by host
tests; hardware validation was not performed. See
[Hardware and MQTT](hardware-mqtt.md) and
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

`networking::decode_command_envelope` reads one reassembled payload. It does not
open a socket. The caller passes monotonic `now`, `wall_offset_ms` (trusted
wall-clock milliseconds minus `now`), the authenticated identity, and the next
remote sequence. The decoder's only effect is `submit` with
`CommandSource::remote`. `submit` is single-threaded. Reassemble MQTT fragments
on that same worker, then call the decoder. Do not call it from code that writes
the output, and do not call it before the wall clock is trusted.

Checks run in order:

1. A payload longer than `command_envelope_max_bytes` (512) is `rejected_oversize`
   and is not parsed.
2. A payload that is not one JSON object, or that breaks the schema 1 rules
   below, is `rejected_malformed`. Container nesting deeper than eight levels is
   malformed. An integer `schema_version` outside int64 range is malformed.
3. A complete JSON object whose integer `schema_version` is not 1 is
   `rejected_unknown_version`, even when other fields would not fit schema 1.
4. `issued_at` and `expires_at` are UTC `YYYY-MM-DDTHH:MM:SSZ` timestamps, with
   an optional fraction of one to three digits. The monotonic deadline is
   `expires_wall_ms - wall_offset_ms`. When that value is `<= now`, the reason
   is `rejected_expired`.

Those four reasons do not call `submit` and do not use the result FIFO. The
caller can publish them. When the deadline is still ahead, the reason is
`submitted` and `admission` is the `submit` status.

A full command FIFO or a full result FIFO rejects a new enable with
`rejected_full`. The enable is not kept. A full result FIFO can also drop the
rejection record; `dropped_results` counts those drops. `CommandType::stop` does
not enter the command FIFO, including when the result FIFO is full.

`command_id` is 1-16 hexadecimal digits and maps to `Command::id`. Identity and
sequence are not payload fields. Identity is a non-zero `uint32_t` chosen by the
caller after authentication. Sequence is the per-source counter the caller
supplies and must increase for each new accepted command. A zero id, identity, or
sequence is submitted and then `rejected_invalid`. Duplicate memory and sequence
cursors reset on reboot.

`type` is `set_output` or `stop`. `enabled` and `max_duration_ms` are required
for `set_output` and forbidden for `stop`. An enable is `rejected_invalid` unless
`max_duration_ms` is strictly below the device `max_run_ms`; the ingress does not
clamp it. The sample payload uses 5000 ms, which the default policy rejects.
The monotonic deadline must be no further away than `command_defaults::max_ttl_ms`
or admission is `rejected_invalid`. `issued_at` must be less than or equal to
`expires_at`. Schema 1 strings are raw text with no escape sequences. Integers
have no fraction, exponent, or leading zero. Unknown keys are malformed in
schema 1.

Admission statuses are `accepted`, `rejected_invalid`, `rejected_expired`,
`rejected_duplicate`, `rejected_stale_sequence`, `rejected_unauthorized`, and
`rejected_full`. After the control loop runs, a queued command is `applied`,
`not_applied`, `expired_before_apply`, `dropped_by_stop`, or `superseded`.
`accepted` does not mean the output turned on, and it does not mean a stop left
the output off. A stop is `applied` only after the control step observes
`AppliedOutput::off`. `unknown` and `on` are `not_applied`. See
[ADR 0008](decisions/0008-output-quiesce.md). `TimedOutput` can still refuse.
A command that expires after decode and before the control loop is recorded by
the ingress; the decoder's `rejected_expired` is only the deadline already due
at decode.

A future MQTT subscription should:

- reassemble into at most 512 bytes on the worker that calls `submit`
- reject the command when trusted time is unavailable, before calling the decoder
- publish `command-results` from the result FIFO when the broker is reachable,
  without retaining one-shot actuator commands
- publish `rejected_oversize`, `rejected_malformed`, `rejected_unknown_version`,
  and decoder `rejected_expired` on that same path, because those never enter
  the FIFO

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

`stop` uses the same identity fields and timestamps and omits `enabled` and
`max_duration_ms`.

```json
{
  "schema_version": 1,
  "command_id": "a1",
  "type": "stop",
  "issued_at": "2026-10-25T03:00:00Z",
  "expires_at": "2026-10-25T03:00:15Z"
}
```

Never retain one-shot actuator commands. The command payload limit is 512 bytes.
Command queue depths stay in `device_config.hpp`. Reassemble fragmented MQTT
payloads into bounded owned storage before parsing. Reject malformed, expired,
unsupported, or unauthorized requests. Reject time-dependent enable commands
before trusted time is available. Deduplicate IDs with a defined lifetime and
reboot policy. MQTT QoS does not replace application-level idempotency for
physical operations.

Acknowledge acceptance separately from completion. Queue saturation produces an
explicit rejection when transport is available; it must not silently accept work.
A reconnect must not replay expired commands. A device may shorten requested
runtime, and all remote requests remain subject to local interlocks.
