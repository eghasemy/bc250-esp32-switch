#pragma once

//*******  Pin definitions  ***************
//
// BC250 PSU controller wiring. The two supported board profiles keep the same
// external behavior but use different GPIOs because the classic ESP32 and the
// ESP32-C3 have different safe pin sets:
//
//   * ESP32-WROOM-32:
//       GPIO33 (BUTTON_SENSE)  <-----> momentary switch terminal A
//       GPIO25 (BUTTON_GND)    <-----> momentary switch terminal B
//       GPIO27 (PS_ON_PIN)     <-----> ATX PS_ON# (green wire, active LOW)
//       GPIO34 (BOARD_SENSE)   <-----> BC250 TPMS1 pin 9 (3.3V = board on)
//     BOARD_SENSE must stay on ADC1 because the setup portal uses WiFi/BLE and
//     ADC2 readings are not reliable while WiFi is active on the original ESP32.
//
//   * ESP32-C3-DevKitM-1 (legacy):
//       GPIO5  (BUTTON_SENSE)  <-----> momentary switch terminal A
//       GPIO6  (BUTTON_GND)    <-----> momentary switch terminal B
//       GPIO4  (PS_ON_PIN)     <-----> ATX PS_ON# (green wire, active LOW)
//       GPIO3  (BOARD_SENSE)   <-----> BC250 TPMS1 pin 9 (3.3V = board on)

#ifndef BC250_NATIVE_USB_SERIAL
#define BC250_NATIVE_USB_SERIAL 0
#endif

#if defined(BC250_BOARD_ESP32_WROOM_32)
const char *const BC250_BOARD_NAME = "ESP32-WROOM-32";
const int BUTTON_SENSE = 33;
const int BUTTON_GND   = 25;
const int PS_ON_PIN    = 27;
const int BOARD_SENSE  = 34;
#define AP_TX_POWER WIFI_POWER_19_5dBm
#elif defined(BC250_BOARD_ESP32_C3_DEVKITM_1)
const char *const BC250_BOARD_NAME = "ESP32-C3-DevKitM-1";
const int BUTTON_SENSE = 5;
const int BUTTON_GND   = 6;
const int PS_ON_PIN    = 4;
const int BOARD_SENSE  = 3;
#define AP_TX_POWER WIFI_POWER_8_5dBm
#else
#error "Unsupported BC250 board profile. Build one of the configured PlatformIO environments."
#endif

// The switch bridges BUTTON_SENSE and BUTTON_GND. BUTTON_GND is driven LOW to
// act as a local ground, and BUTTON_SENSE is read with an internal pull-up:
// pressed reads LOW.

// ATX PS_ON# is active LOW and idles at ~5V (pulled up inside the PSU).
// Driven as OPEN-DRAIN so we never push 3.3V against the PSU's 5V pull-up:
//   LOW  -> sink to GND -> PSU on
//   HIGH -> high-impedance -> PSU pull-up wins -> PSU off

// Hysteresis thresholds for the analog board-sense reading. The gap between
// them keeps a noisy signal sitting near the threshold from chattering:
//   reading rises above HIGH -> treat as "board up"
//   reading falls below LOW  -> treat as "board down"
//   in between               -> hold previous state
const int SENSE_HIGH_MV = 2000;
const int SENSE_LOW_MV  = 800;

// TPMS1 is high-impedance and the ESP32 single-shot ADC is noisy, so an isolated
// analogReadMilliVolts() can spike hundreds of mV above the true level. Once the
// board powers off the line floats near 0V but still throws the occasional spike
// past SENSE_HIGH_MV. A single such spike flips the hysteresis HIGH for one loop,
// which restarts the BOARD_OFF_DEBOUNCE_MS countdown -> shutdown detection stalls
// for an unbounded, random time. Averaging this many samples per reading is a
// low-pass that keeps a lone spike from ever crossing a threshold.
const int SENSE_OVERSAMPLE = 16;

//*******  Logic levels  ***************

const int PS_ON_ASSERT  = LOW;   // PSU on
const int PS_ON_RELEASE = HIGH;  // PSU off (open-drain -> high-Z)

//*******  Timing (milliseconds)  ***************

// Switch debounce window.
const unsigned long DEBOUNCE_MS = 30;

// Hold the button this long while the board is ON to force it off.
const unsigned long LONG_PRESS_MS = 5000;

// Hold the button this long while OFF to enter WiFi setup mode (reconfigure the
// bound controller / password). Longer than LONG_PRESS_MS and only armed for
// presses that begin while OFF, so it never collides with force-off.
const unsigned long SETUP_HOLD_MS = 8000;

// TPMS1 must stay LOW continuously for this long before we treat the board as
// having shut itself down. Filters out brief dips/transients during boot/reset.
const unsigned long BOARD_OFF_DEBOUNCE_MS = 1500;

// How long to wait for TPMS1 to go HIGH after asserting PS_ON#. If the board
// hasn't signalled UP by then we assume the boot failed, release the PSU and
// return to idle (OFF).
const unsigned long BOOT_TIMEOUT_MS = 10000;

// Periodic heartbeat log interval.
const unsigned long HEARTBEAT_MS = 1000;

//*******  WiFi setup portal  ***************

// SoftAP name shown when the device is in setup mode (open network).
const char *const AP_SSID = "BC250 Switch Setup";

// WiFi TX power for the SoftAP. The ESP32-C3 mini profile needs reduced power
// because of arduino-esp32 #6551; the ESP32-WROOM-32 profile keeps the normal
// full-power setting.

//*******  BLE wake  ***************

// The bound controller's BLE MAC is configured via the setup portal and stored
// in NVS (see config.h: config.wakeAddr). When the machine is OFF and that
// controller is advertising, we power on ("machine follows controller").

// The controller counts as "present" while it has been seen within this window.
// While OFF, presence => the machine powers on ("machine follows controller").
const unsigned long BLE_PRESENCE_TIMEOUT_MS = 4000;

// Guard window after any power-off during which BLE presence is ignored. This is
// your chance to also switch the controller off (it then goes absent and the
// machine stays down). If you leave the controller on, once this elapses the
// machine follows it back on. It also rides out the brief reconnect-advertising
// burst the controller emits when it loses its host at shutdown.
const unsigned long BLE_WAKE_COOLDOWN_MS = 15000;
