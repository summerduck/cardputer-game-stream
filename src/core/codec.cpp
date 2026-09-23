#include "codec.h"

#include <string.h>

namespace codec {

namespace {

uint32_t hashLine(const uint8_t *p) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (int i = 0; i < W; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Run-length codes one line into out. Returns the number of runs, 0 if raw bytes are shorter.
int runs(const uint8_t *p, uint8_t *out) {
    int n = 0;
    for (int x = 0; x < W;) {
        int len = 1;
        while (x + len < W && p[x + len] == p[x] && len < 255) len++;
        if (n * 2 + 2 >= W) return 0;
        out[n * 2] = len;
        out[n * 2 + 1] = p[x];
        n++;
        x += len;
    }
    return n;
}

}  // namespace

void Encoder::forceFull() {
    for (auto &s : sent_) s = false;
    sentPaletteSize_ = 0;
}

void Encoder::begin(const uint8_t frame[H][W], const uint16_t *palette, int paletteSize) {
    frame_ = frame;
    palette_ = palette;
    paletteSize_ = paletteSize;
    for (int y = 0; y < H; y++) hash_[y] = hashLine(frame[y]);
    line_ = 0;
}

size_t Encoder::next(uint8_t *out, size_t cap) {
    size_t n = 2;
    bool sendPalette = paletteSize_ != sentPaletteSize_ || memcmp(palette_, sentPalette_, paletteSize_ * 2) != 0;
    out[0] = 1;
    out[1] = sendPalette ? 1 : 0;
    if (sendPalette) {
        out[n++] = paletteSize_;
        for (int i = 0; i < paletteSize_; i++) {
            out[n++] = palette_[i] & 0xFF;
            out[n++] = palette_[i] >> 8;
        }
        memcpy(sentPalette_, palette_, paletteSize_ * 2);
        sentPaletteSize_ = paletteSize_;
    }
    bool lines = false;
    for (; line_ < H && n + MAX_LINE <= cap; line_++) {
        int y = line_;
        if (sent_[y] && sentHash_[y] == hash_[y]) continue;
        sent_[y] = true;
        sentHash_[y] = hash_[y];
        lines = true;
        out[n] = y;
        int r = runs(frame_[y], out + n + 2);
        out[n + 1] = r;
        if (r) {
            n += 2 + r * 2;
        } else {
            memcpy(out + n + 2, frame_[y], W);
            n += 2 + W;
        }
    }
    return lines || sendPalette ? n : 0;
}

bool decode(const uint8_t *msg, size_t len, uint8_t frame[H][W], uint16_t palette[64], int *paletteSize) {
    if (len < 2 || msg[0] != 1) return false;
    size_t i = 2;
    if (msg[1] & 1) {
        if (i >= len || msg[i] > 64) return false;
        int count = msg[i++];
        if (i + count * 2 > len) return false;
        for (int c = 0; c < count; c++, i += 2) palette[c] = msg[i] | (msg[i + 1] << 8);
        *paletteSize = count;
    }
    while (i < len) {
        if (i + 2 > len) return false;
        int y = msg[i], r = msg[i + 1];
        i += 2;
        if (y >= H) return false;
        if (r == 0) {
            if (i + W > len) return false;
            memcpy(frame[y], msg + i, W);
            i += W;
            continue;
        }
        int x = 0;
        for (int k = 0; k < r; k++, i += 2) {
            if (i + 2 > len || x + msg[i] > W) return false;
            memset(frame[y] + x, msg[i + 1], msg[i]);
            x += msg[i];
        }
        if (x != W) return false;
    }
    return true;
}

}  // namespace codec
