#include "text.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace rsvp {

// ---------------------------------------------------------------------------
// UTF-8 helpers

static int utf8Length(uint8_t lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;  // continuation or invalid lead byte
}

bool looksLikeUtf8(Source &src) {
    uint8_t buf[1024];
    uint32_t total = 0;
    int need = 0;
    src.seek(0);
    while (total < 65536) {
        size_t n = src.read(buf, sizeof(buf));
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (need) {
                if ((b & 0xC0) != 0x80) return false;
                need--;
            } else {
                int len = utf8Length(b);
                if (len == 0) return false;
                need = len - 1;
            }
        }
        total += n;
    }
    src.seek(0);
    return true;
}

void appendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string decodeEntity(const std::string &name) {
    static const struct {
        const char *name;
        uint16_t cp;
    } NAMED[] = {
        {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
        {"nbsp", 0xA0},    {"shy", 0xAD},     {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
        {"laquo", 0xAB},   {"raquo", 0xBB},   {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
        {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"copy", 0xA9},    {"deg", 0xB0},     {"times", 0xD7},
        {"middot", 0xB7},  {"bull", 0x2022},  {"numero", 0x2116},
    };
    std::string out;
    if (name.size() > 1 && name[0] == '#') {
        uint32_t cp = (name[1] == 'x' || name[1] == 'X') ? strtoul(name.c_str() + 2, nullptr, 16)
                                                          : strtoul(name.c_str() + 1, nullptr, 10);
        if (cp > 0 && cp < 0x110000) appendUtf8(out, cp);
        return out;
    }
    for (auto &e : NAMED) {
        if (name == e.name) {
            appendUtf8(out, e.cp);
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// CleanSink

bool CleanSink::emit(const char *s, size_t n) {
    if (used_ + n > sizeof(buf_) && !flushBuffer()) return false;
    memcpy(buf_ + used_, s, n);
    used_ += n;
    return true;
}

bool CleanSink::flush() {
    endParagraph();
    return flushBuffer();
}

bool CleanSink::flushBuffer() {
    if (!used_) return true;
    bool ok = out_.write(reinterpret_cast<const uint8_t *>(buf_), used_);
    flushed_ += used_;
    used_ = 0;
    return ok;
}

namespace {

std::string trimTitle(std::string t) {
    while (!t.empty() && t.back() == ' ') t.pop_back();
    size_t start = t.find_first_not_of(' ');
    t = start == std::string::npos ? "" : t.substr(start);
    const size_t MAX = 60;
    if (t.size() > MAX) {
        size_t cut = MAX;
        while (cut > 0 && (static_cast<uint8_t>(t[cut]) & 0xC0) == 0x80) cut--;
        t = t.substr(0, cut) + "…";
    }
    return t;
}

bool startsWith(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

// "Глава 5", "ЧАСТЬ ВТОРАЯ", "Chapter 3", "Пролог", "12", "XIV." ...
bool looksLikeChapterTitle(const std::string &t) {
    static const char *WORDS[] = {"Глава", "ГЛАВА", "Часть", "ЧАСТЬ", "Книга", "КНИГА", "Пролог", "ПРОЛОГ",
                                  "Эпилог", "ЭПИЛОГ", "Chapter", "CHAPTER", "Part ", "PART ", "Prologue",
                                  "Epilogue", "Book ", "BOOK "};
    for (auto *w : WORDS)
        if (startsWith(t, w)) return true;
    // a bare number or roman numeral, optionally followed by '.'
    size_t n = t.size();
    if (n && t[n - 1] == '.') n--;
    if (n == 0 || n > 6) return false;
    bool digits = true, roman = true;
    for (size_t i = 0; i < n; i++) {
        digits &= isdigit(static_cast<uint8_t>(t[i])) != 0;
        roman &= strchr("IVXLC", t[i]) != nullptr;
    }
    return (digits && n <= 3) || roman;  // "1920." is a year, not chapter 1920
}

}  // namespace

void CleanSink::endParagraph() {
    if (guessChapters && paraLen_ > 0 && paraLen_ <= 80 && looksLikeChapterTitle(paraText_))
        chapters.push_back({paraStart_, trimTitle(paraText_)});
    paraLen_ = 0;
    paraText_.clear();
}

void CleanSink::beginHeading() {
    headingPending_ = headingActive_ = true;
    headingText_.clear();
}

void CleanSink::endHeading() {
    if (!headingActive_) return;
    headingActive_ = false;
    std::string title = trimTitle(headingText_);
    if (!headingPending_ && !title.empty()) {
        if (!chapters.empty() && chapters.back().offset == headingOffset_) chapters.back().title = title;
        else chapters.push_back({headingOffset_, title});
    }
    headingPending_ = false;
}

void CleanSink::anchor(const std::string &title) {
    anchorPending_ = true;
    anchorTitle_ = title;
}

bool CleanSink::paragraph() {
    pendingPara_ = true;
    return true;
}

bool CleanSink::codepoint(uint32_t cp, const uint8_t *bytes, size_t n) {
    // Invisible characters that only get in the way of word splitting
    if (cp == 0xAD || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF || cp == 0x2060 ||
        cp == '\r' || cp == 0)
        return true;
    if (cp == '\n') {
        newlineRun_++;
        if (newlineIsParagraph || newlineRun_ >= 2) pendingPara_ = true;
        else pendingSpace_ = true;
        return true;
    }
    if (cp == 0x2028 || cp == 0x2029) {
        pendingPara_ = true;
        return true;
    }
    if (cp == ' ' || cp == '\t' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
        cp == 0x3000) {
        pendingSpace_ = true;
        return true;
    }
    newlineRun_ = 0;
    if (!atStart_) {
        if (pendingPara_) {
            endParagraph();
            if (!emit("\n", 1)) return false;
            if (headingActive_ && !headingPending_) headingText_ += ' ';
        } else if (pendingSpace_) {
            if (!emit(" ", 1)) return false;
            if (headingActive_ && !headingPending_) headingText_ += ' ';
            if (paraLen_ && paraText_.size() < 100) paraText_ += ' ';
            paraLen_++;
        }
    }
    pendingPara_ = pendingSpace_ = false;
    atStart_ = false;

    if (paraLen_ == 0) paraStart_ = position();
    if (paraText_.size() < 100) paraText_.append(reinterpret_cast<const char *>(bytes), n);
    paraLen_ += n;
    if (headingPending_) {
        headingPending_ = false;
        headingOffset_ = position();
    }
    if (headingActive_ && headingText_.size() < 120) headingText_.append(reinterpret_cast<const char *>(bytes), n);
    if (anchorPending_) {
        anchorPending_ = false;
        anchors.push_back({position(), anchorTitle_});
    }
    return emit(reinterpret_cast<const char *>(bytes), n);
}

bool CleanSink::write(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (seqNeed_) {
            if ((b & 0xC0) == 0x80) {
                seq_[seqLen_++] = b;
                if (seqLen_ < seqNeed_) continue;
                uint32_t cp = seq_[0] & (0xFF >> (seqNeed_ + 1));
                for (int k = 1; k < seqNeed_; k++) cp = (cp << 6) | (seq_[k] & 0x3F);
                uint8_t n = seqLen_;
                seqNeed_ = seqLen_ = 0;
                if (!codepoint(cp, seq_, n)) return false;
                continue;
            }
            seqNeed_ = seqLen_ = 0;  // broken sequence: drop it and handle b normally
        }
        int len8 = utf8Length(b);
        if (len8 == 1) {
            if (!codepoint(b, &b, 1)) return false;
        } else if (len8 > 1) {
            seq_[0] = b;
            seqLen_ = 1;
            seqNeed_ = len8;
        }
        // stray continuation bytes are dropped
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cp1251Sink

static const uint16_t CP1251_HIGH[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A,
    0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x003F, 0x2122,
    0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6,
    0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456,
    0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

bool Cp1251Sink::write(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b < 0x80) out += static_cast<char>(b);
        else if (b < 0xC0) appendUtf8(out, CP1251_HIGH[b - 0x80]);
        else appendUtf8(out, 0x410 + (b - 0xC0));
    }
    return out_.write(reinterpret_cast<const uint8_t *>(out.data()), out.size());
}

// ---------------------------------------------------------------------------
// MarkupSink

bool MarkupSink::isBlock(const std::string &n) const {
    static const char *HTML[] = {"p",  "div", "br", "h1", "h2", "h3",      "h4",      "h5",  "h6", "li",
                                 "tr", "dd",  "dt", "hr", "pre", "section", "article", "blockquote"};
    static const char *FB2[] = {"p",      "v",       "title", "subtitle",   "epigraph", "stanza",
                                "section", "cite",   "poem",  "empty-line", "text-author"};
    if (flavor_ == Flavor::Fb2) {
        for (auto *t : FB2)
            if (n == t) return true;
    } else {
        for (auto *t : HTML)
            if (n == t) return true;
    }
    return false;
}

bool MarkupSink::isHeading(const std::string &n) const {
    if (flavor_ == Flavor::Fb2) return n == "title";
    return n == "h1" || n == "h2" || n == "h3";
}

bool MarkupSink::isSkipped(const std::string &n) const {
    if (flavor_ == Flavor::Fb2) return n == "binary" || n == "description";
    return n == "head" || n == "style" || n == "script";
}

void MarkupSink::endTag() {
    size_t i = 0;
    bool closing = false;
    if (i < tag_.size() && tag_[i] == '/') {
        closing = true;
        i++;
    }
    std::string name;
    while (i < tag_.size() && !isspace(static_cast<uint8_t>(tag_[i])) && tag_[i] != '/' && tag_[i] != '>') {
        name += static_cast<char>(tolower(static_cast<uint8_t>(tag_[i])));
        i++;
    }
    size_t colon = name.find(':');  // xhtml:p, fb:p ...
    if (colon != std::string::npos) name = name.substr(colon + 1);
    bool selfClosing = !tag_.empty() && tag_.back() == '/';

    if (!skipUntil_.empty()) {
        if (closing && name == skipUntil_) skipUntil_.clear();
        return;
    }
    if (!closing && !selfClosing && isSkipped(name)) {
        skipUntil_ = name;
        return;
    }
    if (isHeading(name)) {
        out_.paragraph();
        if (closing) out_.endHeading();
        else if (!selfClosing) out_.beginHeading();
        return;
    }
    if (isBlock(name)) out_.paragraph();
}

bool MarkupSink::write(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = static_cast<char>(data[i]);
        switch (state_) {
            case State::Text:
                if (c == '<') {
                    state_ = State::Tag;
                    tag_.clear();
                    quote_ = 0;
                } else if (c == '&') {
                    state_ = State::Entity;
                    entity_.clear();
                } else if (skipUntil_.empty()) {
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (!out_.put(c)) return false;
                }
                break;

            case State::Tag:
                if (quote_) {
                    if (c == quote_) quote_ = 0;
                } else if (c == '>') {
                    endTag();
                    state_ = State::Text;
                    break;
                } else if ((c == '"' || c == '\'') && tag_.find('=') != std::string::npos) {
                    quote_ = c;
                }
                if (tag_.size() < 256) tag_ += c;
                if (tag_ == "!--") {
                    state_ = State::Comment;
                    commentDashes_ = 0;
                }
                break;

            case State::Comment:
                if (c == '-') commentDashes_++;
                else if (c == '>' && commentDashes_ >= 2) state_ = State::Text;
                else commentDashes_ = 0;
                break;

            case State::Entity:
                if (c == ';') {
                    std::string text = decodeEntity(entity_);
                    if (skipUntil_.empty() && !text.empty() &&
                        !out_.write(reinterpret_cast<const uint8_t *>(text.data()), text.size()))
                        return false;
                    state_ = State::Text;
                } else if (entity_.size() > 10 || c == ' ' || c == '<' || c == '&' || c == '\n') {
                    // not an entity after all: emit it literally and reprocess this char as text
                    if (skipUntil_.empty()) {
                        out_.put('&');
                        out_.write(reinterpret_cast<const uint8_t *>(entity_.data()), entity_.size());
                    }
                    state_ = State::Text;
                    i--;
                } else {
                    entity_ += c;
                }
                break;
        }
    }
    return true;
}

}  // namespace rsvp
