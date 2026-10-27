# Code walkthrough

Read these files in order. The default firmware is a simulation; it never drives
physical GPIO outputs. Enabling the SHT3x option changes input, not output behavior.

## 1. Entry point: main/app_main.cpp

ESP-IDF calls `app_main()`. It calls `device::start_device()`; it does not contain
sensor, control, or network logic.

## 2. Wiring: main/device.cpp

`DeviceRuntime` owns concrete objects and passes references to their consumers.
For example, `SensorWorker` receives a sensor, clock, and reading writer. It never
constructs a driver itself. Creating concrete objects here is the composition-root
part of dependency injection, not a violation of it.

The selected sensor is synthetic by default. The SHT3x build option selects the
physical driver and initializes its injected I2C adapter. Output is always simulated
and the root device always requests OFF. The command ingress is wired and idle.
Settings live in `device_config.hpp` and
`boards/esp32s3/board_config.hpp`.

## 3. Single-step work: application/workers.hpp and workers.cpp

`SensorWorker::step()` reads one sample and publishes it. `ControlWorker::step()`
reads the latest sample, evaluates `domain::TimedOutput`, applies the result, and
publishes status. `ReporterWorker::step()` forwards the latest status to its sink.

A snapshot is a one-slot mailbox: a newer value replaces the old value. It is not
a command queue or a history. Host tests supply in-memory snapshots and fake clocks.

## 4. Policy: domain/timed_output.hpp and timed_output.cpp

The state machine owns off/running/fault transitions. It checks measurement age
and maximum continuous runtime using the injected monotonic time. It knows nothing
about GPIO, Wi-Fi, MQTT, or FreeRTOS. Workers own the calls to external ports.

## 5. Commands: domain/command.hpp and application/command_path.hpp

A command is a small plain value: id, sequence, identity, type, source, deadline,
and, for an enable, a requested duration. `validate_command` rejects a command
that is malformed, too long, already expired, or remote while remote admission
is off. `CommandIngress::submit` then rejects duplicates and stale sequences.
A full FIFO returns `rejected_full`. It does not block and it does not drop some
older command to make room.

`CommandRequests` is the `RequestSource` the control worker already polls. After
`TimedOutput` decides, the worker calls `note_outcome`. That is the
acknowledgement that the command was applied or refused. The command code never
calls `OutputSink` itself.

`stop` is the exception to FIFO order. It publishes a one-slot notice, bumps a
generation counter, and the next control step throws away queued commands that
were admitted before that generation. A later, new command can still be accepted.
Retransmitting an id that was already accepted cannot start the output again.
The control loop records that stop as `applied` only when the step observed
`AppliedOutput::off`. `unknown` and `on` are `not_applied`. A newer stop can
replace the notice before that loop runs. The replaced command stays `accepted`
and has no completion record. `accepted` is not a confirmed OFF.

`application::OutputQuiesce` submits one local stop and then polls
`Telemetry.applied_output`. It confirms the output only when a sample newer than
that submit reads `off`. `unknown`, a sample that is still `on`, and a deadline
with no such `off` are unsuccessful. The helper does not stop the network and
does not call OTA. The root firmware does not call it. See
[ADR 0008](decisions/0008-output-quiesce.md).

The root firmware constructs this path and does not call `submit`. MQTT does not
subscribe. `networking::decode_command_envelope` parses one bounded schema 1
payload, converts a trusted wall-clock deadline into monotonic time, and calls
`submit` with `CommandSource::remote`. Call that decoder from the same single
thread that owns `submit`, after reassembly. A full result FIFO rejects a new
enable. Subscription is still a later adapter.

### Add a command type later

1. Add a `CommandType` enumerator.
2. Update `validate_command`. Host builds treat a missing switch case as an error.
3. Decide whether it is queued or immediate, like `stop`. Handle it in
   `CommandRequests` so an unknown queued value is not silently popped.
4. Keep the output behind `TimedOutput`. Do not reset a fault from a remote command.
5. Add host tests for acceptance, rejection, and the safety interaction.

## 6. Scheduling: platform/periodic_task.hpp, snapshot.hpp, command_queue.hpp, and runtime.cpp

`PeriodicTask` repeatedly calls a worker's `step()` and yields until its next period.
Sensor reads can wait without stopping the separately scheduled local controller.
`FreeRtosSnapshot` implements the mailbox using a bounded queue. `FreeRtosFifo`
is the command queue: a zero-wait send fails when the queue is full.

## 7. Optional networking

The composition root loads NVS settings with `NvsConfigStore` before starting tasks.
The store reads the `espkit` namespace and calls portable `migrate_settings`. A
schema 1 record is used as stored. A record with no schema key is upgraded in
memory when its fields validate. A newer schema is used only through those schema
1 fields. The load does not write flash. Invalid records leave networking off.

The composition root owns `WifiStation`, `MqttTelemetry`, `SntpTime`, and
`NetworkService` for the boot and injects the first three through portable
connection ports. `configure` copies the settings once. The network task then
calls `step`.

`step` starts Wi-Fi, starts SNTP when the broker URI is `mqtts://`, and starts
MQTT only after the link is up and, for TLS, the wall clock is ready. `stop`
runs the other way: MQTT client, then SNTP, then Wi-Fi. Doing it in that order
means the clock sync and the station are not torn down while a client is still
using them.

If Wi-Fi `start` fails, the service releases every connection it may have touched
and moves to `failed`. A later `step`, after five seconds, tries again. If MQTT
`start` fails, only that client is released; Wi-Fi and SNTP stay up and MQTT is
retried on its own. TLS waits for a wall clock, but not forever: each SNTP
session gets the same five seconds, then `step` stops SNTP and begins it again.
A second expiry takes the Wi-Fi failure path. `stop` moves to `stopped` and does
not retry until something calls `start`. A link that drops after a successful
start is not a lifecycle failure: the station and the MQTT client reconnect by
themselves. If MQTT `stop`/`destroy` or SNTP `stop` does not finish, the phase
stays where that call failed so the next `stop` or `start` can finish the
release. The MQTT callback only stores atomics; the network worker publishes
retained availability.

The objects stay allocated. What `stop` releases are the SDK resources the
service created: the station netif, the Wi-Fi driver, its event handlers, the
MQTT client, and the SNTP session. The TCP/IP stack and the default event loop
stay. ESP-IDF cannot tear the TCP/IP stack down safely, and the other network
clients share the loop. Calling `stop` twice is safe, including after `start`
returned false.

The reporter logs status and attempts a bounded telemetry enqueue. Offline
reports are dropped; reconnection does not replay old measurements. Sensor and
control tasks do not call this code. Stopping the network does not turn an
output off. Before a disruptive restart, `OutputQuiesce` has to observe `off`.
`unknown` means OFF failed and is not a safe state. The helper does not reboot
and does not stop the network. OTA download is not implemented.

Real board timing, live broker outages, and physical safety still require the
[hardware acceptance checks](hardware-mqtt.md). Host tests cover the transitions.
They do not call ESP-IDF.

### Add this lifecycle to another service later

Do this only when a second service has the same problem: it acquires SDK resources
that can fail partway and must be retryable without rebooting.

1. Add an explicit phase enum for the resources that service acquires. Do not add
   a class per phase, a service locator, or an event bus.
2. Put the phase transitions in portable code and the SDK calls in the adapter.
   `networking/lifecycle.hpp` is the pattern: host tests walk the phases, and
   `wifi_station.cpp` performs one SDK call per phase.
3. `start` returns false only when service-owned resources are already released,
   or when `stop` can still see which phase was reached and finish the release.
4. `stop` walks those phases backwards and is safe when `start` never ran and
   safe when it is called again. If a release call fails, leave that phase in
   place so the next `stop` or `start` finishes it. Leave process-wide resources
   alone when the SDK cannot deinitialize them or when other services share them.
5. Call `start`, `stop`, and `step` from one worker. Do not call them from the
   control worker, and do not stop sensor or control tasks inside `stop`.
6. Retry a failed start from that worker's `step`, after a monotonic delay. Do
   not tight-loop, and do not reboot.
7. If a later operation needs the actuator off, call `OutputQuiesce` and continue
   only on `confirmed_off`. Network `stop` is not output quiescing.
8. Add host tests for the phase order, a failed start, a second `stop`, and a
   restart. Keep the ESP-IDF calls out of those tests.

## Where should I make a change?

| Change | Location |
| --- | --- |
| Select a different sensor/output | `main/device.cpp` |
| Change periods, policy, task budgets | `main/device_config.hpp` |
| Change wiring | `main/boards/esp32s3/board_config.hpp` and Kconfig |
| Add a portable driver | `components/drivers/` |
| Change local safety rules | `components/domain/` and host tests |
| Change command admission or add a command type | `domain/command.hpp`, `application/command_path.hpp`, and host tests |
| Confirm output is off before a restart | `application/output_quiesce.hpp` and host tests |
| Decode a remote command payload | `networking/command_envelope.hpp`, then `CommandIngress::submit` |
| Subscribe to remote commands later | `components/networking/`, calling `decode_command_envelope` |
| Change MQTT encoding/connection | `components/networking/` |
| Add a runtime network or identity setting | `configuration/network_config.hpp`, `settings_migration.hpp`, `NvsConfigStore`, and `tools/provision_device.py` |
| Change network start, stop, or retry | `networking/lifecycle.hpp`, `network_service.cpp`, and the adapter `stop` path |

Read [Architecture](architecture.md) for dependency rules before adding cross-component includes.
