#include "screen.h"

#include <M5Cardputer.h>
#include <string.h>

#include "core/emu.h"
#include "stream.h"

namespace screen {

namespace {

constexpr int LW = 240, LH = 135;
constexpr int BAND = 5;  // lines per DMA transfer, 135 = 27 bands
constexpr int ZOOM_PAN_MAX = emu::H - LH * 2 / 3;  // 54
constexpr int ONE_PAN_MAX = emu::H - LH;          // 9

M5GFX &lcd() { return M5Cardputer.Display; }

// The task reads emu::frame directly: while it is busy the emulator does not draw (see submit()).
uint16_t swapped[64];  // emu::palette byte-swapped, the way the panel takes it over DMA

uint16_t bands[2][LW * BAND];
uint8_t zoomX[LW];  // screen column -> frame column for x1.5

volatile Mode current = ZOOM;
volatile int zoomPan = ZOOM_PAN_MAX, onePan = ONE_PAN_MAX;  // bottom: Zelda's text box and HUD
uint8_t brightness = 80;
volatile bool busy = false, paused = true, clearNeeded = true;
TaskHandle_t task = nullptr;
char hudText[48] = "";

// Frame line shown on screen line y.
int sourceLine(Mode m, int y) {
    switch (m) {
        case ZOOM: return zoomPan + y * 2 / 3;
        case ONE: return onePan + y;
        default: return y * emu::H / LH;
    }
}

void draw() {
    Mode m = current;
    if (m == OFF) return;
    if (clearNeeded) {
        lcd().fillScreen(TFT_BLACK);
        clearNeeded = false;
    }
    for (int i = 0; i < emu::paletteSize(); i++) swapped[i] = __builtin_bswap16(emu::palette[i]);
    int w = m == ZOOM ? LW : emu::W, x0 = (LW - w) / 2;
    lcd().startWrite();
    for (int b = 0; b < LH / BAND; b++) {
        uint16_t *out = bands[b & 1];
        for (int r = 0; r < BAND; r++) {
            const uint8_t *src = emu::frame[sourceLine(m, b * BAND + r)];
            uint16_t *dst = out + r * w;
            if (m == ZOOM)
                for (int x = 0; x < LW; x++) dst[x] = swapped[src[zoomX[x]]];
            else
                for (int x = 0; x < emu::W; x++) dst[x] = swapped[src[x]];
        }
        lcd().waitDMA();  // the other band buffer is free again
        lcd().pushImageDMA(x0, b * BAND, w, BAND, reinterpret_cast<const lgfx::swap565_t *>(out));
    }
    lcd().waitDMA();
    if (hudText[0]) {
        lcd().setTextDatum(top_left);
        lcd().setTextColor(TFT_WHITE, TFT_BLACK);
        lcd().drawString(hudText, 2, 0);
    }
    lcd().endWrite();
}

void loop(void *) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!paused) draw();
        stream::offer(emu::frame, emu::palette, emu::paletteSize());
        busy = false;
    }
}

}  // namespace

const char *modeName(Mode m) {
    static const char *const names[] = {"zoom ×1.5", "1:1", "full frame", "off"};
    return names[m];
}

void begin() {
    for (int x = 0; x < LW; x++) zoomX[x] = x * 2 / 3;
    xTaskCreatePinnedToCore(loop, "screen", 6144, nullptr, 2, &task, 0);
}

void setMode(Mode m) {
    current = m;
    clearNeeded = true;
    if (!paused) lcd().setBrightness(m == OFF ? 0 : brightness);
}

Mode mode() { return current; }

void setBrightness(uint8_t level) {
    brightness = level;
    lcd().setBrightness(paused || current != OFF ? level : 0);
}

void pan(int lines) {
    if (current == ZOOM) zoomPan = constrain(zoomPan + lines, 0, ZOOM_PAN_MAX);
    if (current == ONE) onePan = constrain(onePan + lines, 0, ONE_PAN_MAX);
}

bool submit() {
    if (busy) return false;
    busy = true;
    xTaskNotifyGive(task);
    return true;
}

bool idle() { return !busy; }

uint32_t stackLeft() { return uxTaskGetStackHighWaterMark(task); }

void setHud(const char *text) {
    strncpy(hudText, text, sizeof(hudText) - 1);
    clearNeeded = true;
}

const char *hud() { return hudText; }

void pause() {
    paused = true;
    while (busy) delay(1);
    lcd().setBrightness(brightness);  // menus need the backlight even in OFF mode
}

void resume() {
    paused = false;
    clearNeeded = true;
    lcd().setBrightness(current == OFF ? 0 : brightness);
}

}  // namespace screen
