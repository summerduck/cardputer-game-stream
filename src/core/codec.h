// Frame updates for the phone: only the lines that changed since they were last sent, run-length
// coded, split into messages of a bounded size so the device never needs a whole-frame buffer.
//
// Message layout (all little endian):
//   u8 1 (frame), u8 flags (bit 0: palette follows)
//   [u8 count, count x u16 RGB565]        if bit 0
//   repeated until the end: u8 line, u8 runs, then either 160 raw index bytes (runs == 0)
//   or `runs` pairs of (u8 length 1..255, u8 index)
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace codec {

constexpr int W = 160, H = 144;
constexpr size_t MAX_LINE = 2 + W;
constexpr size_t MIN_MESSAGE = 3 + 64 * 2 + MAX_LINE;  // room for the palette and one line

class Encoder {
public:
    // Everything is sent again (a new viewer connected).
    void forceFull();

    // Takes a new frame: finds the lines that differ from what the viewers have.
    void begin(const uint8_t frame[H][W], const uint16_t *palette, int paletteSize);

    // Writes the next message, at most cap (>= MIN_MESSAGE) bytes, into out. Returns its size,
    // 0 when everything is sent. Lines not sent stay pending for the next frame.
    size_t next(uint8_t *out, size_t cap);

private:
    const uint8_t (*frame_)[W] = nullptr;
    const uint16_t *palette_ = nullptr;
    int paletteSize_ = 0;
    uint32_t hash_[H] = {};     // of the frame given to begin()
    uint32_t sentHash_[H] = {};
    bool sent_[H] = {};         // sentHash_ is valid
    uint16_t sentPalette_[64] = {};
    int sentPaletteSize_ = 0;   // 0 = palette not sent yet
    int line_ = 0;              // where next() continues
};

// The reverse, for tests: applies a message to frame/palette. Returns false on a malformed message.
bool decode(const uint8_t *msg, size_t len, uint8_t frame[H][W], uint16_t palette[64], int *paletteSize);

}  // namespace codec
