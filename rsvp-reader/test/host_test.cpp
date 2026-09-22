// Runs the book pipeline on the Mac: convert a book, then read it word by word like the device does.
// build: see test/run_tests.sh
#include <stdio.h>

#include <string>

#include "../src/core/book.h"
#include "../src/core/rsvp.h"

using namespace rsvp;

struct FileSource : Source {
    FILE *f;
    explicit FileSource(const char *path) : f(fopen(path, "rb")) {}
    ~FileSource() override {
        if (f) fclose(f);
    }
    size_t read(uint8_t *b, size_t n) override { return fread(b, 1, n, f); }
    bool seek(uint32_t p) override { return fseek(f, p, SEEK_SET) == 0; }
    uint32_t size() override {
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        long s = ftell(f);
        fseek(f, cur, SEEK_SET);
        return static_cast<uint32_t>(s);
    }
};

struct FileSink : Sink {
    FILE *f;
    explicit FileSink(const char *path) : f(fopen(path, "wb")) {}
    ~FileSink() override {
        if (f) fclose(f);
    }
    bool write(const uint8_t *d, size_t n) override { return fwrite(d, 1, n, f) == n; }
};

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: host_test <book> <out.txt>\n");
        return 2;
    }
    std::string title = "(no title)";
    std::vector<Chapter> chapters;
    {
        FileSource src(argv[1]);
        FileSink out(argv[2]);
        if (!src.f || !out.f) return 2;
        std::string err = convertBook(formatFromName(argv[1]), src, out, title, chapters, nullptr);
        if (!err.empty()) {
            printf("ERROR: %s\n", err.c_str());
            return 1;
        }
    }
    FileSource text(argv[2]);
    printf("chapters: %zu\n", chapters.size());
    for (size_t i = 0; i < chapters.size(); i++) {
        if (i < 6 || i + 3 >= chapters.size()) {
            // the heading text must really sit at that offset
            char buf[48] = {0};
            text.seek(chapters[i].offset);
            text.read(reinterpret_cast<uint8_t *>(buf), 40);
            printf("  @%u %s   | text there: %.30s\n", chapters[i].offset, chapters[i].title.c_str(), buf);
        } else if (i == 6) printf("  ...\n");
    }
    printf("weight: %.0f (%.1f min at 400wpm)\n", measureWeight(text, nullptr), measureWeight(text, nullptr) / 400);
    WordReader reader(text);
    Word w;
    int words = 0, paragraphs = 0;
    std::string sample;
    double ms = 0;
    uint32_t mid = 0;
    while (reader.next(w)) {
        if (words < 40) {
            size_t s, l;
            orpRange(w.text, s, l);
            sample += w.text.substr(0, s) + "[" + w.text.substr(s, l) + "]" + w.text.substr(s + l) +
                      (w.paragraphEnd ? " ¶\n" : " ");
        }
        if (words == 5000) mid = w.offset;
        ms += delayFactor(w) * 60000.0 / 400;
        words++;
        if (w.paragraphEnd) paragraphs++;
    }
    printf("title: %s\nwords: %d  paragraphs: %d  text bytes: %u  minutes@400wpm: %.1f\n", title.c_str(), words,
           paragraphs, text.size(), ms / 60000);
    printf("--- first words ([ORP], ¶ = paragraph end)\n%s\n", sample.c_str());

    if (mid) {  // resume + rewind check
        reader.seek(mid + 3);  // land in the middle of a word
        reader.next(w);
        printf("--- seek(mid+3) -> offset %u (expected %u): %s\n", w.offset, mid, w.text.c_str());
        uint32_t back = reader.findBack(mid, 10);
        reader.seek(back);
        std::string ten;
        for (int i = 0; i < 11 && reader.next(w); i++) ten += w.text + " ";
        printf("--- 10 words back from mid: %s\n", ten.c_str());
    }
    return 0;
}
