// Word-by-word reading of the clean text produced by convertBook(), plus RSVP timing rules.
#pragma once
#include <string>
#include "io.h"

namespace rsvp {

struct Word {
    std::string text;
    uint32_t offset = 0;        // byte offset of the word in the text file
    bool paragraphEnd = false;  // a paragraph break follows this word
};

class WordReader {
public:
    explicit WordReader(Source &src) : src_(src) {}

    // Positions the reader at the word containing / following byte offset pos.
    void seek(uint32_t pos);
    // Reads the next word; false at the end of the book.
    bool next(Word &w);
    // Offset of the word roughly n words before `from` (for rewinding).
    uint32_t findBack(uint32_t from, int n);

    uint32_t size() { return src_.size(); }

private:
    int peek();  // -1 at EOF
    int get();
    bool fill(uint32_t pos);

    Source &src_;
    uint8_t buf_[1024];
    uint32_t bufStart_ = 0;
    uint32_t bufLen_ = 0;
    uint32_t pos_ = 0;
};

// Byte range [start, start+len) of the letter the eye should fix on (optimal recognition point).
void orpRange(const std::string &word, size_t &start, size_t &len);

// How long this word stays on screen relative to 60000/wpm.
float delayFactor(const Word &w);

// Sum of delayFactor over the whole text: reading time in minutes = weight / wpm.
float measureWeight(Source &text, ProgressFn progress);

}  // namespace rsvp
