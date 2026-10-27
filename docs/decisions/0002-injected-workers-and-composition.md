# ADR 0002: Injected workers and composition-owned lifetime

Component placement and namespaces are refined by [ADR 0003](0003-layered-components.md); the constructor-injection and lifetime policy remains.

Status: accepted. The boot-once composition limit is superseded by
[ADR 0007](0007-service-lifecycle.md).

## Context

The first runtime created a concrete synthetic sensor inside its task function.
Portable sensor ports existed but production workers could not use alternate
implementations without editing execution code.

## Decision

Extract single-step sensor, control, and reporter workers into application code.
Inject portable ports by reference. Place concrete construction in a dedicated
composition component with device lifetime. FreeRTOS schedules generic Work objects.
Use explicit output states and error reporting; failed stop yields unknown output.
Group constants by owning component rather than a project-wide miscellaneous file.

## Consequences

Workers can be tested without ESP-IDF, and driver selection changes only composition.
The system has no runtime DI container or hidden service lookup. Borrowed dependency
lifetimes and bounded calls are mandatory. Sensor, control, and reporter tasks
stay boot-lifetime, and the default composition stays simulation-only. The
network stack is not boot-once; that limit is superseded by
[ADR 0007](0007-service-lifecycle.md). Distribution and swarm coordination remain
separate future work.
