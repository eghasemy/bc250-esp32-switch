#include <Arduino.h>
#include <WiFi.h>
#include "board.h"
#include "ble.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "power.h"
#include "web.h"

// The ESP32 module is permanently powered from the ATX connector (5VSB), so it
// runs independently of the PSU's main rails. It controls the SFX PSU through
// PS_ON# and watches the BC250's TPMS1 line to know whether the board is up.
//
// Control model
// -------------
//   * Asserting PS_ON# turns the PSU on; the BC250 is expected to power up from
//     applied power (BIOS "restore on AC power"), so PSU on == board boots.
//   * We have no wire to the board's power button, so "turning the board off"
//     means cutting PSU power via PS_ON#. This is a hard power-off, not a
//     graceful OS shutdown.
//   * If the board shuts itself down (e.g. OS shutdown), TPMS1 drops to 0 while
//     the PSU is still energized. We detect that and release PS_ON# so the PSU
//     follows the board down.
//   * A background BLE scan watches for the bound controllers advertising; when
//     one appears while we're OFF, that wakes the machine just like a button tap.
//   * The web UI (web.cpp) runs alongside on the router network or the fallback
//     AP, and can request power on / force off through power.h.

enum PowerState {
  STATE_OFF,      // PSU released, board down
  STATE_BOOTING,  // PSU asserted, waiting for TPMS1 to go HIGH
  STATE_ON,       // PSU asserted, board up (TPMS1 HIGH)
};

static PowerState state = STATE_OFF;
static bool       g_setupMode = false;

// --- Button debounce / press tracking ---
static bool          buttonStable      = false;  // debounced "pressed" level
static bool          buttonLastRaw     = false;
static unsigned long buttonLastChange  = 0;
static unsigned long pressStart        = 0;
static bool          pressStartedOff   = false;  // press began while OFF
static bool          longPressFired    = false;
static bool          setupFired        = false;

// --- Board-sense debounce ---
static bool          boardSenseStable  = false;  // debounced TPMS1 HIGH
static bool          boardSenseLastRaw = false;
static unsigned long boardSenseChange  = 0;

// --- BLE wake ---
static unsigned long bleInhibitUntil = 0;  // wakes ignored until this

// --- Web requests (set from the web server task, consumed by loop()) ---
static volatile bool powerOnRequested  = false;
static volatile bool forceOffRequested = false;

// --- Status snapshot for the web server task (written by loop()) ---
static volatile PowerState    pubState      = STATE_OFF;
static volatile bool          pubBoardUp    = false;
static volatile uint32_t      pubSenseMv    = 0;
static volatile bool          pubButtonDown = false;
static volatile unsigned long pubInhibitMs  = 0;

// --- Misc timers ---
static unsigned long bootStart    = 0;
static unsigned long lastHeartbeat = 0;

static const char *stateName(PowerState s) {
  switch (s) {
    case STATE_OFF:     return "OFF";
    case STATE_BOOTING: return "BOOTING";
    case STATE_ON:      return "ON";
  }
  return "?";
}

static void setState(PowerState next) {
  if (next == state) return;
  logEvent("[STATE] %s -> %s", stateName(state), stateName(next));
  state = next;
}

// Raw (pre-time-debounce) board-up level, derived from the TPMS1 voltage with
// hysteresis so a signal hovering near the logic threshold doesn't chatter.
static bool senseLevel = false;

static bool readBoardSense(uint32_t *outMv = nullptr) {
  // Take the lowest of several reads to reject upward ADC/line spikes. Without
  // this, a stray spike past SENSE_HIGH_MV chatters the hysteresis and restarts
  // the board-off debounce, stalling shutdown detection (see SENSE_OVERSAMPLE).
  uint32_t mv = UINT32_MAX;
  for (int i = 0; i < SENSE_OVERSAMPLE; i++) {
    mv = min(mv, analogReadMilliVolts(BOARD_SENSE));
  }
  if (outMv) *outMv = mv;
  if (senseLevel) {
    if (mv < SENSE_LOW_MV) senseLevel = false;
  } else {
    if (mv > SENSE_HIGH_MV) senseLevel = true;
  }
  return senseLevel;
}

static void psuOn() {
  digitalWrite(PS_ON_PIN, PS_ON_ASSERT);
  logEvent("[PSU ] PS_ON# asserted (LOW) -> PSU ON");
}

static void psuOff() {
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  logEvent("[PSU ] PS_ON# released (high-Z) -> PSU OFF");
}

// Shared power-on path, used by the button, the BLE wake and the web UI. Takes
// the loop's `now` rather than calling millis() itself: the BOOTING timeout
// compares against the `now` cached at the top of normalLoop(), and a fresh
// millis() here can land a millisecond past it. Since the comparison is
// unsigned, bootStart > now makes (now - bootStart) underflow to a huge value
// and trip the timeout on the very next loop. Sharing one clock per iteration
// keeps the math monotonic.
static void powerOn(const char *reason, unsigned long now) {
  logEvent("[ACT ] %s -> powering on", reason);
  psuOn();
  bootStart = now;
  setState(STATE_BOOTING);
}

// Shared power-off path. Starts the BLE-wake cooldown so the controller's
// post-shutdown reconnect burst can't immediately wake us again. Takes `now` for
// the same single-clock-per-loop reason as powerOn().
static void powerOff(const char *reason, unsigned long now) {
  logEvent("[ACT ] %s -> powering off", reason);
  psuOff();
  bleInhibitUntil = now + BLE_WAKE_COOLDOWN_MS;
  setState(STATE_OFF);
}

// Persist a setup request and reboot into the WiFi portal.
static void enterSetupMode(const char *reason) {
  logEvent("[ACT ] %s -> entering setup mode", reason);
  setForceSetup(true);
  delay(50);
  ESP.restart();
}

// Debounce a raw level into a stable level. Returns true if the stable value
// changed this call, writing the new value into *stable.
static bool debounce(bool raw, bool *stable, bool *lastRaw,
                     unsigned long *lastChange, unsigned long now,
                     unsigned long window) {
  if (raw != *lastRaw) {
    *lastRaw = raw;
    *lastChange = now;
  }
  if (raw != *stable && (now - *lastChange) >= window) {
    *stable = raw;
    return true;
  }
  return false;
}

// --- power.h ---

PowerStatus powerStatus() {
  PowerStatus s;
  PowerState st = pubState;
  s.state         = g_setupMode ? "SETUP" : stateName(st);
  s.psuOn         = !g_setupMode && st != STATE_OFF;
  s.boardUp       = pubBoardUp;
  s.senseMv       = pubSenseMv;
  s.buttonDown    = pubButtonDown;
  s.wakeInhibitMs = pubInhibitMs;
  return s;
}

bool requestPowerOn() {
  if (g_setupMode || pubState != STATE_OFF) return false;
  powerOnRequested = true;
  return true;
}

bool requestForceOff() {
  if (g_setupMode || pubState == STATE_OFF) return false;
  forceOffRequested = true;
  return true;
}

// --- Setup mode ---

static void setupBegin() {
  Serial.println("=== BC250 PSU controller : SETUP MODE ===");

  // Keep the PSU off and the button readable while configuring.
  pinMode(PS_ON_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_SENSE, INPUT_PULLUP);
}

static void setupLoop(unsigned long now) {
  // Button held in setup mode = escape back to normal mode.
  // Only armed after the button has been seen released: the hold that entered
  // setup mode is usually still down at boot and must not count as an exit.
  static unsigned long pressStart = 0;
  static bool wasDown = false;
  static bool armed = false;
  bool down = (digitalRead(BUTTON_SENSE) == LOW);
  pubButtonDown = down;
  if (!down) {
    armed = true;
  }
  if (!armed) {
    // Ignore the entry hold until it is released.
  } else if (down && !wasDown) {
    pressStart = now;
  } else if (down && (now - pressStart) >= SETUP_HOLD_MS) {
    logEvent("[ACT ] button held, leaving setup mode");
    setForceSetup(false);
    delay(50);
    ESP.restart();
  }
  wasDown = down;
}

// --- Normal mode ---

static void normalBegin() {
  Serial.printf("=== BC250 PSU controller (%s) ===\n", BC250_BOARD_NAME);

  // PS_ON# open-drain, released by default so the PSU stays off at boot.
  pinMode(PS_ON_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);

  // Switch: BUTTON_GND = local ground, BUTTON_SENSE = sensed input with pull-up.
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_SENSE, INPUT_PULLUP);

  // TPMS1 sense: read as ADC over the full 0-3.3V range.
  analogSetAttenuation(ADC_11db);

  // Seed debounced states from the current levels. The board only counts as up
  // if TPMS1 stays high for BOOT_ADOPT_MS (see there).
  buttonStable = buttonLastRaw = (digitalRead(BUTTON_SENSE) == LOW);
  bool boardUp = readBoardSense();
  for (unsigned long t0 = millis(); boardUp && millis() - t0 < BOOT_ADOPT_MS;) {
    delay(10);
    boardUp = readBoardSense();
  }
  if (!boardUp) senseLevel = false;
  boardSenseStable = boardSenseLastRaw = boardUp;

  // If the board is already up when the ESP (re)boots, adopt the ON state
  // rather than assuming OFF — keeps us in sync after an ESP-only reset.
  if (boardSenseStable) {
    psuOn();  // make sure PS_ON# matches reality
    setState(STATE_ON);
  }

  logEvent("[INIT] state=%s board=%s", stateName(state),
           boardSenseStable ? "UP" : "DOWN");
}

static void normalLoop(unsigned long now) {
  // --- Sample & debounce inputs ---
  uint32_t senseMv;
  bool buttonRaw = (digitalRead(BUTTON_SENSE) == LOW);   // pressed == LOW
  bool boardRaw  = readBoardSense(&senseMv);             // board up (hysteresis)

  if (debounce(buttonRaw, &buttonStable, &buttonLastRaw,
               &buttonLastChange, now, DEBOUNCE_MS)) {
    if (buttonStable) {
      // Press began.
      pressStart = now;
      pressStartedOff = (state == STATE_OFF);
      longPressFired = false;
      setupFired = false;
      logEvent("[BTN ] pressed");
    } else {
      // Released. A short tap that began while OFF powers on (power-on is on
      // release so that a long hold from OFF can mean "enter setup" instead).
      unsigned long held = now - pressStart;
      logEvent("[BTN ] released after %lu ms", held);
      if (pressStartedOff && !setupFired && state == STATE_OFF &&
          held < SETUP_HOLD_MS) {
        powerOn("short press while OFF", now);
      }
    }
  }

  // --- Button holds ---
  if (buttonStable && pressStartedOff && !setupFired &&
      (now - pressStart) >= SETUP_HOLD_MS) {
    // Long hold from OFF -> reconfigure (does not power the machine on).
    setupFired = true;
    enterSetupMode("long hold (>8s) while OFF");
  }
  if (buttonStable && !pressStartedOff && !longPressFired && state == STATE_ON &&
      (now - pressStart) >= LONG_PRESS_MS) {
    // Long hold that began while ON -> force off.
    longPressFired = true;
    powerOff("long press (>5s) while ON", now);
  }

  // --- Web UI requests ---
  if (powerOnRequested) {
    powerOnRequested = false;
    if (state == STATE_OFF) powerOn("web UI power on", now);
  }
  if (forceOffRequested) {
    forceOffRequested = false;
    if (state != STATE_OFF) powerOff("web UI force off", now);
  }

  // --- BLE controller wake ("machine follows controller") ---
  // While OFF, a bound controller being present powers the machine on. The
  // guard window after a power-off lets you switch the controller off first (so
  // it goes absent and the machine stays down) and rides out the controller's
  // reconnect-advertising burst at shutdown.
  bool blePresent = bleWakePresent(now);
  bool bleInhibited = (int32_t)(bleInhibitUntil - now) > 0;
  if (state == STATE_OFF && blePresent && !bleInhibited) {
    powerOn("controller present (BLE)", now);
  }

  bool boardChanged = debounce(boardRaw, &boardSenseStable, &boardSenseLastRaw,
                               &boardSenseChange, now,
                               state == STATE_BOOTING ? BOARD_UP_DEBOUNCE_MS
                                                      : BOARD_OFF_DEBOUNCE_MS);

  // --- State-driven board-sense handling ---
  switch (state) {
    case STATE_BOOTING:
      if (boardSenseStable) {
        logEvent("[ACT ] TPMS1 HIGH -> board is up");
        setState(STATE_ON);
      } else if ((now - bootStart) >= BOOT_TIMEOUT_MS) {
        powerOff("boot timed out, board never signalled UP", now);
      }
      break;

    case STATE_ON:
      // Board dropped TPMS1 on its own (OS shutdown / crash) -> follow it down.
      if (boardChanged && !boardSenseStable) {
        powerOff("TPMS1 LOW while ON, board shut down", now);
      }
      break;

    case STATE_OFF:
    default:
      break;
  }

  // --- Publish for the web UI ---
  pubState      = state;
  pubBoardUp    = boardSenseStable;
  pubSenseMv    = senseMv;
  pubButtonDown = buttonStable;
  pubInhibitMs  = bleInhibited ? bleInhibitUntil - now : 0;

  // --- Heartbeat ---
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    Serial.printf("[HB  ] state=%s board=%s btn=%s ble=%s net=%s | sense: %umV (%s)\n",
                  stateName(state),
                  boardSenseStable ? "UP" : "DOWN",
                  buttonStable ? "down" : "up",
                  blePresent ? "present" : "absent",
                  netSummary().c_str(),
                  senseMv, boardRaw ? "high" : "low");
  }
}

void setup() {
  Serial.begin(115200);
  // Give native USB-CDC targets a moment to enumerate so early logs aren't lost.
#if BC250_NATIVE_USB_SERIAL
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
    delay(10);
  }
#endif
  Serial.println();

  loadConfig();

  // The portal only starts on an explicit request (8s button hold while OFF).
  // Otherwise we ALWAYS run the normal controller so button control works even
  // when no Bluetooth controller or router has been configured.
  g_setupMode = getForceSetup();
  if (g_setupMode) {
    setupBegin();
  } else {
    normalBegin();
  }
  bleBegin(g_setupMode);
  netBegin(g_setupMode);
  webBegin(g_setupMode);
}

void loop() {
  unsigned long now = millis();
  if (g_setupMode) {
    setupLoop(now);
  } else {
    normalLoop(now);
  }
  bleLoop(now);
  netLoop(now);
  webLoop(now);
}
