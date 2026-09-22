// Minimal BLE HID keyboard (NimBLE) — pairs with the Freestyle / TVs / phones as "Cardputer KB".
#pragma once
#include <Arduino.h>

namespace blekb {

// USB HID usage IDs for non-printable keys
constexpr uint8_t HID_ENTER = 0x28;
constexpr uint8_t HID_ESC = 0x29;
constexpr uint8_t HID_BACKSPACE = 0x2A;
constexpr uint8_t HID_TAB = 0x2B;
constexpr uint8_t HID_DELETE = 0x4C;
constexpr uint8_t HID_RIGHT = 0x4F;
constexpr uint8_t HID_LEFT = 0x50;
constexpr uint8_t HID_DOWN = 0x51;
constexpr uint8_t HID_UP = 0x52;

bool begin(const char *name);  // false if the BLE stack or advertising failed
bool isReady();
bool isConnected();
void tapKey(uint8_t usage, uint8_t modifiers = 0);  // press + release
bool typeChar(char c);                               // false if the char has no US-layout key

}  // namespace blekb
