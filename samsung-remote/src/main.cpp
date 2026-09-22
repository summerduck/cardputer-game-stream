// Samsung TV / Freestyle remote + Bluetooth keyboard for M5Stack Cardputer ADV.
// TAB switches between REMOTE (IR commands) and KEYBOARD (types text over Bluetooth).
#include <M5Cardputer.h>
#include <IRremote.hpp>
#include "ble_keyboard.h"
#include <esp_system.h>

constexpr uint8_t IR_TX_PIN = 44;
constexpr uint32_t REPEAT_MS = 110;  // resend interval while a key is held
constexpr const char *BLE_NAME = "Cardputer KB";

// Samsung32 addresses of the "Samsung Uni" profiles in Ultimate Remote (7,7 and 5,5)
constexpr uint16_t ADDRESSES[] = {0x0707, 0x0505};
uint8_t addrIndex = 0;

struct Binding {
    char key;
    uint8_t cmd;
    const char *label;
    bool repeat;  // keep sending while held
};

// Codes from Ultimate Remote's Samsung_TV_7_7 table
const Binding BINDINGS[] = {
    {';', 96, "UP", true},     {'.', 97, "DOWN", true},   {',', 101, "LEFT", true},
    {'/', 98, "RIGHT", true},  {'=', 7, "VOL +", true},   {'-', 11, "VOL -", true},
    {'p', 2, "POWER", false},  {'m', 15, "MUTE", false},  {'h', 121, "HOME", false},
    {'s', 1, "SOURCE", false}, {'e', 45, "EXIT", false},  {'n', 26, "MENU", false},
    {'b', 88, "BACK", false},
};
constexpr uint8_t CMD_OK = 104;
constexpr uint8_t CMD_BACK = 88;

enum class Mode { Remote, Keyboard };
Mode mode = Mode::Remote;

const Binding *held = nullptr;
uint32_t lastSend = 0;

String typed;             // text shown in keyboard mode
std::vector<char> prevWord;  // keys held at the previous keyboard change
bool lastConnected = false;
bool bleStarted = false;
bool bleSkipped = false;  // previous boot crashed, keep Bluetooth off so the remote still works

// M5Cardputer.Display is a reference member, so never bind it at global scope (static-init order).

void drawHeader(const char *title, uint16_t color) {
    auto &d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    d.fillRect(0, 0, d.width(), 22, color);
    d.setTextColor(TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(4, 3);
    d.print(title);
    d.setTextSize(1);
    d.setCursor(d.width() - 64, 8);
    d.print("TAB: mode");
}

void drawRemote(const char *sent) {
    auto &d = M5Cardputer.Display;
    drawHeader("REMOTE", TFT_CYAN);
    d.setTextSize(1);
    d.setTextColor(TFT_DARKGREY);
    d.setCursor(4, 28);
    d.printf("IR addr 0x%04X  (A = switch)", ADDRESSES[addrIndex]);

    d.setTextColor(TFT_WHITE);
    const char *legend[] = {
        "; . , /  arrows    ENTER  OK",
        "= / -    volume    ` BKSP back",
        "P power  M mute    H home",
        "S source N menu    E exit",
    };
    for (int i = 0; i < 4; i++) {
        d.setCursor(4, 44 + i * 14);
        d.print(legend[i]);
    }

    if (sent) {
        d.setTextSize(2);
        d.setTextColor(TFT_GREEN);
        d.setCursor(4, 110);
        d.print("> ");
        d.print(sent);
    }
}

void drawKeyboard() {
    auto &d = M5Cardputer.Display;
    bool conn = blekb::isConnected();
    drawHeader("KEYBOARD", conn ? TFT_GREEN : TFT_ORANGE);

    d.setTextSize(1);
    d.setCursor(4, 28);
    if (bleSkipped) {
        d.setTextColor(TFT_RED);
        d.print("BT OFF - last start crashed");
        d.setCursor(4, 40);
        d.print("Restart the Cardputer to try again");
    } else if (!blekb::isReady()) {
        d.setTextColor(TFT_RED);
        d.print("BT FAILED - Bluetooth did not start");
    } else if (conn) {
        d.setTextColor(TFT_GREEN);
        d.print("Bluetooth connected - just type");
    } else {
        d.setTextColor(TFT_ORANGE);
        const char *help[] = {
            "Not connected. On the projector:",
            "Settings > Connection > External",
            "Device Manager > Input Device Manager",
            "> Bluetooth Device List",
        };
        for (int i = 0; i < 4; i++) {
            d.setCursor(4, 28 + i * 10);
            d.print(help[i]);
        }
        d.setCursor(4, 68);
        d.printf("and pair \"%s\"", BLE_NAME);
    }

    d.setTextColor(TFT_DARKGREY);
    d.setCursor(4, 82);
    d.print("ENTER enter  BKSP erase  fn+;.,/ arrows");
    d.setCursor(4, 92);
    d.print("fn+` esc/back   English letters only");

    // Last characters typed, so you can see what went out
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE);
    d.setCursor(4, 110);
    const int maxChars = 18;
    String tail = typed.length() > maxChars ? typed.substring(typed.length() - maxChars) : typed;
    d.print(tail);
    d.print("_");
}

void redraw() {
    if (mode == Mode::Remote) drawRemote(nullptr);
    else drawKeyboard();
}

void sendIr(uint8_t cmd, const char *label) {
    IrSender.sendSamsung(ADDRESSES[addrIndex], cmd, 0);
    lastSend = millis();
    drawRemote(label);
}

const Binding *findBinding(char c) {
    for (auto &b : BINDINGS)
        if (b.key == c) return &b;
    return nullptr;
}

void handleRemote(const Keyboard_Class::KeysState &state) {
    if (state.enter) {
        sendIr(CMD_OK, "OK");
        return;
    }
    if (state.backspace || state.del || state.esc) {
        sendIr(CMD_BACK, "BACK");
        return;
    }
    for (char c : state.word) {
        c = tolower(c);
        if (c == 'a') {
            addrIndex = (addrIndex + 1) % (sizeof(ADDRESSES) / sizeof(ADDRESSES[0]));
            drawRemote(nullptr);
        } else if (c == '`') {
            sendIr(CMD_BACK, "BACK");
        } else if (const Binding *b = findBinding(c)) {
            sendIr(b->cmd, b->label);
            if (b->repeat) held = b;
        }
        return;
    }
}

void handleKeyboard(const Keyboard_Class::KeysState &state) {
    using namespace blekb;
    if (state.fn) {  // fn layer: arrows, esc, delete
        if (state.up) tapKey(HID_UP);
        else if (state.down) tapKey(HID_DOWN);
        else if (state.left) tapKey(HID_LEFT);
        else if (state.right) tapKey(HID_RIGHT);
        else if (state.esc) tapKey(HID_ESC);
        else if (state.del) tapKey(HID_DELETE);
        return;
    }
    if (state.enter) {
        tapKey(HID_ENTER);
        typed = "";
    } else if (state.backspace) {
        tapKey(HID_BACKSPACE);
        if (typed.length()) typed.remove(typed.length() - 1);
    } else {
        // word holds every key currently down; only type the ones that just went down
        for (char c : state.word) {
            if (std::find(prevWord.begin(), prevWord.end(), c) != prevWord.end()) continue;
            if (typeChar(c)) typed += c;
        }
    }
    if (typed.length() > 200) typed = typed.substring(typed.length() - 100);
    drawKeyboard();
}

void setup() {
    auto &d = M5Cardputer.Display;
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    d.setRotation(1);
    IrSender.begin(IR_TX_PIN);
    esp_reset_reason_t r = esp_reset_reason();
    bleSkipped = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                 r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
    redraw();
}

void loop() {
    auto &d = M5Cardputer.Display;
    M5Cardputer.update();

    if (M5Cardputer.Keyboard.isChange()) {
        held = nullptr;
        auto state = M5Cardputer.Keyboard.keysState();
        if (M5Cardputer.Keyboard.isPressed()) {
            if (state.tab) {
                mode = (mode == Mode::Remote) ? Mode::Keyboard : Mode::Remote;
                // Bluetooth starts only when the keyboard is first needed
                if (mode == Mode::Keyboard && !bleStarted && !bleSkipped) {
                    drawHeader("KEYBOARD", TFT_ORANGE);
                    d.setCursor(4, 28);
                    d.setTextColor(TFT_ORANGE);
                    d.print("Starting Bluetooth...");
                    blekb::begin(BLE_NAME);
                    bleStarted = true;
                }
                redraw();
            } else if (mode == Mode::Remote) {
                handleRemote(state);
            } else {
                handleKeyboard(state);
            }
        }
        prevWord = state.word;
    } else if (held && M5Cardputer.Keyboard.isPressed() && millis() - lastSend >= REPEAT_MS) {
        IrSender.sendSamsung(ADDRESSES[addrIndex], held->cmd, 0);
        lastSend = millis();
    }

    // Refresh the keyboard screen when the Bluetooth link comes or goes
    bool conn = blekb::isConnected();
    if (conn != lastConnected) {
        lastConnected = conn;
        if (mode == Mode::Keyboard) drawKeyboard();
    }

    delay(5);
}
