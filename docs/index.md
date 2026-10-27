# Build your next ESP32 device

<div class="hero">
<p class="eyebrow">ESP-IDF · C++20 · FreeRTOS</p>
<p class="hero-lead">Start small. Keep control local. Grow through replaceable components.</p>
<p>A reusable firmware foundation for sensor nodes, actuators, and your first smart-farm controller.</p>
</div>

[Use this template](https://github.com/nawuv/esp-idf-template/generate) · [Get started](getting-started/installation.md) · [Explore examples](examples.md) · [Understand the architecture](architecture.md)

## Your first working loop

```text
Sensor → latest reading → local control → output adapter
                              ↓
                         status reporter
```

The starter samples a synthetic sensor, evaluates local safety rules, and reports
status independently. It never touches GPIO and needs no Wi-Fi password or server.

## Choose your path

| You want to… | Start here |
| --- | --- |
| Build and run your first firmware | [Quick start](getting-started/build-and-run.md) |
| Understand dependency injection | [Architecture](architecture.md) |
| Replace the synthetic sensor | [Sensor tutorial](tutorials/replace-sensor.md) |
| Explore timed output control | [Actuator example](examples.md#actuator-node) |
| Plan a cluster or swarm | [Distributed evolution](distributed-systems.md) |

## What works today

Status: **pre-alpha**, with hardware acceptance testing still pending.

- ESP32-S3 builds with the pinned ESP-IDF 5.4.2 baseline.
- Constructor-injected workers and explicit output states.
- Bounded latest-value queues, a rejecting command FIFO, and independent task scheduling.
- Host tests, a Unity test application, and three standalone example projects.
- Optional [SHT3x, Wi-Fi, and MQTT](hardware-mqtt.md) with versioned NVS credentials.
  A failed network start can be released and retried without rebooting local control.
- English documentation, formatting checks, and configurable release automation.

## What comes next

Physical actuator drivers, MQTT command subscription, secure provisioning, and OTA
execution remain [roadmap items](roadmap.md). Stored network settings migrate in
memory across schema versions and are not rewritten in NVS. Local command admission
is implemented and defaults to an idle, remote-rejected ingress. A portable helper
can confirm applied OFF before a disruptive restart; boot does not call it. OTA
partitions are reserved in the root firmware, but an updater is not implemented. Cluster coordination and robot
motion algorithms are not included.

Read [Safety](safety.md) before adding real outputs. A successful build does not
mean a board or connected actuator has been validated.
