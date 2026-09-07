# BC250 ESP32 Power Switch

An ESP32 power controller for an AMD **BC250** board running as a desktop. The
BC250 is fed from a PCI-E connector and has no ATX power button, so this firmware
drives the SFX PSU's `PS_ON#` line and senses board power, giving you a real power
button — plus optional "turn on when I pick up my controller" via Bluetooth.

## Features

- **Push-button power**: tap to turn on; hold 5 s while running to force off.
- **Follows the board**: if the OS shuts the board down, the PSU is cut automatically.
- **Boot watchdog**: if the board doesn't come up within 10 s, the PSU is released.
- **BLE controller wake** (optional): when a bound controller (e.g. an 8BitDo) powers
  on, the machine powers on with it.
- **WiFi setup portal**: configure the bound controller from a phone — no reflashing.

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

## Button controls

| Action | Result |
|--------|--------|
| Tap while **off** | Power on |
| Hold ≥ 5 s while **on** | Force power off |
| Hold ≥ 8 s while **off** | Enter WiFi setup portal |

The button is the primary control and always works, even with no controller configured.

## Bluetooth controller wake

When a controller is bound (via the portal), the machine **follows the controller**:
turn the controller on and the machine powers up. After a power-off there's a short
guard window so the controller's reconnect burst can't immediately switch it back on —
turn the controller off within that window to keep the machine down.

## Setup portal

Hold the button ≥ 8 s while off (or on first use) to start the portal:

1. Connect to the open WiFi network **`BC250 Switch Setup`** and open `http://192.168.4.1`.
2. Create a password.
3. Pick your controller from the live BLE scan (or enter its MAC).
4. Finish — the device reboots into normal operation.

## Build & flash

PlatformIO (pioarduino). Two steps — firmware and the portal's web UI (a single
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
