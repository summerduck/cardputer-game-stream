// Game Boy / Game Boy Color emulator for M5Stack Cardputer ADV (Walnut-CGB core).
// Readable screen modes (x1.5 zoom, 1:1) and the picture streamed to a phone over Wi-Fi.
#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

#include <ctype.h>

#include <algorithm>
#include <vector>

#include "core/emu.h"
#include "flashrom.h"
#include "font_ui.h"
#include "screen.h"
#include "stream.h"

namespace {

M5GFX &lcd() { return M5Cardputer.Display; }

// Cardputer / Cardputer ADV microSD wiring (same as M5Launcher's board config)
constexpr int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;
SPIClass sdSpi(HSPI);

constexpr uint16_t BG = TFT_BLACK, FG = TFT_WHITE, DIM = 0xBDF7, ACCENT = 0xFD20, SELECT = 0x39E7, LINE = 0x4A69,
                   OK = 0x3EEB;
constexpr int W = 240, H = 135;
constexpr int ROW_TOP = 25, ROW_H = 22, ROWS = 4, FOOTER_Y = 113;

constexpr uint32_t FRAME_US = 16743;  // 70224 cycles at 4.194304 MHz
constexpr uint32_t SAVE_EVERY_MS = 30000;
constexpr int VOLUME_LEVELS = 10;
constexpr uint8_t VOLUME[VOLUME_LEVELS + 1] = {0, 3, 10, 23, 41, 64, 92, 125, 163, 207, 255};
constexpr int SPEAKER_CH = 0;

// Settings, kept in NVS
Preferences prefs;
struct Settings {
    uint8_t mode = screen::ZOOM;
    uint8_t palette = 0;
    bool sound = true;
    uint8_t volume = 5;
    uint8_t execution = emu::COMPATIBLE;
    bool stream = false;
    String dir = "/";
    // The key for each Game Boy button, in emu::Button bit order (A B Select Start → ← ↑ ↓).
    // Letters lower case, '\n' Enter, '\t' Tab.
    char keys[8] = {'x', 'z', ' ', '\n', '/', ',', ';', '.'};
} settings;
const char DEFAULT_KEYS[8] = {'x', 'z', ' ', '\n', '/', ',', ';', '.'};

void loadSettings() {
    prefs.begin("gb", true);
    settings.mode = prefs.getUChar("mode", settings.mode) % screen::MODES;
    settings.palette = prefs.getUChar("palette", settings.palette) % emu::DMG_PALETTES;
    settings.sound = prefs.getBool("sound", settings.sound);
    settings.volume = std::min<int>(prefs.getUChar("volume", settings.volume), VOLUME_LEVELS);
    settings.execution = prefs.getUChar("execution", settings.execution) % emu::EXECUTION_MODES;
    settings.stream = prefs.getBool("stream", settings.stream);
    settings.dir = prefs.getString("dir", settings.dir);
    char keys[8];
    if (prefs.getBytesLength("keys") == sizeof keys && prefs.getBytes("keys", keys, sizeof keys) == sizeof keys)
        memcpy(settings.keys, keys, sizeof keys);
    prefs.end();
}

void saveSettings() {
    prefs.begin("gb", false);
    prefs.putUChar("mode", settings.mode);
    prefs.putUChar("palette", settings.palette);
    prefs.putBool("sound", settings.sound);
    prefs.putUChar("volume", settings.volume);
    prefs.putUChar("execution", settings.execution);
    prefs.putBool("stream", settings.stream);
    prefs.putString("dir", settings.dir);
    prefs.putBytes("keys", settings.keys, sizeof settings.keys);
    prefs.end();
}

// ---------------------------------------------------------------------------
// keyboard

std::vector<char> prevWord;
Keyboard_Class::KeysState prevKeys;

struct Keys {
    Keyboard_Class::KeysState k;
    bool has(char c) const { return std::find(k.word.begin(), k.word.end(), c) != k.word.end(); }
    // Newly pressed since the last poll
    bool hit(char c) const { return has(c) && std::find(prevWord.begin(), prevWord.end(), c) == prevWord.end(); }
    bool up() const { return hit(';') || (k.up && !prevKeys.up); }
    bool down() const { return hit('.') || (k.down && !prevKeys.down); }
    bool left() const { return hit(',') || (k.left && !prevKeys.left); }
    bool right() const { return hit('/') || (k.right && !prevKeys.right); }
    bool enter() const { return k.enter && !prevKeys.enter; }
    bool back() const { return hit('`') || (k.esc && !prevKeys.esc) || (k.backspace && !prevKeys.backspace); }
};

Keys poll() {
    M5Cardputer.update();
    Keys keys{M5Cardputer.Keyboard.keysState()};
    return keys;
}

void endPoll(const Keys &keys) {
    prevWord = keys.k.word;
    prevKeys = keys.k;
}

bool held(const Keys &keys, char c) {
    if (c == '\n') return keys.k.enter;
    if (c == '\t') return keys.k.tab;
    return keys.has(c) || keys.has(toupper(c));  // upper case while Aa is held
}

// Game Boy buttons held on the keyboard: settings.keys, plus the fn arrows for the D-pad.
uint8_t gameButtons(const Keys &keys) {
    uint8_t b = 0;
    for (int i = 0; i < 8; i++)
        if (held(keys, settings.keys[i])) b |= 1 << i;
    if (keys.k.up) b |= emu::UP;
    if (keys.k.down) b |= emu::DOWN;
    if (keys.k.left) b |= emu::LEFT;
    if (keys.k.right) b |= emu::RIGHT;
    return b;
}

String keyName(char c) {
    if (c == '\n') return "Enter";
    if (c == '\t') return "Tab";
    if (c == ' ') return "Space";
    return String((char)toupper(c));
}

void waitKey() {
    while (true) {
        Keys keys = poll();
        bool any = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
        endPoll(keys);
        if (any) return;
        delay(10);
    }
}

// ---------------------------------------------------------------------------
// drawing helpers (18px PT Sans: the text box starts 3px above the caps)

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

void text(const String &s, int x, int y, uint16_t color = FG, textdatum_t datum = top_left, uint16_t bg = BG) {
    lcd().setTextDatum(datum);
    lcd().setTextColor(color, bg);
    lcd().drawString(s, x, y);
}

void header(const String &title, const String &right = "") {
    lcd().fillRect(0, 0, W, 23, BG);
    text(fit(title, W - 14 - lcd().textWidth(right)), 4, -2);
    text(right, W - 4, -2, DIM, top_right);
    lcd().drawFastHLine(0, 23, W, LINE);
}

// The phone stream's state for the top right corner: off, Wi-Fi up with no page open, or pages open.
String phoneStatus(uint16_t *color = nullptr) {
    int n = stream::viewers();
    uint16_t c = !stream::running() ? DIM : n ? OK : ACCENT;
    if (color) *color = c;
    return !stream::running() ? "stream off" : n ? String(n) + " connected" : "waiting";
}

// A header with the phone status on the right. Returns the status shown, for redrawing when it changes.
String statusHeader(const String &title) {
    uint16_t c;
    String status = phoneStatus(&c);
    int sw = lcd().textWidth(status);
    lcd().fillRect(0, 0, W, 23, BG);
    text(fit(title, W - 26 - sw), 4, -2);
    text(status, W - 4, -2, c, top_right);
    lcd().fillCircle(W - 12 - sw, 11, 3, c);
    lcd().drawFastHLine(0, 23, W, LINE);
    return status;
}

void footer(const String &left, const String &right) {
    lcd().fillRect(0, FOOTER_Y, W, H - FOOTER_Y, BG);
    text(fit(left, W - 16 - lcd().textWidth(right)), 4, FOOTER_Y - 2, DIM);
    text(right, W - 4, FOOTER_Y - 2, DIM, top_right);
}

void message(const String &title, const String &l1, const String &l2 = "", const String &l3 = "") {
    lcd().fillScreen(BG);
    header(title);
    text(l1, 6, 30);
    text(l2, 6, 54, DIM);
    text(l3, 6, 78, DIM);
}

// A scrolling list of ROWS rows; value is drawn right-aligned.
void listRow(int i, const String &label, const String &value, bool selected, uint16_t labelColor = FG) {
    int y = ROW_TOP + i * ROW_H;
    uint16_t bg = selected ? SELECT : BG;
    lcd().fillRect(0, y, W, ROW_H, BG);
    if (selected) lcd().fillRoundRect(2, y, W - 4, ROW_H - 1, 4, SELECT);
    int vw = value.length() ? lcd().textWidth(value) + 10 : 0;
    text(value, W - 8, y - 2, ACCENT, top_right, bg);
    text(fit(label, W - 16 - vw), 8, y - 2, labelColor, top_left, bg);
}

int firstVisible(int selected, int n) { return std::max(0, std::min(selected - ROWS / 2, n - ROWS)); }

// ---------------------------------------------------------------------------
// ROM browser

struct Entry {
    String name;
    bool dir;
};

bool isRom(const String &name) {
    String n = name;
    n.toLowerCase();
    return n.endsWith(".gb") || n.endsWith(".gbc");
}

std::vector<Entry> listDir(const String &path) {
    std::vector<Entry> out;
    File d = SD.open(path);
    if (!d || !d.isDirectory()) return out;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.startsWith(".")) continue;
        if (f.isDirectory() || isRom(name)) out.push_back({name, f.isDirectory()});
    }
    std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) return a.dir;
        String x = a.name, y = b.name;
        x.toLowerCase();
        y.toLowerCase();
        return x < y;
    });
    return out;
}

String joinPath(const String &dir, const String &name) { return dir.endsWith("/") ? dir + name : dir + "/" + name; }

String parentDir(const String &dir) {
    int slash = dir.lastIndexOf('/');
    return slash <= 0 ? "/" : dir.substring(0, slash);
}

void drawHelp();
void phoneScreen(bool inGame);
String memoryReport();

// Returns the path of the chosen ROM.
String pickRom() {
    String dir = settings.dir;
    if (!SD.exists(dir)) dir = "/";
    std::vector<Entry> entries = listDir(dir);
    int sel = 0;
    bool redraw = true;
    String title, status;
    while (true) {
        if (redraw) {
            lcd().fillScreen(BG);
            int roms = std::count_if(entries.begin(), entries.end(), [](const Entry &e) { return !e.dir; });
            title = (dir == "/" ? "Game Boy" : dir.substring(dir.lastIndexOf('/') + 1)) + " · " + String(roms);
            status = statusHeader(title);
            if (entries.empty()) {
                text("No .gb / .gbc games here", 6, 30);
                text("Put ROM files on the SD", 6, 54, DIM);
                text("card, in any folder.", 6, 78, DIM);
            }
            int first = firstVisible(sel, entries.size());
            for (int i = 0; i < ROWS && first + i < (int)entries.size(); i++) {
                const Entry &e = entries[first + i];
                listRow(i, e.dir ? e.name + "/" : e.name.substring(0, e.name.lastIndexOf('.')), "", first + i == sel,
                        e.dir ? DIM : FG);
            }
            footer("P — phone", "H — keys");
            redraw = false;
        } else if (phoneStatus() != status) {
            status = statusHeader(title);
        }
        Keys keys = poll();
        int n = entries.size();
        if (n && keys.up()) sel = (sel + n - 1) % n, redraw = true;
        else if (n && keys.down()) sel = (sel + 1) % n, redraw = true;
        else if (n && (keys.enter() || keys.right())) {
            const Entry &e = entries[sel];
            if (e.dir) {
                dir = joinPath(dir, e.name);
                entries = listDir(dir);
                sel = 0;
                redraw = true;
            } else {
                settings.dir = dir;
                saveSettings();
                endPoll(keys);
                return joinPath(dir, e.name);
            }
        } else if ((keys.back() || keys.left()) && dir != "/") {
            String from = dir.substring(dir.lastIndexOf('/') + 1);
            dir = parentDir(dir);
            entries = listDir(dir);
            sel = 0;
            for (int i = 0; i < (int)entries.size(); i++)
                if (entries[i].name == from) sel = i;
            redraw = true;
        } else if (keys.hit('p') || keys.hit('P')) {
            endPoll(keys);
            phoneScreen(false);
            redraw = true;
        } else if (keys.hit('h') || keys.hit('H')) {
            drawHelp();
            endPoll(keys);
            waitKey();
            redraw = true;
        }
        endPoll(keys);
        delay(15);
    }
}

// ---------------------------------------------------------------------------
// help and menu

void drawHelp() {
    const char *k = settings.keys;
    String rows[6][2] = {
        {keyName(k[6]) + " " + keyName(k[5]) + " " + keyName(k[7]) + " " + keyName(k[4]), "D-pad"},
        {keyName(k[0]) + "    " + keyName(k[1]), "A    B"},
        {keyName(k[3]), "Start"},
        {keyName(k[2]), "Select"},
        {"\\   [ ]", "screen, scroll"},
        {"`   - =", "menu, volume"},
    };
    lcd().fillScreen(BG);  // no header: six lines take the whole screen
    for (int i = 0; i < 6; i++) {
        text(fit(rows[i][0], 92), 4, i * 22, ACCENT);
        text(rows[i][1], 100, i * 22, FG);
    }
}

// Keys the game loop and the menu already use.
bool reservedKey(char c) { return strchr("`\\[]-=f", c) != nullptr; }

// Waits for a key to give a Game Boy button: its code, or 0 if cancelled with ` / Esc / Backspace.
char captureKey(const String &button) {
    footer("Key for " + button + "…", "` — cancel");
    while (true) {
        Keys keys = poll();
        char c = 0;
        if (keys.back()) {
            endPoll(keys);
            return 0;
        }
        if (keys.enter()) c = '\n';
        else if (keys.k.tab && !prevKeys.tab) c = '\t';
        else
            for (char w : keys.k.word)
                if (keys.hit(w)) c = tolower(w);
        endPoll(keys);
        if (c && reservedKey(c)) footer(keyName(c) + " is reserved", "` — cancel");
        else if (c) return c;
        delay(10);
    }
}

// Pause menu → Keys: one key per Game Boy button, a key taken from another button swaps with it.
void keysMenu() {
    static const int ORDER[8] = {6, 7, 5, 4, 0, 1, 3, 2};  // ↑ ↓ ← → A B Start Select
    static const char *const NAMES[8] = {"A", "B", "Select", "Start", "Right", "Left", "Up", "Down"};
    constexpr int N = 10;  // the buttons, then "reset" and "help"
    int sel = 0;
    bool redraw = true;
    while (true) {
        if (redraw) {
            lcd().fillScreen(BG);
            header("Keys");
            int first = firstVisible(sel, N);
            for (int i = 0; i < ROWS && first + i < N; i++) {
                int it = first + i;
                if (it < 8) listRow(i, NAMES[ORDER[it]], keyName(settings.keys[ORDER[it]]), it == sel);
                else listRow(i, it == 8 ? "Reset to default" : "Show all keys", "", it == sel);
            }
            footer("Enter — change", "` — back");
            redraw = false;
        }
        Keys keys = poll();
        if (keys.up()) sel = (sel + N - 1) % N, redraw = true;
        else if (keys.down()) sel = (sel + 1) % N, redraw = true;
        else if (keys.back()) {
            endPoll(keys);
            return;
        } else if (keys.enter()) {
            endPoll(keys);
            redraw = true;
            if (sel == 8) {
                memcpy(settings.keys, DEFAULT_KEYS, sizeof DEFAULT_KEYS);
            } else if (sel == 9) {
                drawHelp();
                waitKey();
                continue;
            } else {
                int b = ORDER[sel];
                char c = captureKey(NAMES[b]);
                if (!c) continue;
                for (char &other : settings.keys)
                    if (other == c) other = settings.keys[b];
                settings.keys[b] = c;
            }
            saveSettings();
            continue;
        }
        endPoll(keys);
        delay(15);
    }
}

// P in the game list, Phone in the pause menu: the status on top, how to connect, Enter turns Wi-Fi on / off.
void phoneScreen(bool inGame) {
    String status, note1, note2;
    bool redraw = true;
    while (true) {
        if (redraw) {
            lcd().fillScreen(BG);
            status = statusHeader("Phone");
            bool on = stream::running();
            text("Wi-Fi:", 6, 28, DIM);
            text(stream::SSID, 76, 28, on ? FG : DIM);
            text("pass:", 6, 50, DIM);
            text(stream::PASSWORD, 76, 50, on ? FG : DIM);
            text("Safari:", 6, 72, DIM);
            text(stream::URL, 76, 72, on ? ACCENT : DIM);
            if (note1.length()) {
                text(note1, 6, 94, ACCENT);
                footer(note2, "` — back");
            } else {
                text(on ? "Touch pad: Buttons on the page" : "", 6, 94, DIM);
                footer(on ? "Enter — stream off" : "Enter — stream on", "` — back");
            }
            redraw = false;
        } else if (phoneStatus() != status) {
            status = statusHeader("Phone");
        }
        Keys keys = poll();
        if (keys.back()) {
            endPoll(keys);
            return;
        }
        if (keys.enter()) {
            note1 = note2 = "";
            if (stream::running()) {
                stream::stop();
                settings.stream = false;
            } else {
                footer("Starting Wi-Fi…", "");
                settings.stream = true;  // if it fails in a game: on at the next start, before a game takes the memory
                if (!stream::start()) {
                    note1 = "Not enough memory";
                    note2 = inGame ? "Starts in the game list." : "Wi-Fi did not start.";
                }
            }
            saveSettings();
            redraw = true;
        }
        endPoll(keys);
        delay(15);
    }
}

void applyVolume() { M5Cardputer.Speaker.setVolume(settings.sound ? VOLUME[settings.volume] : 0); }

enum class MenuResult { Resume, Quit };

MenuResult menu() {
    enum Item { RESUME, SCREEN, CPU, PALETTE, SOUND, VOLUME_ITEM, STREAM, KEYS, RESET, QUIT, ITEMS };
    std::vector<int> items;
    for (int i = 0; i < ITEMS; i++)
        if (i != PALETTE || !emu::isColor()) items.push_back(i);
    int sel = 0;
    bool redraw = true;
    String status;
    while (true) {
        if (redraw) {
            lcd().fillScreen(BG);
            status = statusHeader(emu::title());
            int n = items.size(), first = firstVisible(sel, n);
            for (int i = 0; i < ROWS && first + i < n; i++) {
                int it = items[first + i];
                String label, value;
                switch (it) {
                    case RESUME: label = "Resume"; break;
                    case SCREEN: label = "Screen", value = screen::modeName((screen::Mode)settings.mode); break;
                    case CPU: label = "CPU", value = emu::executionModeName((emu::ExecutionMode)settings.execution); break;
                    case PALETTE: label = "Colours", value = emu::dmgPaletteName(settings.palette); break;
                    case SOUND: label = "Sound", value = settings.sound ? "on" : "off"; break;
                    case VOLUME_ITEM: label = "Volume", value = String(settings.volume) + "/10"; break;
                    case STREAM: label = "Phone"; break;
                    case KEYS: label = "Keys"; break;
                    case RESET: label = "Restart game"; break;
                    case QUIT: label = "Quit to game list"; break;
                }
                listRow(i, label, value, first + i == sel);
            }
            footer("Enter — select", "` — back");
            redraw = false;
        } else if (phoneStatus() != status) {
            status = statusHeader(emu::title());
        }
        Keys keys = poll();
        int n = items.size();
        int step = keys.left() ? -1 : keys.right() || keys.enter() ? 1 : 0;
        if (keys.up()) sel = (sel + n - 1) % n, redraw = true;
        else if (keys.down()) sel = (sel + 1) % n, redraw = true;
        else if (keys.back()) {
            endPoll(keys);
            return MenuResult::Resume;
        } else if (step) {
            redraw = true;
            switch (items[sel]) {
                case RESUME:
                    if (keys.enter()) {
                        endPoll(keys);
                        return MenuResult::Resume;
                    }
                    break;
                case SCREEN:
                    settings.mode = (settings.mode + screen::MODES + step) % screen::MODES;
                    screen::setMode((screen::Mode)settings.mode);
                    break;
                case CPU:
                    settings.execution = (settings.execution + emu::EXECUTION_MODES + step) % emu::EXECUTION_MODES;
                    emu::setExecutionMode((emu::ExecutionMode)settings.execution);
                    break;
                case PALETTE:
                    settings.palette = (settings.palette + emu::DMG_PALETTES + step) % emu::DMG_PALETTES;
                    emu::setDmgPalette(settings.palette);
                    break;
                case SOUND:
                    settings.sound = !settings.sound;
                    applyVolume();
                    break;
                case VOLUME_ITEM:
                    settings.volume = constrain(settings.volume + step, 1, VOLUME_LEVELS);
                    applyVolume();
                    break;
                case STREAM:
                    endPoll(keys);
                    phoneScreen(true);
                    break;
                case KEYS:
                    endPoll(keys);
                    keysMenu();
                    break;
                case RESET:
                    if (keys.enter()) {
                        emu::reset();
                        endPoll(keys);
                        return MenuResult::Resume;
                    }
                    break;
                case QUIT:
                    if (keys.enter()) {
                        endPoll(keys);
                        return MenuResult::Quit;
                    }
                    break;
            }
            saveSettings();
        }
        endPoll(keys);
        delay(15);
    }
}

// ---------------------------------------------------------------------------
// playing

struct SdRom : emu::RomSource {
    File f;
    explicit SdRom(const String &path) : f(SD.open(path, FILE_READ)) {}
    uint32_t size() override { return f ? f.size() : 0; }
    bool read(uint32_t offset, uint8_t *dst, uint32_t len) override {
        return f.seek(offset) && f.read(dst, len) == len;
    }
};

String savePath(const String &rom) { return rom.substring(0, rom.lastIndexOf('.')) + ".sav"; }

void loadSave(const String &path) {
    if (!emu::saveSize()) return;
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    uint32_t n = std::min<uint32_t>(f.size(), emu::saveSize());
    for (uint32_t off = 0; off < n; off += emu::SAVE_CHUNK)
        f.read(emu::saveChunk(off / emu::SAVE_CHUNK), std::min<uint32_t>(emu::SAVE_CHUNK, n - off));
    f.close();
    emu::clearSaveDirty();
}

bool writeSave(const String &path) {
    if (!emu::saveSize() || !emu::saveDirty()) return true;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    bool ok = true;
    for (uint32_t off = 0; ok && off < emu::saveSize(); off += emu::SAVE_CHUNK) {
        uint32_t n = std::min<uint32_t>(emu::SAVE_CHUNK, emu::saveSize() - off);
        ok = f.write(emu::saveChunk(off / emu::SAVE_CHUNK), n) == n;
    }
    f.close();
    if (ok) emu::clearSaveDirty();
    return ok;
}

int16_t audio[3][AUDIO_SAMPLES_TOTAL];

// ROM cache size, from the free heap as a whole (the cache is made of separate 2 KB pages, so
// fragmentation does not matter). 80 KB keeps page misses rare in Game Boy Color games, 48 KB is
// plenty for Game Boy ones (test/run_tests.sh). With the stream on, leave room for its messages;
// with it off, above 80 KB leave room to turn it on later.
uint32_t cacheBudget(uint32_t saveBytes, uint32_t romBytes) {
    constexpr uint32_t MISC = 24 * 1024, STREAM_MESSAGES = 16 * 1024, STREAM_START = 64 * 1024;
    constexpr uint32_t GOOD = 80 * 1024, MAX = 160 * 1024;
    uint32_t tables = romBytes / emu::PAGE_SIZE * 5;  // page map and flags
    uint32_t reserve = MISC + saveBytes + tables + (stream::running() ? STREAM_MESSAGES : 0);
    uint32_t heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t free = heap > reserve ? heap - reserve : 0;
    free -= free / 64;  // malloc's own header per page
    if (stream::running()) return std::min(free, MAX);
    return std::min({free, MAX, std::max(GOOD, free > STREAM_START ? free - STREAM_START : 0)});
}

String memoryReport() {
    return "RAM: " + String(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024) + " KB, block " +
           String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024) + " KB";
}

// Reads pages out of the flash-mapped ROM into the RAM cache: a miss costs ~20 us, not an SD read.
struct MappedRom : emu::RomSource {
    const uint8_t *rom;
    uint32_t n;
    MappedRom(const uint8_t *r, uint32_t size) : rom(r), n(size) {}
    uint32_t size() override { return n; }
    bool read(uint32_t offset, uint8_t *dst, uint32_t len) override {
        memcpy(dst, rom + offset, len);
        return true;
    }
};
constexpr uint32_t MIN_CACHE = 32 * 1024, FLASH_CACHE = 64 * 1024;

void copyProgress(uint32_t done, uint32_t total) {
    if (done == 0 || done == 64 * 1024) {
        lcd().fillScreen(BG);
        header("First run of this game");
        text("Copying to flash memory,", 6, 30);
        text("later starts will be quick.", 6, 54, DIM);
    }
    lcd().fillRoundRect(8, 90, W - 16, 10, 5, LINE);
    lcd().fillRoundRect(8, 90, std::max<int>(10, (W - 16) * (uint64_t)done / total), 10, 5, ACCENT);
}

int gamesStarted = 0;  // since boot

void play(const String &path) {
    message("Loading", fit(path.substring(path.lastIndexOf('/') + 1), W - 12));
    SdRom rom(path);
    if (!rom.f) {
        message("Could not open", "Can't read the file on SD.", "", "Any key — back");
        waitKey();
        return;
    }
    // The ROM goes to flash (copied once per game), so no SD reads while playing. Hot pages still go
    // to a small RAM cache when there is room: flash-mapped reads share the flash cache with the code.
    // Without the flash partition, or for a ROM bigger than it, pages come from the SD card.
    const uint8_t *mapped = flashrom::map(path, rom.size(), copyProgress);
    uint32_t budget = cacheBudget(emu::saveSizeFromHeader(rom), rom.size());
    MappedRom flashRom(mapped, rom.size());
    const char *err;
    if (!mapped) {
        err = emu::load(rom, budget);
    } else {
        err = budget >= MIN_CACHE ? emu::load(flashRom, std::min(budget, FLASH_CACHE)) : "";
        if (err) err = emu::load(rom, 0, mapped);  // no room for a cache: read the flash directly
    }
    if (err) {
        flashrom::unmap();
        if (gamesStarted > 0) {
            // Memory left fragmented by the last game and Wi-Fi: start this one after a clean reboot.
            prefs.begin("gb", false);
            prefs.putString("next", path);
            prefs.end();
            message("Switching game", "Restarting…");
            delay(300);
            ESP.restart();
        }
        message("Can't start", err, memoryReport(), "Any key — back");
        waitKey();
        return;
    }
    Serial.printf("%s: %u KB, %s, %s, cache %u pages, save %u B, heap %u free, largest %u\n", emu::title(),
                  emu::romSize() / 1024, emu::isColor() ? "GBC" : "GB", mapped ? "ROM in flash" : "ROM from SD",
                  emu::cacheSlots(), emu::saveSize(),
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    gamesStarted++;
    String save = savePath(path);
    loadSave(save);
    emu::setDmgPalette(settings.palette);
    emu::setExecutionMode((emu::ExecutionMode)settings.execution);
    applyVolume();
    lcd().fillScreen(BG);
    screen::resume();
    screen::setMode((screen::Mode)settings.mode);

    uint32_t next = micros(), lastSave = millis(), statAt = millis();
    uint32_t frames = 0, drawn = 0, busyUs = 0, missesAt = emu::pageMisses();
    int audioBuf = 0;
    bool quit = false, audioLate = false;
    uint32_t frameNo = 0, skippedDue = 0, hudAt = millis(), hudFrames = 0, hudDrawn = 0, hudUs = 0;
    while (!quit) {
        Keys keys = poll();
        if (keys.back()) {
            screen::pause();
            emu::setButtons(0);
            M5Cardputer.Speaker.stop(SPEAKER_CH);
            writeSave(save);
            endPoll(keys);
            quit = menu() == MenuResult::Quit;
            if (quit) break;
            lcd().fillScreen(BG);
            screen::resume();
            next = micros();
            continue;
        }
        if (keys.hit('\\')) {
            settings.mode = (settings.mode + 1) % screen::MODES;
            screen::setMode((screen::Mode)settings.mode);
            saveSettings();
        }
        if (keys.hit('f') || keys.hit('F')) screen::setHud(screen::hud()[0] ? "" : "...");
        // Held [ ] scroll one frame line per emulated frame: the whole zoom range in about 0.9 s.
        if (keys.has('[')) screen::pan(-1);
        if (keys.has(']')) screen::pan(1);
        if (keys.hit('-') || keys.hit('=')) {
            settings.volume = constrain(settings.volume + (keys.hit('=') ? 1 : -1), 1, VOLUME_LEVELS);
            applyVolume();
            saveSettings();
        }
        emu::setButtons(gameButtons(keys) | stream::phoneButtons());
        endPoll(keys);

        // Draw only when the screen task has taken the last frame and we are not behind.
        // Draw every other frame (30 fps is plenty, and it halves the picture work), skip a due one when
        // behind, but never more than three in a row: the picture must keep moving.
        bool late = settings.sound ? audioLate : (int32_t)(micros() - next) > (int32_t)FRAME_US / 2;
        bool want = settings.mode != screen::OFF || stream::viewers() > 0;
        bool due = want && (frameNo++ & 1) == 0;
        bool render = due && screen::idle() && (!late || skippedDue >= 3);
        if (due) skippedDue = render ? 0 : skippedDue + 1;
        uint32_t t0 = micros();
        if (!emu::runFrame(render)) {
            screen::pause();
            bool fastFailed = emu::executionMode() == emu::FAST;
            if (fastFailed) {
                settings.execution = emu::COMPATIBLE;
                emu::setExecutionMode(emu::COMPATIBLE);
                saveSettings();
            }
            message("Game crashed", String("Error: ") + emu::error(),
                    fastFailed ? "Fast CPU is off now; start again." : "The save was written.",
                    "Any key — back");
            writeSave(save);
            waitKey();
            break;
        }
        uint32_t us = micros() - t0;
        busyUs += us;
        hudUs += us;
        frames++;
        hudFrames++;
        if (render && screen::submit()) drawn++, hudDrawn++;
        if (millis() - hudAt >= 1000) {
            if (screen::hud()[0]) {
                char line[40];
                snprintf(line, sizeof(line), "%u fps · %u drawn · %.1f ms", (unsigned)hudFrames, (unsigned)hudDrawn,
                         hudFrames ? hudUs / 1000.0f / hudFrames : 0.0f);
                screen::setHud(line);
            }
            hudAt = millis();
            hudFrames = hudDrawn = hudUs = 0;
        }

        if (settings.sound) {
            // The speaker queue sets the pace: wait while two buffers are still waiting to play.
            emu::audioFrame(audio[audioBuf]);
            audioLate = M5Cardputer.Speaker.isPlaying(SPEAKER_CH) == 0;  // ran dry: skip drawing next frame
            while (M5Cardputer.Speaker.isPlaying(SPEAKER_CH) >= 2) delay(1);
            M5Cardputer.Speaker.playRaw(audio[audioBuf], AUDIO_SAMPLES_TOTAL, AUDIO_SAMPLE_RATE, true, 1, SPEAKER_CH);
            audioBuf = (audioBuf + 1) % 3;
            next = micros();
        } else {
            next += FRAME_US;
            int32_t wait = next - micros();
            if (wait > 1000) delay(wait / 1000);
            else if (wait < -100000) next = micros();  // far behind (a slow SD read): don't rush to catch up
        }

        if (millis() - lastSave > SAVE_EVERY_MS) {
            writeSave(save);
            lastSave = millis();
        }
        if (millis() - statAt >= 5000) {
            float sec = (millis() - statAt) / 1000.0f;
            Serial.printf("%.1f fps, drawn %.1f fps, emu %u us/frame, %u page misses, heap %u (largest %u), "
                          "screen stack left %u, viewers %d, stream %u frames %u messages\n",
                          frames / sec, drawn / sec, frames ? busyUs / frames : 0, emu::pageMisses() - missesAt,
                          ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), screen::stackLeft(),
                          stream::viewers(), stream::sentFrames(), stream::sentMessages());
            frames = drawn = busyUs = 0;
            missesAt = emu::pageMisses();
            statAt = millis();
        }
    }
    screen::pause();
    M5Cardputer.Speaker.stop(SPEAKER_CH);
    writeSave(save);
    emu::unload();
    flashrom::unmap();
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);
    lcd().setRotation(1);
    lcd().setBrightness(80);
    lcd().loadFont(FONT_UI);
    loadSettings();
    screen::begin();
    M5Cardputer.Speaker.begin();
    applyVolume();

    Serial.printf("boot: heap %u free, largest %u\n", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    message("Game Boy", "Looking for the SD card…");
    sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    while (!SD.begin(SD_CS, sdSpi, 25000000)) {
        message("No SD card", "Insert a card with games", "and press any key.");
        waitKey();
    }
}

void loop() {
    if (settings.stream) stream::start();
    // A game chosen just before a reboot (see play()) starts right away.
    prefs.begin("gb", false);
    String next = prefs.getString("next", "");
    if (next.length()) prefs.remove("next");
    prefs.end();
    play(next.length() && SD.exists(next) ? next : pickRom());
}
