# ADR 0007: Restartable lifecycle for the network stack

Status: accepted. Supersedes the "initialization failure is terminal until reboot"
sentence in [ADR 0004](0004-portable-component-api.md), the boot-once composition
limit in [ADR 0002](0002-injected-workers-and-composition.md), and the one-shot
startup limit in [ADR 0003](0003-layered-components.md).

## Context

ADR 0004 left a failed network start terminal until reboot. Retrying it would
have reused SDK objects from the attempt that failed. On ESP-IDF that attempt is
not one call. Bringing up the station does several things in order: initialize
the TCP/IP stack, create the default event loop, create the station netif,
initialize the Wi-Fi driver, register event handlers, then start the radio.
MQTT is the same shape at a smaller scale: create a client, register its
handler, then start it. SNTP is `esp_sntp_init` until `esp_sntp_stop`.

If a later call fails, the earlier objects are still allocated. Calling the same
start sequence again returns `ESP_ERR_INVALID_STATE`, leaks a netif, or creates
a second MQTT client. The old code returned false and stored `failed` with no
release path, so the only recovery was a reboot. A reboot also stops sensor and
control tasks that were still healthy.

Local control must keep running when the network does not. Control must not call
networking. A later OTA update will need the actuator forced OFF before the
restart, but that is a different ordered action from releasing the radio. ADR
0001 already says OTA waits for output quiescing; this change does not add OTA.

`esp_netif_create_default_wifi_sta` aborts the task if netif creation fails.
That would take local control down with the network. `esp_netif_deinit` returns
"not supported". The default event loop is shared with SNTP and MQTT, so deleting
it on a station restart would break the next start.

## Decision

Give the network stack an explicit lifecycle. The states live in enums. There is
no class per state, no service locator, and no lifecycle event bus.

`NetworkState` is what `NetworkService` shows the network worker:

| State | Meaning |
| --- | --- |
| `disabled` | No settings copied. `step` and `stop` do nothing. |
| `starting` | Settings are owned. The next `step` starts Wi-Fi. |
| `waiting_for_wifi` | The station driver started. `step` polls the link. |
| `waiting_for_time` | The link is up and TLS still needs a wall clock. The wait is bounded on the monotonic clock. |
| `waiting_for_mqtt` | Link and clock are ready, or the last MQTT `start` failed. |
| `running` | MQTT `start` succeeded. Link loss stays here. |
| `failed` | Wi-Fi `start` failed, or SNTP used its session budget without a wall clock. Resources are released. `step` retries after `network_init_retry_ms`. |
| `stopped` | `stop` ran. Resources are released. `step` does not retry. `configure` may replace the owned copy. |

`configure` copies settings from `disabled` and replaces the owned copy from
`stopped`. The `stopped` call stays `stopped` until `start` and does not write
NVS. Every other state ignores `configure`, so a running client cannot race a
new copy. `start` is legal from `stopped` and `failed`;
the next `step` brings the stack up. `stop` from anything except `disabled`
releases resources and lands on `stopped`, which cancels the automatic retry.
Calling `stop` again is safe and releases again, so a cleanup the SDK refused
can be retried.

Start order is Wi-Fi, then SNTP when the URI is `mqtts://`, then MQTT once the
link and (for TLS) the clock are ready. Stop order is the `connection_stop_order`
list: MQTT, SNTP, Wi-Fi. SNTP is started when the station driver starts, not
when the address arrives, matching the previous sequence.

Wi-Fi `start` failure releases every connection, even ones that had not started,
and enters `failed`. MQTT `start` failure releases only the client. The station
and SNTP stay up, the state stays `waiting_for_mqtt`, and a later `step` retries
MQTT after the same delay. A dropped link after `running` is not `failed`.
`WifiStation::step` reconnects with its existing backoff, and the MQTT client
reconnects on its own. Reinitializing the driver on every roam would be a worse
failure mode.

Resource phases are separate from `NetworkState`, because the coordinator and
the adapters fail at different grains:

- `WifiPhase` records process stack, station netif, driver, handlers, and radio.
  `release_wifi_phase` walks service-owned phases backwards and stops at
  `process_ready`. The adapter performs one SDK release per phase. If a release
  call fails, the phase stays put so the next `stop` or `start` continues there.
- `MqttPhase` is `idle`, `created`, or `started`. `mqtt_cleanup` stops a started
  client before destroy, and only destroys a client that never started. The SDK
  returns `ESP_FAIL` for `esp_mqtt_client_stop` in that second case.
  `esp_mqtt_client_destroy` also stops a client whose task is still running, so
  a crash between `start` returning and the phase update still has a release path.
  A failed `esp_mqtt_client_stop` leaves `started`. A stop that succeeded with a
  failed destroy leaves `created`, which is the phase that still has to be
  destroyed, so the next `stop` does not call `esp_mqtt_client_stop` again.
  The client handle is kept. `start` does not create another client until the
  phase is `idle`.
- `SntpPhase` is `idle` or `started`. `ready()` reads the wall clock, which can
  stay valid after `esp_sntp_stop`. TLS does not have to wait again just because
  SNTP was restarted. `stop` returns to `idle` only when `esp_sntp_stop` leaves
  the session disabled. `begin` does not call `esp_sntp_init` while the phase
  is still `started`, and it does not record `started` unless the session is
  enabled afterwards.

`waiting_for_time` uses the monotonic clock, not `ready()`. Each SNTP session
waits `network_init_retry_ms`. On expiry `step` stops SNTP and begins it again.
After `network_time_session_budget` sessions (the one opened with the station,
then one restart) the bring-up releases every connection and enters `failed`,
then retries like a Wi-Fi `start` failure. A dropped link does not move that
deadline or grant another session. `ready()` stays `time >= 2024-01-01`. A
clock that never reaches that instant is a coordinator test; it is not folded
into `ready()`.

The station adapter does not call `esp_netif_create_default_wifi_sta`. It calls
`esp_netif_new`, `esp_netif_attach_wifi_station`, and
`esp_wifi_set_default_wifi_sta_handlers`, which return errors. `stop` then uses
`esp_wifi_stop`, unregisters the instance handlers, `esp_wifi_deinit`, and
`esp_netif_destroy_default_wifi`. `esp_netif_init` and
`esp_event_loop_create_default` run once. A second event-loop create that returns
`ESP_ERR_INVALID_STATE` counts as already done. Neither object is deleted on
`stop`.

`DeviceRuntime` in `main/device.cpp` owns the four network objects for the boot.
It does not destroy them on `stop`, and it does not stop the sensor, control, or
reporter tasks. The first `configure` runs on the startup thread before tasks
exist. After that, the network worker is the only caller of `configure`,
`start`, `stop`, `step`, and `state`. A later `configure` only replaces the
owned copy from `stopped`. The composition root does not call it again. The
reporter may call `report` during `stop`. `MqttTelemetry` keeps the
client pointer behind a mutex, clears the pointer, then unlocks before
`esp_mqtt_client_stop` and `destroy`, so the MQTT task callback (atomics only)
cannot deadlock on that mutex. If those calls fail, the pointer is stored again
with the phase that still needs release. The callback records a connection by
bumping an atomic. The network worker enqueues the retained online availability
message; the callback does not. The Wi-Fi callback only stores atomics.
`connected()` is cleared at the beginning of `WifiStation::stop` so reporters
stop enqueueing before the driver is gone.

Network `stop` is not output quiescing. It must not call `OutputSink` or the
command ingress. A future OTA or maintenance restart that needs a safe actuator
belongs to the composition root, in this order.
[ADR 0008](0008-output-quiesce.md) is that check. Network `stop` still does not
perform it:

1. Admit a local stop through the existing command path so `TimedOutput` releases
   the output.
2. Observe applied status until it is off. A failed OFF stays unknown and is not
   a confirmed safe state.
3. Only then call network `stop` if that operation needs the radio released.
4. Leave the control task running. A stopped network is not what makes the
   output safe.

The phase transitions and the coordinator are host-tested. The adapter `.cpp`
files keep the ESP-IDF calls. Unity does not gain a lifecycle case: the target
harness cannot execute SDK teardown without hardware, and the portable rules are
already covered by GoogleTest.

## Alternatives rejected

- **Keep init failure terminal until reboot.** That was ADR 0004. It avoids a
  cleanup contract by throwing away healthy control tasks. This ADR is that
  contract.
- **A class per state, or a virtual lifecycle base every component inherits.**
  Two concrete sequences (coordinator states and Wi-Fi phases) do not need the
  State pattern. A new service copies the enum and the reverse `stop`, as the
  walkthrough describes, when it actually acquires partial SDK resources.
- **DI container, service locator, or lifecycle event bus.** The composition root
  already owns the objects. Workers already call `step` directly.
- **Stop sensor and control tasks as part of network `stop`.** Local safety must
  run while the broker is down. OTA quiesce is a separate, explicit request.
- **Treat network `stop` as safe OFF.** Applied status would lie. Failed OFF is
  already defined as unknown, and only the control worker may write the output.
- **Deinitialize the TCP/IP stack and delete the default event loop on every
  retry.** `esp_netif_deinit` is unsupported. The event loop is shared. Retry
  would then fail for a different reason.
- **Call `esp_netif_create_default_wifi_sta`.** It aborts on failure, which kills
  the calling task. The fallible calls return to `failed` instead.
- **Reinitialize Wi-Fi on every disconnect.** Association loss already has a
  bounded backoff inside `WifiStation::step`. Tearing down the driver there
  drops the link longer and races the event callback.
- **Retry a failed start in a tight loop, or reboot the chip from `failed`.**
  The network task already wakes on a one-second period. The retry waits five
  seconds on the monotonic clock. A reboot would stop independent workers.
- **Automatic reconfigure from NVS on every retry.** Reloading flash on the
  retry path is a configuration-schema concern, not a lifecycle one, and a
  second writer would race the network worker. Replacing the owned copy is an
  explicit `configure` from `stopped` on that worker, not an NVS read.
- **Persist the phase in NVS.** A phase describes RAM and SDK objects that do
  not exist after reboot. The boot always starts from `idle`.

## Consequences

Out-of-tree `WifiConnection`, `MqttConnection`, and `NetworkTime` implementations
must add `stop()`. `MqttConnection` must also add `publish_availability()`. An
empty body is enough when that adapter does not publish availability; the MQTT
adapter publishes from the network worker. `NetworkTime::stop` must leave the
session started when the implementation could not stop it, and `begin` must not
open a second session in that case. `NetworkService` now takes an
`application::Clock&` so the retry delay does not depend on the task period.
`NetworkState::failed` is no longer terminal. Callers that treated it as "give
up until reboot" should call `stop` when they want the retry suppressed. A
stalled wall clock also enters `failed` after the SNTP session budget.

`configure` from `stopped` replaces the owned settings and leaves the service
`stopped` until `start`. After startup the network worker is the only caller.
The call does not write NVS, and there is no second writer. Every other state
still ignores `configure`.

`esp_mqtt_client_stop` must not run on the MQTT task. The network worker is the
caller, and it is also the caller of `esp_mqtt_client_enqueue` for retained
availability. Hardware was not validated: host tests use fakes, and the ESP32-S3
build only checks that the adapter compiles. Association, broker loss, a real
partial `esp_wifi_init` failure, a failed `esp_mqtt_client_stop` or
`esp_mqtt_client_destroy`, and a wall clock that never reaches 2024-01-01 still
need a board.

Enabling `ESPKIT_NETWORK_ENABLED` does not by itself start the stack. The
composition root calls `configure` only when NVS yields a valid configuration,
then the network task's `step` drives the lifecycle. Sensor, control, and the
idle output request are unchanged when that configuration is missing.
