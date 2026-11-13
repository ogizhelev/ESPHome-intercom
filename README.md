# ESPHome Intercom (ESP8266)

A self‑contained ESPHome **external component** (`intercom`) that turns an ESP8266‑based
apartment intercom ("domofon"/"unifon") handset into a Home Assistant device. It detects
incoming calls on the building intercom bus, decodes the dialled apartment number, **opens the
door**, and runs an auto‑open + mute scheduler — all inside the component. The YAML only wires
the pins and the defaults; every control is auto‑created as a Home Assistant entity, editable
in the UI, and persisted across reboots.


## What it does
- **Ring detection** — `Doorbell (RING)` turns on for the whole bus‑activity (ring) burst, for
  **any** apartment, even one it can't decode.
- **Call decoding** (`basic`) — the dialled apartment is a **unary pulse count** (N short ~15 µs
  pulses before the first long >2 ms gap ⇒ id N). While a decoded ring is active, `Decoding
  successful` is on and `Decoder` shows the apartment id. All call indicators clear when the
  ring stops (or when the door is opened — see `clear_on_open`).
- **Open door** — reproduces the recovered, non‑blocking **hook‑flash** on the off‑hook line;
  there is no separate door relay. Triggered by the `Open door` button or auto‑open.
- **One scheduler** drives both **auto‑open** and **mute** — each with an enable, an hour
  window (`from_hour`→`until_hour`, wraps past midnight), and **per‑day Mon…Sun** switches.
- **Everything persists** — all numbers/switches restore across reboots (flash), with the YAML
  values as first‑boot defaults.

## Not verified
- `code_open` encoding — not captured; `Open with code` stays off.

## Install
```yaml
external_components:
  - source: github://ogizhelev/ESPHome-intercom@main
    components: [intercom]
    refresh: 0s        # always re-fetch @main; raise/remove once you pin a version

time:
  - platform: homeassistant     # or sntp (see below)
    id: homeassistant_time

intercom:
  id: intercom1
  bus_pin: GPIO5
  offhook_pin: GPIO13
  mute_pin: GPIO12
  ring_pin: GPIO14
  time_id: homeassistant_time
```
A complete, ready‑to‑flash config (WiFi, AP, captive portal, API, OTA, web server, time, and
optional diagnostics) is in [`example/intercom.yaml`](example/intercom.yaml).

## Configuration

### Pins & core
| Option | Req? | Default | Description |
|---|---|---|---|
| `id` | — | auto | Component id. |
| `bus_pin` | **yes** | — | Call‑decode input (the intercom bus; `basic` pulse‑count). |
| `offhook_pin` | **yes** | — | Off‑hook / door actuator (hook‑flash). |
| `mute_pin` | no | — | Mute output. |
| `ring_pin` | no | — | Hardware ring line. Used only as an **auto‑open trigger** on its rising edge; the `Doorbell (RING)` sensor is driven by bus activity, not this pin. |
| `time_id` | **yes** | — | `id` of the active `time:` platform (used by the scheduler). Must match the time platform you enable below. |

### Polarity / debug (all optional)
| Option | Default | Description |
|---|---|---|
| `offhook_inverted` | `false` | Door‑line polarity. `false` = active‑high (GPIO HIGH = off‑hook), hardware‑confirmed. `true` = active‑low wiring. Inverts both idle and active levels. |
| `mute_inverted` | `false` | Mute‑line polarity. Set `true` if the headset is muted when mute is *off* (active‑low mute). |
| `ring_inverted` | `false` | Polarity of the optional `ring_pin`. |
| `dump_raw` | `false` | Log the raw bus pulse train (signed µs) for calibration. |

### Ring timing (all optional)
| Option | Default | Description |
|---|---|---|
| `frame_gap` | `120ms` | Bus‑idle time that marks a ring as **stopped**. Raise it if a real ring briefly goes quiet mid‑burst and you don't want RING to drop. |
| `ring_min_hold` | `120ms` | Minimum time `Doorbell (RING)` stays on per burst (so a very short preamble‑only ring is still visible). |
| `clear_on_open` | `true` | When the door opens (button or auto‑open), immediately clear `Doorbell (RING)` / `Decoding successful` / `Open with code` / `Decoder`, and don't re‑raise them for that ring. `false` = clear only when the ring stops. |

### `auto_open:` (defaults for the auto‑created, UI‑editable, persisted entities)
| Key | Default | Range | Description |
|---|---|---|---|
| `mode` | `schedule` | off / on / schedule | Control mode (see below). |
| `on_ring` | `true` | bool | Open on any ring (while armed).|
| `open_apartment` | `0` | 0–255 | Open when a decoded call id equals this (0 = off). |
| `from_hour` | `8` | 0–23 | Window start (hour). |
| `until_hour` | `18` | 0–23 | Window end (hour, exclusive; wraps if `from > until`). |
| `days` | `[mon,tue,wed,thu,fri]` | mon…sun | Days the window applies. |

Auto‑open is **armed** when `mode` is `on`, or when `mode` is `schedule` and the current time is
inside the window on a selected day. While armed, a trigger opens the door: `on_ring` (any ring)
or a decoded id equal to `open_apartment`.

### `mute:` (same pattern, independent settings)
| Key | Default | Range | Description |
|---|---|---|---|
| `mode` | `schedule` | off / on / schedule | Control mode (see below). |
| `from_hour` | `22` | 0–23 | Mute window start. |
| `until_hour` | `7` | 0–23 | Mute window end (exclusive; wraps past midnight). |
| `days` | `[mon…sun]` | mon…sun | Days the mute window applies. |

Mute is effective when `mode` is `on`, or `mode` is `schedule` and the time is in the window.
(The hour window is half‑open `[from, until)`, so `from == until` means *never*.)

### Control mode (both features)
Each feature has a 3‑way **mode** (a select entity, default `Schedule`):
- **Off** — forced off (user override).
- **On** — forced on (user override).
- **Schedule** — follow the time window above.

`Off`/`On` are a **sticky** user override — they hold until you set the mode back to `Schedule`.
The YAML `mode` is just the first‑boot default; the live mode is the UI select (persisted).

## Auto‑created entities
- **button:** `Open door`
- **binary_sensor:** `Doorbell (RING)`, `Decoding successful`, `Open with code`, `Mute active`,
  `Auto open active`
- **sensor:** `Decoder` (apartment id; shown only while a call is active)
- **select** (persisted): `Mute`, `Auto open` — each `Off` / `On` / `Schedule`
- **number** (persisted): `Open apartment`, `Auto open from hour`, `Auto open until hour`,
  `Mute from hour`, `Mute until hour`
- **switch** (persisted): `Auto open on RING`, and per‑day `Auto open Mon…Sun` + `Mute Mon…Sun`
- **text_sensor:** `Mute source`, `Auto open source` — `UserOn` / `UserOff` / `Schedule`

**Mode vs active vs source.** The `Mute` / `Auto open` **selects** are the control (user override,
persisted). The read‑only `… active` binary_sensors show the **on/off** result right now; the
`… source` text_sensors show **which selector position is driving** it — `UserOn` (mode On),
`UserOff` (mode Off), or `Schedule` (mode Schedule) — independent of on/off. Changing the mode
re‑evaluates immediately (e.g. switching Mute to `Off` unmutes GPIO12 at once), plus a 60 s tick
for clock drift.

> UI values override the YAML defaults after the first change (stored in flash). To reset,
> change them in Home Assistant.

## Time source
The scheduler needs a valid clock.
- **Home Assistant** (default in the example): works once the device is connected to HA.
- **SNTP** (standalone, no HA): in the example, comment out the `homeassistant` platform,
  uncomment the `sntp` block, and set `intercom.time_id: sntp_time`.

If the clock is never valid, auto‑open/mute windows simply never trigger.

## Calibration & troubleshooting
1. **Decode:** set `dump_raw: true`, flash, watch the ESPHome/web log — `Dump Raw:` lines are the
   bus pulse train. For `basic`, the apartment id is the count of short (~15 µs) pulses before the
   first long (>2 ms) gap; usually no tuning needed.
2. **Door won't open:** confirm with the `Open door` button. If the firmware clearly commands open
   but nothing happens (or the line sits engaged), your off‑hook stage may be active‑low — set
   `offhook_inverted: true` and re‑flash.
3. **Headset muted when it shouldn't be:** set `mute_inverted: true` (active‑low mute line).
4. **RING blips too briefly:** raise `frame_gap` and/or `ring_min_hold`.
5. **Diagnostics:** the example enables `Uptime` / `WiFi` / `ESPHome Version` / `IP Address` /
   `Restart device`. Uncomment the `debug:` component + the `debug`‑platform sensors for
   heap / fragmentation / loop‑time / device‑info / reset‑reason.

## License
MIT — see [LICENSE](LICENSE).
