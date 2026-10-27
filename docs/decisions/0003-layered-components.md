# ADR 0003: Layered components and project-owned composition

Status: accepted. Refines ADR 0002 without changing the constructor-injection policy.
The one-shot startup limit is superseded by [ADR 0007](0007-service-lifecycle.md).

## Context

A flat root namespace, mixed prefixed/unprefixed component names, and product
settings inside runtime made the template hard to navigate. The network adapter
combined Wi-Fi, MQTT, configuration, and connection coordination. ESP-IDF already
owns names such as `hal`, making unprefixed components risky.

## Decision

Use `{domain,application,drivers,platform,configuration,networking}` namespaces.
Build-component names originally used `espkit_*`; ADR 0004 removes that prefix. Domain owns portable values and policy;
application owns consumed ports/workers. Concrete adapters implement these ports.
Move product wiring, board settings, and task budgets to `main/` under `device`.
Split networking into injected `WifiStation`, `MqttTelemetry`, and `NetworkService`.
Enforce owned include/CMake edges in CI. Do not add general-purpose factories,
service locators, event buses, or per-state classes without a concrete need.

## Migration and consequences

This is a source API break. Update includes and qualified names; there are no
compatibility aliases. Examples, host tests, Unity registration, and tutorials move
with the API. NVS schema, MQTT topics/payload, disabled outputs, and default
simulation behavior are unchanged. The runtime objects stay boot-lifetime.
Network startup is not one-shot; that limit is superseded by
[ADR 0007](0007-service-lifecycle.md).
The simulation example explicitly reuses `main/device.cpp`; standalone node examples
own their deployment task settings. Network/hardware acceptance remains unverified.
