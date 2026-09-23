// Keeps the current ROM in the "gbrom" flash partition, mapped into memory: no RAM cache and no SD
// reads while playing. The partition comes from the installer image (tools/make_installer.py).
#pragma once
#include <Arduino.h>

namespace flashrom {

// Size of the partition, 0 if there is none (installed without the installer image).
uint32_t capacity();

// Maps the ROM at path, copying it from the SD card first if the partition holds another one.
// progress(done, total) is called during the copy. Returns nullptr if it can't (no partition,
// ROM too big, flash error): then the caller falls back to reading from the SD card.
const uint8_t *map(const String &path, uint32_t size, void (*progress)(uint32_t done, uint32_t total));
void unmap();

}  // namespace flashrom
