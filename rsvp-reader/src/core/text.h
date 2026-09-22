// Text clean-up filters: encoding detection, cp1251 -> UTF-8, HTML/XML -> plain text.
#pragma once
#include <string>
#include <vector>
#include "io.h"

namespace rsvp {

struct Chapter {
    uint32_t offset;    // byte offset of the chapter heading in the clean text
    std::string title;
};

// True if the first 64 KB of the source are valid UTF-8 (a sequence cut at the end is fine).
bool looksLikeUtf8(Source &src);

// Appends the UTF-8 encoding of a code point.
void appendUtf8(std::string &out, uint32_t cp);

// Decodes "&amp;", "&#1025;", "&#x401;" ... (name without '&' and ';'). Empty string if unknown.
std::string decodeEntity(const std::string &name);

// Collapses whitespace, drops soft hyphens / zero-width chars and \r, keeps single '\n' as
// paragraph marks. Everything written to a book cache goes through this.
class CleanSink : public Sink {
public:
    explicit CleanSink(Sink &out) : out_(out) {}
    bool write(const uint8_t *data, size_t len) override;
    bool paragraph();  // request a paragraph break
    bool flush();
    // Plain text: true when every line break is a paragraph (one line per paragraph),
    // false when lines are hard-wrapped and only blank lines separate paragraphs.
    bool newlineIsParagraph = true;
    // Plain text has no markup: guess chapters from short paragraphs like "Глава 5" or "IV".
    bool guessChapters = false;

    // Markup headings (<h1>, FB2 <title>): the text between begin and end becomes a chapter.
    void beginHeading();
    void endHeading();
    // Fallback chapter starting at the next visible character (EPUB spine files).
    void anchor(const std::string &title);

    std::vector<Chapter> chapters;  // from headings / guesses
    std::vector<Chapter> anchors;   // from anchor()

private:
    bool codepoint(uint32_t cp, const uint8_t *bytes, size_t n);
    bool emit(const char *s, size_t n);
    uint32_t position() const { return flushed_ + used_; }
    bool flushBuffer();
    void endParagraph();
    uint32_t flushed_ = 0;
    bool headingPending_ = false, headingActive_ = false;
    uint32_t headingOffset_ = 0;
    std::string headingText_;
    bool anchorPending_ = false;
    std::string anchorTitle_;
    uint32_t paraStart_ = 0;
    std::string paraText_;  // first bytes of the current paragraph (for guessChapters)
    uint32_t paraLen_ = 0;
    Sink &out_;
    bool pendingSpace_ = false;
    bool pendingPara_ = false;
    bool atStart_ = true;
    uint8_t seq_[4];  // UTF-8 sequence being assembled
    uint8_t seqLen_ = 0;
    uint8_t seqNeed_ = 0;
    int newlineRun_ = 0;
    char buf_[512];
    size_t used_ = 0;
};

// Windows-1251 bytes in, UTF-8 out.
class Cp1251Sink : public Sink {
public:
    explicit Cp1251Sink(Sink &out) : out_(out) {}
    bool write(const uint8_t *data, size_t len) override;

private:
    Sink &out_;
};

// Streams (X)HTML or FB2 markup in, visible text out (to a CleanSink).
class MarkupSink : public Sink {
public:
    enum class Flavor { Html, Fb2 };
    MarkupSink(CleanSink &out, Flavor flavor) : out_(out), flavor_(flavor) {}
    bool write(const uint8_t *data, size_t len) override;

private:
    enum class State { Text, Tag, Entity, Comment };
    void endTag();
    bool isHeading(const std::string &name) const;
    bool isBlock(const std::string &name) const;
    bool isSkipped(const std::string &name) const;

    CleanSink &out_;
    Flavor flavor_;
    State state_ = State::Text;
    std::string tag_;     // raw tag text between '<' and '>'
    std::string entity_;  // entity name between '&' and ';'
    char quote_ = 0;      // quote char while inside an attribute value
    std::string skipUntil_;  // closing tag that ends a skipped element (head, style, binary ...)
    int commentDashes_ = 0;
};

}  // namespace rsvp
