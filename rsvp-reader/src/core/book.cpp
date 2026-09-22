#include "book.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <vector>

#include "text.h"
#include "tinfl_port.h"

namespace rsvp {

namespace {

// ---------------------------------------------------------------------------
// small string helpers

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(tolower(static_cast<uint8_t>(c)));
    return s;
}

bool endsWith(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Value of attr="..." (or '...') inside one tag's text, empty if absent.
std::string attr(const std::string &tag, const char *name) {
    std::string key = std::string(name) + "=";
    size_t pos = 0;
    while ((pos = tag.find(key, pos)) != std::string::npos) {
        bool boundary = pos == 0 || isspace(static_cast<uint8_t>(tag[pos - 1]));
        pos += key.size();
        if (!boundary || pos >= tag.size()) continue;
        char q = tag[pos];
        if (q != '"' && q != '\'') continue;
        size_t end = tag.find(q, pos + 1);
        if (end == std::string::npos) return "";
        return tag.substr(pos + 1, end - pos - 1);
    }
    return "";
}

// Text inside the first <name ...>...</name> (a namespace prefix like dc: is allowed), entities decoded.
std::string elementText(const std::string &xml, const char *name) {
    for (size_t lt = xml.find('<'); lt != std::string::npos; lt = xml.find('<', lt + 1)) {
        size_t end = xml.find_first_of(" \t\r\n/>", lt + 1);
        if (end == std::string::npos) return "";
        std::string tag = xml.substr(lt + 1, end - lt - 1);
        size_t colon = tag.find(':');
        if (colon != std::string::npos) tag = tag.substr(colon + 1);
        if (tag != name) continue;
        size_t gt = xml.find('>', end);
        size_t close = gt == std::string::npos ? gt : xml.find('<', gt);
        if (close == std::string::npos) return "";
        std::string raw = xml.substr(gt + 1, close - gt - 1), out;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '&') {
                size_t semi = raw.find(';', i);
                if (semi != std::string::npos && semi - i < 12) {
                    out += decodeEntity(raw.substr(i + 1, semi - i - 1));
                    i = semi;
                    continue;
                }
            }
            out += (raw[i] == '\n' || raw[i] == '\r' || raw[i] == '\t') ? ' ' : raw[i];
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        while (!out.empty() && out[0] == ' ') out.erase(0, 1);
        return out;
    }
    return "";
}

std::string urlDecode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<uint8_t>(s[i + 1])) &&
            isxdigit(static_cast<uint8_t>(s[i + 2]))) {
            out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Joins an href relative to baseDir ("OEBPS/") and resolves "./" and "../".
std::string resolvePath(const std::string &baseDir, std::string href) {
    size_t hash = href.find('#');
    if (hash != std::string::npos) href = href.substr(0, hash);
    std::string full = urlDecode(href[0] == '/' ? href.substr(1) : baseDir + href);
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= full.size()) {
        size_t slash = full.find('/', start);
        std::string part = full.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) out += (i ? "/" : "") + parts[i];
    return out;
}

uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// ---------------------------------------------------------------------------
// ZIP (just enough for EPUB: stored + deflate, no zip64)

struct ZipEntry {
    std::string name;
    uint16_t method;
    uint32_t compSize, size, localOffset;
};

class Zip {
public:
    explicit Zip(Source &src) : src_(src) {}

    bool open() {
        uint32_t fileSize = src_.size();
        if (fileSize < 22) return false;
        uint32_t tail = fileSize < 65557 ? fileSize : 65557;
        std::vector<uint8_t> buf(tail);
        src_.seek(fileSize - tail);
        if (src_.read(buf.data(), tail) != tail) return false;
        int eocd = -1;
        for (int i = static_cast<int>(tail) - 22; i >= 0; i--) {
            if (le32(&buf[i]) == 0x06054b50) {
                eocd = i;
                break;
            }
        }
        if (eocd < 0) return false;
        uint16_t count = le16(&buf[eocd + 10]);
        uint32_t cdSize = le32(&buf[eocd + 12]), cdOffset = le32(&buf[eocd + 16]);
        if (cdOffset + cdSize > fileSize) return false;

        std::vector<uint8_t> cd(cdSize);
        src_.seek(cdOffset);
        if (src_.read(cd.data(), cdSize) != cdSize) return false;
        size_t p = 0;
        for (uint16_t i = 0; i < count && p + 46 <= cdSize; i++) {
            if (le32(&cd[p]) != 0x02014b50) return false;
            ZipEntry e;
            e.method = le16(&cd[p + 10]);
            e.compSize = le32(&cd[p + 20]);
            e.size = le32(&cd[p + 24]);
            uint16_t nameLen = le16(&cd[p + 28]), extraLen = le16(&cd[p + 30]), commentLen = le16(&cd[p + 32]);
            e.localOffset = le32(&cd[p + 42]);
            if (p + 46 + nameLen > cdSize) return false;
            e.name.assign(reinterpret_cast<const char *>(&cd[p + 46]), nameLen);
            entries_.push_back(e);
            p += 46 + nameLen + extraLen + commentLen;
        }
        return true;
    }

    const ZipEntry *find(const std::string &name) const {
        for (auto &e : entries_)
            if (e.name == name) return &e;
        std::string l = lower(name);  // some EPUBs get the case of hrefs wrong
        for (auto &e : entries_)
            if (lower(e.name) == l) return &e;
        return nullptr;
    }

    // Streams the uncompressed entry into out.
    bool extract(const ZipEntry &e, Sink &out) {
        uint8_t local[30];
        src_.seek(e.localOffset);
        if (src_.read(local, 30) != 30 || le32(local) != 0x04034b50) return false;
        uint32_t dataStart = e.localOffset + 30 + le16(local + 26) + le16(local + 28);
        src_.seek(dataStart);

        const size_t IN_SIZE = 4096;
        std::unique_ptr<uint8_t[]> in(new (std::nothrow) uint8_t[IN_SIZE]);
        if (!in) return false;

        if (e.method == 0) {
            uint32_t left = e.compSize;
            while (left) {
                size_t n = src_.read(in.get(), left < IN_SIZE ? left : IN_SIZE);
                if (!n || !out.write(in.get(), n)) return false;
                left -= n;
            }
            return true;
        }
        if (e.method != 8) return false;

        std::unique_ptr<tinfl_decompressor> inflator(new (std::nothrow) tinfl_decompressor);
        std::unique_ptr<uint8_t[]> dict(new (std::nothrow) uint8_t[TINFL_LZ_DICT_SIZE]);
        if (!inflator || !dict) return false;
        tinfl_init(inflator.get());

        uint32_t remaining = e.compSize;
        size_t inPos = 0, inAvail = 0, dictOfs = 0;
        for (;;) {
            if (inAvail == 0 && remaining) {
                size_t n = src_.read(in.get(), remaining < IN_SIZE ? remaining : IN_SIZE);
                if (!n) return false;
                remaining -= n;
                inPos = 0;
                inAvail = n;
            }
            size_t inBytes = inAvail, outBytes = TINFL_LZ_DICT_SIZE - dictOfs;
            tinfl_status status = tinfl_decompress(inflator.get(), in.get() + inPos, &inBytes, dict.get(),
                                                   dict.get() + dictOfs, &outBytes,
                                                   remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            inPos += inBytes;
            inAvail -= inBytes;
            if (outBytes && !out.write(dict.get() + dictOfs, outBytes)) return false;
            dictOfs = (dictOfs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
            if (status == TINFL_STATUS_DONE) return true;
            if (status < TINFL_STATUS_DONE) return false;
            if (status == TINFL_STATUS_NEEDS_MORE_INPUT && inAvail == 0 && remaining == 0) return false;
        }
    }

    // Small entries (container.xml, OPF) into memory.
    bool readAll(const ZipEntry &e, std::string &out, uint32_t maxSize = 192 * 1024) {  // fits the ESP32 heap
        if (e.size > maxSize) return false;
        struct StringSink : Sink {
            std::string &s;
            explicit StringSink(std::string &str) : s(str) {}
            bool write(const uint8_t *d, size_t n) override {
                s.append(reinterpret_cast<const char *>(d), n);
                return true;
            }
        } sink(out);
        out.clear();
        out.reserve(e.size);
        return extract(e, sink);
    }

private:
    Source &src_;
    std::vector<ZipEntry> entries_;
};

// ---------------------------------------------------------------------------

std::string convertEpub(Source &src, CleanSink &out, std::string &title, ProgressFn progress) {
    Zip zip(src);
    if (!zip.open()) return "Not a valid EPUB (zip)";

    std::string container, opf;
    const ZipEntry *ce = zip.find("META-INF/container.xml");
    if (!ce || !zip.readAll(*ce, container)) return "EPUB has no container.xml";
    std::string opfPath;  // first <rootfile full-path="..."> (skipping the <rootfiles> wrapper)
    for (size_t rf = 0; opfPath.empty() && (rf = container.find("<rootfile", rf)) != std::string::npos; rf++)
        opfPath = attr(container.substr(rf, container.find('>', rf) - rf), "full-path");
    const ZipEntry *oe = opfPath.empty() ? nullptr : zip.find(opfPath);
    if (!oe || !zip.readAll(*oe, opf)) return "EPUB has no OPF";
    std::string baseDir = opfPath.find('/') == std::string::npos ? "" : opfPath.substr(0, opfPath.rfind('/') + 1);

    std::string t = elementText(opf, "title");
    if (!t.empty()) title = t;

    // manifest: id -> href
    std::vector<std::pair<std::string, std::string>> items;
    for (size_t pos = 0; (pos = opf.find("<item", pos)) != std::string::npos;) {
        size_t end = opf.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = opf.substr(pos, end - pos);
        pos = end;
        if (tag.compare(0, 6, "<item ") != 0 && tag.compare(0, 6, "<item\t") != 0) continue;
        items.emplace_back(attr(tag, "id"), attr(tag, "href"));
    }
    // spine order
    std::vector<std::string> chapters;
    for (size_t pos = 0; (pos = opf.find("<itemref", pos)) != std::string::npos;) {
        size_t end = opf.find('>', pos);
        if (end == std::string::npos) break;
        std::string idref = attr(opf.substr(pos, end - pos), "idref");
        pos = end;
        for (auto &it : items)
            if (it.first == idref) chapters.push_back(resolvePath(baseDir, it.second));
    }
    opf.clear();
    opf.shrink_to_fit();
    if (chapters.empty()) return "EPUB has no chapters";

    int done = 0;
    for (auto &path : chapters) {
        const ZipEntry *e = zip.find(path);
        if (e) {
            out.anchor("Часть " + std::to_string(done + 1));
            MarkupSink markup(out, MarkupSink::Flavor::Html);
            if (!zip.extract(*e, markup)) return "Could not unpack " + path;
            out.paragraph();
        }
        if (progress) progress(++done * 100 / static_cast<int>(chapters.size()));
    }
    return "";
}

// Copies the whole source through a sink, reporting progress.
bool pump(Source &src, Sink &sink, ProgressFn progress) {
    uint8_t buf[2048];
    uint32_t total = src.size(), done = 0;
    int lastPct = -1;
    src.seek(0);
    for (;;) {
        size_t n = src.read(buf, sizeof(buf));
        if (!n) return true;
        if (!sink.write(buf, n)) return false;
        done += n;
        int pct = total ? static_cast<int>(static_cast<uint64_t>(done) * 100 / total) : 100;
        if (progress && pct != lastPct) progress(lastPct = pct);
    }
}

std::string head(Source &src, size_t n) {
    std::string s(n, '\0');
    src.seek(0);
    s.resize(src.read(reinterpret_cast<uint8_t *>(&s[0]), n));
    src.seek(0);
    return s;
}

std::string convertFb2(Source &src, CleanSink &out, std::string &title, ProgressFn progress) {
    std::string start = head(src, 16384);
    std::string encoding = lower(attr(start.substr(0, start.find("?>") == std::string::npos ? 0 : start.find("?>")), "encoding"));
    bool cp1251 = encoding == "windows-1251" || encoding == "cp1251" || (encoding.empty() && !looksLikeUtf8(src));

    MarkupSink markup(out, MarkupSink::Flavor::Fb2);
    Cp1251Sink decoder(markup);
    std::string t = elementText(start, "book-title");
    if (!t.empty()) {
        if (cp1251) {  // title bytes are still cp1251 here
            struct StringSink : Sink {
                std::string s;
                bool write(const uint8_t *d, size_t n) override {
                    s.append(reinterpret_cast<const char *>(d), n);
                    return true;
                }
            } conv;
            Cp1251Sink(conv).write(reinterpret_cast<const uint8_t *>(t.data()), t.size());
            t = conv.s;
        }
        title = t;
    }
    if (!pump(src, cp1251 ? static_cast<Sink &>(decoder) : static_cast<Sink &>(markup), progress))
        return "Write error";
    return "";
}

std::string convertTxt(Source &src, CleanSink &out, ProgressFn progress) {
    bool utf8 = looksLikeUtf8(src);
    // Hard-wrapped text (short lines, blank line between paragraphs) vs one line per paragraph
    std::string sample = head(src, 16384);
    size_t lines = 1, blank = 0;
    for (size_t i = 0; i < sample.size(); i++) {
        if (sample[i] != '\n') continue;
        lines++;
        size_t j = i + 1;
        while (j < sample.size() && (sample[j] == '\r' || sample[j] == ' ' || sample[j] == '\t')) j++;
        if (j < sample.size() && sample[j] == '\n') blank++;
    }
    size_t avgLine = sample.size() / lines;
    out.newlineIsParagraph = !(avgLine < 100 && blank * 4 >= lines / 10);
    out.guessChapters = true;

    Cp1251Sink decoder(out);
    if (!pump(src, utf8 ? static_cast<Sink &>(out) : static_cast<Sink &>(decoder), progress)) return "Write error";
    return "";
}

}  // namespace

Format formatFromName(const std::string &name) {
    std::string l = lower(name);
    if (endsWith(l, ".txt")) return Format::Txt;
    if (endsWith(l, ".epub")) return Format::Epub;
    if (endsWith(l, ".fb2")) return Format::Fb2;
    return Format::Unknown;
}

std::string convertBook(Format format, Source &src, Sink &sink, std::string &title, std::vector<Chapter> &chapters,
                        ProgressFn progress) {
    CleanSink out(sink);
    std::string err;
    switch (format) {
        case Format::Epub: err = convertEpub(src, out, title, progress); break;
        case Format::Fb2: err = convertFb2(src, out, title, progress); break;
        case Format::Txt: err = convertTxt(src, out, progress); break;
        default: return "Unsupported file type";
    }
    if (!out.flush() && err.empty()) err = "Write error (storage full?)";
    // EPUBs without headings: fall back to one chapter per spine file
    chapters = !out.chapters.empty() ? out.chapters : out.anchors;
    return err;
}

}  // namespace rsvp
