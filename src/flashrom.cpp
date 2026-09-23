#include "flashrom.h"

#include <SD.h>
#include <esp_partition.h>
#include <string.h>

namespace flashrom {

namespace {

constexpr const char *LABEL = "gbrom";
constexpr uint32_t HEADER = 0x1000;  // first sector: what the partition holds; the ROM follows
constexpr uint32_t MAGIC = 0x4D4F5247;  // "GROM"

struct Header {
    uint32_t magic;
    uint32_t size;
    char path[248];
};

const esp_partition_t *part() {
    static const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, LABEL);
    return p;
}

esp_partition_mmap_handle_t handle;
bool mapped = false;

bool holds(const String &path, uint32_t size) {
    Header h;
    if (esp_partition_read(part(), 0, &h, sizeof(h)) != ESP_OK) return false;
    return h.magic == MAGIC && h.size == size && strncmp(h.path, path.c_str(), sizeof(h.path)) == 0;
}

// The header goes last, so a copy cut short (switched off) is never taken for a good one.
bool copy(const String &path, uint32_t size, void (*progress)(uint32_t, uint32_t)) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t span = (HEADER + size + 0xFFF) & ~0xFFFu;
    if (esp_partition_erase_range(part(), 0, span) != ESP_OK) return false;
    static uint8_t buf[4096];
    for (uint32_t done = 0; done < size;) {
        uint32_t n = std::min<uint32_t>(sizeof(buf), size - done);
        if (f.read(buf, n) != n) return false;
        if (esp_partition_write(part(), HEADER + done, buf, n) != ESP_OK) return false;
        done += n;
        if (progress && (done % (64 * 1024) == 0 || done == size)) progress(done, size);
    }
    f.close();
    Header h = {};
    h.magic = MAGIC;
    h.size = size;
    strncpy(h.path, path.c_str(), sizeof(h.path) - 1);
    return esp_partition_write(part(), 0, &h, sizeof(h)) == ESP_OK;
}

}  // namespace

uint32_t capacity() { return part() && part()->size > HEADER ? part()->size - HEADER : 0; }

const uint8_t *map(const String &path, uint32_t size, void (*progress)(uint32_t, uint32_t)) {
    unmap();
    if (!part() || size > capacity()) return nullptr;
    if (!holds(path, size) && !copy(path, size, progress)) return nullptr;
    const void *ptr = nullptr;
    // Map whole 2 KB emulator pages past the end: the last page may be read beyond the ROM.
    uint32_t len = std::min<uint32_t>(part()->size, (HEADER + size + 0xFFFF) & ~0xFFFFu);
    if (esp_partition_mmap(part(), 0, len, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) return nullptr;
    mapped = true;
    return static_cast<const uint8_t *>(ptr) + HEADER;
}

void unmap() {
    if (mapped) esp_partition_munmap(handle);
    mapped = false;
}

}  // namespace flashrom
