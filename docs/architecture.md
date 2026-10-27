# Architecture

ESPKit separates portable device behavior from SDK adapters and product wiring.
Start with [Code walkthrough](code-walkthrough.md) for the execution order.

## Names and directories

Project components use their responsibility as their name. A component is a
build/dependency boundary; a namespace is a C++ name boundary. Public headers
mirror their owning namespace. Check for SDK component collisions when adding names.

| Component | Namespace | Responsibility |
| --- | --- | --- |
| `domain` | `domain` | Measurements, status, timed-output policy, and command checks |
| `application` | `application` | Portable ports, command ingress, and single-step workers |
| `drivers` | `drivers` | Synthetic/SHT3x inputs and simulated output |
| `platform` | `platform` | ESP-IDF I2C, monotonic clock, FreeRTOS queues/tasks, log sink |
| `configuration` | `configuration` | Network settings validation, schema migration, and NVS loading |
| `networking` | `networking` | Wi-Fi lifecycle, MQTT telemetry, and connection coordination |
| `main/` | `device` | Product selection, board settings, task budgets, and object wiring |

For example, `drivers::Sht3xSensor` is declared in
`components/drivers/include/drivers/sht3x.hpp`.
Header paths use `application/...`. Kconfig symbols retain `ESPKIT_` because
they share a global configuration namespace; the stored NVS namespace remains
`espkit` to preserve provisioned settings.
Avoid generic `core`, `utils`, and `common` buckets. Do not add a component or a
namespace for each class. Split components when independent reuse or dependencies
justify a new build boundary.

## Dependency direction

```text
main (composition root)
  ├── drivers ──────────┐
  ├── platform ─────────┤
  ├── networking ───────┤──> application ──> domain
  │      └── configuration (independent settings adapter)
  └── domain / application
```

Domain, application, and drivers contain no ESP-IDF/FreeRTOS headers. Domain
has no dependency on application. Ports belong to the application that consumes
them, not to the SDK implementation. Platform, networking, and configuration
must not depend on each other except networking's explicit configuration dependency.
Public dependencies use CMake `REQUIRES`; implementation-only ones use `PRIV_REQUIRES`.
Run `python3 tools/check_architecture.py`; CI checks owned include and component edges.
This is a lightweight boundary check, not a general C++ dependency analyzer.

## Patterns

| Pattern | Concrete use |
| --- | --- |
| Constructor injection | Workers borrow sensor, output, clock, and snapshot ports |
| Composition root | `main/device.cpp` selects and owns concrete implementations |
| Adapter | ESP-IDF I2C/clock and FreeRTOS implement portable application ports |
| Strategy | Sensor/output ports allow synthetic and physical implementations |
| State machine | `domain::TimedOutput` explicitly tracks off/running/fault |
| Service lifecycle | Network phases and `NetworkState`; stop releases resources in reverse order |
| Producer-consumer | Bounded latest-value queues connect workers |
| Bounded FIFO with rejection | Command ingress returns a status instead of overwriting or blocking |
| Separate stop mailbox | A full command FIFO cannot delay `CommandType::stop` |
| In-memory schema migration | `migrate_settings` upgrades stored network fields without rewriting NVS |

No DI container, service locator, Singleton, generic event bus, or factory hierarchy
is required. A replaceable control-policy interface should wait until a second
policy needs it. Network coordination uses an explicit state enum rather than
one dynamically allocated object per state.

## Lifetime and concurrency

`app_main()` calls `device::start_device()`. A function-local static `DeviceRuntime`
constructs after SDK startup. Members are declared before objects borrowing them.
The runtime object and the sensor, control, and reporter tasks are boot-lifetime.
`start_device()` does not stop them. The network stack is different: it has an
explicit lifecycle so a failed or partial SDK initialization can be released and
retried, and so Wi-Fi can be stopped and started again, without rebooting and
without stopping local control. See [ADR 0007](decisions/0007-service-lifecycle.md).

Sensor (500 ms), control (100 ms), and reporter (2000 ms) workers run independently.
Two length-one FreeRTOS queues carry latest-value snapshots; reads do not consume
them. Overwrite is intentional. Commands use a separate bounded FIFO that rejects
when full, plus a one-slot stop notice. `CommandIngress::submit` is single-threaded;
the FIFO and stop mailbox are the cross-task boundary. The default firmware does
not call `submit`, so the output request stays off. Startup seeds a reading and
applies safe OFF before tasks start.

`DeviceRuntime` owns `WifiStation`, `SntpTime`, `MqttTelemetry`, and
`NetworkService` for the boot. `stop` does not destroy those objects. Before any
tasks start, the composition root loads validated settings through
`configuration::NvsConfigStore` and passes a copy to `NetworkService::configure()`.
That call is ignored after the service leaves `disabled`; a later restart reuses
the owned copy. After startup, only the network task calls `start`, `stop`,
`step`, and `state`.

`step` starts Wi-Fi, then SNTP when the URI is `mqtts://`, waits for the link and
for injected `NetworkTime` readiness, then starts MQTT. `stop` releases in the
opposite order: MQTT, SNTP, then Wi-Fi. A failed `start` releases the resources
that stage owns. Wi-Fi bring-up failure releases the whole stack and moves to
`failed`, which `step` retries after `network_init_retry_ms`. MQTT bring-up
failure releases only the client and retries it, leaving the station and SNTP up.
`waiting_for_time` is bounded on the monotonic clock. One SNTP session waits
`network_init_retry_ms`; on expiry `step` stops SNTP and begins it again. After
`network_time_session_budget` sessions the bring-up fails like a Wi-Fi `start`
failure. `NetworkTime::ready()` stays a wall-clock check. A failed MQTT or SNTP
release leaves that phase in place so the next `stop` or `start` can finish it.
`stopped` does not retry until `start`. Losing the link after a successful start
is not `failed`; the station and the MQTT client reconnect on their own.

The TCP/IP stack and the default event loop stay initialized across that restart.
ESP-IDF does not support `esp_netif_deinit`, and SNTP and MQTT share the event
loop. The station netif, Wi-Fi driver, handlers, MQTT client, and SNTP session are
the resources `stop` actually releases. `esp_netif_create_default_wifi_sta` aborts
on failure, so the adapter uses the fallible netif calls instead; an abort would
stop the control task as well.

The reporter calls `NetworkService::report()`, which gates transmission on the
Wi-Fi link and delegates to MQTT. That call may run while the network task is
stopping. The MQTT adapter serializes use of the client pointer. Wi-Fi and MQTT
callbacks only update atomics. The network worker enqueues retained online
availability after the callback records a connection. Config strings remain
valid because `NetworkService` owns its settings for the boot lifetime.

`NetworkService::stop` does not read the sensor and does not change the output.
A later operation that needs a quiet actuator, including OTA, has to request safe
OFF through the command path and observe the applied status first. A failed OFF
stays unknown. That quiesce sequence is not implemented here. Control never calls
networking. Telemetry uses bounded nonblocking enqueue with QoS 0 and no offline
backlog. Sensor bus waits remain on the sensor task. Task budgets require hardware
measurement; compilation is not timing validation.

## Safety and status

`TimedOutput` rejects invalid/stale input and maximum runtime expiry, latching a
fault. Repeated ON requests cannot extend an interval. The worker applies the
policy decision; output errors latch fault and trigger a best-effort OFF. Failed
OFF is reported as unknown, not confirmed OFF. Applied status is software
acknowledgement, not physical feedback. Cooldowns, duty limits, independent
interlocks, and hardware watchdog behavior are product responsibilities.

Physical outputs remain disabled. Remote command sources stay rejected unless
`ESPKIT_REMOTE_COMMANDS` is enabled, and that option does not subscribe to MQTT.
Log status identifies sensor origin separately from the simulated output mode.
See [Safety](safety.md) and [ADR 0005](decisions/0005-command-ingress.md).

## Constants and settings

- `main/device_config.hpp`: sampling periods, task stacks/priorities, command queue depths, and selected policy.
- `main/boards/esp32s3/board_config.hpp`: GPIO and bus settings, exposed by `main/Kconfig.projbuild`.
- `domain/policy_defaults.hpp`: reusable safe-policy defaults, overridable through `OutputPolicy`.
- `platform/constants.hpp`: queue capacity and time conversions, not deployment task budgets.
- Driver `.cpp` files: fixed protocol commands, CRC values, and timing requirements.
- Runtime NVS: validated network credentials and identity, schema 1, with in-memory migration; see [Hardware and MQTT](hardware-mqtt.md) and [ADR 0006](decisions/0006-versioned-settings.md).

Use units in names. Configuration objects express values that vary by device;
constants express fixed protocol/library rules. Do not collect every literal into
one global constants file. NVS storage is not automatically encrypted.

## Conventions and evolution

Use C++20, `PascalCase` types, `snake_case` functions/files, trailing underscores
for fields, and English comments/docs. Public headers have no `using namespace`
directives; local aliases are acceptable in composition `.cpp` files. Use RAII
where lifetime is supported, borrowed references for injected dependencies, and
monotonic time for safety. Run `python3 tools/format.py` with clang-format 18.

See [ADR 0004](decisions/0004-portable-component-api.md) for migration details and
[Cluster and swarm evolution](distributed-systems.md) for future boundaries.

## Host build

Host tests add the component directories with `add_subdirectory()` and link their
library targets. Each component declares its portable sources once for both builds;
ESP-IDF adds SDK adapter sources. Configuration validation and network coordination
are included in the portable host targets.
