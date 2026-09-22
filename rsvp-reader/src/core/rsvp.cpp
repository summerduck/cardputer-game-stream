#include "rsvp.h"

#include <ctype.h>

namespace rsvp {

namespace {

constexpr size_t MAX_WORD_BYTES = 64;

bool isSpace(int c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Decodes the code point starting at s[i] and returns its byte length.
size_t decode(const std::string &s, size_t i, uint32_t &cp) {
    uint8_t b = static_cast<uint8_t>(s[i]);
    size_t n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    cp = n == 1 ? b : b & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; k++) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    return n;
}

bool isLetter(uint32_t cp) {
    if (cp < 0x80) return isalnum(static_cast<int>(cp));
    if (cp >= 0x2000 && cp <= 0x206F) return false;  // general punctuation: — – … “ ” „
    return cp != 0xAB && cp != 0xBB && cp != 0xA0 && cp != 0xB0 && cp != 0xB7 && cp != 0xD7;
}

}  // namespace

// ---------------------------------------------------------------------------
// WordReader

bool WordReader::fill(uint32_t pos) {
    if (pos >= bufStart_ && pos < bufStart_ + bufLen_) return true;
    // moving backwards: load the window that ends at pos so a backward scan doesn't re-read per byte
    uint32_t start = (pos < bufStart_ && pos >= sizeof(buf_) - 1) ? pos - (sizeof(buf_) - 1) : pos < bufStart_ ? 0 : pos;
    if (!src_.seek(start)) return false;
    bufStart_ = start;
    bufLen_ = src_.read(buf_, sizeof(buf_));
    return pos < bufStart_ + bufLen_;
}

int WordReader::peek() { return fill(pos_) ? buf_[pos_ - bufStart_] : -1; }

int WordReader::get() {
    int c = peek();
    if (c >= 0) pos_++;
    return c;
}

void WordReader::seek(uint32_t pos) {
    uint32_t sz = src_.size();
    pos_ = pos > sz ? sz : pos;
    if (pos_ == 0) return;
    // Back up to the start of the word we landed in
    while (pos_ > 0) {
        pos_--;
        if (isSpace(peek())) {
            pos_++;
            break;
        }
    }
}

bool WordReader::next(Word &w) {
    while (isSpace(peek())) get();
    if (peek() < 0) return false;
    w.text.clear();
    w.offset = pos_;
    w.paragraphEnd = false;
    for (int c = peek(); c >= 0 && !isSpace(c); c = peek()) {
        // cut very long "words" (URLs, garbage) at a character boundary
        if (w.text.size() >= MAX_WORD_BYTES && (c & 0xC0) != 0x80) break;
        w.text += static_cast<char>(get());
    }
    for (int c = peek(); isSpace(c); c = peek()) {
        if (c == '\n') w.paragraphEnd = true;
        get();
    }
    return true;
}

uint32_t WordReader::findBack(uint32_t from, int n) {
    uint32_t saved = pos_;
    auto byteBefore = [this](uint32_t p) {
        pos_ = p - 1;
        return peek();
    };
    uint32_t p = from;
    for (int i = 0; i < n && p > 0; i++) {
        while (p > 0 && isSpace(byteBefore(p))) p--;
        while (p > 0 && !isSpace(byteBefore(p))) p--;
    }
    pos_ = saved;
    return p;
}

// ---------------------------------------------------------------------------
// ORP + timing

void orpRange(const std::string &word, size_t &start, size_t &len) {
    // letters only: leading/trailing quotes, brackets and punctuation don't count
    size_t first = word.size(), letters = 0;
    for (size_t i = 0; i < word.size();) {
        uint32_t cp;
        size_t n = decode(word, i, cp);
        if (isLetter(cp)) {
            if (first == word.size()) first = i;
            letters++;
        }
        i += n;
    }
    if (letters == 0) {  // a lone dash or symbol
        uint32_t cp;
        start = 0;
        len = word.empty() ? 0 : decode(word, 0, cp);
        return;
    }
    size_t pivot = letters <= 1 ? 0 : letters <= 5 ? 1 : letters <= 9 ? 2 : letters <= 13 ? 3 : 4;
    size_t idx = 0;
    for (size_t i = first; i < word.size();) {
        uint32_t cp;
        size_t n = decode(word, i, cp);
        if (isLetter(cp)) {
            if (idx == pivot) {
                start = i;
                len = n;
                return;
            }
            idx++;
        }
        i += n;
    }
    start = first;
    len = 1;
}

float delayFactor(const Word &w) {
    const std::string &s = w.text;
    // find the last char that is not a closing quote / bracket
    size_t end = s.size();
    while (end > 0) {
        size_t i = end - 1;
        while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) i--;
        uint32_t cp;
        decode(s, i, cp);
        if (cp == '"' || cp == '\'' || cp == ')' || cp == ']' || cp == 0xBB || cp == 0x201D || cp == 0x2019) {
            end = i;
            continue;
        }
        break;
    }
    float f = 1.0f;
    if (end > 0) {
        size_t i = end - 1;
        while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) i--;
        uint32_t cp;
        decode(s, i, cp);
        if (cp == '.' || cp == '!' || cp == '?' || cp == 0x2026) f = 2.2f;
        else if (cp == ',' || cp == ';' || cp == ':') f = 1.5f;
    }
    size_t letters = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp;
        i += decode(s, i, cp);
        if (isLetter(cp)) letters++;
    }
    if (letters >= 13) f *= 1.6f;
    else if (letters >= 9) f *= 1.3f;
    if (w.paragraphEnd && f < 2.8f) f = 2.8f;
    return f;
}

float measureWeight(Source &text, ProgressFn progress) {
    WordReader reader(text);
    Word w;
    float weight = 0;
    uint32_t size = text.size(), n = 0;
    int last = -1;
    while (reader.next(w)) {
        weight += delayFactor(w);
        if (progress && (++n & 1023) == 0 && size) {
            int pct = static_cast<int>(static_cast<uint64_t>(w.offset) * 100 / size);
            if (pct != last) progress(last = pct);
        }
    }
    return weight;
}

}  // namespace rsvp
