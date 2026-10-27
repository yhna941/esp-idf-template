# Cluster and swarm evolution

Status: architectural direction, not an implemented distributed runtime.

## What this foundation supports

A node owns its control policy and actuator. Sensor, request, output, clock,
transport, and reporting implementations can evolve independently. Local work
can execute without a server. `CommandIngress` validates a command before
`RequestSource` exposes a boolean request to `TimedOutput`. The ingress is not
a protocol or proof of distributed safety. MQTT command subscription is not
implemented. `decode_command_envelope` maps one schema 1 payload onto `submit`
and does not write the output.

For richer robots, introduce product-specific typed snapshots and actions instead
of encoding all sensors into one float or all motor commands into one boolean.
The generic snapshot ports can carry those types without changing FreeRTOS
transport mechanics. Define actual data rates and timing budgets before generalizing.

## Add in response to a concrete application

1. Define node identity, capabilities, message schema/version, and transport limits.
2. Separate transport adapters from application coordination rules. MQTT can serve
   telemetry and supervisory commands; do not assume it meets motion-loop deadlines.
3. Add command envelopes with sender, sequence/ID, validity, and bounded payload.
   Define duplicate, reorder, reconnect, reboot, and offline behavior explicitly.
4. Use locally enforced command leases and stale-peer handling. A coordinator
   disconnect must not leave an actuator running indefinitely.
5. If roles or leader election are needed, specify epochs/fencing and split-brain
   behavior. A generic leader manager is not part of the current foundation.
6. Model membership, packet loss, delay, clock drift, partitions, and recovery in
   host simulations, then verify timing and collision avoidance on hardware.

## Safety boundary

Peer or coordinator commands never write outputs directly. They pass through
local validation, interlocks, and runtime limits. Each actuator retains one owner.
Do not compare uptime values from different MCUs as if their monotonic clocks
shared an origin. Clock synchronization and relative-age bounds require explicit
protocol design. Emergency stop paths must not wait for a congested normal command
queue or depend solely on a network coordinator.

Collision avoidance, localization, synchronized motion, consensus, and formation
control are product capabilities still to be designed and tested. This template
provides replaceable boundaries, not those algorithms or their timing guarantees.
