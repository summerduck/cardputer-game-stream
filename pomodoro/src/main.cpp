// Pomodoro timer for M5Stack Cardputer ADV: focus / short break / long break, with a chime.
#include <M5Cardputer.h>
#include <Preferences.h>

#include "font_ui.h"

// M5Cardputer.Display is a reference member: never bind it at global scope (static-init order).
static M5GFX &lcd() { return M5Cardputer.Display; }

namespace {

constexpr uint16_t BG = TFT_BLACK;
constexpr uint16_t FG = TFT_WHITE;
constexpr uint16_t DIM = 0x8410;      // grey
constexpr uint16_t SELECT = 0x2124;   // list highlight
constexpr uint16_t LINE = 0x2945;
constexpr uint16_t TOMATO = 0xFA45;   // focus
constexpr uint16_t GREEN = 0x3EAB;    // short break
constexpr uint16_t BLUE = 0x4D7F;     // long break
constexpr int W = 240, H = 135;
constexpr int CLOCK_Y = 26, CLOCK_H = 54;
constexpr int BAR_Y = 86;
constexpr uint32_t DIM_AFTER_MS = 20000;  // running: dim the screen after this long without keys

constexpr uint8_t BRIGHTNESS[] = {10, 20, 50, 90, 150, 255};
constexpr int BRIGHT_LEVELS = sizeof(BRIGHTNESS);

enum Phase { FOCUS, SHORT, LONG };
const char *PHASE_NAME[] = {"Фокус", "Перерыв", "Длинный перерыв"};
const uint16_t PHASE_COLOR[] = {TOMATO, GREEN, BLUE};

struct Settings {
    int minutes[3] = {25, 5, 15};
    int longEvery = 4;       // a long break after this many focus rounds
    bool autoStart = false;  // start the next phase without a key press
    bool sound = true;
};
Settings cfg;
Preferences prefs;

enum class Screen { Timer, Settings, Help };
Screen screen = Screen::Timer;

M5Canvas *clockCanvas = nullptr;

Phase phase = FOCUS;
bool running = false;
bool overtime = false;     // the phase ended and waits for a key
uint32_t endAt = 0;        // millis() when the running phase ends
uint32_t remainingMs = 0;  // while paused
uint32_t overSince = 0;    // millis() when the phase ended
int doneInCycle = 0;       // focus rounds since the last long break
uint32_t totalDone = 0;    // focus rounds ever (kept in flash)
int reminders = 0;         // chimes left while nobody reacts

int brightLevel = 3;
bool dimmed = false;
uint32_t lastInput = 0;
int settingSel = 0;
uint32_t toastUntil = 0;
String shownClock, shownRow;
int shownBar = -1;

std::vector<char> prevKeys;

// ---------------------------------------------------------------------------
// sound: a few notes played from loop() without blocking

struct Note {
    uint16_t hz, ms;
};
Note melody[8];
int melodyLen = 0, melodyPos = 0;
uint32_t nextNoteAt = 0;

void playMelody(const Note *notes, int n) {
    if (!cfg.sound) return;
    memcpy(melody, notes, n * sizeof(Note));
    melodyLen = n;
    melodyPos = 0;
    nextNoteAt = millis();
}

void soundTick() {
    if (melodyPos >= melodyLen || static_cast<int32_t>(millis() - nextNoteAt) < 0) return;
    const Note &n = melody[melodyPos++];
    if (n.hz) M5Cardputer.Speaker.tone(n.hz, n.ms);
    nextNoteAt = millis() + n.ms + 40;
}

const Note FOCUS_DONE[] = {{784, 140}, {988, 140}, {1175, 140}, {1568, 320}};  // rising: time to rest
const Note BREAK_DONE[] = {{1175, 140}, {988, 140}, {784, 320}};                // falling: back to work
const Note CLICK[] = {{1400, 25}};

// ---------------------------------------------------------------------------
// settings in flash

void loadSettings() {
    prefs.begin("pomodoro", true);
    cfg.minutes[FOCUS] = prefs.getInt("focus", cfg.minutes[FOCUS]);
    cfg.minutes[SHORT] = prefs.getInt("short", cfg.minutes[SHORT]);
    cfg.minutes[LONG] = prefs.getInt("long", cfg.minutes[LONG]);
    cfg.longEvery = prefs.getInt("every", cfg.longEvery);
    cfg.autoStart = prefs.getBool("auto", cfg.autoStart);
    cfg.sound = prefs.getBool("sound", cfg.sound);
    brightLevel = prefs.getInt("bright", brightLevel);
    totalDone = prefs.getUInt("total", 0);
    prefs.end();
    if (brightLevel < 0 || brightLevel >= BRIGHT_LEVELS) brightLevel = 3;
}

void saveSettings() {
    prefs.begin("pomodoro", false);
    prefs.putInt("focus", cfg.minutes[FOCUS]);
    prefs.putInt("short", cfg.minutes[SHORT]);
    prefs.putInt("long", cfg.minutes[LONG]);
    prefs.putInt("every", cfg.longEvery);
    prefs.putBool("auto", cfg.autoStart);
    prefs.putBool("sound", cfg.sound);
    prefs.putInt("bright", brightLevel);
    prefs.end();
}

void saveTotal() {
    prefs.begin("pomodoro", false);
    prefs.putUInt("total", totalDone);
    prefs.end();
}

// ---------------------------------------------------------------------------
// drawing helpers

void text(const String &s, int x, int y, uint16_t color = FG, textdatum_t datum = top_left) {
    lcd().setTextDatum(datum);
    lcd().setTextColor(color, BG);
    lcd().drawString(s, x, y);
}

void row(const String &left, const String &right, int y, uint16_t leftColor, uint16_t rightColor = DIM) {
    lcd().fillRect(0, y, W, 16, BG);
    text(left, 4, y, leftColor);
    text(right, W - 4, y, rightColor, top_right);
}

String battery() {
    int level = M5Cardputer.Power.getBatteryLevel();
    return level > 0 ? String(level) + "%" : "";
}

void header(const String &title, uint16_t color, const String &right) {
    lcd().fillRect(0, 0, W, 20, BG);
    text(title, 4, 2, color);
    text(right, W - 4, 2, DIM, top_right);
    lcd().drawFastHLine(0, 20, W, LINE);
}

String mmss(uint32_t ms) {
    uint32_t s = (ms + 999) / 1000;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02u:%02u", unsigned(s / 60), unsigned(s % 60));
    return buf;
}

// ---------------------------------------------------------------------------
// timer state

uint32_t phaseMs(Phase p) { return cfg.minutes[p] * 60000UL; }

uint32_t leftMs() {
    if (overtime) return 0;
    if (!running) return remainingMs;
    int32_t left = static_cast<int32_t>(endAt - millis());
    return left > 0 ? left : 0;
}

void setBrightness(int level) { lcd().setBrightness(BRIGHTNESS[level]); }

void wake() {
    lastInput = millis();
    if (dimmed) {
        dimmed = false;
        setBrightness(brightLevel);
    }
}

void setPhase(Phase p) {
    phase = p;
    running = false;
    overtime = false;
    remainingMs = phaseMs(p);
}

void start() {
    if (overtime) return;
    running = true;
    endAt = millis() + remainingMs;
}

void pause() {
    remainingMs = leftMs();
    running = false;
}

Phase nextPhase() {
    if (phase != FOCUS) return FOCUS;
    return doneInCycle >= cfg.longEvery ? LONG : SHORT;
}

// Moves on to the next phase; `counted` = the focus round was finished, not skipped.
void advance(bool counted) {
    if (phase == FOCUS && counted) {
        doneInCycle++;
        totalDone++;
        saveTotal();
    }
    if (phase == LONG) doneInCycle = 0;  // the dots stay full through the long break
    setPhase(nextPhase());
}

// ---------------------------------------------------------------------------
// timer screen

void drawClock() {
    auto &c = *clockCanvas;
    c.fillSprite(BG);
    uint16_t color = overtime ? PHASE_COLOR[phase] : running ? PHASE_COLOR[phase] : DIM;
    if (overtime && (millis() / 500) % 2) color = BG;  // blink while waiting
    c.setTextColor(color);
    c.setTextDatum(middle_center);
    c.drawString(mmss(leftMs()), W / 2, CLOCK_H / 2);
    c.pushSprite(0, CLOCK_Y);
}

// State and the key that moves it on.
String statusLine() {
    if (overtime) return "+" + mmss(millis() - overSince) + (phase == FOCUS ? " · Enter — отдохнуть" : " · Enter — работать");
    if (running) return "пробел — пауза";
    return leftMs() == phaseMs(phase) ? "пробел — старт" : "пауза · пробел — дальше";
}

void drawDots(int y) {
    lcd().fillRect(0, y, 120, 16, BG);
    for (int i = 0; i < cfg.longEvery; i++) {
        int x = 10 + i * 16;
        if (i < doneInCycle) lcd().fillCircle(x, y + 7, 5, TOMATO);
        else lcd().drawCircle(x, y + 7, 5, DIM);
    }
}

// Per-frame refresh: redraws only what changed.
void refreshTimer(bool force) {
    String clock = mmss(leftMs()) + (running ? "r" : "") + (overtime ? String((millis() / 500) % 2) : "");
    if (force || clock != shownClock) {
        shownClock = clock;
        drawClock();
    }
    uint32_t total = phaseMs(phase);
    int bar = overtime ? W - 8 : total ? static_cast<int>((W - 8) * uint64_t(total - leftMs()) / total) : 0;
    if (force || bar != shownBar) {
        shownBar = bar;
        lcd().fillRect(4, BAR_Y, W - 8, 4, LINE);
        lcd().fillRect(4, BAR_Y, bar, 4, PHASE_COLOR[phase]);
    }
    String status = statusLine();
    if (force || status != shownRow) {
        shownRow = status;
        lcd().fillRect(0, 95, W, 16, BG);
        text(status, W / 2, 95, overtime ? PHASE_COLOR[phase] : DIM, top_center);
    }
}

void drawTimer() {
    lcd().fillScreen(BG);
    header(PHASE_NAME[phase], PHASE_COLOR[phase], battery());
    drawDots(114);
    text("всего " + String(totalDone), W - 4, 114, DIM, top_right);
    refreshTimer(true);
}

void toast(const String &note) {
    header(note, FG, "");
    toastUntil = millis() + 1200;
}

// ---------------------------------------------------------------------------
// settings and help

const int SETTING_ROWS = 6;

String settingValue(int i) {
    switch (i) {
        case 0: return String(cfg.minutes[FOCUS]) + " мин";
        case 1: return String(cfg.minutes[SHORT]) + " мин";
        case 2: return String(cfg.minutes[LONG]) + " мин";
        case 3: return String(cfg.longEvery);
        case 4: return cfg.autoStart ? "вкл" : "выкл";
        default: return cfg.sound ? "вкл" : "выкл";
    }
}

void drawSettings() {
    static const char *NAMES[SETTING_ROWS] = {"Фокус", "Перерыв", "Длинный перерыв", "Помидоров до длинного", "Автостарт", "Звук"};
    lcd().fillScreen(BG);
    header("Настройки", FG, "");
    const int rowH = 16, top = 22;
    for (int i = 0; i < SETTING_ROWS; i++) {
        int y = top + i * rowH;
        bool sel = i == settingSel;
        uint16_t bg = sel ? SELECT : BG;
        if (sel) lcd().fillRoundRect(2, y, W - 4, rowH - 1, 4, SELECT);
        lcd().setTextColor(sel ? FG : 0xC618, bg);
        lcd().setTextDatum(top_left);
        lcd().drawString(NAMES[i], 8, y);
        lcd().setTextColor(sel ? TOMATO : DIM, bg);
        lcd().setTextDatum(top_right);
        lcd().drawString(settingValue(i), W - 8, y);
    }
    row(", / — меньше / больше", "` — готово", H - 16, DIM);
}

void changeSetting(int dir) {
    auto clamp = [](int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); };
    switch (settingSel) {
        case 0: cfg.minutes[FOCUS] = clamp(cfg.minutes[FOCUS] + dir * 5, 5, 120); break;
        case 1: cfg.minutes[SHORT] = clamp(cfg.minutes[SHORT] + dir, 1, 30); break;
        case 2: cfg.minutes[LONG] = clamp(cfg.minutes[LONG] + dir * 5, 5, 60); break;
        case 3: cfg.longEvery = clamp(cfg.longEvery + dir, 2, 8); break;
        case 4: cfg.autoStart = !cfg.autoStart; break;
        default: cfg.sound = !cfg.sound; break;
    }
    saveSettings();
    // a phase that hasn't started yet takes the new length
    if (!running && !overtime) remainingMs = phaseMs(phase);
    doneInCycle = std::min(doneInCycle, cfg.longEvery);
    drawSettings();
}

void drawHelp() {
    static const char *ROWS[][2] = {
        {"пробел", "старт / пауза"},  {"; .", "+1 / −1 минута"},   {"N", "следующая фаза"},
        {"R", "начать фазу заново"}, {"T  /  M", "настройки / звук"}, {"B", "яркость"},
    };
    lcd().fillScreen(BG);
    header("Клавиши", FG, "");
    for (int i = 0; i < 6; i++) {
        text(ROWS[i][0], 6, 24 + i * 18, TOMATO);
        text(ROWS[i][1], 78, 24 + i * 18, FG);
    }
}

void redraw() {
    switch (screen) {
        case Screen::Timer: drawTimer(); break;
        case Screen::Settings: drawSettings(); break;
        case Screen::Help: drawHelp(); break;
    }
}

// ---------------------------------------------------------------------------
// input

bool newlyPressed(char c, const std::vector<char> &now) {
    bool isDown = std::find(now.begin(), now.end(), c) != now.end();
    bool wasDown = std::find(prevKeys.begin(), prevKeys.end(), c) != prevKeys.end();
    return isDown && !wasDown;
}

void handleTimer(const Keyboard_Class::KeysState &k) {
    auto pressed = [&](char lower, char upper = 0) {
        return newlyPressed(lower, k.word) || (upper && newlyPressed(upper, k.word));
    };
    if (k.enter || k.space) {
        if (overtime) {
            advance(true);
            start();
        } else if (running) {
            pause();
        } else {
            start();
        }
        playMelody(CLICK, 1);
        drawTimer();
    } else if (pressed(';') || pressed('=') || k.up || pressed('.') || pressed('-') || k.down) {
        if (overtime) return;
        bool more = pressed(';') || pressed('=') || k.up;
        uint32_t left = leftMs();
        if (more) left += 60000;
        else left = left > 61000 ? left - 60000 : 1000;
        if (running) endAt = millis() + left;
        else remainingMs = left;
        refreshTimer(true);
    } else if (pressed('n', 'N')) {
        advance(overtime);
        drawTimer();
    } else if (pressed('r', 'R')) {
        setPhase(phase);
        drawTimer();
    } else if (pressed('t', 'T')) {
        screen = Screen::Settings;
        drawSettings();
    } else if (pressed('m', 'M')) {
        cfg.sound = !cfg.sound;
        saveSettings();
        toast(cfg.sound ? "Звук включён" : "Звук выключен");
    } else if (pressed('b', 'B')) {
        brightLevel = (brightLevel + 1) % BRIGHT_LEVELS;
        setBrightness(brightLevel);
        saveSettings();
        toast("Яркость " + String(brightLevel + 1) + "/" + String(BRIGHT_LEVELS));
    } else if (pressed('h', 'H')) {
        screen = Screen::Help;
        drawHelp();
    }
}

void handleSettings(const Keyboard_Class::KeysState &k) {
    if (newlyPressed(';', k.word) || k.up) {
        settingSel = (settingSel + SETTING_ROWS - 1) % SETTING_ROWS;
        drawSettings();
    } else if (newlyPressed('.', k.word) || k.down) {
        settingSel = (settingSel + 1) % SETTING_ROWS;
        drawSettings();
    } else if (newlyPressed(',', k.word) || k.left) {
        changeSetting(-1);
    } else if (newlyPressed('/', k.word) || k.right || k.enter) {
        changeSetting(1);
    } else if (newlyPressed('`', k.word) || k.backspace || k.esc || newlyPressed('t', k.word) || newlyPressed('T', k.word)) {
        screen = Screen::Timer;
        drawTimer();
    }
}

// ---------------------------------------------------------------------------
// phase end

void phaseEnded() {
    const Note *m = phase == FOCUS ? FOCUS_DONE : BREAK_DONE;
    int n = phase == FOCUS ? 4 : 3;
    playMelody(m, n);
    wake();
    if (cfg.autoStart) {
        advance(true);
        start();
    } else {
        running = false;
        overtime = true;
        overSince = millis();
        reminders = 3;  // remind once a minute, three times
    }
    if (screen == Screen::Timer) drawTimer();
}

void remindTick() {
    if (!overtime || !reminders) return;
    uint32_t over = millis() - overSince;
    if (over >= uint32_t(4 - reminders) * 60000) {
        reminders--;
        playMelody(phase == FOCUS ? FOCUS_DONE : BREAK_DONE, phase == FOCUS ? 4 : 3);
    }
}

}  // namespace

void setup() {
    auto c = M5.config();
    M5Cardputer.begin(c, true);
    lcd().setRotation(1);
    lcd().loadFont(FONT_UI);
    loadSettings();
    setBrightness(brightLevel);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(90);

    clockCanvas = new M5Canvas(&lcd());
    clockCanvas->setColorDepth(16);
    clockCanvas->createSprite(W, CLOCK_H);
    clockCanvas->setFont(&fonts::Font7);  // 7-segment digits, 48 px

    setPhase(FOCUS);
    lastInput = millis();
    drawTimer();
}

void loop() {
    M5Cardputer.update();

    if (M5Cardputer.Keyboard.isChange()) {
        auto k = M5Cardputer.Keyboard.keysState();
        if (M5Cardputer.Keyboard.isPressed()) {
            bool wasDimmed = dimmed;
            wake();
            if (!wasDimmed) {  // a key on a dimmed screen only brightens it
                switch (screen) {
                    case Screen::Timer: handleTimer(k); break;
                    case Screen::Settings: handleSettings(k); break;
                    case Screen::Help:
                        screen = Screen::Timer;
                        drawTimer();
                        break;
                }
            }
        }
        prevKeys = k.word;
    }

    if (running && static_cast<int32_t>(millis() - endAt) >= 0) phaseEnded();
    remindTick();
    soundTick();

    if (screen == Screen::Timer) refreshTimer(false);
    if (toastUntil && static_cast<int32_t>(millis() - toastUntil) >= 0) {
        toastUntil = 0;
        if (screen == Screen::Timer) header(PHASE_NAME[phase], PHASE_COLOR[phase], battery());
    }
    if (running && !dimmed && millis() - lastInput > DIM_AFTER_MS) {
        dimmed = true;
        setBrightness(0);
    }

    delay(20);
}
