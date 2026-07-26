# esp32-mqtt-controlled-relay-board

Firmware for an ESP32 driving a six-channel relay board over MQTT. Each channel
is switched by its own topic, reports its state back retained, and survives a
reboot with the state it had before.

Built for always-on use: it never blocks at startup, reconnects on its own after
a network or broker outage, and does not write to flash unless a relay actually
changed.

---

## Hardware

An ESP32 dev board and a six-channel relay module. The relay inputs are
**active LOW** — the GPIO is driven LOW to energise a channel.

| Channel | GPIO |
|---|---|
| Device 1 | 4 |
| Device 2 | **23** |
| Device 3 | 18 |
| Device 4 | 19 |
| Device 5 | 21 |
| Device 6 | 22 |

> **Rewiring note.** Device 2 is on **GPIO 23**, not GPIO 5. GPIO 5 is an ESP32
> strapping pin: it is sampled at reset and emits a PWM burst during boot, so a
> relay module that loads or pulls it down can stop the board starting. If you
> are updating an existing build, move Device 2's control wire from GPIO 5 to
> GPIO 23.

All six channels are parked OFF before their pins are switched to outputs, so
the loads do not pulse on at boot, brownout or watchdog reset.

---

## MQTT interface

### Commands (subscribe)

| Topic | Purpose |
|---|---|
| `relay/device1` … `relay/device6` | Switch that channel |

Subscribed at QoS 1. Accepted payloads, case-insensitive with surrounding
whitespace ignored:

| On | Off |
|---|---|
| `on`, `1`, `true` | `off`, `0`, `false` |

Anything else is ignored and logged rather than acted on. In particular:

- **Empty payloads are never a command.** Brokers and tools publish an empty
  retained message to delete or clear a topic; that must not switch a relay.
- **Payloads longer than 15 bytes are rejected**, not truncated — truncating
  `onwards` down to `on` would actuate a channel nobody asked for.
- **Retained commands are ignored.** A retained command is redelivered on every
  reconnect, which would re-actuate relays from a stale message. Commands are
  treated as events; the state to restore at boot comes from NVS instead.
  Inbound commands are therefore dropped for 1.5 s after subscribing, which is
  when the broker delivers its retained backlog.

### State and liveness (publish)

| Topic | Payload | Retained |
|---|---|---|
| `relay/device1/state` … `relay/device6/state` | `on` / `off` | yes |
| `relay/status` | `1` online, `0` offline | yes |
| `relay/version` | firmware version | yes |

`relay/status` is also registered as the MQTT Last Will (QoS 1, retained), so
the broker publishes `0` if the board drops off without disconnecting cleanly.
That lets a subscriber tell "all relays off" apart from "board is gone".

Status, version and a full snapshot of all six relay states are re-published on
every connect, so subscribers resync after any outage. Subscriptions are
established *before* status is announced — a session that cannot receive
commands never advertises itself as up.

---

## Configuration

All configuration is at the top of `esp32_relay_mqtt/esp32_relay_mqtt.ino`.

| Setting | Meaning |
|---|---|
| `ssid` / `password` | Wi-Fi credentials |
| `mqttServer` | Broker hostname or IP |
| `mqttUser` / `mqttPassword` | Broker credentials |
| `MQTT_USE_TLS` | `0` plaintext (default), `1` TLS |
| `MQTT_CA_CERT` | Broker CA certificate, required when TLS is on |
| `saveState` | Persist relay states across reboots (default `true`) |

### TLS

`MQTT_USE_TLS` ships as `0` for compatibility with an existing cleartext
broker. **Plain MQTT sends your broker username and password, and every relay
command, in the clear** — readable and injectable by anyone on the network path.
Turn TLS on once your broker supports it:

1. Set `#define MQTT_USE_TLS 1`.
2. Paste the CA that signed your broker's certificate into `MQTT_CA_CERT`
   (for a self-signed broker, its own certificate).
3. Delete the `MQTT_CA_CERT_IS_PLACEHOLDER` line directly below it.

The port follows the toggle automatically — 1883 without TLS, 8883 with — so
the "TLS enabled but still pointed at the cleartext port" failure cannot happen.
The broker is verified against `MQTT_CA_CERT` with no unverified fallback, and
enabling TLS without supplying a certificate is a **compile error**, not a
silent handshake failure at runtime.

### Keeping credentials out of git

The sketch holds credentials as literals. `.gitignore` already excludes
`secrets.h` and `arduino_secrets.h`, so you can move them into one of those and
`#include` it if you would rather not have them in a tracked file.

---

## Connection behaviour

| Parameter | Value |
|---|---|
| MQTT keep-alive | 45 s |
| Reconnect backoff | 1 s, doubling to a 20 s cap |
| Backoff after auth rejection | 60 s |
| Wi-Fi retry interval | 10 s |
| Retained-command settle window | 1.5 s |

Startup never blocks. If the access point or broker is unreachable at power-on,
the board still reaches its main loop and keeps retrying; the relays hold their
restored state throughout. An authentication rejection backs off to 60 s rather
than retrying every second, so a retry storm cannot keep a fail2ban-style broker
lockout alive.

The MQTT client ID is derived from the board's MAC address
(`esp32-relay-XXXXXX`) and logged on every connect. A broker permits one
connection per client ID and evicts the existing session when a second client
presents the same one, so a shared literal would let a second board — or anyone
who can reach the broker — disconnect this device at will.

Disconnect causes are logged by name (`bad username or password`,
`broker unavailable`, `connection lost`, …) rather than as a bare numeric code.

---

## State persistence

Relay states are stored in NVS via the `Preferences` library, namespace
`relay`, keys `d1`–`d6`. A write happens **only when a relay actually changes
state**, and NVS wear-levels internally.

This replaces the earlier raw EEPROM emulation, whose `commit()` erased and
rewrote a full 4 KB flash sector on *every* inbound message — including
unmatched topics and invalid payloads. At roughly 100 000 erase cycles, a
publisher sending ten messages a second would have worn the sector out in a few
hours.

> **Upgrading:** existing EEPROM state is not migrated. All six channels come up
> off once after flashing this version, then persist normally.

---

## Building and flashing

Requires the `esp32` board package and the **PubSubClient** library
(`Preferences` ships with the ESP32 core). Verified against esp32 core 3.3.11
and PubSubClient 2.8.

With the Arduino IDE: install the ESP32 boards package, add PubSubClient via
Library Manager, open `esp32_relay_mqtt/esp32_relay_mqtt.ino`, select your ESP32
board and upload.

With `arduino-cli`:

```bash
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install PubSubClient

arduino-cli compile --fqbn esp32:esp32:esp32 esp32_relay_mqtt
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 esp32_relay_mqtt
```

Serial monitor runs at 115200 baud. The SSID is deliberately not logged.

---

## Usage

```bash
mosquitto_pub -h broker.local -u user -P pass -t relay/device1 -m on
mosquitto_pub -h broker.local -u user -P pass -t relay/device3 -m off

# Watch state and liveness
mosquitto_sub -h broker.local -u user -P pass -t 'relay/#' -v
```

---

## Known limitations

- **Topics are global.** `relay/deviceN` carries no per-board segment, so two
  boards on the same broker will respond to each other's commands. Anyone
  authorised on the broker can switch these relays — if they are driving mains
  loads, restrict the topics with broker ACLs.
- **A command published within 1.5 s of a reconnect is dropped**, a consequence
  of the retained-command settle window described above.
- **PubSubClient publishes at QoS 0.** Subscriptions and the Last Will use
  QoS 1, but outbound state publishes are fire-and-forget.
