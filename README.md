# BC250 ESP32 Power Switch

An ESP32 power controller for an AMD **BC250** board running as a desktop. The
BC250 is fed from a PCI-E connector and has no ATX power button, so this firmware
drives the SFX PSU's `PS_ON#` line and senses board power, giving you a real power
button — plus optional "turn on when I pick up my controller" via Bluetooth.

## Features

- **Push-button power**: tap to turn on; hold 5 s while running to force off.
- **Follows the board**: if the OS shuts the board down, the PSU is cut automatically.
- **Boot watchdog**: if the board doesn't come up within 10 s, the PSU is released.
- **BLE controller wake** (optional): when any of up to 5 bound controllers (e.g. an
  8BitDo or Xbox controller) powers on, the machine powers on with it.
- **Web UI**: always on at `http://bc250-switch.local` on your home WiFi — live power
  state, power on / force off, controller management with nicknames, WiFi settings and
  an event log. Falls back to its own network if the router is unreachable.
- **LED bar** (optional): drives a WS2812 strip that mirrors Steam's Game Mode LED
  settings from the PC (WLED-compatible, for
  [steamos-led-wled](https://github.com/rpf16rj/steamos-led-wled)), with boot and
  standby lighting that follows the power state, and status lights for what the switch
  itself is doing (button holds, controller wake, boot progress, shutdown, failures).
- **Configurable pins**: every GPIO can be moved from the web UI.

## Board targets

The default PlatformIO environment now targets an **ESP32-WROOM-32** module
(`esp32-wroom-32` / `board = esp32dev`). The original **ESP32-C3-DevKitM-1**
mapping is still available as a legacy environment so existing hardware keeps
working.

| Environment | Module | Serial | Notes |
|-------------|--------|--------|-------|
| `esp32-wroom-32` | ESP32-WROOM-32 (`esp32dev`) | UART0 at 115200 | Default target, 4 MB flash, no PSRAM required |
| `esp32-c3-devkitm-1` | ESP32-C3-DevKitM-1 | USB-CDC at 115200 | Legacy mapping retained for backward compatibility |
| `esp32-c3-uart` | ESP32-C3 boards with a USB-UART bridge (CH340/CP2102) | UART0 at 115200 | Same pins as `esp32-c3-devkitm-1` |

## Wiring

The ESP32 module is permanently powered from the ATX connector's **5 V standby**,
so it runs whether the machine is on or off. Share a common ground between the ESP,
the PSU, and the board.

The tables below are the default pins. Each one can be moved to another GPIO from the
web UI's **Pins** card (see [Web UI](#web-ui)).

### ESP32-WROOM-32 wiring (default)

| ESP32-WROOM-32 | Connects to | Notes |
|----------------|-------------|-------|
| GPIO33 | Momentary switch, terminal A | Read with internal pull-up |
| GPIO25 | Momentary switch, terminal B | Driven LOW as the switch's ground |
| GPIO27 | ATX `PS_ON#` (green wire) | **Open-drain**, active LOW: LOW = PSU on, released = off |
| GPIO34 | BC250 `TPMS1` (pin 9) | ADC1 input; ~3.3 V when the board is up, 0 when off |
| GPIO26 | LED strip `DIN` (optional) | See [LED bar](#led-bar) |
| 5VSB / GND | PSU standby + common ground | Permanent power for the ESP |

### ESP32-C3-DevKitM-1 wiring (legacy)

| ESP32-C3 | Connects to | Notes |
|----------|-------------|-------|
| GPIO5 | Momentary switch, terminal A | Read with internal pull-up |
| GPIO6 | Momentary switch, terminal B | Driven LOW as the switch's ground |
| GPIO4 | ATX `PS_ON#` (green wire) | **Open-drain**, active LOW: LOW = PSU on, released = off |
| GPIO3 | BC250 `TPMS1` (pin 9) | ~3.3 V when the board is up, 0 when off |
| GPIO7 | LED strip `DIN` (optional) | See [LED bar](#led-bar) |
| 5VSB / GND | PSU standby + common ground | Permanent power for the ESP |

`PS_ON#` idles at ~5 V (pulled up inside the PSU). The ESP pin is driven open-drain so
the 3.3 V part never fights the 5 V rail — it only ever sinks to ground to switch the
PSU on.

`TPMS1` is a higher-impedance signal that hovers near the logic threshold, so it's read
as an analog voltage with hysteresis rather than a digital pin. On the original ESP32,
keep it on an **ADC1** pin: the setup portal uses WiFi and BLE together, and ADC2 is
not reliable while WiFi is active.

### Connector pinouts

**ATX 24-pin main connector** — tap three pins:

```
               +3.3V ─┤  1 │ 13 ├─ +3.3V
               +3.3V ─┤  2 │ 14 ├─ −12V
                 GND ─┤  3 │ 15 ├─ GND
                 +5V ─┤  4 │ 16 ├─ PS_ON#   ◄── connect to `PS_ON_PIN`
                 GND ─┤  5 │ 17 ├─ GND      ◄── ESP GND (any GND pin works)
                 +5V ─┤  6 │ 18 ├─ GND
                 GND ─┤  7 │ 19 ├─ GND
              PWR_OK ─┤  8 │ 20 ├─ (RSVD)
ESP 5V/VIN ◄── +5VSB ─┤  9 │ 21 ├─ +5V
                +12V ─┤ 10 │ 22 ├─ +5V
                +12V ─┤ 11 │ 23 ├─ +5V
               +3.3V ─┤ 12 │ 24 ├─ GND
```

**TPMS1 header** — single pin for board-power sense:

```
   PCICLK ─┤  1   2 ├─ GND
    FRAME ─┤  3   4 ├─ SMB_CLK_MAIN
  PCIRST# ─┤  5   6 ├─ SMB_DATA_MAIN
     LAD3 ─┤  7   8 ├─ LAD2
       3V ─┤  9  10 ├─ LAD1      ◄── pin 9 (3V) = board-on sense ──► `BOARD_SENSE`
     LAD0 ─┤ 11  12 ├─ GND
          ─┤     14 ├─ S_PWRDWN#
     3VSB ─┤ 15  16 ├─ SERIRQ#
      GND ─┤ 17  18 ├─ GND
```

Pin 9 is the only TPMS1 pin used: it reads ~3.3 V when the board is powered and 0 V when
off. No ground wire is needed from this header — the ESP already shares ground with the
board through the ATX connector. Use the board-specific wiring table above to map
`PS_ON_PIN` and `BOARD_SENSE` to actual GPIO numbers.

## Button controls

| Action | Result |
|--------|--------|
| Tap while **off** | Power on |
| Hold ≥ 5 s while **on** | Force power off |
| Hold ≥ 8 s while **off** | Enter WiFi setup portal |
| Release, then hold ≥ 8 s in **setup mode** | Leave the portal without changes |

The button is the primary control and always works, even with no controller configured.

## Bluetooth controller wake

Up to 5 controllers can be bound from the web UI, each with an optional nickname. The
machine **follows the controllers**: turn any bound controller on and the machine powers
up. After a power-off there's a 15 s guard window so the controller's reconnect burst
can't immediately switch it back on — turn the controller off within that window to keep
the machine down.

Detection uses the controller's Bluetooth LE advertisements, so the controller has to be
in Bluetooth mode (an Xbox controller in dongle mode is invisible to the ESP32). A newly
bound controller only starts waking the machine after it has been switched off once, so
binding a controller that is on doesn't power the machine up.

## Web UI

In normal operation the switch joins your router and serves the web UI at
`http://bc250-switch.local` (or its IP address, shown in the serial log and the UI):

- **Power**: live state, board-sense voltage and PSU state. *Power on* when off; *Force
  off* when on, which needs a second tap because it cuts PSU power like pulling the plug.
  There's no wire to the BC250's power button, so shut down from the OS when you can —
  the switch follows the board down.
- **Controllers**: bound controllers with presence, rename and remove; *Add a
  controller* runs a 30 s Bluetooth scan to pick from, or takes a MAC address.
- **WiFi**: the router to join (2.4 GHz only). Saving reconnects right away.
- **Password** change, and the last 40 events.
- **Pins**: which GPIO does each job. Only pins that are safe for the job on that chip are
  offered: no flash, strapping or serial pins, and an ADC1 pin for `TPMS1`. The button's
  second terminal can also go straight to GND. Saving restarts the switch, so it's only
  allowed while the machine is off (a restart releases `PS_ON#`). Move the wires with the
  PSU unplugged first: a wrong `PS_ON#` or `TPMS1` pin can switch the PSU unexpectedly.

Everything needs the password. Sessions last until you sign out, 7 days of inactivity,
or a restart of the switch.

**Fallback network**: if no router is set, or it stays unreachable for 20 s, the switch
brings up the open network **`BC250 Switch Setup`** (UI at `http://192.168.4.1`) and
keeps retrying the router every minute. The fallback network goes away once the router
is back and nobody is connected to it.

## LED bar

The switch can drive a WS2812 / NeoPixel strip (up to 64 LEDs) that shows Steam's Game
Mode LED settings, like the Steam Machine's light bar. The PC side is
[steamos-led-wled](https://github.com/rpf16rj/steamos-led-wled): it reads the LED state
from Steam and sends frames over the network with the WLED realtime UDP protocol. The
switch answers as a WLED device, so no separate LED controller is needed.

**Wiring**: strip `DIN` to the LED pin (GPIO26 on the WROOM, GPIO7 on the C3), strip
`GND` to common ground, strip `5V` to the PSU's 5 V standby so it can light while the
machine is off. The ESP's 3.3 V data usually works with a short data wire; if the strip
flickers or shows wrong colours, put a 74AHCT125 level shifter in the data line.
Budget up to 60 mA per LED at full white against the standby rail; the brightness cap
in the UI limits that.

**Set up**:

1. In the web UI, open **LED bar**, tick *Drive the LED bar*, set the LED count and save.
   *Light up for 5 s* checks the wiring and colour order.
2. On the BC250, install steamos-led-wled. Give the installer the switch's IP address
   (shown on the LED bar page; reserve it in your router), UDP port `21324` and the LED
   count. Answer **no** to the WLED boot preset: the switch has its own boot animation.

**What it shows**, in priority order: a test colour; dark, if the PC switched the bar
off (until it switches it back on or the machine powers on or off); the PC's frames,
while they keep coming (each packet holds for its timeout); otherwise a local scene —
off or a standby glow while the machine is off, a Steam-blue boot animation while it
starts until the PC takes over, and off, a colour or a rainbow while it runs without
the PC sending. Changes crossfade.

**Status lights** show what the switch itself is doing, over the PC's frames and the
scenes. Each can be turned off or recoloured on the LED bar page:

| Light | When |
|-------|------|
| Controller wake | A sweep across the bar as a controller turns the machine on |
| Starting | Fills while waiting for the board to come up, then flashes full |
| Power off | Fades out once the machine is off, so you know it's down |
| Boot failed | Slow red pulses for a minute when the board never came up |
| Force-off hold | Fills while the button is held, forcing the machine off when full |
| Setup mode | Fills while the button is held for setup mode; a sweeping dot while in it |
| Looking for controllers | Breathes while a controller scan from the web UI runs |
| WiFi lost | The end LEDs pulse while the router can't be reached (over the local scenes only) |

Holds only show once they're longer than a tap. While the machine is off (or in setup
mode) the bar is capped at a separate, lower **standby brightness**.

The WLED part covers the realtime UDP protocols (WARLS, DRGB, DRGBW, DNRGB) and the
HTTP calls the bridge makes (`/json/info`, `/json/state`, `/win&T=`). These are open on
the network like WLED's, but only reach the LED bar, and are off unless the bar is
enabled with *Let the PC control the bar over the network* ticked.

## Setup mode

The first password can only be created in setup mode, which needs physical access: hold
the button ≥ 8 s while off. It never starts on its own, even on first use — without a
password the button still works as a plain power switch.

1. Connect to the open WiFi network **`BC250 Switch Setup`** and open `http://192.168.4.1`.
2. Create a password (or sign in, if one is set).
3. Optionally bind controllers and set the router now; both can be done later.
4. Finish — the device reboots into normal operation.

The PSU stays off in setup mode. To leave without finishing, release the button and hold
it ≥ 8 s again. The hold that entered setup mode doesn't count, however long you keep it
down.

## Build & flash

PlatformIO (pioarduino). Two steps — firmware and the web UI (a single
`app/index.html` packed into SPIFFS):

```bash
pio run -e esp32-wroom-32 -t upload     # default WROOM-32 firmware
pio run -e esp32-wroom-32 -t uploadfs   # WROOM-32 web UI filesystem
```

To build or flash the retained ESP32-C3 target instead:

```bash
pio run -e esp32-c3-devkitm-1 -t upload
pio run -e esp32-c3-devkitm-1 -t uploadfs
```

If the C3 board connects through a CH340/CP2102 chip (it shows up as
`/dev/ttyUSB*` rather than `/dev/ttyACM*`), use `esp32-c3-uart` instead so the
serial log appears on that port.

## Notes

- **Bootstrapping pins**: on ESP32-WROOM-32, avoid moving the external wiring to
  strap pins (`GPIO0`, `GPIO2`, `GPIO4`, `GPIO5`, `GPIO12`, `GPIO15`) or to the
  UART0 pins (`GPIO1`, `GPIO3`) if you rely on serial flashing/logging.
- **Flash / PSRAM**: the project uses a custom 4 MB flash layout in
  [partitions.csv](partitions.csv) and does **not** require PSRAM.
- **WiFi TX power**: the retained ESP32-C3 *mini* profile still uses the
  reduced-power SoftAP workaround from
  [arduino-esp32 #6551](https://github.com/espressif/arduino-esp32/issues/6551).
  The ESP32-WROOM-32 profile keeps the normal full-power setting.
- Serial debug runs at **115200** baud — USB-CDC on the `esp32-c3-devkitm-1` profile,
  UART0 on `esp32-c3-uart` and ESP32-WROOM-32 boards.
- Pin assignments and all timing constants live in [include/board.h](include/board.h).
- **Don't restart or flash the ESP32 while the machine is on.** While it resets, `PS_ON#`
  is released and the PSU cuts power. On boards with a USB auto-reset circuit, opening
  the serial port also resets the ESP32.
- The web UI is plain HTTP on your LAN. Don't expose port 80 to the internet.
