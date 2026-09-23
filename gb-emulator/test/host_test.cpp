// Host tests: codec round trip, and real ROMs through the emulator with the page cache.
//   ./run_tests.sh                       codec tests only
//   ./run_tests.sh game.gbc [cacheKB]    also runs the game, prints cache misses and stream size,
//                                        writes out/<name>-<frame>.ppm and out/<name>.stream
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <string>
#include <vector>

#include "codec.h"
#include "emu.h"

static int failures = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                           \
        }                                                         \
    } while (0)

struct FileRom : emu::RomSource {
    FILE *f;
    uint32_t n;
    uint32_t reads = 0;
    explicit FileRom(const char *path) : f(fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
    }
    ~FileRom() override { fclose(f); }
    uint32_t size() override { return n; }
    bool read(uint32_t off, uint8_t *dst, uint32_t len) override {
        reads++;
        fseek(f, off, SEEK_SET);
        return fread(dst, 1, len, f) == len;
    }
};

constexpr size_t CHUNK = 4096;  // what the device sends per message
static uint8_t msg[CHUNK];

// Sends a frame the way the device does; returns bytes and the number of messages.
static size_t sendFrame(codec::Encoder &enc, const uint8_t f[144][160], const uint16_t *pal, int n,
                        uint8_t view[144][160], uint16_t *viewPal, int *viewN, FILE *record, int *messages) {
    size_t total = 0;
    enc.begin(f, pal, n);
    while (size_t len = enc.next(msg, CHUNK)) {
        CHECK(len <= CHUNK);
        CHECK(codec::decode(msg, len, view, viewPal, viewN));
        if (record) {
            uint32_t l = len;
            fwrite(&l, 4, 1, record);
            fwrite(msg, 1, len, record);
        }
        total += len;
        (*messages)++;
    }
    return total;
}

static void testCodec() {
    static uint8_t a[144][160], b[144][160];
    uint16_t pal[64], pal2[64] = {};
    for (int i = 0; i < 64; i++) pal[i] = i * 1000;
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) a[y][x] = (y * 7 + (x / 9) * 3 + (x * y % 5 == 0)) % 12;
    codec::Encoder enc;
    int ps = 0, messages = 0;
    size_t n = sendFrame(enc, a, pal, 12, b, pal2, &ps, nullptr, &messages);
    CHECK(n > 0 && messages >= 1);
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    CHECK(ps == 12 && memcmp(pal, pal2, 24) == 0);
    messages = 0;
    CHECK(sendFrame(enc, a, pal, 12, b, pal2, &ps, nullptr, &messages) == 0);  // nothing changed
    a[77][3] = 5;
    a[143][159] = 9;
    n = sendFrame(enc, a, pal, 12, b, pal2, &ps, nullptr, &messages);
    CHECK(n > 0 && n < 400 && messages == 1);
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    for (int y = 0; y < 144; y++)  // noisy frame: raw lines, several messages
        for (int x = 0; x < 160; x++) a[y][x] = (x * 7 + y * 13) % 64;
    pal[3] = 7;
    messages = 0;
    sendFrame(enc, a, pal, 64, b, pal2, &ps, nullptr, &messages);
    CHECK(messages >= 6);
    CHECK(memcmp(a, b, sizeof(a)) == 0 && ps == 64 && pal2[3] == 7);
    // A frame cut off after one message: the rest goes with the next frame.
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) a[y][x] = (x * 5 + y * 3) % 64;
    enc.begin(a, pal, 64);
    n = enc.next(msg, CHUNK);
    CHECK(codec::decode(msg, n, b, pal2, &ps));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    sendFrame(enc, a, pal, 64, b, pal2, &ps, nullptr, &messages);
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    enc.forceFull();  // a new viewer gets everything
    static uint8_t fresh[144][160];
    uint16_t freshPal[64];
    int freshN = 0;
    sendFrame(enc, a, pal, 64, fresh, freshPal, &freshN, nullptr, &messages);
    CHECK(memcmp(a, fresh, sizeof(a)) == 0 && freshN == 64);
    memset(a[20], 4, 160);  // one run
    enc.begin(a, pal, 64);
    n = enc.next(msg, CHUNK);
    CHECK(n == 2 + 2 + 2);
    CHECK(codec::decode(msg, n, b, pal2, &ps));
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    msg[3] = 200;  // corrupt run length
    CHECK(!codec::decode(msg, n, b, pal2, &ps));
}

static void writePpm(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    fprintf(f, "P6 160 144 255\n");
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) {
            uint16_t c = emu::palette[emu::frame[y][x]];
            uint8_t rgb[3] = {uint8_t((c >> 11) << 3), uint8_t(((c >> 5) & 63) << 2), uint8_t((c & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

// Scripted play: mash Start/A through the intro, then walk around pressing A.
static uint8_t script(int f) {
    int t = f % 240;
    if (f < 1800) return (t < 6 ? emu::START : 0) | (t >= 120 && t < 126 ? emu::A : 0);
    static const uint8_t dirs[] = {emu::RIGHT, emu::DOWN, emu::LEFT, emu::UP};
    return dirs[(f / 90) % 4] | (t < 5 ? emu::A : 0) | (t >= 200 && t < 204 ? emu::B : 0);
}

static std::vector<uint8_t> mappedRom;

// cacheKB 0: the whole ROM in memory, as with the flash partition on the device.
static void runGame(const char *path, uint32_t cacheKB) {
    FileRom rom(path);
    const uint8_t *mapped = nullptr;
    if (cacheKB == 0) {
        mappedRom.assign((rom.size() + emu::PAGE_SIZE - 1) / emu::PAGE_SIZE * emu::PAGE_SIZE, 0xFF);
        rom.read(0, mappedRom.data(), rom.size());
        mapped = mappedRom.data();
    }
    const char *err = emu::load(rom, cacheKB * 1024, mapped);
    if (err) {
        printf("%s: %s\n", path, err);
        failures++;
        return;
    }
    std::string name = std::string(path);
    name = name.substr(name.find_last_of('/') + 1);
    name = name.substr(0, name.find('.'));
    for (auto &c : name)
        if (c == ' ') c = '_';
    FILE *stream = fopen(("out/" + name + ".stream").c_str(), "wb");

    const int FRAMES = 5400;  // 90 s of play
    codec::Encoder enc;
    static uint8_t view[144][160];
    uint16_t viewPal[64];
    int viewN = 0, messages = 0;
    uint64_t streamBytes = 0;
    uint32_t worstMisses = 0, prevMisses = emu::pageMisses();
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < FRAMES; f++) {
        emu::setButtons(script(f));
        bool render = f % 2 == 0;
        if (!emu::runFrame(render)) {
            printf("%s: crashed at frame %d: %s\n", name.c_str(), f, emu::error());
            failures++;
            break;
        }
        uint32_t m = emu::pageMisses() - prevMisses;
        prevMisses = emu::pageMisses();
        if (f > 60 && m > worstMisses) worstMisses = m;
        if (render) {  // the stream sends every other frame (30 fps)
            streamBytes += sendFrame(enc, emu::frame, emu::palette, emu::paletteSize(), view, viewPal, &viewN,
                                     stream, &messages);
            if (memcmp(view, emu::frame, sizeof(view)) != 0) {
                printf("%s: stream differs from the frame at %d\n", name.c_str(), f);
                failures++;
                break;
            }
        }
        if (f % 900 == 0 || f == FRAMES - 2) writePpm("out/" + name + "-" + std::to_string(f) + ".ppm");
    }
    fclose(stream);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%-28s %s %5u KB rom, cache %3u KB (%2u pages of %u KB): %6u misses, %.2f/frame, worst %u/frame | "
           "stream %.1f KB/s | host %.0f fps\n",
           name.substr(0, 28).c_str(), emu::isColor() ? "GBC" : "GB ", emu::romSize() / 1024, cacheKB, emu::cacheSlots(),
           emu::PAGE_SIZE / 1024, emu::pageMisses(), emu::pageMisses() / double(FRAMES), worstMisses,
           streamBytes / 1024.0 / (FRAMES / 59.7), FRAMES / sec);
    emu::unload();
}

int main(int argc, char **argv) {
    testCodec();
    uint32_t cacheKB = argc > 2 ? atoi(argv[2]) : 128;
    if (argc > 1) runGame(argv[1], cacheKB);
    if (failures) printf("%d failure(s)\n", failures);
    else if (argc < 2) printf("codec tests passed\n");
    return failures ? 1 : 0;
}
