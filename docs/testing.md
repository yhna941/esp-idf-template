# Testing

## Host

Run `bash tools/test-host.sh`. GoogleTest builds the production policy source with
C++20 and strict warnings, without linking ESP-IDF. Tests cover timeout boundaries,
non-extending requests, sensor validity/freshness, clock regression, invalid policy,
and fault reset. Worker tests inject alternate sensors, a fake clock, in-memory
snapshots, request sources, and failed outputs. They verify missing/stale input,
fault propagation, failed-stop unknown state, and reporting without ESP-IDF.
Command tests cover a full queue, invalid and expired commands, duplicates,
stale sequences, remote rejection, stop while the FIFO is full, and commands
that cannot override a `TimedOutput` fault.
Settings tests cover unversioned upgrade, schema 1, a newer schema, corrupt and
partial records, out-of-range values, and empty optional credentials. They call
`migrate_settings` directly and do not open NVS.
Network lifecycle tests cover start order, reverse-order stop, a repeated stop,
Wi-Fi and MQTT init failure, retry after the monotonic delay, a stalled SNTP
wait (separate from the wall-clock `ready()` rule), and control continuing
while the network is stopped. Resource-phase transitions, including a failed
MQTT or SNTP release, are tested without ESP-IDF. The Unity application does not run those cases: SDK teardown
needs a board, and the host tests already cover the portable contract.
Add domain-specific boundary tests when control rules grow.

## Target

A Unity policy smoke test lives in `components/domain/test/`, with a dedicated
application in `tests/target/`. Build and flash it using:

```sh
idf.py -C tests/target set-target esp32s3
idf.py -C tests/target build
idf.py -C tests/target -p /dev/ttyACM0 flash monitor
```

Select the domain test from the serial Unity menu. These tests require a
physical board to execute. Validate GPIO polarity, NVS, bus timeouts, RTOS timing,
stack high-water marks, and driver behavior on physical hardware. Use a protected
self-hosted runner for HIL. Never expose connected actuators to untrusted PR jobs.

## Integration

For the optional networking path, run a local test broker and inject disconnects, invalid
credentials, duplicate and expired commands, fragmented payloads, queue pressure,
and simultaneous reconnects. Observe control deadlines while the network is busy.

CI compiles default and hardware-enabled root firmware, the Unity application, and all three examples. It runs
host tests, firmware bundle integrity tests, release-rule checks, formatting, and
strict documentation builds. `python3 tools/check_architecture.py` checks owned
include and CMake dependency boundaries. It does not claim on-device validation.

## Packaging and release rules

```sh
python3 -m unittest discover -s tests/tools -v
npm ci --ignore-scripts
npm run check:release
```

The Python tests verify archive paths, binary checksums, reproducible bundles,
and rejection of path traversal/target mismatch. Node 24 and Git tests exercise real temporary histories: docs skip, feature/breaking
increments, dev numbering, stable promotion, new template history, artifact mismatches,
and checksum rejection. PR titles and planned firmware versions are checked as well.
Live GitHub publishing still requires repository access and activation settings.
