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

## Board targets

The default PlatformIO environment now targets an **ESP32-WROOM-32** module
(`esp32-wroom-32` / `board = esp32dev`). The original **ESP32-C3-DevKitM-1**
mapping is still available as a legacy environment so existing hardware keeps
working.

| Environment | Module | Serial | Notes |
|-------------|--------|--------|-------|
| `esp32-wroom-32` | ESP32-WROOM-32 (`esp32dev`) | UART0 at 115200 | Default target, 4 MB flash, no PSRAM required |
| `esp32-c3-devkitm-1` | ESP32-C3-DevKitM-1 | USB-CDC at 115200 | Legacy mapping retained for backward compatibility |

## Wiring

The ESP32 module is permanently powered from the ATX connector's **5 V standby**,
so it runs whether the machine is on or off. Share a common ground between the ESP,
the PSU, and the board.

### ESP32-WROOM-32 wiring (default)

| ESP32-WROOM-32 | Connects to | Notes |
|----------------|-------------|-------|
| GPIO33 | Momentary switch, terminal A | Read with internal pull-up |
| GPIO25 | Momentary switch, terminal B | Driven LOW as the switch's ground |
| GPIO27 | ATX `PS_ON#` (green wire) | **Open-drain**, active LOW: LOW = PSU on, released = off |
| GPIO34 | BC250 `TPMS1` (pin 9) | ADC1 input; ~3.3 V when the board is up, 0 when off |
| 5VSB / GND | PSU standby + common ground | Permanent power for the ESP |

### ESP32-C3-DevKitM-1 wiring (legacy)

| ESP32-C3 | Connects to | Notes |
|----------|-------------|-------|
| GPIO5 | Momentary switch, terminal A | Read with internal pull-up |
| GPIO6 | Momentary switch, terminal B | Driven LOW as the switch's ground |
| GPIO4 | ATX `PS_ON#` (green wire) | **Open-drain**, active LOW: LOW = PSU on, released = off |
| GPIO3 | BC250 `TPMS1` (pin 9) | ~3.3 V when the board is up, 0 when off |
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

### HP Common Slot server PSU (e.g. HSTNS-PL14)

HP Common Slot server PSUs such as the 460 W **HSTNS-PL14** (DPS-460EB) are 12 V-only,
which is all the BC250 needs. They have no 24-pin connector, no `PS_ON#` green wire
and no 5 V standby, so the wiring differs from ATX. The firmware is unchanged.

HP doesn't publish this pinout. The 33↔36 enable bridge was confirmed on an HSTNS-PL14;
the other pins come from community write-ups of the Common Slot family. Verify them with
a multimeter on your own unit before connecting anything.

| PSU pin | Signal | Connects to |
|---------|--------|-------------|
| 1–13 (large blades) | +12 V main | BC250 PCI-E power |
| 14–26 (large blades) | GND main | BC250 ground, ESP GND, buck converter GND |
| 37 | +12 V standby (on whenever mains is present) | 12 V→5 V buck converter input (ESP power) |
| 33 | Enable | Optocoupler output (collector), see below |
| 36 | Enable return | Optocoupler output (emitter), see below |

**Bridging pins 33 and 36 turns on the 12 V main output.** Opening the bridge turns
it off. The ESP makes that bridge through an optocoupler.

> **Never connect pin 36 to pin 37 (12 V standby).** Pin 36 is the enable return, not
> a pull-up input, and tying it to 12 V standby has destroyed a PSU.

**Powering the ESP**: there is no 5 V standby, so feed the ESP's 5V/VIN pin from a
small 12 V→5 V buck converter on pin 37. Don't use a linear regulator or resistor
from 12 V; it overheats once WiFi and BLE are active.

**Switching the PSU**: use an optocoupler such as a PC817, so the PSU's enable circuit
never connects to the ESP electrically:

```
ESP 3V3 ──[ 330 Ω ]──► LED anode (PC817 pin 1)
PS_ON_PIN ───────────► LED cathode (PC817 pin 2)
PSU pin 33 ──────────► collector (PC817 pin 4)
PSU pin 36 ──────────► emitter (PC817 pin 3)
```

The LED's cathode goes to `PS_ON_PIN`, which is open-drain and active LOW. Pulling it
LOW lights the LED, which closes the bridge, which turns the PSU on. Releasing it turns
the PSU off. That's the same logic the firmware already uses for `PS_ON#`, so no code
changes are needed. While the ESP resets or flashes, the pin floats, the LED is dark
and the PSU turns off, the same as with ATX.

The phototransistor has a polarity. With mains connected and the bridge open, measure
pin 33 against pin 36: the more positive pin goes to the collector. If pin 36 reads
higher, swap the two.

If you use a Common Slot breakout board with an on/off switch, connect the
optocoupler output across the switch terminals instead, and leave the switch off.

The button and the `BOARD_SENSE` wire to TPMS1 pin 9 stay as described above.

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

Everything needs the password. Sessions last until you sign out, 7 days of inactivity,
or a restart of the switch.

**Fallback network**: if no router is set, or it stays unreachable for 20 s, the switch
brings up the open network **`BC250 Switch Setup`** (UI at `http://192.168.4.1`) and
keeps retrying the router every minute. The fallback network goes away once the router
is back and nobody is connected to it.

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
- Serial debug runs at **115200** baud — USB-CDC on the ESP32-C3 profile, UART0 on
  ESP32-WROOM-32 boards.
- Pin assignments and all timing constants live in [include/board.h](include/board.h).
- **Don't restart or flash the ESP32 while the machine is on.** While it resets, `PS_ON#`
  is released and the PSU cuts power. On boards with a USB auto-reset circuit, opening
  the serial port also resets the ESP32.
- The web UI is plain HTTP on your LAN. Don't expose port 80 to the internet.
