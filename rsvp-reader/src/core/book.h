// Turns .txt / .epub / .fb2 into one clean UTF-8 text stream: words separated by spaces,
// paragraphs by '\n'. The reader then only ever deals with that format.
#pragma once
#include <string>
#include "io.h"
#include "text.h"

namespace rsvp {

enum class Format { Txt, Epub, Fb2, Unknown };

Format formatFromName(const std::string &name);

// Converts a book to clean text. title receives the book title when the file has one
// (EPUB dc:title, FB2 book-title); otherwise it is left untouched. chapters receives the chapter
// starts found (headings, or guessed "Глава N" lines in plain text).
// Returns an empty string on success, otherwise a short error message.
std::string convertBook(Format format, Source &src, Sink &out, std::string &title, std::vector<Chapter> &chapters,
                        ProgressFn progress);

}  // namespace rsvp
