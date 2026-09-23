// The only translation unit that includes walnut_cgb.h (it defines its functions in the header).
#include "emu.h"

#include <setjmp.h>

#include <algorithm>
#include <stdlib.h>
#include <string.h>

// Walnut-CGB forwards sound register access to these.
extern "C++" uint8_t audio_read(uint16_t addr);
extern "C++" void audio_write(uint16_t addr, uint8_t val);

#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define WALNUT_GB_12_COLOUR 1
#define WALNUT_FULL_GBC_SUPPORT 1
#include "walnut_cgb.h"

namespace emu {

uint8_t frame[H][W];
uint16_t palette[64];

namespace {

constexpr uint32_t PAGE_MASK = PAGE_SIZE - 1;
constexpr uint32_t PINNED = 0x4000 / PAGE_SIZE;  // bank 0 is read all the time: never evicted

struct gb_s gb;
struct minigb_apu_ctx apu;
RomSource *source = nullptr;
uint32_t romBytes = 0, numPages = 0;
uint8_t **pageMap = nullptr;   // page -> its bytes in the cache, nullptr = not loaded
uint8_t **slots = nullptr;     // nSlots pages, each its own allocation: the heap is fragmented
uint8_t *used = nullptr;       // page -> read since the clock hand last passed its slot
int32_t *slotPage = nullptr;   // slot -> page, -1 = free
uint32_t nSlots = 0, hand = 0, misses = 0;
// Battery RAM in 8 KB pieces: with Wi-Fi on, the heap has no 32 KB block left.
constexpr uint32_t CHUNK_BITS = 13;
uint8_t *cartRam[128 * 1024 >> CHUNK_BITS] = {};
uint32_t cartRamSize = 0;
bool ramDirty = false;
char titleText[17];
const char *lastError = nullptr;
jmp_buf crash;
int dmgPalette = 0;

const uint8_t blank[4] = {0xFF, 0xFF, 0xFF, 0xFF};

// Loads a page into a free slot, or evicts one not read lately ("clock": a read page gets
// one more round). Bank 0 slots are never evicted.
const uint8_t *loadPage(uint32_t page) {
    misses++;
    uint32_t slot;
    while (true) {
        slot = hand;
        hand = hand + 1 < nSlots ? hand + 1 : PINNED;
        int32_t old = slotPage[slot];
        if (old < 0 || !used[old]) break;
        used[old] = 0;
    }
    if (slotPage[slot] >= 0) pageMap[slotPage[slot]] = nullptr;
    uint8_t *dst = slots[slot];
    uint32_t off = page * PAGE_SIZE;
    uint32_t len = romBytes - off < PAGE_SIZE ? romBytes - off : PAGE_SIZE;
    if (!source->read(off, dst, len)) memset(dst, 0xFF, len);
    if (len < PAGE_SIZE) memset(dst + len, 0xFF, PAGE_SIZE - len);
    slotPage[slot] = page;
    pageMap[page] = dst;
    return dst;
}

inline const uint8_t *romAt(uint32_t addr) {
    uint32_t page = addr >> EMU_PAGE_BITS;
    if (page >= numPages) return blank;
    const uint8_t *p = pageMap[page];
    if (__builtin_expect(p == nullptr, 0)) p = loadPage(page);
    used[page] = 1;
    return p + (addr & PAGE_MASK);
}

uint8_t romRead(struct gb_s *, const uint_fast32_t addr) { return *romAt(addr); }

// Multi-byte reads go byte by byte when they cross into the next page, which may not be loaded.
uint16_t romRead16(struct gb_s *, const uint_fast32_t addr) {
    if ((addr & PAGE_MASK) > PAGE_SIZE - 2) return *romAt(addr) | (*romAt(addr + 1) << 8);
    uint16_t v;
    memcpy(&v, romAt(addr), 2);
    return v;
}

uint32_t romRead32(struct gb_s *, const uint_fast32_t addr) {
    if ((addr & PAGE_MASK) > PAGE_SIZE - 4)
        return *romAt(addr) | (*romAt(addr + 1) << 8) | (*romAt(addr + 2) << 16) | (uint32_t(*romAt(addr + 3)) << 24);
    uint32_t v;
    memcpy(&v, romAt(addr), 4);
    return v;
}

uint8_t ramRead(struct gb_s *, const uint_fast32_t addr) {
    return addr < cartRamSize ? cartRam[addr >> CHUNK_BITS][addr & (SAVE_CHUNK - 1)] : 0xFF;
}

void ramWrite(struct gb_s *, const uint_fast32_t addr, const uint8_t val) {
    if (addr >= cartRamSize) return;
    uint8_t &b = cartRam[addr >> CHUNK_BITS][addr & (SAVE_CHUNK - 1)];
    if (b != val) {
        b = val;
        ramDirty = true;
    }
}

// Walnut-CGB must not return from this: jump back out of runFrame().
void onError(struct gb_s *, const enum gb_error_e err, const uint16_t) {
    static const char *const names[] = {"unknown error", "invalid opcode", "invalid read", "invalid write", "halt"};
    lastError = names[err < 5 ? err : 0];
    longjmp(crash, 1);
}

void drawLine(struct gb_s *g, const uint8_t *pixels, const uint_fast8_t line) {
    if (line >= H) return;
    uint8_t *row = frame[line];
    if (g->cgb.cgbMode) {
        memcpy(row, pixels, W);
    } else {
        // shade in bits 0-1, palette in bits 4-5 (OBJ0, OBJ1, BG) -> 0..11
        for (int x = 0; x < W; x++) row[x] = ((pixels[x] & 0x30) >> 2) | (pixels[x] & 3);
    }
}

// RGB888 x 12: OBJ0, OBJ1, BG, lightest to darkest.
struct DmgPalette {
    const char *name;
    uint32_t c[4];
};
const DmgPalette DMG[DMG_PALETTES] = {
    {"контраст", {0xFFFFFF, 0xA8A8A8, 0x505050, 0x000000}},
    {"Pocket", {0xE8E8D8, 0xA0A090, 0x585848, 0x101008}},
    {"зелёная", {0xE0F8D0, 0x88C070, 0x346856, 0x081820}},
    {"оригинал", {0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F}},
};

uint16_t rgb565(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F); }

void freeAll() {
    free(pageMap);
    for (uint32_t i = 0; slots && i < nSlots; i++) free(slots[i]);
    free(slots);
    free(slotPage);
    free(used);
    for (auto &c : cartRam) {
        free(c);
        c = nullptr;
    }
    pageMap = nullptr;
    slots = nullptr;
    slotPage = nullptr;
    used = nullptr;
    cartRamSize = 0;
    source = nullptr;
}

}  // namespace

uint32_t saveSizeFromHeader(RomSource &src) {
    uint8_t h[3];  // 0x147 cartridge type, 0x148 ROM size, 0x149 RAM size
    if (!src.read(0x147, h, 3)) return 0;
    if (h[0] == 0x05 || h[0] == 0x06) return 512;  // MBC2: built-in RAM
    static const uint32_t sizes[] = {0, 2048, 8192, 32768, 131072, 65536};
    return h[2] < 6 ? sizes[h[2]] : 0;
}

const char *load(RomSource &src, uint32_t cacheBytes, const uint8_t *mapped) {
    freeAll();
    source = &src;
    romBytes = src.size();
    if (romBytes < 0x8000) return "файл меньше 32 КБ";
    numPages = (romBytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t wanted = mapped ? 0 : cacheBytes / PAGE_SIZE;
    if (wanted > numPages) wanted = numPages;
    pageMap = static_cast<uint8_t **>(calloc(numPages, sizeof(uint8_t *)));
    slots = static_cast<uint8_t **>(calloc(wanted ? wanted : 1, sizeof(uint8_t *)));
    slotPage = static_cast<int32_t *>(malloc((wanted ? wanted : 1) * sizeof(int32_t)));
    used = static_cast<uint8_t *>(calloc(numPages, 1));
    nSlots = 0;
    if (!pageMap || !slots || !slotPage || !used) {
        freeAll();
        return "мало памяти";
    }
    hand = 0;
    misses = 0;
    if (mapped) {
        // The whole ROM is in memory already (flash mapped): every page is present, no cache.
        for (uint32_t p = 0; p < numPages; p++) pageMap[p] = const_cast<uint8_t *>(mapped) + p * PAGE_SIZE;
    } else {
        while (nSlots < wanted && (slots[nSlots] = static_cast<uint8_t *>(malloc(PAGE_SIZE)))) nSlots++;
        if (nSlots < PINNED + 8) {
            freeAll();
            return "мало памяти";
        }
        for (uint32_t i = 0; i < nSlots; i++) slotPage[i] = -1;
        for (uint32_t p = 0; p < PINNED; p++) loadPage(p);  // fills slots 0..PINNED-1
    }

    enum gb_init_error_e e = gb_init(&gb, romRead, romRead16, romRead32, ramRead, ramWrite, onError, nullptr);
    if (e == GB_INIT_CARTRIDGE_UNSUPPORTED) {
        freeAll();
        return "картридж не поддерживается";
    }
    if (e != GB_INIT_NO_ERROR) {
        freeAll();
        return "повреждённый ROM";
    }
    uint32_t ramSize = gb_get_save_size(&gb);
    if (ramSize > sizeof(cartRam) / sizeof(cartRam[0]) * SAVE_CHUNK) {
        freeAll();
        return "картридж не поддерживается";
    }
    for (uint32_t i = 0; i * SAVE_CHUNK < ramSize; i++) {
        uint32_t n = std::min<uint32_t>(SAVE_CHUNK, ramSize - i * SAVE_CHUNK);
        // No room: give back ROM cache pages (the newest first) until the piece fits.
        while (!(cartRam[i] = static_cast<uint8_t *>(malloc(n))) && nSlots > PINNED + 8) {
            nSlots--;
            if (slotPage[nSlots] >= 0) pageMap[slotPage[nSlots]] = nullptr;
            free(slots[nSlots]);
            slots[nSlots] = nullptr;
            if (hand >= nSlots) hand = PINNED;
        }
        if (!cartRam[i]) {
            freeAll();
            return "мало памяти для сохранения";
        }
        memset(cartRam[i], 0xFF, n);
    }
    cartRamSize = ramSize;
    ramDirty = false;
    gb_init_lcd(&gb, drawLine);
    minigb_apu_audio_init(&apu);
    gb_get_rom_name(&gb, titleText);
    lastError = nullptr;
    memset(frame, 0, sizeof(frame));
    setDmgPalette(dmgPalette);
    return nullptr;
}

void unload() { freeAll(); }

bool runFrame(bool render) {
    if (lastError) return false;
    // walnut skips drawing while frame_skip is on and its counter is clear
    gb.direct.frame_skip = !render;
    gb.display.frame_skip_count = false;
    if (setjmp(crash)) return false;
    gb_run_frame(&gb);  // not the dual-fetch variant: it crashes Harry Potter (invalid opcode), ~8% slower only
    if (render && gb.cgb.cgbMode) memcpy(palette, gb.cgb.fixPalette, sizeof(gb.cgb.fixPalette));
    return true;
}

const char *error() { return lastError; }

void setButtons(uint8_t pressed) { gb.direct.joypad = ~pressed; }

void reset() {
    gb_reset(&gb);
    lastError = nullptr;
}

int paletteSize() { return gb.cgb.cgbMode ? 64 : 12; }
bool isColor() { return gb.cgb.cgbMode; }

const char *dmgPaletteName(int i) { return DMG[i].name; }

void setDmgPalette(int i) {
    dmgPalette = i;
    if (gb.cgb.cgbMode) return;
    for (int p = 0; p < 3; p++)
        for (int s = 0; s < 4; s++) palette[p * 4 + s] = rgb565(DMG[i].c[s]);
}

const char *title() { return titleText; }
uint32_t romSize() { return romBytes; }
uint32_t saveSize() { return cartRamSize; }
uint8_t *saveChunk(uint32_t i) { return cartRam[i]; }
bool saveDirty() { return ramDirty; }
void clearSaveDirty() { ramDirty = false; }

void audioFrame(int16_t *out) { minigb_apu_audio_callback(&apu, out); }

uint32_t pageMisses() { return misses; }
uint32_t cacheSlots() { return nSlots; }

}  // namespace emu

uint8_t audio_read(uint16_t addr) { return minigb_apu_audio_read(&emu::apu, addr); }
void audio_write(uint16_t addr, uint8_t val) { minigb_apu_audio_write(&emu::apu, addr, val); }
