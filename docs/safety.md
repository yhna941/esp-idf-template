# Safety and failure behavior

This repository currently simulates all I/O. It does not control a pump.

The portable policy disables output for invalid, non-finite, stale, or future-dated
sensor values, invalid durations, a regressing clock, and maximum runtime expiry.
Faults latch until an explicit reset while output is off. Physical value ranges
and units must be validated by a product-specific policy.

Command admission is separate from that policy and cannot replace it. The ingress
checks the deadline, requested duration, identity, source, duplicate id, and
sequence, and it rejects a full queue instead of blocking. `stop` does not wait
behind queued enables. A retransmission of an accepted id cannot restart the
runtime. There is no command that resets a fault. Remote sources stay rejected
unless `ESPKIT_REMOTE_COMMANDS` is enabled. That option does not subscribe to
MQTT or enable a physical output. See [ADR 0005](decisions/0005-command-ingress.md).

Before enabling real hardware:

1. Set a safe output latch before enabling its GPIO direction. Verify boot/reset
   pin behavior, relay polarity, and a default-OFF circuit.
2. Have one control task own each output, with bounded driver calls.
3. Enforce maximum on-time, minimum off-time, sample freshness, and duty limits.
4. Keep safe local control independent of MQTT connectivity.
5. Give remote manual commands a deadline and a bounded runtime. A retransmission
   must never restart the runtime deadline. Do not expose unrestricted fault reset.
6. Provide a stop/interlock path that cannot be delayed by a full command queue.
7. Validate watchdog behavior, brownouts, sensor disconnects, bus stalls, reboot,
   output write failure, and storage corruption on the actual board.
8. Use independent hardware cutoff where the consequences require it. A stalled
   task, stuck relay, or failed transistor cannot be handled by policy code alone.

A network outage does not itself force every product off: verified local automatic
operation may continue. Missing or invalid safety inputs do force a safe state.
Select these rules explicitly for each product. The starter defaults to OFF.

OTA must quiesce outputs and confirm this before restarting. Network `stop` does
not do that. `application::OutputQuiesce` submits one local stop and polls
`Telemetry.applied_output`. The attempt is confirmed only when a sample taken
after that stop reads `off`. `unknown` and a sample that is still `on` are
unsuccessful, and so is reaching the deadline without that newer `off` sample.
A failed OFF stays unknown. The helper does not call network `stop` or
`esp_ota_*`. An overwritten stop in the one-slot mailbox stays `accepted` and
has no completion record; `accepted` is not a safe output. See
[ADR 0008](decisions/0008-output-quiesce.md). The helper is not called on boot.
The partition layout only reserves storage. `decide_boot_health` holds,
confirms, or refuses from a completed control step and an applied output of
off, and it ignores broker reachability. Download, image checks, and marking a
slot valid are not present.
