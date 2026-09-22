// Where books live: the SD card if there is one, otherwise the "books" flash partition.
// Layout on either:  /books/<original files>   /rsvp/<id>.txt (clean text)  .m (title)  .p (position)
#pragma once
#include <FS.h>

#include <string>
#include <vector>

#include "core/book.h"
#include "core/rsvp.h"
#include "core/io.h"

namespace storage {

bool begin();          // mounts SD or internal flash
bool available();
bool onSd();
fs::FS &fs();
uint64_t freeBytes();

struct Book {
    String file;   // name inside /books
    String id;     // stable id derived from the file name
    String title;  // from the book metadata, else the file name
    rsvp::Format format;
    int percent;   // reading progress, -1 = not opened yet
};

std::vector<Book> listBooks();
String textPath(const Book &b);  // clean text cache
bool isPrepared(const Book &b);
// Converts the original into the clean text cache. Empty string = ok, else an error to show.
// progress gets 0..100 for the conversion, then 100..200 while measuring reading time.
String prepare(Book &b, rsvp::ProgressFn progress);
void removeBook(const Book &b);

std::vector<rsvp::Chapter> loadChapters(const Book &b);
float loadWeight(const Book &b);  // sum of word delay factors: minutes = weight / wpm

uint32_t loadPosition(const Book &b);
void savePosition(const Book &b, uint32_t offset);
int loadWpm();
void saveWpm(int wpm);
int loadBrightness();  // level index
void saveBrightness(int level);

// Makes an uploaded file name safe for /books (strips paths, limits length for LittleFS).
String safeName(const String &name);

// rsvp::Source / Sink over an Arduino File
struct FileSource : rsvp::Source {
    File f;
    explicit FileSource(File file) : f(file) {}
    size_t read(uint8_t *b, size_t n) override { return f.read(b, n); }
    bool seek(uint32_t p) override { return f.seek(p); }
    uint32_t size() override { return f.size(); }
};

struct FileSink : rsvp::Sink {
    File f;
    explicit FileSink(File file) : f(file) {}
    bool write(const uint8_t *d, size_t n) override { return f.write(d, n) == n; }
};

}  // namespace storage
