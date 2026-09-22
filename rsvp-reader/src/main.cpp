// RSVP speed reader for M5Stack Cardputer ADV: one word at a time, with the focus letter in red.
#include <M5Cardputer.h>

#include "core/rsvp.h"
#include "font_ui.h"
#include "font_word.h"
#include "storage.h"
#include "upload.h"

// M5Cardputer.Display is a reference member: never bind it at global scope (static-init order).
static M5GFX &lcd() { return M5Cardputer.Display; }

namespace {

constexpr uint16_t BG = TFT_BLACK;
constexpr uint16_t FG = TFT_WHITE;
constexpr uint16_t DIM = 0x8410;      // grey
constexpr uint16_t ACCENT = 0xF8A2;   // red-orange focus letter
constexpr uint16_t SELECT = 0x2124;   // list highlight
constexpr int W = 240, H = 135;
constexpr int WORD_H = 60;
constexpr int WORD_Y_PAUSED = 21, WORD_Y_PLAYING = (135 - WORD_H) / 2;
constexpr int PIVOT_X = 92;           // where the focus letter sits
constexpr int WPM_MIN = 100, WPM_MAX = 1000, WPM_STEP = 25;
constexpr uint32_t SAVE_EVERY_MS = 15000;

constexpr uint8_t BRIGHTNESS[] = {10, 20, 50, 90, 150, 255};
constexpr int BRIGHT_LEVELS = sizeof(BRIGHTNESS);

enum class Screen { Library, Reader, Chapters, Upload, Help };
Screen screen = Screen::Library;

M5Canvas *wordCanvas = nullptr;

// library
std::vector<storage::Book> books;
int selected = 0;
bool confirmDelete = false;

// reader
storage::Book book;
File textFile;
storage::FileSource *textSrc = nullptr;
rsvp::WordReader *reader = nullptr;
rsvp::Word word;
bool haveWord = false;
bool playing = false;
bool finished = false;
int wpm = 300;
uint32_t nextWordAt = 0;
uint32_t lastSave = 0;
int wordY = WORD_Y_PAUSED;
std::vector<rsvp::Chapter> chapters;
float bookWeight = 0;  // sum of word delay factors, see rsvp::measureWeight
int chapterSel = 0;

int brightLevel = 3;
Screen helpFrom = Screen::Library;
uint32_t toastUntil = 0;  // a short note at the top of the screen is visible until then

std::vector<char> prevKeys;

// ---------------------------------------------------------------------------
// drawing helpers

// Cuts a UTF-8 string so it fits in maxWidth pixels with the current font, adding "…".
String fit(const String &s, int maxWidth) {
    if (lcd().textWidth(s) <= maxWidth) return s;
    String t = s;
    while (t.length() > 0) {
        int cut = t.length() - 1;
        while (cut > 0 && (static_cast<uint8_t>(t[cut]) & 0xC0) == 0x80) cut--;
        t = t.substring(0, cut);
        if (lcd().textWidth(t + "…") <= maxWidth) return t + "…";
    }
    return "…";
}

void text(const String &s, int x, int y, uint16_t color = FG, textdatum_t datum = top_left) {
    lcd().setTextDatum(datum);
    lcd().setTextColor(color, BG);
    lcd().drawString(s, x, y);
}

// Left and right aligned parts of one line: never overlaps into "…" like one long string would.
void row(const String &left, const String &right, int y, uint16_t leftColor, uint16_t rightColor = DIM) {
    text(left, 4, y, leftColor);
    text(right, W - 4, y, rightColor, top_right);
}

void header(const String &title, const String &right) {
    lcd().fillRect(0, 0, W, 20, BG);
    text(fit(title, W - 14 - lcd().textWidth(right)), 4, 2, FG);
    text(right, W - 4, 2, DIM, top_right);
    lcd().drawFastHLine(0, 20, W, 0x2945);
}

String battery() {
    int level = M5Cardputer.Power.getBatteryLevel();
    return level > 0 ? String(level) + "%" : "";
}

void message(const String &title, const String &l1, const String &l2 = "", const String &l3 = "") {
    lcd().fillScreen(BG);
    header(title, battery());
    text(l1, 8, 34);
    text(l2, 8, 56, DIM);
    text(l3, 8, 78, DIM);
}

// ---------------------------------------------------------------------------
// library screen

void drawLibrary() {
    lcd().fillScreen(BG);
    String where = storage::onSd() ? "SD" : "память";
    header("Книги · " + where, battery());

    if (!storage::available()) {
        text("Некуда сохранять книги.", 8, 30);
        text("Вставь SD-карту и", 8, 52, DIM);
        text("перезагрузи Cardputer.", 8, 70, DIM);
        return;
    }
    if (books.empty()) {
        text("Книг пока нет.", 8, 28);
        text("W — загрузить с телефона", 8, 52, ACCENT);
        text("по Wi-Fi, или положи", 8, 72, DIM);
        text(".txt / .epub / .fb2 в /books", 8, 90, DIM);
        text("на SD-карте.", 8, 108, DIM);
        return;
    }

    const int rowH = 19, rows = 5, top = 23;
    int first = selected - rows / 2;
    first = std::max(0, std::min(first, static_cast<int>(books.size()) - rows));
    for (int i = 0; i < rows && first + i < static_cast<int>(books.size()); i++) {
        const auto &b = books[first + i];
        int y = top + i * rowH;
        bool sel = first + i == selected;
        if (sel) lcd().fillRoundRect(2, y, W - 4, rowH - 1, 4, SELECT);
        String pct = b.percent < 0 ? "новая" : String(b.percent) + "%";
        lcd().setTextColor(sel ? FG : 0xC618, sel ? SELECT : BG);
        lcd().setTextDatum(top_left);
        lcd().drawString(fit(b.title, W - 62), 8, y + 1);
        lcd().setTextColor(b.percent < 0 ? ACCENT : DIM, sel ? SELECT : BG);
        lcd().setTextDatum(top_right);
        lcd().drawString(pct, W - 8, y + 1);
    }

    lcd().fillRect(0, H - 17, W, 17, BG);
    if (confirmDelete) {
        text(fit("Удалить? Y — да, другая клавиша — нет", W - 8), 4, H - 16, ACCENT);
    } else {
        row("W — загрузить", "H — клавиши", H - 16, DIM);
    }
}

void openLibrary() {
    screen = Screen::Library;
    confirmDelete = false;
    books = storage::listBooks();
    if (selected >= static_cast<int>(books.size())) selected = std::max(0, static_cast<int>(books.size()) - 1);
    drawLibrary();
}

// ---------------------------------------------------------------------------
// reader screen

void drawWord() {
    auto &c = *wordCanvas;
    c.fillSprite(BG);
    c.drawFastHLine(0, 2, W, 0x2945);
    c.drawFastHLine(0, WORD_H - 3, W, 0x2945);

    if (!haveWord) {
        c.pushSprite(0, wordY);
        return;
    }
    const std::string &s = word.text;
    size_t orpStart, orpLen;
    rsvp::orpRange(s, orpStart, orpLen);
    String pre = s.substr(0, orpStart).c_str();
    String piv = s.substr(orpStart, orpLen).c_str();
    String post = s.substr(orpStart + orpLen).c_str();

    c.setTextSize(1.0f);
    int total = c.textWidth(s.c_str());
    if (total > W - 8) {  // very long word: shrink to fit
        c.setTextSize(std::max(0.5f, (W - 8) / static_cast<float>(total)));
        total = c.textWidth(s.c_str());
    }
    int preW = c.textWidth(pre), pivW = c.textWidth(piv);
    int x = PIVOT_X - preW - pivW / 2;
    if (x < 4) x = 4;
    if (x + total > W - 4) x = W - 4 - total;
    int pivotCenter = x + preW + pivW / 2;

    c.drawFastVLine(pivotCenter, 3, 7, DIM);
    c.drawFastVLine(pivotCenter, WORD_H - 10, 7, DIM);

    const int baseline = 42;
    c.setTextDatum(baseline_left);
    c.setTextColor(FG);
    c.drawString(pre, x, baseline);
    c.setTextColor(ACCENT);
    c.drawString(piv, x + preW, baseline);
    c.setTextColor(FG);
    c.drawString(post, x + preW + pivW, baseline);
    c.pushSprite(0, wordY);
}

uint32_t textSize() { return reader ? reader->size() : 0; }

// Chapter the reader is in: index of the last chapter starting at or before pos, -1 if none.
int chapterAt(uint32_t pos) {
    int ci = -1;
    for (size_t i = 0; i < chapters.size() && chapters[i].offset <= pos; i++) ci = i;
    return ci;
}

String chapterName(int i) {
    String t = chapters[i].title.c_str();
    bool number = t.length() > 0;
    for (unsigned k = 0; k < t.length(); k++) number &= isdigit(static_cast<uint8_t>(t[k])) != 0;
    return number ? "Глава " + t : t;
}

// Reading time for a stretch of text, using the book's measured words-per-byte density.
String timeFor(uint32_t bytes) {
    uint32_t size = textSize();
    float words = (bookWeight > 0 && size) ? bytes * (bookWeight / size) : bytes / 12.0f;
    uint32_t minutes = static_cast<uint32_t>(words / wpm + 0.5f);
    if (minutes < 1) return "<1 мин";
    if (minutes < 60) return String(minutes) + " мин";
    return String(minutes / 60) + " ч " + String(minutes % 60) + " м";
}

// Paused screen: title, progress, time left and key hints. Nothing of this shows while playing.
void drawReaderStatus() {
    uint32_t size = textSize();
    uint32_t pos = haveWord ? word.offset : 0;
    int pct = size ? static_cast<int>(pos * 100ULL / size) : 0;
    int chapter = chapterAt(pos);
    header(book.title, (chapter >= 0 ? "гл. " + String(chapter + 1) + "/" + String(chapters.size()) + " · " : String("")) +
                           String(pct) + "%");

    const int barY = 84;
    lcd().fillRect(0, barY, W, H - barY, BG);
    lcd().fillRoundRect(4, barY, W - 8, 3, 1, 0x2945);
    lcd().fillRoundRect(4, barY, std::max(3, (W - 8) * pct / 100), 3, 1, ACCENT);

    if (finished) {
        text("Конец книги!", 4, 89, FG);
        text("` — к списку книг", 4, 119, DIM);
        return;
    }
    int ci = chapterAt(pos);
    if (ci >= 0) {
        uint32_t end = ci + 1 < static_cast<int>(chapters.size()) ? chapters[ci + 1].offset : size;
        row("глава " + timeFor(end > pos ? end - pos : 0), "книга " + timeFor(size - pos), 89, FG, FG);
    } else {
        row("до конца книги", timeFor(size > pos ? size - pos : 0), 89, FG, FG);
    }
    row(String(wpm) + " сл/мин", "; . скорость", 104, DIM);
    row("пробел — старт", "H — клавиши", 119, DIM);
}

// Playing: only the word, centred. Paused: everything.
void drawReader() {
    lcd().fillScreen(BG);
    wordY = playing ? WORD_Y_PLAYING : WORD_Y_PAUSED;
    drawWord();
    if (!playing) drawReaderStatus();
}

void toast(const String &note) {
    int top = screen == Screen::Reader && playing ? 4 : 1;
    lcd().fillRect(0, top - 1, W, 18, BG);
    text(note, W / 2, top, ACCENT, top_center);
    toastUntil = millis() + 1200;
}

void saveProgress() {
    if (haveWord) storage::savePosition(book, word.offset);
    lastSave = millis();
}

void closeReader() {
    saveProgress();
    storage::saveWpm(wpm);
    delete reader;
    reader = nullptr;
    delete textSrc;
    textSrc = nullptr;
    if (textFile) textFile.close();
    playing = false;
}

void showPrepareProgress(int pct) {
    static bool measuring = false;
    if (pct > 100 && !measuring) text("Считаю время чтения…", 8, 34);
    measuring = pct > 100;
    if (pct > 100) pct -= 100;
    lcd().fillRect(8, 60, W - 16, 30, BG);
    lcd().fillRoundRect(8, 64, W - 16, 8, 4, 0x2945);
    lcd().fillRoundRect(8, 64, std::max(8, (W - 16) * pct / 100), 8, 4, ACCENT);
    text(String(pct) + "%", W / 2, 76, DIM, top_center);
}

void openReader(storage::Book &b) {
    if (!storage::isPrepared(b)) {
        message(b.title, "Готовлю книгу…", "", "");
        String err = storage::prepare(b, showPrepareProgress);
        if (!err.isEmpty()) {
            message("Не получилось", err, "", "Любая клавиша — назад");
            while (true) {
                M5Cardputer.update();
                if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
                delay(10);
            }
            openLibrary();
            return;
        }
    }
    book = b;
    textFile = storage::fs().open(storage::textPath(book), "r");
    if (!textFile) {
        openLibrary();
        return;
    }
    textSrc = new storage::FileSource(textFile);
    reader = new rsvp::WordReader(*textSrc);
    chapters = storage::loadChapters(book);
    bookWeight = storage::loadWeight(book);
    reader->seek(storage::loadPosition(book));
    haveWord = reader->next(word);
    finished = !haveWord;
    playing = false;
    screen = Screen::Reader;
    drawReader();
}

void advance() {
    if (!reader->next(word)) {
        playing = false;
        finished = true;
        saveProgress();
        drawReader();
        return;
    }
    haveWord = true;
    drawWord();
    nextWordAt = millis() + static_cast<uint32_t>(60000.0f / wpm * rsvp::delayFactor(word));
    if (millis() - lastSave > SAVE_EVERY_MS) saveProgress();
}

void jumpWords(int n) {
    uint32_t target = n < 0 ? reader->findBack(word.offset, -n) : word.offset;
    reader->seek(target);
    haveWord = reader->next(word);
    for (int i = 0; n > 0 && i < n && haveWord; i++) haveWord = reader->next(word);
    finished = false;
    if (playing) drawWord();
    else drawReader();
}

void jumpTo(uint32_t offset) {
    reader->seek(offset);
    haveWord = reader->next(word);
    finished = !haveWord;
    if (playing) nextWordAt = millis() + 60000 / wpm;
    if (playing) drawWord();
    else drawReader();
}

// '[' goes to the start of this chapter, or to the previous one when already near the start.
void previousChapter() {
    int ci = chapterAt(word.offset);
    if (ci < 0) return;
    uint32_t start = chapters[ci].offset;
    if (word.offset <= start + 200 && ci > 0) start = chapters[ci - 1].offset;
    jumpTo(start);
}

void nextChapter() {
    int ci = chapterAt(word.offset);
    if (ci + 1 < static_cast<int>(chapters.size())) jumpTo(chapters[ci + 1].offset);
}

// ---------------------------------------------------------------------------
// chapter list

void drawChapters() {
    lcd().fillScreen(BG);
    header("Главы · " + String(chapters.size()), "");
    const int rowH = 19, rows = 5, top = 23;
    int n = chapters.size();
    int current = chapterAt(word.offset);
    int first = std::max(0, std::min(chapterSel - rows / 2, n - rows));
    for (int i = 0; i < rows && first + i < n; i++) {
        int idx = first + i;
        int y = top + i * rowH;
        bool sel = idx == chapterSel;
        if (sel) lcd().fillRoundRect(2, y, W - 4, rowH - 1, 4, SELECT);
        lcd().setTextColor(idx == current ? ACCENT : (sel ? FG : 0xC618), sel ? SELECT : BG);
        lcd().setTextDatum(top_left);
        lcd().drawString(fit(chapterName(idx), W - 16), 8, y + 1);
    }
    text(fit("Enter — перейти   ` — назад", W - 8), 4, H - 16, DIM);
}

void openChapters() {
    if (chapters.empty()) {
        toast("В этой книге нет глав");
        return;
    }
    if (playing) {
        playing = false;
        saveProgress();
    }
    chapterSel = std::max(0, chapterAt(word.offset));
    screen = Screen::Chapters;
    drawChapters();
}

void drawHelp() {
    static const char *READER[][2] = {
        {"пробел", "старт / пауза"},  {"; .", "скорость"},       {", /", "10 слов назад / вперёд"},
        {"[ ]", "пред. / след. глава"}, {"C  /  B", "главы  /  яркость"}, {"`", "к списку книг"},
    };
    static const char *LIBRARY[][2] = {
        {"; .", "выбрать книгу"},  {"Enter", "читать"}, {"W", "загрузить по Wi-Fi"},
        {"B", "яркость"},          {"fn+Del", "удалить книгу"}, {"", ""},
    };
    auto &rows = helpFrom == Screen::Library ? LIBRARY : READER;
    lcd().fillScreen(BG);
    header("Клавиши", "");
    for (int i = 0; i < 6; i++) {
        text(rows[i][0], 6, 24 + i * 18, ACCENT);
        text(rows[i][1], 74, 24 + i * 18, FG);
    }
}

void openHelp() {
    if (screen == Screen::Reader && playing) {
        playing = false;
        saveProgress();
    }
    helpFrom = screen;
    screen = Screen::Help;
    drawHelp();
}

void cycleBrightness() {
    brightLevel = (brightLevel + 1) % BRIGHT_LEVELS;
    lcd().setBrightness(BRIGHTNESS[brightLevel]);
    storage::saveBrightness(brightLevel);
    toast("Яркость " + String(brightLevel + 1) + "/" + String(BRIGHT_LEVELS));
}

// ---------------------------------------------------------------------------
// upload screen

void drawUpload(const String &last = "") {
    lcd().fillScreen(BG);
    header("Загрузка по Wi-Fi", battery());
    text("1. На телефоне подключись к Wi-Fi", 4, 26);
    text(upload::SSID, 20, 44, ACCENT);
    text("2. Открой в браузере", 4, 64);
    text("192.168.4.1", 20, 82, ACCENT);
    if (last.length()) text(fit("Загружено: " + last, W - 8), 4, 102, 0x07E0);
    else text(upload::clientCount() ? "Телефон подключён" : "Жду телефон…", 4, 102, DIM);
    text("` — готово, к списку книг", 4, 116, DIM);
}

// ---------------------------------------------------------------------------
// input

bool newlyPressed(char c, const std::vector<char> &now) {
    bool isDown = std::find(now.begin(), now.end(), c) != now.end();
    bool wasDown = std::find(prevKeys.begin(), prevKeys.end(), c) != prevKeys.end();
    return isDown && !wasDown;
}

void handleLibrary(const Keyboard_Class::KeysState &k) {
    if (confirmDelete) {
        bool yes = std::find(k.word.begin(), k.word.end(), 'y') != k.word.end() ||
                   std::find(k.word.begin(), k.word.end(), 'Y') != k.word.end();
        if (yes && selected < static_cast<int>(books.size())) storage::removeBook(books[selected]);
        openLibrary();
        return;
    }
    if (newlyPressed('b', k.word) || newlyPressed('B', k.word)) {
        cycleBrightness();
        return;
    }
    if (newlyPressed('h', k.word) || newlyPressed('H', k.word)) {
        openHelp();
        return;
    }
    if (newlyPressed('w', k.word) || newlyPressed('W', k.word)) {
        if (!storage::available()) return;
        screen = Screen::Upload;
        upload::start();
        drawUpload();
        return;
    }
    if (books.empty()) return;
    int n = books.size();
    if (newlyPressed(';', k.word) || k.up) selected = (selected + n - 1) % n;
    else if (newlyPressed('.', k.word) || k.down) selected = (selected + 1) % n;
    else if (k.enter) {
        openReader(books[selected]);
        return;
    } else if (k.del) confirmDelete = true;
    else return;
    drawLibrary();
}

void handleReader(const Keyboard_Class::KeysState &k) {
    auto pressed = [&](char lower, char upper = 0) {
        return newlyPressed(lower, k.word) || (upper && newlyPressed(upper, k.word));
    };
    if (k.enter || k.space) {
        if (finished) return;
        playing = !playing;
        if (playing) nextWordAt = millis() + 60000 / wpm;
        else saveProgress();
        drawReader();
    } else if (pressed(';') || pressed('=') || k.up || pressed('.') || pressed('-') || k.down) {
        bool faster = pressed(';') || pressed('=') || k.up;
        wpm = faster ? std::min(WPM_MAX, wpm + WPM_STEP) : std::max(WPM_MIN, wpm - WPM_STEP);
        if (playing) toast(String(wpm) + " сл/мин");
        else drawReaderStatus();
    } else if (pressed(',') || k.left) {
        jumpWords(-10);
    } else if (pressed('/') || k.right) {
        jumpWords(10);
    } else if (pressed('[')) {
        previousChapter();
    } else if (pressed(']')) {
        nextChapter();
    } else if (pressed('c', 'C')) {
        openChapters();
    } else if (pressed('b', 'B')) {
        cycleBrightness();
    } else if (pressed('h', 'H')) {
        openHelp();
    } else if (pressed('`') || k.backspace || k.esc) {
        closeReader();
        openLibrary();
    }
}

void handleChapters(const Keyboard_Class::KeysState &k) {
    int n = chapters.size();
    if (newlyPressed(';', k.word) || k.up) chapterSel = (chapterSel + n - 1) % n;
    else if (newlyPressed('.', k.word) || k.down) chapterSel = (chapterSel + 1) % n;
    else if (k.enter) {
        screen = Screen::Reader;
        jumpTo(chapters[chapterSel].offset);
        return;
    } else if (newlyPressed('`', k.word) || k.backspace || k.esc) {
        screen = Screen::Reader;
        drawReader();
        return;
    } else {
        return;
    }
    drawChapters();
}

void handleUpload(const Keyboard_Class::KeysState &k) {
    if (newlyPressed('`', k.word) || k.backspace || k.esc) {
        upload::stop();
        openLibrary();
    }
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    lcd().setRotation(1);
    lcd().setBrightness(BRIGHTNESS[brightLevel]);
    lcd().loadFont(FONT_UI);
    lcd().fillScreen(BG);

    wordCanvas = new M5Canvas(&lcd());
    wordCanvas->setColorDepth(16);
    wordCanvas->createSprite(W, WORD_H);
    wordCanvas->loadFont(FONT_WORD);

    message("RSVP Reader", "Ищу книги…");
    storage::begin();
    wpm = storage::loadWpm();
    brightLevel = std::max(0, std::min(BRIGHT_LEVELS - 1, storage::loadBrightness()));
    lcd().setBrightness(BRIGHTNESS[brightLevel]);
    openLibrary();
}

void loop() {
    M5Cardputer.update();

    if (M5Cardputer.Keyboard.isChange()) {
        auto k = M5Cardputer.Keyboard.keysState();
        if (M5Cardputer.Keyboard.isPressed()) {
            switch (screen) {
                case Screen::Library: handleLibrary(k); break;
                case Screen::Reader: handleReader(k); break;
                case Screen::Chapters: handleChapters(k); break;
                case Screen::Help:  // any key closes the help
                    screen = helpFrom;
                    if (screen == Screen::Library) drawLibrary();
                    else drawReader();
                    break;
                case Screen::Upload: handleUpload(k); break;
            }
        }
        prevKeys = k.word;
    }

    if (screen == Screen::Reader && playing && static_cast<int32_t>(millis() - nextWordAt) >= 0) advance();

    if (toastUntil && static_cast<int32_t>(millis() - toastUntil) >= 0) {
        toastUntil = 0;
        switch (screen) {  // put back whatever the note covered
            case Screen::Library: drawLibrary(); break;
            case Screen::Reader:
                if (playing) lcd().fillRect(0, 0, W, WORD_Y_PLAYING, BG);
                else drawReaderStatus();
                break;
            case Screen::Chapters: drawChapters(); break;
            case Screen::Upload:
            case Screen::Help: break;
        }
    }

    if (screen == Screen::Upload) {
        upload::loop();
        static uint32_t lastRefresh = 0;
        static int lastClients = -1;
        String done = upload::takeLastUploaded();
        if (done.length()) {
            drawUpload(done);
        } else if (millis() - lastRefresh > 1000 && upload::clientCount() != lastClients) {
            lastClients = upload::clientCount();
            lastRefresh = millis();
            drawUpload();
        }
    }

    delay(screen == Screen::Reader && playing ? 1 : 5);
}
