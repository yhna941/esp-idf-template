# ADR 0005: Bounded command ingress in front of TimedOutput

Status: accepted.

## Context

The device publishes telemetry and keeps the actuator off. Snapshots are
one-slot mailboxes: a new value replaces the old one, which is correct for a
sensor reading and wrong for a command. A lost or reordered command can start
an output late, restart a deadline, or fill memory if the queue is unbounded.

[Safety](../safety.md) already requires a deadline, a bounded runtime, rejection
of retransmissions that would restart that runtime, no unrestricted fault reset,
and a stop path that still works when the normal command queue is full. Control
must keep working when the network does not. ADR 0003 and ADR 0004 keep those
rules in portable code with no ESP-IDF types.

## Decision

Split the path into a pure check, a single admission object, and the existing
control worker.

`domain::validate_command` checks shape, range, monotonic deadline, and whether
a remote source is allowed. It stores nothing. `application::CommandIngress::submit`
adds boot-lifetime duplicate and per-source sequence checks, then either pushes
a `QueuedCommand` or rejects it. A full command FIFO or a full result FIFO
returns `rejected_full` and does not keep the command. `CommandType::stop` does
not enter that FIFO. It publishes a one-slot `StopNotice`. The control loop
drops queued work admitted under an older stop generation and requests OFF.

`CommandRequests` implements the existing `RequestSource`. The control worker
still calls `TimedOutput` and only then `note_outcome`. A command can shorten
the active lease and cannot move its start time later. Duplicate ids are
rejected, so a retransmission cannot restart the lease. Fault reset is not a
command.

Results use the same `CommandStatus` for admission and for the later control
acknowledgement. `accepted` means the ingress kept the command or armed a stop.
`applied` or `not_applied` is recorded after `TimedOutput`. Callers that share
a thread with `submit` can also use the return value. If the result FIFO cannot
store a record, a counter increments. Stop still arms when that FIFO is full.

The FreeRTOS adapter is `platform::FreeRtosFifo`. It sends with a zero wait and
treats a full queue as rejection. Host tests use an in-memory FIFO with the
same port. `main/device.cpp` constructs both queues and the ingress. Nothing in
the default firmware calls `submit`, so the output request stays off.
`ESPKIT_REMOTE_COMMANDS` defaults to off; when off, `CommandSource::remote` is
`rejected_unauthorized`. The option does not subscribe to MQTT.

Duplicate ids and sequence cursors live in RAM and reset on reboot. The history
holds eight ids. After that, only the oldest id is forgotten; a stale sequence
still rejects a replay. `networking::decode_command_envelope` bounds the payload
and converts a trusted wall-clock deadline into monotonic `expires_at`, then
calls `submit` with `CommandSource::remote`. It does not authenticate the caller
or open a socket. The caller supplies identity and sequence after authentication.
The decoder must not call `OutputSink`.

## Alternatives rejected

- **Latest-value mailbox for commands.** Overwrite drops an enable or a stop
  without a rejection. Snapshots stay latest-value. Commands do not.
- **Unbounded queue.** A burst can exhaust RAM. The bound is the product setting
  in `device_config.hpp`.
- **MQTT callback writes the output.** That bypasses `TimedOutput` and makes
  control depend on the network task. The callback seam is `submit`.
- **Event bus, handler registry, or per-command virtual hierarchy.** Two
  concrete command types do not need a dispatch framework. A new type is a
  `CommandType` enumerator plus the switches that already fail closed.
- **DI container or service locator.** The composition root already owns the
  concrete queues, ingress, and workers.
- **Persisted idempotency in NVS.** That needs a schema migration and a wear
  policy. This boot's monotonic clock is not comparable with the previous boot.
- **Remote fault reset.** Safety forbids an unrestricted reset. A faulted
  `TimedOutput` stays faulted until product code calls `reset_fault` locally.
- **MQTT subscribe in this change.** The portable contract is the part other
  adapters can share. Subscribing now would publish a second protocol before
  that contract is in place, and the default telemetry topics must stay stable.

## Consequences

`RequestSource::note_outcome` has an empty default, so existing request sources
keep compiling. Enabling `ESPKIT_REMOTE_COMMANDS` only changes admission of
`CommandSource::remote`. Default logs, telemetry topics, and the idle output
request are unchanged because `submit` is not called.

A command duration must be shorter than `max_run_ms`. The request is released
before `TimedOutput` treats a still-running output as a fault. A clock jump
across that limit still faults. Stop completion is recorded for the notice the
control loop observes; an earlier stop in the same quiet period has already
returned `accepted` and has advanced the generation. That overwritten stop is
not given a later completion record. [ADR 0008](0008-output-quiesce.md) states
why, and why `accepted` is not a confirmed safe output. A stop the control loop
does observe is `applied` only when that step's applied output is `off`.
`submit` stays single-threaded: reassemble a remote payload on that worker. A
full result FIFO rejects a new enable. The decoder does not subscribe. Hardware
was not validated.
