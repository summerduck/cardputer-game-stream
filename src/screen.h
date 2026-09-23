// Draws finished frames on the Cardputer screen and hands them to the stream, on the other core.
#pragma once
#include <stdint.h>

namespace screen {

enum Mode : uint8_t {
    ZOOM,  // x1.5: full width, 90 of 144 lines, panned with [ ]
    ONE,   // 1:1: 135 of 144 lines, panned with [ ]
    FIT,   // whole frame: 9 lines dropped
    OFF,   // screen off (while watching the stream)
    MODES
};
const char *modeName(Mode m);

void begin();
void setMode(Mode m);
Mode mode();
void pan(int lines);  // moves the view by frame lines: < 0 up, > 0 down

// Hands emu::frame to the screen task if the previous one is done. Call after a rendered frame.
// Until idle() the emulator must not draw: the task reads emu::frame (no copy, saves 23 KB).
bool submit();
bool idle();
uint32_t stackLeft();

// A line of text drawn over the picture (the F key's speed readout); "" hides it.
void setHud(const char *text);
const char *hud();  // bytes of the task's stack never used, for the serial report

// Stops drawing (menus use the display, backlight on) until resume(). Waits for the frame in
// progress. Starts paused.
void pause();
void resume();

}  // namespace screen
