#include "pins.h"

#include "board.h"

#if defined(BC250_BOARD_ESP32_WROOM_32)
// 0, 2, 5, 12, 15 strap; 1, 3 are the serial port; 6-11 the flash; 34-39 are
// input-only with no pull-ups.
static const int8_t GENERAL[] = {4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
static const int8_t SENSE[]   = {32, 33, 34, 35, 36, 39};
#elif defined(BC250_BOARD_ESP32_C3_DEVKITM_1)
// 2, 8, 9 strap; 12-17 are the flash; 18/19 USB; 20/21 the serial port.
static const int8_t GENERAL[] = {0, 1, 3, 4, 5, 6, 7, 10};
static const int8_t SENSE[]   = {0, 1, 3, 4};
#endif

template <size_t N>
static bool listed(const int8_t (&list)[N], int pin) {
  for (int8_t p : list) {
    if (p == pin) return true;
  }
  return false;
}

PinConfig defaultPins() {
  return {DEFAULT_PS_ON_PIN, DEFAULT_BOARD_SENSE, DEFAULT_BUTTON_SENSE,
          DEFAULT_BUTTON_GND, DEFAULT_LED_PIN};
}

const char *pinsError(const PinConfig &p) {
  if (!listed(GENERAL, p.psOn))   return "PS_ON# can't use that pin";
  if (!listed(SENSE, p.sense))    return "TPMS1 sense needs an ADC1 pin";
  if (!listed(GENERAL, p.button)) return "the button can't use that pin";
  if (p.buttonGnd != -1 && !listed(GENERAL, p.buttonGnd)) return "the button ground can't use that pin";
  if (!listed(GENERAL, p.led))    return "LED data can't use that pin";
  const int8_t used[] = {p.psOn, p.sense, p.button, p.buttonGnd, p.led};
  for (size_t i = 0; i < sizeof(used); i++) {
    for (size_t j = i + 1; j < sizeof(used); j++) {
      if (used[i] != -1 && used[i] == used[j]) return "each pin can only have one job";
    }
  }
  return nullptr;
}

void pinOptionsJson(JsonObject out) {
  JsonArray general = out["general"].to<JsonArray>();
  for (int8_t p : GENERAL) general.add(p);
  JsonArray sense = out["sense"].to<JsonArray>();
  for (int8_t p : SENSE) sense.add(p);
}
