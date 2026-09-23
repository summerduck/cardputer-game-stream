// Game Boy / Game Boy Color machine around Walnut-CGB, portable (device and host tests).
// The ROM is paged in from a RomSource on demand, so ROMs larger than RAM run.
#pragma once
#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "minigb_apu.h"
}

namespace emu {

constexpr int W = 160, H = 144;

// Page size of the ROM cache. 2 KB pages with clock eviction measured best: Zelda Oracle,
// Harry Potter and Rayman miss 0.05-0.24 pages per frame with a 96-128 KB cache (test/run_tests.sh).
#ifndef EMU_PAGE_BITS
#define EMU_PAGE_BITS 11
#endif
constexpr uint32_t PAGE_SIZE = 1u << EMU_PAGE_BITS;

// Where ROM bytes come from: a file on the SD card on the device, a file on the host.
struct RomSource {
    virtual ~RomSource() = default;
    virtual uint32_t size() = 0;
    virtual bool read(uint32_t offset, uint8_t *dst, uint32_t len) = 0;
};

// Game Boy buttons, a set bit = pressed.
enum Button : uint8_t { A = 0x01, B = 0x02, SELECT = 0x04, START = 0x08, RIGHT = 0x10, LEFT = 0x20, UP = 0x40, DOWN = 0x80 };

// Cartridge RAM the game will need, from the ROM header (to budget memory before load()).
uint32_t saveSizeFromHeader(RomSource &src);

// Loads the ROM header and prepares a page cache of cacheBytes. With mapped (the whole ROM already
// in addressable memory, e.g. flash mapped) there is no cache and src is not read.
// Returns nullptr or an error text.
const char *load(RomSource &src, uint32_t cacheBytes, const uint8_t *mapped = nullptr);
void unload();

// One emulated frame (1/59.7 s). With render = false the picture is not drawn (frame skip).
// Returns false if the game crashed the emulator (error() tells why).
enum ExecutionMode : uint8_t { COMPATIBLE, FAST, EXECUTION_MODES };
void setExecutionMode(ExecutionMode mode);
ExecutionMode executionMode();
const char *executionModeName(ExecutionMode mode);
bool runFrame(bool render);
const char *error();
void setButtons(uint8_t pressed);
void reset();

// The last drawn frame as palette indices, and the RGB565 colours for them.
extern uint8_t frame[H][W];
extern uint16_t palette[64];
int paletteSize();  // 12 for Game Boy games, 64 for Game Boy Color games
bool isColor();

// Colours for original Game Boy games.
constexpr int DMG_PALETTES = 4;
const char *dmgPaletteName(int i);
void setDmgPalette(int i);

const char *title();  // from the ROM header
uint32_t romSize();

// Battery-backed cartridge RAM (the save game).
// It lives in SAVE_CHUNK pieces (the last one may be shorter).
constexpr uint32_t SAVE_CHUNK = 8192;
uint32_t saveSize();
uint8_t *saveChunk(uint32_t i);
bool saveDirty();     // written since the last clearSaveDirty()
void clearSaveDirty();

// Sound: AUDIO_SAMPLES_TOTAL interleaved stereo samples per frame.
void audioFrame(int16_t *out);

// ROM cache statistics.
uint32_t pageMisses();
uint32_t cacheSlots();

}  // namespace emu
