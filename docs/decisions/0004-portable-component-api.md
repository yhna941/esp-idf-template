# ADR 0004: Responsibility-based names and portable network coordination

Status: accepted. Updates ADR 0003 naming and network composition.
Keeping initialization failure terminal until reboot is superseded by
[ADR 0007](0007-service-lifecycle.md).

## Decision

Use `domain`, `application`, `drivers`, `platform`, `configuration`, and `networking`
for component names, include roots, and namespaces. This template favors direct
names that users can adopt without renaming a library brand throughout their code.
Retain `ESPKIT_` Kconfig symbols and the `espkit` NVS namespace/schema for compatibility.

Separate `NvsConfigStore` from portable settings validation. Load settings in the
composition root before tasks start and configure the network service with a copy.
Inject Wi-Fi, MQTT, and time readiness through portable ports. Keeping
initialization failure terminal until reboot is superseded by
[ADR 0007](0007-service-lifecycle.md), which supplies the resource-cleanup
contract. Existing adapter reconnect behavior continues after successful
initialization. Test startup, TLS gating, and failures on the host. Split
platform headers by clock, snapshot, log, and task responsibility.

Host tests link component libraries instead of repeating production source paths.
Each component lists portable sources once and adds SDK sources only in IDF builds.

## Migration

- Rename `components/espkit_<layer>` to `components/<layer>` in custom build paths.
- Replace `espkit/<layer>/...` includes with `<layer>/...` and `espkit::<layer>`
  qualified names with `<layer>`.
- Replace `platform/adapters.hpp` with the clock, snapshot, log_telemetry, or
  periodic_task header used by the caller.
- Replace `load_network_config` with `NvsConfigStore::load` from
  `configuration/nvs_config_store.hpp`.
- Inject `NetworkTime` into `NetworkService`; call `configure` with validated
  settings before worker or reporter tasks start. `state()` is worker-only.
- The default firmware project name changes from `espkit_device` to `device`;
  scripts consuming the application binary by name must use `device.bin`.
- Clean existing build directories before rebuilding after these component renames.

MQTT topics/payload, stored credentials, Kconfig options, policy behavior, and
simulated hardware outputs retain their existing contracts.
