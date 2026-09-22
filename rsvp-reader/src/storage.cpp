#include "storage.h"

#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include <algorithm>

namespace storage {

namespace {

// Cardputer / Cardputer ADV microSD wiring (same as M5Launcher's board config)
constexpr int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;
constexpr const char *FLASH_PARTITION = "books";  // created by the installer image

SPIClass sdSpi(HSPI);
fs::FS *current = nullptr;
bool sd = false;

uint32_t crc32(const char *s) {
    uint32_t crc = 0xFFFFFFFF;
    for (; *s; s++) {
        crc ^= static_cast<uint8_t>(*s);
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

String readSmall(const String &path) {
    File f = current->open(path, "r");
    if (!f) return "";
    String s = f.readString();
    f.close();
    return s;
}

void writeSmall(const String &path, const String &value) {
    File f = current->open(path, "w");
    if (!f) return;
    f.print(value);
    f.close();
}

String base(const Book &b) { return "/rsvp/" + b.id; }

}  // namespace

bool begin() {
    sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (SD.begin(SD_CS, sdSpi, 20000000)) {
        current = &SD;
        sd = true;
    } else if (LittleFS.begin(true, "/littlefs", 5, FLASH_PARTITION)) {
        current = &LittleFS;
        sd = false;
    } else {
        current = nullptr;
        return false;
    }
    if (!current->exists("/books")) current->mkdir("/books");
    if (!current->exists("/rsvp")) current->mkdir("/rsvp");
    return true;
}

bool available() { return current != nullptr; }
bool onSd() { return sd; }
fs::FS &fs() { return *current; }

uint64_t freeBytes() {
    if (!current) return 0;
    if (sd) return SD.totalBytes() - SD.usedBytes();
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

std::vector<Book> listBooks() {
    std::vector<Book> books;
    if (!current) return books;
    File dir = current->open("/books");
    if (!dir) return books;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        f.close();
        if (name.startsWith(".")) continue;  // macOS ._ files etc.
        rsvp::Format fmt = rsvp::formatFromName(name.c_str());
        if (fmt == rsvp::Format::Unknown) continue;

        Book b;
        b.file = name;
        b.format = fmt;
        char id[9];
        snprintf(id, sizeof(id), "%08lx", static_cast<unsigned long>(crc32(name.c_str())));
        b.id = id;
        b.title = readSmall(base(b) + ".m");
        if (b.title.isEmpty()) b.title = name.substring(0, name.lastIndexOf('.'));
        b.percent = -1;
        if (current->exists(textPath(b))) {
            File t = current->open(textPath(b), "r");
            uint32_t size = t ? t.size() : 0;
            if (t) t.close();
            String pos = readSmall(base(b) + ".p");
            if (!pos.isEmpty() && size) b.percent = static_cast<int>(pos.toInt() * 100ULL / size);
        }
        books.push_back(b);
    }
    dir.close();
    std::sort(books.begin(), books.end(), [](const Book &a, const Book &b) { return a.title < b.title; });
    return books;
}

String textPath(const Book &b) { return base(b) + ".txt"; }

// .s (reading time) is written last, so it marks a finished preparation; books prepared by an
// older version without it get prepared again (same text, so saved positions stay valid).
bool isPrepared(const Book &b) { return current && current->exists(textPath(b)) && current->exists(base(b) + ".s"); }

String prepare(Book &b, rsvp::ProgressFn progress) {
    String tmp = base(b) + ".tmp";
    String err;
    {
        File in = current->open("/books/" + b.file, "r");
        if (!in) return "Не открывается файл";
        File out = current->open(tmp, "w");
        if (!out) {
            in.close();
            return "Нет места для книги";
        }
        FileSource src(in);
        FileSink sink(out);
        std::string title;
        std::vector<rsvp::Chapter> chapters;
        std::string e = rsvp::convertBook(b.format, src, sink, title, chapters, progress);
        in.close();
        out.close();
        err = e.c_str();
        if (err.isEmpty() && !title.empty()) {
            b.title = title.c_str();
            writeSmall(base(b) + ".m", b.title);
        }
        if (err.isEmpty()) {
            File cf = current->open(base(b) + ".c", "w");
            if (cf) {
                for (auto &c : chapters) cf.printf("%u\t%s\n", static_cast<unsigned>(c.offset), c.title.c_str());
                cf.close();
            }
        }
    }
    if (!err.isEmpty()) {
        current->remove(tmp);
        if (err.startsWith("Write error")) return "Не хватило места";
        return err;
    }
    current->remove(textPath(b));
    current->rename(tmp, textPath(b));

    static rsvp::ProgressFn outer;  // second phase reports 100..200
    outer = progress;
    File t = current->open(textPath(b), "r");
    if (!t) return "Не открывается текст";
    FileSource text(t);
    float weight = rsvp::measureWeight(text, [](int pct) {
        if (outer) outer(100 + pct);
    });
    t.close();
    writeSmall(base(b) + ".s", String(weight, 1));
    return "";
}

std::vector<rsvp::Chapter> loadChapters(const Book &b) {
    std::vector<rsvp::Chapter> out;
    File f = current->open(base(b) + ".c", "r");
    if (!f) return out;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        int tab = line.indexOf('\t');
        if (tab <= 0) continue;
        out.push_back({static_cast<uint32_t>(line.substring(0, tab).toInt()), line.substring(tab + 1).c_str()});
    }
    f.close();
    return out;
}

float loadWeight(const Book &b) { return readSmall(base(b) + ".s").toFloat(); }

void removeBook(const Book &b) {
    current->remove("/books/" + b.file);
    current->remove(textPath(b));
    current->remove(base(b) + ".m");
    current->remove(base(b) + ".p");
    current->remove(base(b) + ".c");
    current->remove(base(b) + ".s");
}

uint32_t loadPosition(const Book &b) { return readSmall(base(b) + ".p").toInt(); }

void savePosition(const Book &b, uint32_t offset) { writeSmall(base(b) + ".p", String(offset)); }

int loadWpm() {
    if (!current) return 300;
    int wpm = readSmall("/rsvp/wpm").toInt();
    return (wpm >= 100 && wpm <= 1000) ? wpm : 300;
}

void saveWpm(int wpm) {
    if (current) writeSmall("/rsvp/wpm", String(wpm));
}

int loadBrightness() {
    if (!current || !current->exists("/rsvp/bright")) return 2;
    return readSmall("/rsvp/bright").toInt();
}

void saveBrightness(int level) {
    if (current) writeSmall("/rsvp/bright", String(level));
}

String safeName(const String &name) {
    String n = name;
    int slash = std::max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
    if (slash >= 0) n = n.substring(slash + 1);
    for (const char *bad = ":*?\"<>|"; *bad; bad++) n.replace(String(*bad), "_");
    // LittleFS allows 64 bytes per name: keep the extension, cut the rest at a UTF-8 boundary
    const unsigned MAX = 60;
    if (n.length() > MAX) {
        int dot = n.lastIndexOf('.');
        String ext = dot >= 0 ? n.substring(dot) : "";
        unsigned keep = MAX - ext.length();
        while (keep > 0 && (static_cast<uint8_t>(n[keep]) & 0xC0) == 0x80) keep--;
        n = n.substring(0, keep) + ext;
    }
    return n;
}

}  // namespace storage
