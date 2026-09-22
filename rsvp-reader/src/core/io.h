// Tiny I/O interfaces so the parsing code runs both on the Cardputer (File) and on a PC (tests).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rsvp {

struct Source {
    virtual ~Source() = default;
    virtual size_t read(uint8_t *buf, size_t len) = 0;
    virtual bool seek(uint32_t pos) = 0;
    virtual uint32_t size() = 0;
};

struct Sink {
    virtual ~Sink() = default;
    virtual bool write(const uint8_t *data, size_t len) = 0;
    bool put(char c) { return write(reinterpret_cast<const uint8_t *>(&c), 1); }
};

// Called with 0..100 during long conversions
typedef void (*ProgressFn)(int percent);

}  // namespace rsvp
