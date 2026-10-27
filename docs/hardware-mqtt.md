# First hardware and MQTT slice

Status: pre-alpha. Code and compile coverage are available; physical board and
Wi-Fi/broker execution still require the hardware acceptance checks below.

## What is implemented

- Optional SHT30/31/35-DIS driver over the ESP-IDF new I2C master API.
- CRC checks for both temperature and humidity words; the first data stream exports temperature in Celsius.
- Explicit units, measurement timestamps, and synthetic/physical data origin.
- Wi-Fi station retries with bounded exponential backoff and jitter.
- MQTT telemetry and retained availability with TLS certificate-bundle verification.
- Runtime credentials loaded from a versioned `espkit` NVS namespace.

The default firmware still uses synthetic input, simulated output, and no network.
No GPIO actuator, MQTT command subscription, irrigation automation, or OTA download
is implemented. A schema 1 decoder can map one reassembled payload onto the
command ingress. The default firmware does not call it. Remote commands stay
rejected unless `ESPKIT_REMOTE_COMMANDS` is enabled, and that option does not
open a broker connection. The SHT3x measures air temperature/humidity, not soil
moisture.

## Build a hardware-enabled configuration

From the repository root, in ESP-IDF 5.4.2:

```sh
idf.py -B build-hardware \
  -D SDKCONFIG="$PWD/build-hardware/sdkconfig" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;config/hardware.defaults' \
  -D IDF_TARGET=esp32s3 build
idf.py -B build-hardware menuconfig
```

Under **ESPKit board and sensor**, select SDA/SCL GPIOs that match your actual PCB
or development board. Defaults are -1, which prevents I2C initialization. Review
pins reserved for flash, PSRAM, USB, boot strapping, or onboard devices. No universal
pin map is assumed. The code currently gates this sensor option to ESP32-S3.
Use a new build directory when changing host/container or SDK environments.

## Wire the sensor

Verify the exact module and its supply requirements. ESP32 GPIO signals use 3.3 V
logic; ensure module pull-ups do too. Connect common ground, SDA, and SCL. External
pull-ups are required; internal pull-ups are not enabled by this adapter. SHT3x
address is 0x44 by default, with 0x45 selectable. Bus speed defaults to 100 kHz,
I/O waits to 50 ms, and conversion delay to at least 20 ms.

The protocol follows the [manufacturer datasheet](https://sensirion.com/media/documents/213E6A3B/63A5A569/Datasheet_SHT3x_DIS.pdf).
Read failures and invalid CRC produce invalid readings; they never reuse a previous
value as a fresh successful measurement. Sensor waits happen in the sensor worker.

## Provision runtime credentials

Create a local configuration, fill it in privately, and generate an NVS image:

```sh
mkdir -p local
cp config/device.example.json local/device.json
chmod 600 local/device.json
python tools/provision_device.py --config local/device.json
```

The generation tool is intended for a Linux/macOS SDK shell. `local/` is ignored
by Git. The CSV and generated image contain credentials in plaintext and are
written with mode 0600. They must never be uploaded as artifacts or committed.
Flash encryption, encrypted NVS, and secure factory provisioning remain future work.

The tool uses the root partition layout: NVS offset 0x9000, size 0x6000. To flash,
explicitly acknowledge replacing **all** contents of that NVS partition:

```sh
python tools/provision_device.py --config local/device.json \
  --flash --replace-nvs --port /dev/ttyACM0
idf.py -B build-hardware -p /dev/ttyACM0 flash monitor
```

Do not use this offset on a different partition layout. Existing NVS settings are
replaced, not merged. The application never automatically erases NVS to recover
from an initialization failure, and loading settings does not rewrite the namespace.

The durable layout is schema 1 in the `espkit` namespace: `schema` as a `u8`,
required strings `wifi_ssid`, `wifi_pass`, `mqtt_uri`, `device_id`, and `site_id`,
plus optional `mqtt_user` and `mqtt_pass`. A record with no `schema` key is the
unversioned layout. This firmware keeps it when those schema 1 fields validate,
and applies that upgrade only in memory. A schema newer than this firmware is
used only when the same fields still validate. Unknown keys are left untouched.
Corrupt, partial, and out-of-range records are ignored, so networking stays off
and local control continues. Values are not shortened to force them in range.
See [ADR 0006](decisions/0006-versioned-settings.md). On rejection the firmware
logs `rejected NVS settings problem=<n> schema=<version>` (`1` corrupt, `2`
partial, `3` out of range, `4` incompatible). An in-memory upgrade or a newer
schema logs `loaded NVS settings outcome=<n> schema=<version>` (`1` migrated,
`2` compatible with a newer schema). The log does not include stored values.

## Network behavior

Enable **ESPKit networking** in menuconfig. Credentials must specify a WPA2/WPA3
network password of 8-63 bytes. Broker credentials are separate fields; URI-embedded
credentials are rejected. Device/site IDs contain only letters, digits, '-' and '_'.

Default transport is `mqtts://`. A trusted CA and valid hostname are required.
TLS startup waits for a plausible wall clock, synchronized through SNTP using
`pool.ntp.org`. Clock synchronization is operational time setup, not a trusted-time
attestation. Local control deadlines always use the monotonic clock.
Custom private-CA configuration is not yet exposed by this first adapter.

For an isolated development broker, explicitly enable **Allow mqtt://** and pass
`--allow-plaintext` to the provisioning tool. This sends credentials/data without
TLS. Do not use it as a production default. Local development broker configuration
is under `tools/mqtt/` and binds to localhost unless explicitly changed.

Telemetry uses `iot/v1/{site_id}/{device_id}/telemetry`, QoS 0, no offline backlog,
and no retain. Availability uses a retained online message and an offline last will.
MQTT's reconnect interval has per-device startup jitter; Wi-Fi uses exponential
backoff with jitter. MQTT outbox storage is limited to 4096 bytes.

A failed station or MQTT `start` releases the resources that attempt acquired and
retries without rebooting. A wall clock that never becomes valid for TLS does
the same after a bounded SNTP wait: the session is stopped and begun again, then
the stack is released. A release the SDK does not finish stays recorded so the
next stop can retry it. Stopping the network does not stop sensor or control
tasks and does not change the output. The TCP/IP stack and the default event loop
stay initialized across that restart. See
[ADR 0007](decisions/0007-service-lifecycle.md). Hardware was not used to confirm
the cleanup.

Only the reporter calls the MQTT sink. Network initialization/retries run in a
separate task. While offline, reports are dropped and counted; control and sensor
snapshots continue. No remote message can directly enable an output.

Example sensor fields:

```json
{"value":25.5,"unit":"celsius","origin":"physical","valid":true,"sampled_at_ms":1000}
```

Invalid numbers are encoded as JSON null. `valid` describes the measurement;
use timestamps and the fault state when evaluating freshness. The device uptime
and sampled-at timestamp share a local clock, not a global clock across devices.

## Hardware acceptance checklist

- [ ] Record board/module, sensor model, wiring, flash size, and commit.
- [ ] Confirm plausible temperature and CRC-valid measurements.
- [ ] Disconnect the sensor; verify invalid readings and a latched local fault.
- [ ] Connect Wi-Fi and observe telemetry/availability at the broker.
- [ ] Stop the broker and Wi-Fi separately; observe local task operation and reconnects.
- [ ] Verify invalid credentials do not prevent local startup or erase stored settings.
- [ ] Reboot and verify NVS restore; measure task stack margins and scheduling delays.

These checks are not replaced by compilation, host tests, or a simulated broker.
Physical pump/motor output remains disabled until its own board-specific safety work.
