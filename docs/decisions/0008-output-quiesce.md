# ADR 0008: Confirm output quiesce before a disruptive restart

Status: accepted.

## Context

[ADR 0001](0001-portable-control-and-simulation.md) reserves OTA until output
quiescing exists. [ADR 0007](0007-service-lifecycle.md) separates that from the
network lifecycle: network `stop` must not call the output or the command
ingress. A restart that needs a quiet actuator belongs to the composition root,
in order: admit a local stop, observe applied status until it is off, and only
then release the radio. A failed OFF stays unknown and is not a confirmed safe
state. [Safety](../safety.md) states the same rule.

`CommandRequests::note_outcome` recorded every latched stop as `applied` and
ignored the applied level and the fault flag. The control worker publishes
`AppliedOutput::unknown` when the best-effort OFF write fails. The boolean
passed alongside that flag is false for both a successful OFF and a failed OFF,
because a failed write is not "left on". A restart that trusted `applied`, or
that trusted the boolean alone, would proceed after a failed OFF.

The stop mailbox is one slot. [ADR 0005](0005-command-ingress.md) already notes
that completion is recorded for the notice the control loop observes. A newer
stop replaces the previous notice. That replaced command has already returned
`accepted`, and nothing writes a later completion for it.

## Decision

`application::OutputQuiesce` is the portable check. It borrows the command
ingress and the telemetry reader. `submit` builds one `CommandType::stop` with
`CommandSource::local` and passes it to `CommandIngress::submit`. `poll` reads
`Telemetry.applied_output`. The helper does not call network `stop` and does not
call `esp_ota_*`. It does not sleep or take a FreeRTOS primitive. The caller
polls with the same monotonic clock the control worker uses, and the control
task keeps stepping.

A sample counts only when its uptime is strictly greater than the submit time
and strictly greater than the uptime of a sample that was already published at
submit. Until such a sample arrives, `poll` returns `pending`, or `deadline`
once `now >= deadline`.

| Qualifying sample | Status | Safe to restart |
| --- | --- | --- |
| `AppliedOutput::off` | `confirmed_off` | Yes. This is the only successful status. |
| `AppliedOutput::unknown` | `unknown` | No. Terminal for this attempt. |
| `AppliedOutput::on` | `still_on` | No. Terminal for this attempt. |
| None before the deadline | `deadline` | No. |
| Stop was not accepted | `not_admitted` | No. A later `off` sample does not change it. |

`unknown` and `still_on` do not wait out the rest of the deadline. The control
step that published the sample already ran the stop. A later `off` does not
rehabilitate that attempt. Retry with a new stop if the product still needs one.

`CommandRequests::note_outcome` now takes that step's `AppliedOutput`. The
latched stop is `applied` only when the acknowledgement is `off` and the output
was not left on. `unknown`, `on`, and a fault paired with any level other than
`off` are `not_applied`. A latched fault together with an observed `off` is
still `applied`: the fault is not an applied level. The control worker passes
the same `AppliedOutput` it publishes. The port's default is `unknown`, so a
caller that omits the level cannot confirm a stop.

An overwritten stop stays `accepted` and has no completion record. The mailbox
does not retain the replaced id, and the ingress shares no other channel with
the control task. Writing `superseded` from the ingress would race a control
step that had already latched that notice and might still record `applied` or
`not_applied`. The helper does not read the result FIFO. `accepted` is admission,
not a safe output. The stop the control loop actually observes is the one that
becomes `applied` or `not_applied`.

Boot does not call the helper. OTA download is still not implemented. A later
restart calls `OutputQuiesce` first, continues only on `confirmed_off`, and only
then stops the network or restarts. Those later steps stay in the composition
root.

## Alternatives rejected

- **Call network `stop` or `esp_ota_*` inside the helper.** Those steps come
  after a confirmed off. Starting them here would couple a failed OFF to a
  restart that had already begun. [ADR 0007](0007-service-lifecycle.md) keeps
  network `stop` free of the output.
- **Treat `accepted` or the old unconditional `applied` as quiesce.** `accepted`
  means the ingress armed a stop. The old `applied` was written even when the
  level was unknown.
- **Confirm when the boolean output level is false.** That boolean is false
  after a successful OFF and after a failed OFF.
- **Treat a latched fault as proof of off, or as a veto of an observed off.**
  The fault latch means the policy or a write failed closed. The applied
  enumeration is the acknowledgement.
- **Keep polling after `unknown` or `on` until the deadline.** The publishing
  step already handled the stop. Waiting does not turn unknown into off.
- **Busy-loop on the clock, or delay inside the helper.** Application code stays
  free of ESP-IDF and FreeRTOS. The caller decides how to wait.
- **A service locator, event bus, or a class per restart state.** One concrete
  helper is the use. Statuses are an enum.
- **Synthesize `superseded` for every overwritten stop.** The replaced id is
  gone. A second writer on the ingress task races the control task.
- **Confirm a sample published at or before submit.** That sample does not show
  this stop. A pre-stop `off` is not an observation of this attempt.

## Consequences

Overrides of `RequestSource::note_outcome` must accept `AppliedOutput`. Callers
that do not override it still compile. The worker is the production caller and
passes the published level.

`submit` is single-shot after acceptance. A rejected attempt can be retried on
the same object. The caller owns the id, sequence, and identity. `expires_at`
is the command admission deadline. The poll deadline is a separate monotonic
time.

The control task must keep running and publishing telemetry. Stopping it freezes
the last sample, and the helper then hits the deadline or reads a stale level.
The sample uptime and the submit time have to come from one monotonic clock. A
backward step can make an older sample look newer than the submit time; the
helper also ignores a sample that is not newer than the baseline uptime captured
at submit.

Hardware was not validated. Host tests use fakes. The ESP32-S3 build only
checks that the helper compiles into the firmware image. Nothing on the device
calls it.
