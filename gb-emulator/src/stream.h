// Wi-Fi access point with a web page that shows the game screen on a phone.
#pragma once
#include <stdint.h>

namespace stream {

constexpr const char *SSID = "Cardputer-GB";
constexpr const char *PASSWORD = "gameboy123";
constexpr const char *URL = "192.168.4.1";

// Returns false when there is not enough memory (a big game's ROM cache took it).
bool start();
void stop();
bool running();
int viewers();

// Sends the changed part of a frame to the viewers, at most 30 times a second. Screen task only.
void offer(const uint8_t frame[144][160], const uint16_t *palette, int paletteSize);

// Counters for the serial report: whole frames delivered, messages sent.
uint32_t sentFrames();
uint32_t sentMessages();

// Buttons held on the phone's on-screen pad (emu::Button bits).
uint8_t phoneButtons();

}  // namespace stream
